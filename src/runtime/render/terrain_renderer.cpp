#include "render/terrain_renderer.h"

#include <algorithm>
#include <cstring>

#include "terrain_ps_spirv.h"
#include "terrain_vs_spirv.h"

namespace runtime {

namespace {

constexpr uint32_t kAtlasWidth = 4096;  // texels; the atlas grows in rows of tiles
constexpr uint32_t kFloats = 9;         // per vertex: position, normal, uv, morph

}  // namespace

bool TerrainRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
                           std::string &error) {
  device_ = device;
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = terrain_vs_spirv;
  vs.code_size = terrain_vs_spirv_size;
  vs.entrypoint = "vs_main";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = terrain_ps_spirv;
  ps.code_size = terrain_ps_spirv_size;
  ps.entrypoint = "ps_main";
  ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  ps.num_samplers = 3;  // light.hlsli's shadow map and clouds, then the atlas
  ps.num_uniform_buffers = 1;
  SDL_GPUShader *vsh = SDL_CreateGPUShader(device, &vs);
  SDL_GPUShader *psh = SDL_CreateGPUShader(device, &ps);
  if (!vsh || !psh) {
    error = std::string("terrain shader creation failed: ") + SDL_GetError();
    if (vsh) SDL_ReleaseGPUShader(device, vsh);
    if (psh) SDL_ReleaseGPUShader(device, psh);
    return false;
  }
  SDL_GPUVertexBufferDescription buffers[2] = {};
  buffers[0].slot = 0;
  buffers[0].pitch = kFloats * 4;
  buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  buffers[1].slot = 1;
  buffers[1].pitch = sizeof(TerrainInstance);
  buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  SDL_GPUVertexAttribute attrs[9] = {};
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24};
  attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 32};
  attrs[4] = {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  attrs[5] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
  attrs[6] = {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32};
  attrs[7] = {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48};
  attrs[8] = {8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 64};
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
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // skirts hang both ways
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
  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
  si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler_ = SDL_CreateGPUSampler(device, &si);
  if (!pipeline_ || !sampler_) {
    error = std::string("terrain pipeline creation failed: ") + SDL_GetError();
    return false;
  }
  return true;
}

void TerrainRenderer::shutdown() {
  if (!device_) return;
  if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
  instances_ = nullptr;
  instance_capacity_ = 0;
  if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
  staging_ = nullptr;
  staging_capacity_ = 0;
  if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
  sampler_ = nullptr;
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  pipeline_ = nullptr;
}

TerrainMesh TerrainRenderer::upload(SDL_GPUCopyPass *copy, const Terrain &t) {
  TerrainMesh g;
  const size_t nodes = t.nodes.size();
  if (t.vertices.empty() || t.indices.empty() || nodes == 0 || t.image_size == 0 ||
      t.images.size() < nodes * size_t(t.image_size) * t.image_size)
    return g;
  auto put = [&](SDL_GPUBufferUsageFlags usage, const void *data, uint32_t size) {
    SDL_GPUBufferCreateInfo b = {};
    b.usage = usage;
    b.size = size;
    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(device_, &b);
    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb.size = size;
    SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(device_, &tb);
    std::memcpy(SDL_MapGPUTransferBuffer(device_, xfer, false), data, size);
    SDL_UnmapGPUTransferBuffer(device_, xfer);
    SDL_GPUTransferBufferLocation src = {xfer, 0};
    SDL_GPUBufferRegion region = {buf, 0, size};
    SDL_UploadToGPUBuffer(copy, &src, &region, false);
    SDL_ReleaseGPUTransferBuffer(device_, xfer);
    return buf;
  };
  const uint32_t vsize = static_cast<uint32_t>(t.vertices.size() * 4), isize = static_cast<uint32_t>(t.indices.size() * 4);
  g.vertices = put(SDL_GPU_BUFFERUSAGE_VERTEX, t.vertices.data(), vsize);
  g.indices = put(SDL_GPU_BUFFERUSAGE_INDEX, t.indices.data(), isize);
  // Every node's image as a tile of one atlas, row after row of tiles.
  const uint32_t S = t.image_size;
  g.tile = S;
  g.columns = std::max<uint32_t>(1, kAtlasWidth / S);
  g.width = g.columns * S;
  const uint32_t rows = static_cast<uint32_t>((nodes + g.columns - 1) / g.columns);
  g.height = rows * S;
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D;
  ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  ti.width = g.width;
  ti.height = g.height;
  ti.layer_count_or_depth = 1;
  ti.num_levels = 1;
  g.atlas = SDL_CreateGPUTexture(device_, &ti);
  const uint32_t asize = g.width * g.height * 4;
  SDL_GPUTransferBufferCreateInfo tb = {};
  tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tb.size = asize;
  SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(device_, &tb);
  auto *texels = static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(device_, xfer, false));
  std::memset(texels, 0, asize);
  for (size_t n = 0; n < nodes; ++n) {
    const uint32_t cx = static_cast<uint32_t>(n % g.columns) * S, cy = static_cast<uint32_t>(n / g.columns) * S;
    const uint32_t *img = t.images.data() + n * size_t(S) * S;
    for (uint32_t y = 0; y < S; ++y) std::memcpy(texels + size_t(cy + y) * g.width + cx, img + size_t(y) * S, S * 4);
  }
  SDL_UnmapGPUTransferBuffer(device_, xfer);
  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = xfer;
  src.pixels_per_row = g.width;
  src.rows_per_layer = g.height;
  SDL_GPUTextureRegion dst = {};
  dst.texture = g.atlas;
  dst.w = g.width;
  dst.h = g.height;
  dst.d = 1;
  SDL_UploadToGPUTexture(copy, &src, &dst, false);
  SDL_ReleaseGPUTransferBuffer(device_, xfer);
  g.bytes = uint64_t(vsize) + isize + asize;
  bytes_ += g.bytes;
  return g;
}

