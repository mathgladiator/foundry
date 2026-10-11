#include "render/cel_renderer.h"

#include <cmath>
#include <cstring>

#include "cel_ps_spirv.h"
#include "cel_vs_spirv.h"

namespace runtime {

namespace {

// Vertex layout: float3 position, float3 normal, RGBA8 color (28 bytes).
struct Vertex {
  float position[3];
  float normal[3];
  uint32_t color;
};

}  // namespace

bool CelRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
                       std::string &error) {
  device_ = device;
  if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
    error = "this GPU backend doesn't take SPIR-V (only Vulkan builds the cel shader so far)";
    return false;
  }
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = cel_vs_spirv;
  vs.code_size = cel_vs_spirv_size;
  vs.entrypoint = "vs_main";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = cel_ps_spirv;
  ps.code_size = cel_ps_spirv_size;
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
  buffers[1].pitch = sizeof(Instance);
  buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  SDL_GPUVertexAttribute attrs[6] = {};
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24};
  attrs[3] = {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  attrs[4] = {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
  attrs[5] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32};

  SDL_GPUColorTargetDescription color = {};
  color.format = color_format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vsh;
  info.fragment_shader = psh;
  info.vertex_input_state.vertex_buffer_descriptions = buffers;
  info.vertex_input_state.num_vertex_buffers = 2;
  info.vertex_input_state.vertex_attributes = attrs;
  info.vertex_input_state.num_vertex_attributes = 6;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // imported meshes may have mixed winding
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
  return true;
}

void CelRenderer::shutdown() {
  if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  instances_ = nullptr;
  pipeline_ = nullptr;
  instance_capacity_ = 0;
}

void CelRenderer::upload_buffer(SDL_GPUBuffer *buffer, const void *data, uint32_t size) {
  SDL_GPUTransferBufferCreateInfo t = {};
  t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  t.size = size;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &t);
  std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), data, size);
  SDL_UnmapGPUTransferBuffer(device_, tb);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTransferBufferLocation src = {tb, 0};
  SDL_GPUBufferRegion dst = {buffer, 0, size};
  SDL_UploadToGPUBuffer(copy, &src, &dst, false);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(device_, true, &fence, 1);
  SDL_ReleaseGPUFence(device_, fence);
  SDL_ReleaseGPUTransferBuffer(device_, tb);
}

GpuMesh CelRenderer::upload(const runtime::Mesh &mesh) {
  GpuMesh g;
  const size_t n = mesh.positions.size();
  std::vector<Vertex> verts(n);
  for (size_t i = 0; i < n; ++i) {
    const auto &p = mesh.positions[i];
    Vertex &v = verts[i];
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    foundry::vec3 nn = mesh.normals.size() == n ? mesh.normals[i] : foundry::vec3{0, 1, 0};
    v.normal[0] = nn.x;
    v.normal[1] = nn.y;
    v.normal[2] = nn.z;
    v.color = mesh.colors.size() == n ? mesh.colors[i] : 0xffb4b4b4u;
  }
  SDL_GPUBufferCreateInfo vb = {};
  vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  vb.size = static_cast<Uint32>(verts.size() * sizeof(Vertex));
  g.vertices = SDL_CreateGPUBuffer(device_, &vb);
  upload_buffer(g.vertices, verts.data(), vb.size);
  SDL_GPUBufferCreateInfo ib = {};
  ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
  ib.size = static_cast<Uint32>(mesh.indices.size() * 4);
  g.indices = SDL_CreateGPUBuffer(device_, &ib);
  upload_buffer(g.indices, mesh.indices.data(), ib.size);
  g.index_count = static_cast<uint32_t>(mesh.indices.size());
  return g;
}

void CelRenderer::release(GpuMesh &mesh) {
  if (mesh.vertices) SDL_ReleaseGPUBuffer(device_, mesh.vertices);
  if (mesh.indices) SDL_ReleaseGPUBuffer(device_, mesh.indices);
  mesh = {};
}

void CelRenderer::set_instances(const std::vector<Instance> &instances) {
  const uint32_t count = static_cast<uint32_t>(instances.size());
  if (count == 0) return;
  if (count > instance_capacity_) {
    if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
    SDL_GPUBufferCreateInfo b = {};
    b.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    b.size = static_cast<Uint32>(count * sizeof(Instance));
    instances_ = SDL_CreateGPUBuffer(device_, &b);
    instance_capacity_ = count;
  }
  upload_buffer(instances_, instances.data(), static_cast<uint32_t>(count * sizeof(Instance)));
}

void CelRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const GpuMesh &mesh, uint32_t count,
                       const float view_proj[16]) {
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_GPUBufferBinding vbs[2] = {{mesh.vertices, 0}, {instances_, 0}};
  SDL_BindGPUVertexBuffers(pass, 0, vbs, 2);
  SDL_GPUBufferBinding ib = {mesh.indices, 0};
  SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_PushGPUVertexUniformData(cmd, 0, view_proj, 64);
  SDL_DrawGPUIndexedPrimitives(pass, mesh.index_count, count, 0, 0, 0);
}

void view_projection(const float eye[3], const float target[3], float fov_y, float aspect, float near_z,
                     float far_z, float out[16]) {
  float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float &v : f) v /= fl;
  float up[3] = {0, 1, 0};
  float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
  float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (float &v : r) v /= rl;
  float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  // View matrix rows: r, u, -f (right-handed), translation -dot(axis, eye).
  float view[16] = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                    -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]),
                    -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                    (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]), 1};
  // Perspective with depth in [0, 1] (Vulkan/D3D/Metal convention); y flipped for Vulkan is handled by SDL.
  float t = 1.0f / std::tan(fov_y * 0.5f);
  float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, far_z / (near_z - far_z), -1,
                    0, 0, near_z * far_z / (near_z - far_z), 0};
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += proj[k * 4 + rr] * view[c * 4 + k];
      out[c * 4 + rr] = s;
    }
}

}  // namespace runtime
