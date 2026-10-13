#include "viewport/gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "viewport_down_ps_spirv.h"
#include "viewport_full_vs_spirv.h"
#include "viewport_ink_ps_spirv.h"
#include "viewport_line_ps_spirv.h"
#include "viewport_line_vs_spirv.h"
#include "viewport_mesh_ps_spirv.h"
#include "viewport_mesh_vs_spirv.h"
#include "viewport_shadow_ps_spirv.h"
#include "viewport_sky_ps_spirv.h"
#include "viewport_sky_vs_spirv.h"

namespace viewport {

namespace {

std::atomic<GpuRenderer *> g_renderer{nullptr};

using foundry::vec3;
float dot3(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr int kSupersample = 2;
constexpr uint64_t kMeshBudget = uint64_t(2) << 30;  // GPU bytes of cached meshes
constexpr size_t kMeshes = 4;

struct CameraUniform {
  float clip[16];  // column-major
  float eye[4];    // w: orthographic
  float forward[4];
};

struct LineVertex {
  float p[3];
  float c[4];
  float hidden;
};

SDL_GPUShader *shader(SDL_GPUDevice *device, const unsigned char *code, size_t size, const char *entry,
                      SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms) {
  SDL_GPUShaderCreateInfo info = {};
  info.code = code;
  info.code_size = size;
  info.entrypoint = entry;
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.stage = stage;
  info.num_samplers = samplers;
  info.num_uniform_buffers = uniforms;
  return SDL_CreateGPUShader(device, &info);
}

SDL_GPUTexture *texture(SDL_GPUDevice *device, int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = usage;
  info.width = static_cast<Uint32>(w);
  info.height = static_cast<Uint32>(h);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  return SDL_CreateGPUTexture(device, &info);
}

uint64_t sample_of(const runtime::Mesh &m) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };
  const size_t nv = m.positions.size(), ni = m.indices.size();
  for (size_t k = 0; k < 64 && nv; ++k) mix(&m.positions[k * nv / 64], sizeof(vec3));
  for (size_t k = 0; k < 64 && ni; ++k) mix(&m.indices[k * ni / 64], 4);
  for (size_t k = 0; k < 16 && m.colors.size(); ++k) mix(&m.colors[k * m.colors.size() / 16], 4);
  return h;
}

}  // namespace

void set_gpu_renderer(GpuRenderer *renderer) { g_renderer = renderer; }
GpuRenderer *gpu_renderer() { return g_renderer; }

