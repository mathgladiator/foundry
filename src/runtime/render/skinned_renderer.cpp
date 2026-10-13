#include "render/skinned_renderer.h"

#include <algorithm>
#include <cstring>

#include "scatter_cs_spirv.h"
#include "skinned_ps_spirv.h"
#include "skinned_vs_spirv.h"

namespace runtime {

namespace {

// Vertex layout: float3 position, float3 normal, RGBA8 color, two 16-bit bones, a weight (36 bytes).
struct Vertex {
  float position[3];
  float normal[3];
  uint32_t color;
  uint16_t bone_a, bone_b;
  float bone_w;
};

}  // namespace

bool SkinnedRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format,
                           SDL_GPUTextureFormat depth_format, std::string &error) {
  device_ = device;
  if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
    error = "this GPU backend doesn't take SPIR-V (only Vulkan builds the shaders so far)";
    return false;
  }
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = skinned_vs_spirv;
  vs.code_size = skinned_vs_spirv_size;
  vs.entrypoint = "vs_main";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  vs.num_storage_buffers = 1;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = skinned_ps_spirv;
  ps.code_size = skinned_ps_spirv_size;
  ps.entrypoint = "ps_main";
  ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  SDL_GPUShader *vsh = SDL_CreateGPUShader(device, &vs);
  SDL_GPUShader *psh = SDL_CreateGPUShader(device, &ps);
  if (!vsh || !psh) {
    error = std::string("shader creation failed: ") + SDL_GetError();
    return false;
  }
  SDL_GPUVertexBufferDescription buffers[2] = {};
  buffers[0].slot = 0;
  buffers[0].pitch = sizeof(Vertex);
  buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  buffers[1].slot = 1;
  buffers[1].pitch = sizeof(SkinnedInstance);
  buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  SDL_GPUVertexAttribute attrs[9] = {};
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24};
  attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT2, 28};
  attrs[4] = {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 32};
  attrs[5] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  attrs[6] = {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
  attrs[7] = {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32};
  attrs[8] = {8, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 48};

  SDL_GPUColorTargetDescription color = {};
  color.format = color_format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vsh;
  info.fragment_shader = psh;
  info.vertex_input_state.vertex_buffer_descriptions = buffers;
  info.vertex_input_state.num_vertex_buffers = 2;
  info.vertex_input_state.vertex_attributes = attrs;
  info.vertex_input_state.num_vertex_attributes = 9;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // leaves and cut faces are seen from both sides
  info.depth_stencil_state.enable_depth_test = true;
  info.depth_stencil_state.enable_depth_write = true;
  info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
  info.target_info.color_target_descriptions = &color;
  info.target_info.num_color_targets = 1;
  info.target_info.depth_stencil_format = depth_format;
  info.target_info.has_depth_stencil_target = true;
  pipeline_ = SDL_CreateGPUGraphicsPipeline(device, &info);
  SDL_ReleaseGPUShader(device, vsh);
  SDL_ReleaseGPUShader(device, psh);
  if (!pipeline_) {
    error = std::string("pipeline creation failed: ") + SDL_GetError();
    return false;
  }
  SDL_GPUComputePipelineCreateInfo cs = {};
  cs.code = scatter_cs_spirv;
  cs.code_size = scatter_cs_spirv_size;
  cs.entrypoint = "cs_main";
  cs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  cs.num_readonly_storage_buffers = 2;
  cs.num_readwrite_storage_buffers = 1;
  cs.num_uniform_buffers = 1;
  cs.threadcount_x = 64;
  cs.threadcount_y = 1;
  cs.threadcount_z = 1;
  scatter_ = SDL_CreateGPUComputePipeline(device, &cs);
  if (!scatter_) {
    error = std::string("compute pipeline creation failed: ") + SDL_GetError();
    return false;
  }
  return true;
}

void SkinnedRenderer::shutdown() {
  drop(instances_, instance_capacity_);
  drop(bones_, bone_capacity_);
  drop(rows_, rows_capacity_);
  drop(slots_, slots_capacity_);
  instance_capacity_ = bone_capacity_ = rows_capacity_ = slots_capacity_ = 0;
  if (scatter_) SDL_ReleaseGPUComputePipeline(device_, scatter_);
  scatter_ = nullptr;
  if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
  staging_ = nullptr;
  staging_capacity_ = 0;
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  pipeline_ = nullptr;
}