void TerrainRenderer::release(TerrainMesh &mesh) {
  if (mesh.vertices) SDL_ReleaseGPUBuffer(device_, mesh.vertices);
  if (mesh.indices) SDL_ReleaseGPUBuffer(device_, mesh.indices);
  if (mesh.atlas) SDL_ReleaseGPUTexture(device_, mesh.atlas);
  bytes_ -= std::min(bytes_, mesh.bytes);
  mesh = {};
}

TerrainInstance *TerrainRenderer::begin_frame(uint32_t count) {
  frame_count_ = count;
  if (count == 0) return nullptr;
  const uint32_t size = count * static_cast<uint32_t>(sizeof(TerrainInstance));
  if (size > staging_capacity_) {
    if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
    staging_capacity_ = size + size / 2;
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = staging_capacity_;
    staging_ = SDL_CreateGPUTransferBuffer(device_, &t);
  }
  mapped_ = true;
  return static_cast<TerrainInstance *>(SDL_MapGPUTransferBuffer(device_, staging_, true));
}

void TerrainRenderer::end_frame(SDL_GPUCopyPass *copy) {
  if (!mapped_) return;
  SDL_UnmapGPUTransferBuffer(device_, staging_);
  mapped_ = false;
  const uint32_t size = frame_count_ * static_cast<uint32_t>(sizeof(TerrainInstance));
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

void TerrainRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<TerrainDraw> &draws,
                           const float view_proj[16], const float eye[3], const LightBinding &light) {
  if (draws.empty() || !instances_) return;
  struct {
    float view_proj[16];
    float eye[4];
    float haze[4];  // rgb, and density per metre (the classic light's aerial perspective)
  } frame = {};
  std::memcpy(frame.view_proj, view_proj, sizeof frame.view_proj);
  frame.eye[0] = eye[0];
  frame.eye[1] = eye[1];
  frame.eye[2] = eye[2];
  frame.haze[0] = 0.66f;
  frame.haze[1] = 0.74f;
  frame.haze[2] = 0.82f;
  frame.haze[3] = 1.0f / 3200.0f;
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_PushGPUVertexUniformData(cmd, 0, &frame, sizeof frame);
  SDL_PushGPUFragmentUniformData(cmd, 0, &light.uniform, sizeof light.uniform);
  const TerrainMesh *bound = nullptr;
  for (uint32_t i = 0; i < draws.size(); ++i) {
    const TerrainDraw &d = draws[i];
    if (!d.mesh->vertices || d.count == 0) continue;
    if (d.mesh != bound) {
      SDL_GPUBufferBinding vbs[2] = {{d.mesh->vertices, 0}, {instances_, 0}};
      SDL_BindGPUVertexBuffers(pass, 0, vbs, 2);
      SDL_GPUBufferBinding ib = {d.mesh->indices, 0};
      SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
      SDL_GPUTextureSamplerBinding samplers[3] = {light.samplers[0], light.samplers[1], {d.mesh->atlas, sampler_}};
      SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
      bound = d.mesh;
    }
    SDL_DrawGPUIndexedPrimitives(pass, d.count, 1, d.first, 0, i);
  }
}

}  // namespace runtime