bool GpuRenderer::init(SDL_GPUDevice *device, std::string &error) {
  device_ = device;
  if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
    error = "the GPU viewport needs SPIR-V (Vulkan)";
    return false;
  }
  SDL_GPUShader *mesh_vs = shader(device, viewport_mesh_vs_spirv, viewport_mesh_vs_spirv_size, "mesh_vs",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  SDL_GPUShader *mesh_ps = shader(device, viewport_mesh_ps_spirv, viewport_mesh_ps_spirv_size, "mesh_ps",
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 2);  // the light's shadow and cloud maps; Look and the light
  SDL_GPUShader *full_vs = shader(device, viewport_full_vs_spirv, viewport_full_vs_spirv_size, "full_vs",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader *ink_ps = shader(device, viewport_ink_ps_spirv, viewport_ink_ps_spirv_size, "ink_ps",
                                 SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
  SDL_GPUShader *down_ps = shader(device, viewport_down_ps_spirv, viewport_down_ps_spirv_size, "down_ps",
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  SDL_GPUShader *line_vs = shader(device, viewport_line_vs_spirv, viewport_line_vs_spirv_size, "line_vs",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  SDL_GPUShader *line_ps = shader(device, viewport_line_ps_spirv, viewport_line_ps_spirv_size, "line_ps",
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  SDL_GPUShader *sky_vs = shader(device, viewport_sky_vs_spirv, viewport_sky_vs_spirv_size, "skyfull_vs",
                                 SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader *sky_ps = shader(device, viewport_sky_ps_spirv, viewport_sky_ps_spirv_size, "sky_ps",
                                 SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 3);  // stars, glow, clouds; the sky is the third slot
  SDL_GPUShader *shadow_ps = shader(device, viewport_shadow_ps_spirv, viewport_shadow_ps_spirv_size, "shadow_ps",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
  auto release_shaders = [&] {
    for (SDL_GPUShader *s : {mesh_vs, mesh_ps, full_vs, ink_ps, down_ps, line_vs, line_ps, sky_vs, sky_ps, shadow_ps})
      if (s) SDL_ReleaseGPUShader(device, s);
  };
  if (!mesh_vs || !mesh_ps || !full_vs || !ink_ps || !down_ps || !line_vs || !line_ps || !sky_vs || !sky_ps || !shadow_ps) {
    error = std::string("viewport shaders: ") + SDL_GetError();
    release_shaders();
    return false;
  }
  const SDL_GPUTextureFormat rgba = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  {  // the mesh: shaded colour, albedo, and linear depth, with a depth buffer
    SDL_GPUVertexBufferDescription vb[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      vb[i].slot = i;
      vb[i].pitch = i == 2 ? 4 : 12;
      vb[i].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    }
    SDL_GPUVertexAttribute attrs[3] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                                       {1, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                                       {2, 2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 0}};
    SDL_GPUColorTargetDescription ct[3] = {};
    ct[0].format = rgba;
    ct[1].format = rgba;
    ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = mesh_vs;
    info.fragment_shader = mesh_ps;
    info.vertex_input_state.vertex_buffer_descriptions = vb;
    info.vertex_input_state.num_vertex_buffers = 3;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 3;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // two-sided, as the CPU renderer
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    info.target_info.color_target_descriptions = ct;
    info.target_info.num_color_targets = 3;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    mesh_ = SDL_CreateGPUGraphicsPipeline(device, &info);
    // The shadow map: the same vertices from the key light, depth only, biased by slope.
    info.fragment_shader = shadow_ps;
    info.target_info.color_target_descriptions = nullptr;
    info.target_info.num_color_targets = 0;
    info.rasterizer_state.enable_depth_bias = true;
    info.rasterizer_state.depth_bias_constant_factor = 1.0f;
    info.rasterizer_state.depth_bias_slope_factor = 1.75f;
    shadow_ = SDL_CreateGPUGraphicsPipeline(device, &info);
    // The sky: after the mesh, at the far plane, into the same three targets.
    SDL_GPUGraphicsPipelineCreateInfo sky = {};
    sky.vertex_shader = sky_vs;
    sky.fragment_shader = sky_ps;
    sky.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sky.depth_stencil_state.enable_depth_test = true;
    sky.depth_stencil_state.enable_depth_write = false;
    sky.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    sky.target_info.color_target_descriptions = ct;
    sky.target_info.num_color_targets = 3;
    sky.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    sky.target_info.has_depth_stencil_target = true;
    sky_ = SDL_CreateGPUGraphicsPipeline(device, &sky);
  }
  auto full = [&](SDL_GPUShader *ps, bool blend) {
    SDL_GPUColorTargetDescription ct = {};
    ct.format = rgba;
    if (blend) {
      ct.blend_state.enable_blend = true;
      ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
      ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
      ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = full_vs;
    info.fragment_shader = ps;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(device, &info);
  };
  ink_ = full(ink_ps, false);
  down_ = full(down_ps, false);
  {  // overlay lines, blended over the final image
    SDL_GPUVertexBufferDescription vb = {};
    vb.slot = 0;
    vb.pitch = sizeof(LineVertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[3] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                                       {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12},
                                       {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 28}};
    SDL_GPUColorTargetDescription ct = {};
    ct.format = rgba;
    ct.blend_state.enable_blend = true;
    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = line_vs;
    info.fragment_shader = line_ps;
    info.vertex_input_state.vertex_buffer_descriptions = &vb;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 3;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets = 1;
    lines_ = SDL_CreateGPUGraphicsPipeline(device, &info);
  }
  release_shaders();
  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  nearest_ = SDL_CreateGPUSampler(device, &si);
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
  linear_ = SDL_CreateGPUSampler(device, &si);
  if (!mesh_ || !ink_ || !down_ || !lines_ || !sky_ || !shadow_ || !nearest_ || !linear_) {
    error = std::string("viewport pipelines: ") + SDL_GetError();
    shutdown();
    return false;
  }
  if (!sky_renderer_.init(device, SDL_GPU_TEXTUREFORMAT_INVALID, SDL_GPU_TEXTUREFORMAT_INVALID, error)) {
    shutdown();
    return false;
  }
  return true;
}

void GpuRenderer::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!device_) return;
  for (Mesh &m : meshes_) release(m);
  meshes_.clear();
  release_targets();
  for (SDL_GPUGraphicsPipeline *p : {mesh_, ink_, down_, lines_, sky_, shadow_})
    if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
  mesh_ = ink_ = down_ = lines_ = sky_ = shadow_ = nullptr;
  sky_renderer_.shutdown();
  if (nearest_) SDL_ReleaseGPUSampler(device_, nearest_);
  if (linear_) SDL_ReleaseGPUSampler(device_, linear_);
  nearest_ = linear_ = nullptr;
  device_ = nullptr;
}

void GpuRenderer::release(Mesh &m) {
  for (SDL_GPUBuffer *b : {m.position, m.normal, m.color, m.index})
    if (b) SDL_ReleaseGPUBuffer(device_, b);
  m = {};
}

void GpuRenderer::release_targets() {
  for (SDL_GPUTexture *t : {t_.color, t_.albedo, t_.depth, t_.zbuf, t_.inked, t_.final})
    if (t) SDL_ReleaseGPUTexture(device_, t);
  if (t_.download) SDL_ReleaseGPUTransferBuffer(device_, t_.download);
  t_ = {};
}

bool GpuRenderer::targets_for(int w, int h) {
  if (t_.final && t_.w == w && t_.h == h) return true;
  release_targets();
  const int W = w * kSupersample, H = h * kSupersample;
  const auto target = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  t_.color = texture(device_, W, H, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, target);
  t_.albedo = texture(device_, W, H, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, target);
  t_.depth = texture(device_, W, H, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, target);
  t_.zbuf = texture(device_, W, H, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  t_.inked = texture(device_, W, H, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, target);
  t_.final = texture(device_, w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, target);
  SDL_GPUTransferBufferCreateInfo ti = {};
  ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  ti.size = static_cast<Uint32>(w * h * 4);
  t_.download = SDL_CreateGPUTransferBuffer(device_, &ti);
  t_.w = w;
  t_.h = h;
  return t_.color && t_.albedo && t_.depth && t_.zbuf && t_.inked && t_.final && t_.download;
}

SDL_GPUBuffer *GpuRenderer::upload(const void *data, uint32_t size, SDL_GPUBufferUsageFlags usage) {
  SDL_GPUBufferCreateInfo bi = {};
  bi.usage = usage;
  bi.size = std::max<uint32_t>(size, 4);
  SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(device_, &bi);
  if (!buf || size == 0) return buf;
  // In chunks, so a region of millions of triangles needs no giant staging buffer.
  const uint32_t chunk = std::min<uint32_t>(size, 64u << 20);
  SDL_GPUTransferBufferCreateInfo ti = {};
  ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  ti.size = chunk;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &ti);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  for (uint32_t at = 0; at < size; at += chunk) {
    const uint32_t n = std::min(chunk, size - at);
    void *dst = SDL_MapGPUTransferBuffer(device_, tb, true);
    std::memcpy(dst, static_cast<const uint8_t *>(data) + at, n);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUTransferBufferLocation src = {tb, 0};
    SDL_GPUBufferRegion region = {buf, at, n};
    SDL_UploadToGPUBuffer(copy, &src, &region, false);
  }
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device_, tb);
  return buf;
}

GpuRenderer::Mesh *GpuRenderer::mesh_for(const runtime::Mesh &m) {
  const uint64_t sample = sample_of(m);
  for (auto it = meshes_.begin(); it != meshes_.end(); ++it)
    if (it->positions == m.positions.data() && it->vertices == m.positions.size() && it->indices == m.indices.size() &&
        it->sample == sample) {
      meshes_.splice(meshes_.begin(), meshes_, it);
      return &meshes_.front();
    }
  Mesh g;
  g.positions = m.positions.data();
  g.vertices = m.positions.size();
  g.indices = m.indices.size();
  g.sample = sample;
  const size_t nv = m.positions.size();
  std::vector<vec3> normals;
  const std::vector<vec3> *nrm = &m.normals;
  if (m.normals.size() != nv) {  // none: from the faces around each vertex
    normals.assign(nv, vec3{0, 0, 0});
    for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
      const uint32_t a = m.indices[k], b = m.indices[k + 1], c = m.indices[k + 2];
      if (a >= nv || b >= nv || c >= nv) continue;
      const vec3 e1 = {m.positions[b].x - m.positions[a].x, m.positions[b].y - m.positions[a].y, m.positions[b].z - m.positions[a].z};
      const vec3 e2 = {m.positions[c].x - m.positions[a].x, m.positions[c].y - m.positions[a].y, m.positions[c].z - m.positions[a].z};
      const vec3 f = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
      for (uint32_t v : {a, b, c}) normals[v] = {normals[v].x + f.x, normals[v].y + f.y, normals[v].z + f.z};
    }
    nrm = &normals;
  }
  std::vector<uint32_t> colors;
  const std::vector<uint32_t> *col = &m.colors;
  if (m.colors.size() != nv) {  // none: the CPU renderer's default albedo
    colors.assign(nv, 0xffa8b3b8u);
    col = &colors;
  }
  const uint32_t vsize = static_cast<uint32_t>(nv * sizeof(vec3)), isize = static_cast<uint32_t>(m.indices.size() * 4);
  g.position = upload(m.positions.data(), vsize, SDL_GPU_BUFFERUSAGE_VERTEX);
  g.normal = upload(nrm->data(), vsize, SDL_GPU_BUFFERUSAGE_VERTEX);
  g.color = upload(col->data(), static_cast<uint32_t>(nv * 4), SDL_GPU_BUFFERUSAGE_VERTEX);
  g.index = upload(m.indices.data(), isize, SDL_GPU_BUFFERUSAGE_INDEX);
  g.bytes = uint64_t(vsize) * 2 + nv * 4 + isize;
  meshes_.push_front(g);
  // Keep the few most recent, within the budget.
  uint64_t total = 0;
  size_t kept = 0;
  for (auto it = meshes_.begin(); it != meshes_.end();) {
    total += it->bytes;
    if (++kept > kMeshes || (kept > 1 && total > kMeshBudget)) {
      release(*it);
      it = meshes_.erase(it);
    } else {
      ++it;
    }
  }
  return &meshes_.front();
}

bool GpuRenderer::render(const Scene &scene, int width, int height, Image &out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!device_ || width <= 0 || height <= 0) return false;
  const auto t0 = std::chrono::steady_clock::now();
  if (!targets_for(width, height)) return false;
  const int W = width * kSupersample, H = height * kSupersample;
  const Framing fr = frame(scene, W, H);

  // World to clip, from the framing: x and y as the CPU renderer projects them, depth over a range
  // that holds the whole mesh.
  const runtime::Mesh *mesh = scene.mesh && !scene.mesh->indices.empty() ? scene.mesh : nullptr;
  // Under a sky: its frame, and a ground to catch the shadows.
  const bool sky = scene.sky && scene.sky->observatory && !scene.silhouette;
  runtime::sky::Frame sky_frame;
  if (sky) {
    sky_renderer_.set(scene.sky->observatory, scene.sky->state.constellations);
    sky_frame = runtime::sky::compute(*scene.sky->observatory, scene.sky->state);
  }
  const runtime::Mesh *ground = nullptr;
  float ground_radius = 0;
  if (sky && scene.sky->ground) {
    ground_radius = std::max(2000.0f, fr.radius * 40);  // out to where the fog meets the horizon
    const float *gc = scene.sky->observatory->ground;
    const float key[4] = {ground_radius, gc[0], gc[1], gc[2]};
    if (ground_.positions.empty() || std::memcmp(key, ground_key_, sizeof key) != 0) {
      ground_ = ground_disc(0, 0, ground_radius, gc);
      std::memcpy(ground_key_, key, sizeof key);
    }
    ground = &ground_;
  }
  float near_z = 1e30f, far_z = 0;
  float mesh_lo[3] = {0, 0, 0}, mesh_hi[3] = {0, 0, 0};
  auto reach = [&](vec3 p) {
    const float z = dot3({p.x - fr.eye.x, p.y - fr.eye.y, p.z - fr.eye.z}, fr.forward);
    near_z = std::min(near_z, z);
    far_z = std::max(far_z, z);
  };
  {
    const float r = fr.radius * 3 + 2;
    const vec3 c = {fr.eye.x + fr.forward.x * r, fr.eye.y + fr.forward.y * r, fr.eye.z + fr.forward.z * r};
    reach(c);
    if (mesh) {  // the mesh's box corners
      vec3 lo = mesh->positions[0], hi = mesh->positions[0];
      const size_t step = std::max<size_t>(1, mesh->positions.size() / 65536);
      for (size_t i = 0; i < mesh->positions.size(); i += step) {
        const vec3 &p = mesh->positions[i];
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
      for (int k = 0; k < 8; ++k) reach({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
      for (int k = 0; k < 3; ++k) {
        mesh_lo[k] = k == 0 ? lo.x : k == 1 ? lo.y : lo.z;
        mesh_hi[k] = k == 0 ? hi.x : k == 1 ? hi.y : hi.z;
      }
    }
    if (ground)
      for (int k = 0; k < 4; ++k) reach({k & 1 ? ground_radius : -ground_radius, 0, k & 2 ? ground_radius : -ground_radius});
  }
  const float pad = 0.05f * (far_z - near_z) + 1.0f;
  far_z += pad;
  near_z = fr.ortho ? near_z - pad : std::max(0.01f, std::min(near_z - pad, far_z * 1e-4f + 0.01f));
  if (!fr.ortho) near_z = std::max(near_z, 0.01f);
  const float sx = fr.scale / fr.cx, sy = fr.scale / fr.cy;
  float m[4][4] = {};
  auto row = [&](int r, vec3 axis, float scale, float add) {
    m[r][0] = axis.x * scale;
    m[r][1] = axis.y * scale;
    m[r][2] = axis.z * scale;
    m[r][3] = -dot3(axis, fr.eye) * scale + add;
  };
  row(0, fr.right, sx, 0);
  row(1, fr.up, sy, 0);
  if (fr.ortho) {
    row(2, fr.forward, 1.0f / (far_z - near_z), -near_z / (far_z - near_z));
    m[3][3] = 1;
  } else {
    const float a = far_z / (far_z - near_z), b = -near_z * far_z / (far_z - near_z);
    row(2, fr.forward, a, b);
    row(3, fr.forward, 1, 0);
  }
  CameraUniform cam = {};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) cam.clip[c * 4 + r] = m[r][c];
  cam.eye[0] = fr.eye.x;
  cam.eye[1] = fr.eye.y;
  cam.eye[2] = fr.eye.z;
  cam.eye[3] = fr.ortho ? 1.0f : 0.0f;
  cam.forward[0] = fr.forward.x;
  cam.forward[1] = fr.forward.y;
  cam.forward[2] = fr.forward.z;
  const float look[4] = {scene.silhouette ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, 0, 0};

  Mesh *gm = ground ? mesh_for(*ground) : nullptr;
  Mesh *g = mesh ? mesh_for(*mesh) : nullptr;
  if (g && (!g->position || !g->normal || !g->color || !g->index)) return false;
  if (gm && (!gm->position || !gm->normal || !gm->color || !gm->index)) gm = nullptr;
  auto draw_mesh = [&](SDL_GPURenderPass *pass, const Mesh *m) {
    SDL_GPUBufferBinding vbs[3] = {{m->position, 0}, {m->normal, 0}, {m->color, 0}};
    SDL_BindGPUVertexBuffers(pass, 0, vbs, 3);
    SDL_GPUBufferBinding ib = {m->index, 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(m->indices), 1, 0, 0, 0);
  };
  // The overlay lines, uploaded each render (they're few).
  std::vector<LineVertex> lines;
  for (const OverlaySegment &s : overlay_segments(scene, fr))
    for (vec3 p : {s.a, s.b}) lines.push_back({{p.x, p.y, p.z}, {s.r, s.g, s.bl, s.alpha}, s.hidden});
  SDL_GPUBuffer *line_buf = lines.empty() ? nullptr
                                          : upload(lines.data(), static_cast<uint32_t>(lines.size() * sizeof(LineVertex)),
                                                   SDL_GPU_BUFFERUSAGE_VERTEX);

  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
  {  // the sky's textures, baked when its observatory or constellations changed
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    sky_renderer_.upload(copy);
    SDL_EndGPUCopyPass(copy);
  }
  // The key light's shadow map, around the model and the ground its shadow falls on.
  runtime::ShadowView shadow;
  bool shadowed = false;
  if (sky && g && scene.sky->state.shadows && sky_frame.key_dir[1] > 0.03f) {
    if (SDL_GPUTexture *map = sky_renderer_.shadow_map()) {
      const float centre[3] = {0.5f * (mesh_lo[0] + mesh_hi[0]), 0, 0.5f * (mesh_lo[2] + mesh_hi[2])};
      const float size = std::max({mesh_hi[0] - mesh_lo[0], mesh_hi[1] - mesh_lo[1], mesh_hi[2] - mesh_lo[2]});
      // Long shadows reach out to about three heights from the model.
      const float reach_m = 0.5f * size + std::min(3.0f, 1.0f / std::max(sky_frame.key_dir[1], 0.2f)) * std::max(mesh_hi[1], 0.5f);
      shadow = runtime::shadow_view(sky_frame.key_dir, centre, reach_m + 0.5f, runtime::SkyRenderer::kShadowSize);
      CameraUniform light_cam = {};
      std::memcpy(light_cam.clip, shadow.view_proj, sizeof light_cam.clip);
      SDL_GPUDepthStencilTargetInfo dt = {};
      dt.texture = map;
      dt.clear_depth = 1;
      dt.load_op = SDL_GPU_LOADOP_CLEAR;
      dt.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt);
      SDL_BindGPUGraphicsPipeline(pass, shadow_);
      SDL_PushGPUVertexUniformData(cmd, 0, &light_cam, sizeof light_cam);
      draw_mesh(pass, g);
      SDL_EndGPURenderPass(pass);
      shadowed = true;
    }
  }
  const float eye3[3] = {fr.eye.x, fr.eye.y, fr.eye.z};
  const runtime::LightBinding light =
      sky ? sky_renderer_.light(sky_frame, scene.sky->state, eye3, scene.sky->time, shadowed ? &shadow : nullptr)
          : sky_renderer_.classic_light();
  {  // the mesh
    SDL_GPUColorTargetInfo ct[3] = {};
    for (int i = 0; i < 3; ++i) {
      ct[i].load_op = SDL_GPU_LOADOP_CLEAR;
      ct[i].store_op = SDL_GPU_STOREOP_STORE;
      ct[i].clear_color = {0, 0, 0, 0};
    }
    ct[0].texture = t_.color;
    ct[1].texture = t_.albedo;
    ct[2].texture = t_.depth;
    SDL_GPUDepthStencilTargetInfo dt = {};
    dt.texture = t_.zbuf;
    dt.clear_depth = 1;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, ct, 3, &dt);
    if (g || gm) {
      SDL_BindGPUGraphicsPipeline(pass, mesh_);
      SDL_PushGPUVertexUniformData(cmd, 0, &cam, sizeof cam);
      SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
      SDL_PushGPUFragmentUniformData(cmd, 1, &light.uniform, sizeof light.uniform);
      SDL_BindGPUFragmentSamplers(pass, 0, light.samplers, 2);
      if (gm) {  // the ground, as backdrop
        const float backdrop[4] = {look[0], look[1], 1, 0};
        SDL_PushGPUFragmentUniformData(cmd, 0, backdrop, sizeof backdrop);
        draw_mesh(pass, gm);
        SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
      }
      if (g) draw_mesh(pass, g);
    }
    if (sky && sky_renderer_.ready()) {
      runtime::SkyCamera sc;
      const float axes[3][3] = {{fr.right.x, fr.right.y, fr.right.z}, {fr.up.x, fr.up.y, fr.up.z}, {fr.forward.x, fr.forward.y, fr.forward.z}};
      std::memcpy(sc.eye, eye3, sizeof sc.eye);
      std::memcpy(sc.right, axes[0], sizeof sc.right);
      std::memcpy(sc.up, axes[1], sizeof sc.up);
      std::memcpy(sc.forward, axes[2], sizeof sc.forward);
      sc.tan_y = fr.ortho ? 0.45f : fr.cy / fr.scale;
      sc.tan_x = fr.ortho ? 0.45f * fr.cx / fr.cy : fr.cx / fr.scale;
      const runtime::SkyUniform su = sky_renderer_.sky_uniform(sky_frame, scene.sky->state, sc, scene.sky->time);
      SDL_GPUTextureSamplerBinding ss[3];
      sky_renderer_.sky_samplers(ss);
      SDL_BindGPUGraphicsPipeline(pass, sky_);
      SDL_BindGPUFragmentSamplers(pass, 0, ss, 3);
      SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
      SDL_PushGPUFragmentUniformData(cmd, 1, &light.uniform, sizeof light.uniform);
      SDL_PushGPUFragmentUniformData(cmd, 2, &su, sizeof su);
      SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
  }
  auto fullscreen = [&](SDL_GPUTexture *target, SDL_GPUGraphicsPipeline *pipe, const SDL_GPUTextureSamplerBinding *bind,
                        uint32_t n, bool uniforms) {
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    SDL_BindGPUFragmentSamplers(pass, 0, bind, n);
    if (uniforms) SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
  };
  {  // ink and background, then down to the final size
    const SDL_GPUTextureSamplerBinding ink[3] = {{t_.color, nearest_}, {t_.albedo, nearest_}, {t_.depth, nearest_}};
    fullscreen(t_.inked, ink_, ink, 3, true);
    const SDL_GPUTextureSamplerBinding down[1] = {{t_.inked, linear_}};
    fullscreen(t_.final, down_, down, 1, false);
  }
  if (line_buf) {  // overlay lines
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = t_.final;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, lines_);
    SDL_GPUBufferBinding vb = {line_buf, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    const SDL_GPUTextureSamplerBinding depth = {t_.depth, nearest_};
    SDL_BindGPUFragmentSamplers(pass, 0, &depth, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &cam, sizeof cam);
    SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(lines.size()), 1, 0, 0);
    SDL_EndGPURenderPass(pass);
  }
  {  // read it back
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = t_.final;
    src.w = static_cast<Uint32>(width);
    src.h = static_cast<Uint32>(height);
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {t_.download, 0, 0, 0};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
  }
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!fence) {
    if (line_buf) SDL_ReleaseGPUBuffer(device_, line_buf);
    return false;
  }
  SDL_WaitForGPUFences(device_, true, &fence, 1);
  SDL_ReleaseGPUFence(device_, fence);
  if (line_buf) SDL_ReleaseGPUBuffer(device_, line_buf);
  out.width = width;
  out.height = height;
  out.rgba.resize(size_t(width) * height * 4);
  const auto *px = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device_, t_.download, false));
  std::memcpy(out.rgba.data(), px, out.rgba.size());
  SDL_UnmapGPUTransferBuffer(device_, t_.download);
  for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

}  // namespace viewport