SDL_GPUBuffer *SkinnedRenderer::make(SDL_GPUBufferUsageFlags usage, uint32_t size) {
  SDL_GPUBufferCreateInfo b = {};
  b.usage = usage;
  b.size = size;
  SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(device_, &b);
  if (buf) {
    ++created_;
    bytes_ += size;
  }
  return buf;
}

void SkinnedRenderer::drop(SDL_GPUBuffer *&buffer, uint32_t size) {
  if (!buffer) return;
  SDL_ReleaseGPUBuffer(device_, buffer);  // SDL defers the release until the GPU is done with it
  ++released_;
  bytes_ -= size;
  buffer = nullptr;
}

void SkinnedRenderer::put(SDL_GPUCopyPass *copy, SDL_GPUBuffer *dst, const void *data, uint32_t size) {
  SDL_GPUTransferBufferCreateInfo t = {};
  t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  t.size = size;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &t);
  std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), data, size);
  SDL_UnmapGPUTransferBuffer(device_, tb);
  SDL_GPUTransferBufferLocation src = {tb, 0};
  SDL_GPUBufferRegion region = {dst, 0, size};
  SDL_UploadToGPUBuffer(copy, &src, &region, false);
  SDL_ReleaseGPUTransferBuffer(device_, tb);
}

SkinnedMesh SkinnedRenderer::upload(SDL_GPUCopyPass *copy, const DrawMesh &mesh) {
  SkinnedMesh g;
  const size_t n = mesh.positions.size();
  if (n == 0 || mesh.indices.empty()) return g;
  std::vector<Vertex> verts(n);
  const bool skinned = mesh.bone_a.size() == n && mesh.bone_b.size() == n && mesh.bone_w.size() == n;
  for (size_t i = 0; i < n; ++i) {
    Vertex &v = verts[i];
    const auto &p = mesh.positions[i];
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    foundry::vec3 nn = mesh.normals.size() == n ? mesh.normals[i] : foundry::vec3{0, 1, 0};
    v.normal[0] = nn.x;
    v.normal[1] = nn.y;
    v.normal[2] = nn.z;
    v.color = mesh.colors.size() == n ? mesh.colors[i] : 0xffb4b4b4u;
    v.bone_a = skinned ? static_cast<uint16_t>(mesh.bone_a[i]) : 0;
    v.bone_b = skinned ? static_cast<uint16_t>(mesh.bone_b[i]) : 0;
    v.bone_w = skinned ? mesh.bone_w[i] : 0;
  }
  const uint32_t vsize = static_cast<uint32_t>(verts.size() * sizeof(Vertex));
  const uint32_t isize = static_cast<uint32_t>(mesh.indices.size() * 4);
  g.vertices = make(SDL_GPU_BUFFERUSAGE_VERTEX, vsize);
  g.indices = make(SDL_GPU_BUFFERUSAGE_INDEX, isize);
  put(copy, g.vertices, verts.data(), vsize);
  put(copy, g.indices, mesh.indices.data(), isize);
  g.index_count = static_cast<uint32_t>(mesh.indices.size());
  g.bytes = uint64_t(vsize) + isize;
  return g;
}

void SkinnedRenderer::release(SkinnedMesh &mesh) {
  const uint32_t isize = mesh.index_count * 4;
  drop(mesh.vertices, static_cast<uint32_t>(mesh.bytes - isize));
  drop(mesh.indices, isize);
  mesh = {};
}

void SkinnedRenderer::grow(SDL_GPUBuffer *&buffer, uint32_t &capacity, SDL_GPUBufferUsageFlags usage, uint32_t need) {
  if (need <= capacity && buffer) return;
  drop(buffer, capacity);
  capacity = std::max<uint32_t>(need + need / 2, 4096);
  buffer = make(usage, capacity);
}

uint32_t SkinnedRenderer::alloc_bones(uint32_t count) {
  auto it = bone_free_.find(count);
  if (it != bone_free_.end() && !it->second.empty()) {
    const uint32_t offset = it->second.back();
    it->second.pop_back();
    return offset;
  }
  const uint32_t offset = bone_top_;
  bone_top_ += count;
  return offset;
}

void SkinnedRenderer::free_bones(uint32_t offset, uint32_t count) {
  if (count) bone_free_[count].push_back(offset);
}

