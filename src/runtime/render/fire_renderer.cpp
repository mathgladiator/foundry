#include "render/fire_renderer.h"

#include <algorithm>
#include <cstring>

#include "fire_ps_spirv.h"
#include "fire_vs_spirv.h"

namespace runtime {

namespace {

// Per sprite (40 bytes): where and how big, its colour and alpha, its stretch, and whether it adds.
struct Instance {
  float pos_size[4];
  float color[4];
  float stretch, additive;
};

}  // namespace

bool FireRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
                        std::string &error) {
  device_ = device;
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = fire_vs_spirv;
  vs.code_size = fire_vs_spirv_size;
  vs.entrypoint = "vs_main";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = fire_ps_spirv;
  ps.code_size = fire_ps_spirv_size;
  ps.entrypoint = "ps_main";
  ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  ps.num_samplers = 2;  // light.hlsli: the shadow map and the clouds
  ps.num_uniform_buffers = 1;
  SDL_GPUShader *vsh = SDL_CreateGPUShader(device, &vs);
  SDL_GPUShader *psh = SDL_CreateGPUShader(device, &ps);
  if (!vsh || !psh) {
    error = std::string("fire shader creation failed: ") + SDL_GetError();
    if (vsh) SDL_ReleaseGPUShader(device, vsh);
    if (psh) SDL_ReleaseGPUShader(device, psh);
    return false;
  }
  SDL_GPUVertexBufferDescription buffer = {};
  buffer.slot = 0;
  buffer.pitch = sizeof(Instance);
  buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  SDL_GPUVertexAttribute attrs[3] = {};
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32};
  for (int k = 0; k < 2; ++k) {
    SDL_GPUColorTargetDescription color = {};
    color.format = color_format;
    color.blend_state.enable_blend = true;
    if (k == 0) {  // smoke: over
      color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    } else {  // light: added
      color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    }
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vsh;
    info.fragment_shader = psh;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 3;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = false;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = depth_format;
    info.target_info.has_depth_stencil_target = true;
    (k == 0 ? blend_ : add_) = SDL_CreateGPUGraphicsPipeline(device, &info);
  }
  SDL_ReleaseGPUShader(device, vsh);
  SDL_ReleaseGPUShader(device, psh);
  if (!blend_ || !add_) {
    error = std::string("fire pipeline creation failed: ") + SDL_GetError();
    return false;
  }
  return true;
}

void FireRenderer::shutdown() {
  if (!device_) return;
  if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
  if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
  if (blend_) SDL_ReleaseGPUGraphicsPipeline(device_, blend_);
  if (add_) SDL_ReleaseGPUGraphicsPipeline(device_, add_);
  instances_ = nullptr;
  staging_ = nullptr;
  blend_ = add_ = nullptr;
  capacity_ = staging_capacity_ = 0;
}

void FireRenderer::upload(SDL_GPUCopyPass *copy, const std::vector<fire::Sprite> &sprites) {
  smoke_ = light_ = 0;
  if (sprites.empty() || !device_) return;
  const uint32_t size = static_cast<uint32_t>(sprites.size() * sizeof(Instance));
  if (size > staging_capacity_) {
    if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
    staging_capacity_ = size + size / 2;
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = staging_capacity_;
    staging_ = SDL_CreateGPUTransferBuffer(device_, &t);
  }
  Instance *out = static_cast<Instance *>(SDL_MapGPUTransferBuffer(device_, staging_, true));
  for (size_t i = 0; i < sprites.size(); ++i) {
    const fire::Sprite &s = sprites[i];
    Instance &d = out[i];
    std::memcpy(d.pos_size, s.pos, sizeof s.pos);
    d.pos_size[3] = s.size;
    std::memcpy(d.color, s.color, sizeof s.color);
    d.stretch = s.stretch;
    d.additive = s.additive ? 1.0f : 0.0f;
    if (s.additive) ++light_;
    else ++smoke_;
  }
  SDL_UnmapGPUTransferBuffer(device_, staging_);
  if (size > capacity_ || !instances_) {
    if (instances_) SDL_ReleaseGPUBuffer(device_, instances_);
    capacity_ = std::max<uint32_t>(size + size / 2, 16384);
    SDL_GPUBufferCreateInfo b = {};
    b.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    b.size = capacity_;
    instances_ = SDL_CreateGPUBuffer(device_, &b);
  }
  SDL_GPUTransferBufferLocation src = {staging_, 0};
  SDL_GPUBufferRegion r = {instances_, 0, size};
  SDL_UploadToGPUBuffer(copy, &src, &r, true);
}

void FireRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const float view_proj[16], const float view[16],
                        const LightBinding &light) {
  if (smoke_ + light_ == 0 || !instances_) return;
  struct {
    float view_proj[16];
    float right[4], up[4];
  } frame = {};
  std::memcpy(frame.view_proj, view_proj, sizeof frame.view_proj);
  // The view matrix's rows are the camera's axes in world space (column-major: element [col * 4 + row]).
  frame.right[0] = view[0];
  frame.right[1] = view[4];
  frame.right[2] = view[8];
  frame.up[0] = view[1];
  frame.up[1] = view[5];
  frame.up[2] = view[9];
  SDL_GPUBufferBinding vb = {instances_, 0};
  for (int k = 0; k < 2; ++k) {
    const uint32_t count = k == 0 ? smoke_ : light_, first = k == 0 ? 0 : smoke_;
    if (count == 0) continue;
    SDL_BindGPUGraphicsPipeline(pass, k == 0 ? blend_ : add_);
    SDL_PushGPUVertexUniformData(cmd, 0, &frame, sizeof frame);
    SDL_BindGPUFragmentSamplers(pass, 0, light.samplers, 2);
    SDL_PushGPUFragmentUniformData(cmd, 0, &light.uniform, sizeof light.uniform);
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_DrawGPUPrimitives(pass, 6, count, 0, first);
  }
}

}  // namespace runtime
