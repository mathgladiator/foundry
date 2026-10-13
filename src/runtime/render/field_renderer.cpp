#include "render/field_renderer.h"

#include <algorithm>
#include <cstring>

#include "field_ps_spirv.h"
#include "field_vs_spirv.h"

namespace runtime {

namespace {

// Vertex layout: float3 position, float3 normal, RGBA8 colour, float sway (32 bytes).
struct Vertex {
  float position[3];
  float normal[3];
  uint32_t color;
  float sway;
};

}  // namespace

bool FieldRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
                         std::string &error) {
  device_ = device;
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = field_vs_spirv;
  vs.code_size = field_vs_spirv_size;
  vs.entrypoint = "vs_main";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = field_ps_spirv;
  ps.code_size = field_ps_spirv_size;
  ps.entrypoint = "ps_main";
  ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  SDL_GPUShader *vsh = SDL_CreateGPUShader(device, &vs);
  SDL_GPUShader *psh = SDL_CreateGPUShader(device, &ps);
  if (!vsh || !psh) {
    error = std::string("field shader creation failed: ") + SDL_GetError();
    if (vsh) SDL_ReleaseGPUShader(device, vsh);
    if (psh) SDL_ReleaseGPUShader(device, psh);
    return false;
  }
  SDL_GPUVertexBufferDescription buffers[2] = {};
  buffers[0].slot = 0;
  buffers[0].pitch = sizeof(Vertex);
  buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  buffers[1].slot = 1;
  buffers[1].pitch = sizeof(FieldInstance);
  buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  SDL_GPUVertexAttribute attrs[8] = {};
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24};
  attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 28};
  attrs[4] = {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  attrs[5] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 16};
  attrs[6] = {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24};
  attrs[7] = {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 28};
  SDL_GPUColorTargetDescription color = {};
  color.format = color_format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vsh;
  info.fragment_shader = psh;
  info.vertex_input_state.vertex_buffer_descriptions = buffers;
  info.vertex_input_state.num_vertex_buffers = 2;
  info.vertex_input_state.vertex_attributes = attrs;
  info.vertex_input_state.num_vertex_attributes = 8;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // blades are one sheet, seen from both sides
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
    error = std::string("field pipeline creation failed: ") + SDL_GetError();
    return false;
  }
  return true;
}

void FieldRenderer::shutdown() {
  if (!device_) return;
  if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
  instances_ = nullptr;
  instance_capacity_ = 0;
  if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
  staging_ = nullptr;
  staging_capacity_ = 0;
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  pipeline_ = nullptr;
}

FieldMesh FieldRenderer::upload(SDL_GPUCopyPass *copy, const DrawMesh &mesh) {
  FieldMesh g;
  const size_t n = mesh.positions.size();
  if (n == 0 || mesh.indices.empty()) return g;
  std::vector<Vertex> verts(n);
  for (size_t i = 0; i < n; ++i) {
    Vertex &v = verts[i];
    v.position[0] = mesh.positions[i].x;
    v.position[1] = mesh.positions[i].y;
    v.position[2] = mesh.positions[i].z;
    const foundry::vec3 nn = mesh.normals.size() == n ? mesh.normals[i] : foundry::vec3{0, 1, 0};
    v.normal[0] = nn.x;
    v.normal[1] = nn.y;
    v.normal[2] = nn.z;
    v.color = mesh.colors.size() == n ? mesh.colors[i] : 0xff808080u;
    v.sway = mesh.sway.size() == n ? mesh.sway[i] : 0.0f;
  }
  const uint32_t vsize = static_cast<uint32_t>(n * sizeof(Vertex)), isize = static_cast<uint32_t>(mesh.indices.size() * 4);
  auto put = [&](SDL_GPUBufferUsageFlags usage, const void *data, uint32_t size) {
    SDL_GPUBufferCreateInfo b = {};
    b.usage = usage;
    b.size = size;
    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(device_, &b);
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = size;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &t);
    std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), data, size);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUTransferBufferLocation src = {tb, 0};
    SDL_GPUBufferRegion region = {buf, 0, size};
    SDL_UploadToGPUBuffer(copy, &src, &region, false);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    return buf;
  };
  g.vertices = put(SDL_GPU_BUFFERUSAGE_VERTEX, verts.data(), vsize);
  g.indices = put(SDL_GPU_BUFFERUSAGE_INDEX, mesh.indices.data(), isize);
  g.index_count = static_cast<uint32_t>(mesh.indices.size());
  g.bytes = uint64_t(vsize) + isize;
  bytes_ += g.bytes;
  return g;
}

void FieldRenderer::release(FieldMesh &mesh) {
  if (mesh.vertices) SDL_ReleaseGPUBuffer(device_, mesh.vertices);
  if (mesh.indices) SDL_ReleaseGPUBuffer(device_, mesh.indices);
  bytes_ -= std::min(bytes_, mesh.bytes);
  mesh = {};
}

FieldInstance *FieldRenderer::begin_frame(uint32_t count) {
  frame_count_ = count;
  if (count == 0) return nullptr;
  const uint32_t size = count * static_cast<uint32_t>(sizeof(FieldInstance));
  if (size > staging_capacity_) {
    if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
    staging_capacity_ = size + size / 2;
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = staging_capacity_;
    staging_ = SDL_CreateGPUTransferBuffer(device_, &t);
  }
  mapped_ = true;
  return static_cast<FieldInstance *>(SDL_MapGPUTransferBuffer(device_, staging_, true));
}

void FieldRenderer::end_frame(SDL_GPUCopyPass *copy) {
  if (!mapped_) return;
  SDL_UnmapGPUTransferBuffer(device_, staging_);
  mapped_ = false;
  const uint32_t size = frame_count_ * static_cast<uint32_t>(sizeof(FieldInstance));
  if (size > instance_capacity_ || !instances_) {
    if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
    instance_capacity_ = std::max<uint32_t>(size + size / 2, 4096);
    SDL_GPUBufferCreateInfo b = {};
    b.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    b.size = instance_capacity_;
    instances_ = SDL_CreateGPUBuffer(device_, &b);
  }
  SDL_GPUTransferBufferLocation src = {staging_, 0};
  SDL_GPUBufferRegion r = {instances_, 0, size};
  SDL_UploadToGPUBuffer(copy, &src, &r, true);
}

void FieldRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<FieldRange> &ranges,
                         const float view_proj[16], float time) {
  if (ranges.empty() || !instances_) return;
  struct {
    float view_proj[16];
    float clock[4];
  } frame = {};
  std::memcpy(frame.view_proj, view_proj, sizeof frame.view_proj);
  frame.clock[0] = time;
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_PushGPUVertexUniformData(cmd, 0, &frame, sizeof frame);
  for (const FieldRange &r : ranges) {
    if (!r.mesh->vertices || r.count == 0) continue;
    SDL_GPUBufferBinding vbs[2] = {{r.mesh->vertices, 0}, {instances_, 0}};
    SDL_BindGPUVertexBuffers(pass, 0, vbs, 2);
    SDL_GPUBufferBinding ib = {r.mesh->indices, 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(pass, r.mesh->index_count, r.count, 0, 0, r.first);
  }
}

}  // namespace runtime