SkinnedRenderer::Frame SkinnedRenderer::begin_frame(uint32_t instances, uint32_t changed_bones) {
  frame_instances_ = instances;
  frame_bones_ = changed_bones;
  Frame f;
  const size_t isize = size_t(instances) * sizeof(SkinnedInstance), rsize = size_t(changed_bones) * 48,
               ssize = size_t(changed_bones) * 4;
  const uint32_t total = static_cast<uint32_t>(isize + rsize + ssize);
  if (total == 0) return f;
  if (total > staging_capacity_) {
    if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
    staging_capacity_ = total + total / 2;
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = staging_capacity_;
    staging_ = SDL_CreateGPUTransferBuffer(device_, &t);
  }
  // Cycling gives a fresh buffer while the GPU still reads last frame's.
  auto *dst = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device_, staging_, true));
  mapped_ = true;
  f.instances = reinterpret_cast<SkinnedInstance *>(dst);
  f.rows = reinterpret_cast<float *>(dst + isize);
  f.slots = reinterpret_cast<uint32_t *>(dst + isize + rsize);
  return f;
}

void SkinnedRenderer::end_frame(SDL_GPUCopyPass *copy) {
  // The bone buffer grows to the allocator's high-water mark, keeping every slot's bones.
  const uint32_t need = std::max<uint32_t>(bone_top_, 1) * 48;
  if (!bones_ || need > bone_capacity_) {
    const uint32_t capacity = std::max<uint32_t>(need + need / 2, 1 << 16);
    SDL_GPUBuffer *bigger = make(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, capacity);
    if (bones_) {
      SDL_GPUBufferLocation from = {bones_, 0}, to = {bigger, 0};
      SDL_CopyGPUBufferToBuffer(copy, &from, &to, bone_capacity_, false);
      drop(bones_, bone_capacity_);
    }
    bones_ = bigger;
    bone_capacity_ = capacity;
  }
  if (!mapped_) return;
  SDL_UnmapGPUTransferBuffer(device_, staging_);
  mapped_ = false;
  const uint32_t isize = frame_instances_ * static_cast<uint32_t>(sizeof(SkinnedInstance)), rsize = frame_bones_ * 48,
                 ssize = frame_bones_ * 4;
  if (isize) {
    grow(instances_, instance_capacity_, SDL_GPU_BUFFERUSAGE_VERTEX, isize);
    SDL_GPUTransferBufferLocation src = {staging_, 0};
    SDL_GPUBufferRegion r = {instances_, 0, isize};
    SDL_UploadToGPUBuffer(copy, &src, &r, true);
  }
  if (frame_bones_) {
    grow(rows_, rows_capacity_, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, rsize);
    grow(slots_, slots_capacity_, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, ssize);
    SDL_GPUTransferBufferLocation src = {staging_, isize};
    SDL_GPUBufferRegion r = {rows_, 0, rsize};
    SDL_UploadToGPUBuffer(copy, &src, &r, true);
    SDL_GPUTransferBufferLocation src2 = {staging_, isize + rsize};
    SDL_GPUBufferRegion r2 = {slots_, 0, ssize};
    SDL_UploadToGPUBuffer(copy, &src2, &r2, true);
  }
}

void SkinnedRenderer::scatter(SDL_GPUCommandBuffer *cmd) {
  if (frame_bones_ == 0 || !bones_) return;
  // Not cycled: the buffer keeps every slot's bones, so this pass writes into the one being drawn.
  SDL_GPUStorageBufferReadWriteBinding rw = {};
  rw.buffer = bones_;
  rw.cycle = false;
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
  SDL_BindGPUComputePipeline(pass, scatter_);
  SDL_GPUBuffer *read[2] = {rows_, slots_};
  SDL_BindGPUComputeStorageBuffers(pass, 0, read, 2);
  const uint32_t count = frame_bones_;
  SDL_PushGPUComputeUniformData(cmd, 0, &count, sizeof count);
  SDL_DispatchGPUCompute(pass, (count + 63) / 64, 1, 1);
  SDL_EndGPUComputePass(pass);
  frame_bones_ = 0;
}

void SkinnedRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<DrawRange> &ranges,
                           const float view_proj[16]) {
  if (ranges.empty() || !instances_ || !bones_) return;
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_BindGPUVertexStorageBuffers(pass, 0, &bones_, 1);
  SDL_PushGPUVertexUniformData(cmd, 0, view_proj, 64);
  for (const DrawRange &r : ranges) {
    if (!r.mesh->vertices || r.count == 0) continue;
    SDL_GPUBufferBinding vbs[2] = {{r.mesh->vertices, 0}, {instances_, 0}};
    SDL_BindGPUVertexBuffers(pass, 0, vbs, 2);
    SDL_GPUBufferBinding ib = {r.mesh->indices, 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(pass, r.mesh->index_count, r.count, 0, 0, r.first);
  }
}

}  // namespace runtime
