#include "render/sky_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sky_ps_spirv.h"
#include "sky_vs_spirv.h"

namespace runtime {

namespace {

constexpr float kRad = 3.14159265f / 180.0f;

uint16_t half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
  const uint32_t m = x & 0x7fffffu;
  if (e <= 0) return static_cast<uint16_t>(sign);
  if (e >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

void set4(float out[4], const float v[3], float w) {
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  out[3] = w;
}
void set4(float out[4], float x, float y, float z, float w) {
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

}  // namespace

SkyCamera sky_camera(const float view[16], const float projection[16]) {
  SkyCamera c;
  const float t[3] = {view[12], view[13], view[14]};
  for (int i = 0; i < 3; ++i) {
    c.right[i] = view[i * 4 + 0];
    c.up[i] = view[i * 4 + 1];
    c.forward[i] = -view[i * 4 + 2];
  }
  for (int i = 0; i < 3; ++i) c.eye[i] = -(view[i * 4 + 0] * t[0] + view[i * 4 + 1] * t[1] + view[i * 4 + 2] * t[2]);
  c.tan_x = projection[0] != 0 ? 1.0f / std::fabs(projection[0]) : 1.0f;
  c.tan_y = projection[5] != 0 ? 1.0f / std::fabs(projection[5]) : 1.0f;
  if (projection[11] == 0.0f) {  // orthographic: give the sky a view of its own
    c.tan_y = 0.45f;
    c.tan_x = 0.45f * std::fabs(projection[5] / std::max(std::fabs(projection[0]), 1e-6f));
  }
  return c;
}

ShadowView shadow_view(const float key_dir[3], const float centre[3], float radius, uint32_t size) {
  ShadowView v;
  float f[3] = {-key_dir[0], -key_dir[1], -key_dir[2]};
  const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float &x : f) x /= std::max(fl, 1e-6f);
  const bool vertical = std::fabs(f[1]) > 0.99f;
  const float up[3] = {0.0f, vertical ? 0.0f : 1.0f, vertical ? 1.0f : 0.0f};
  float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
  const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (float &x : r) x /= std::max(rl, 1e-6f);
  const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const float R = std::max(radius, 1.0f);
  const float D = R + 80.0f;  // depth: casters up to 80 m beyond the box
  v.texel_m = 2 * R / static_cast<float>(size);
  // Snap the centre to whole texels across the light's view.
  float c[3] = {centre[0], centre[1], centre[2]};
  const float cx = dot(c, r), cy = dot(c, u);
  const float sx = std::floor(cx / v.texel_m) * v.texel_m - cx, sy = std::floor(cy / v.texel_m) * v.texel_m - cy;
  for (int i = 0; i < 3; ++i) c[i] += r[i] * sx + u[i] * sy;
  float m[4][4] = {};
  for (int i = 0; i < 3; ++i) {
    m[0][i] = r[i] / R;
    m[1][i] = u[i] / R;
    m[2][i] = f[i] / (2 * D);
  }
  m[0][3] = -dot(r, c) / R;
  m[1][3] = -dot(u, c) / R;
  m[2][3] = (-dot(f, c) + D) / (2 * D);
  m[3][3] = 1;
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) v.view_proj[col * 4 + row] = m[row][col];
  v.bias = 1.5f * v.texel_m / (2 * D);
  std::copy(c, c + 3, v.centre);
  v.radius = R;
  return v;
}

bool SkyRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
                       std::string &error) {
  device_ = device;
  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
  si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  repeat_ = SDL_CreateGPUSampler(device, &si);
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  clamp_ = SDL_CreateGPUSampler(device, &si);
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
  nearest_ = SDL_CreateGPUSampler(device, &si);
  lit_ = texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  clear_ = texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  if (!repeat_ || !clamp_ || !nearest_ || !lit_ || !clear_) {
    error = std::string("sky resources: ") + SDL_GetError();
    return false;
  }
  if (color_format == SDL_GPU_TEXTUREFORMAT_INVALID) return true;
  SDL_GPUShaderCreateInfo vs = {};
  vs.code = sky_vs_spirv;
  vs.code_size = sky_vs_spirv_size;
  vs.entrypoint = "sky_vs";
  vs.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  SDL_GPUShaderCreateInfo ps = {};
  ps.code = sky_ps_spirv;
  ps.code_size = sky_ps_spirv_size;
  ps.entrypoint = "sky_ps";
  ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  ps.num_samplers = 3;
  ps.num_uniform_buffers = 1;
  SDL_GPUShader *vsh = SDL_CreateGPUShader(device, &vs);
  SDL_GPUShader *psh = SDL_CreateGPUShader(device, &ps);
  if (!vsh || !psh) {
    error = std::string("sky shaders: ") + SDL_GetError();
    if (vsh) SDL_ReleaseGPUShader(device, vsh);
    if (psh) SDL_ReleaseGPUShader(device, psh);
    return false;
  }
  SDL_GPUColorTargetDescription color = {};
  color.format = color_format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vsh;
  info.fragment_shader = psh;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.depth_stencil_state.enable_depth_test = true;
  info.depth_stencil_state.enable_depth_write = false;
  info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  info.target_info.color_target_descriptions = &color;
  info.target_info.num_color_targets = 1;
  info.target_info.depth_stencil_format = depth_format;
  info.target_info.has_depth_stencil_target = true;
  pipeline_ = SDL_CreateGPUGraphicsPipeline(device, &info);
  SDL_ReleaseGPUShader(device, vsh);
  SDL_ReleaseGPUShader(device, psh);
  if (!pipeline_) {
    error = std::string("sky pipeline: ") + SDL_GetError();
    return false;
  }
  return true;
}

void SkyRenderer::shutdown() {
  if (!device_) return;
  for (SDL_GPUTexture *t : {stars_, glow_, clouds_, shadow_, lit_, clear_})
    if (t) SDL_ReleaseGPUTexture(device_, t);
  stars_ = glow_ = clouds_ = shadow_ = lit_ = clear_ = nullptr;
  for (SDL_GPUSampler *s : {repeat_, clamp_, nearest_})
    if (s) SDL_ReleaseGPUSampler(device_, s);
  repeat_ = clamp_ = nearest_ = nullptr;
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  pipeline_ = nullptr;
  observatory_.reset();
  device_ = nullptr;
}

SDL_GPUTexture *SkyRenderer::texture(SDL_GPUTextureType type, SDL_GPUTextureFormat format, uint32_t size, uint32_t layers,
                                     SDL_GPUTextureUsageFlags usage) {
  SDL_GPUTextureCreateInfo info = {};
  info.type = type;
  info.format = format;
  info.usage = usage;
  info.width = info.height = size;
  info.layer_count_or_depth = layers;
  info.num_levels = 1;
  return SDL_CreateGPUTexture(device_, &info);
}

void SkyRenderer::set(std::shared_ptr<const sky::Observatory> observatory, const std::vector<uint8_t> &states) {
  if (!observatory) {
    observatory_.reset();
    return;
  }
  if (observatory != observatory_) {
    observatory_ = std::move(observatory);
    std::vector<float> rgba;
    sky::bake_stars(*observatory_, kStarSize, rgba);
    star_texels_.resize(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i) star_texels_[i] = half(rgba[i]);
    pending_stars_ = true;
    std::vector<float> milky;
    sky::bake_milky_way(*observatory_, kGlowSize, milky);
    milky_texels_.resize(milky.size());
    for (size_t i = 0; i < milky.size(); ++i)
      milky_texels_[i] = static_cast<uint8_t>(std::lround(std::clamp(milky[i] / kMilkyWayMax, 0.0f, 1.0f) * 255));
    sky::bake_clouds(observatory_->cloud_seed, kCloudSize, cloud_texels_);
    pending_clouds_ = true;
    states_.assign(1, 0xff);  // the glow is rebaked below
  }
  if (states != states_ || !glow_) {
    states_ = states;
    std::vector<float> glow;
    sky::bake_glow(*observatory_, states_, kGlowSize, glow);
    glow_texels_.resize(glow.size() * 2);  // R the lit glow, G the galaxy's
    for (size_t i = 0; i < glow.size(); ++i) {
      glow_texels_[i * 2] = static_cast<uint8_t>(std::lround(std::clamp(glow[i], 0.0f, 1.0f) * 255));
      glow_texels_[i * 2 + 1] = i < milky_texels_.size() ? milky_texels_[i] : 0;
    }
    pending_glow_ = true;
  }
}

void SkyRenderer::put(SDL_GPUCopyPass *copy, SDL_GPUTexture *tex, uint32_t layer, uint32_t size, const void *data,
                      uint32_t bytes) {
  SDL_GPUTransferBufferCreateInfo ti = {};
  ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  ti.size = bytes;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &ti);
  if (!tb) return;
  void *dst = SDL_MapGPUTransferBuffer(device_, tb, false);
  std::memcpy(dst, data, bytes);
  SDL_UnmapGPUTransferBuffer(device_, tb);
  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = tb;
  src.pixels_per_row = size;
  src.rows_per_layer = size;
  SDL_GPUTextureRegion region = {};
  region.texture = tex;
  region.layer = layer;
  region.w = region.h = size;
  region.d = 1;
  SDL_UploadToGPUTexture(copy, &src, &region, false);
  SDL_ReleaseGPUTransferBuffer(device_, tb);  // freed once the copy has run
}

void SkyRenderer::upload(SDL_GPUCopyPass *copy) {
  if (!device_ || !copy) return;
  if (!dummies_uploaded_) {
    const float one = 1.0f;
    const uint8_t zero[2] = {0, 0};
    put(copy, lit_, 0, 1, &one, 4);
    put(copy, clear_, 0, 1, zero, 2);
    dummies_uploaded_ = true;
  }
  const auto cube = SDL_GPU_TEXTURETYPE_CUBE;
  if (pending_stars_) {
    if (!stars_) stars_ = texture(cube, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, kStarSize, 6, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const uint32_t face = uint32_t(kStarSize) * kStarSize * 4;
    if (stars_)
      for (uint32_t f = 0; f < 6; ++f) put(copy, stars_, f, kStarSize, star_texels_.data() + size_t(f) * face, face * 2);
    star_texels_.clear();
    star_texels_.shrink_to_fit();
    pending_stars_ = false;
  }
  if (pending_glow_) {
    if (!glow_) glow_ = texture(cube, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, kGlowSize, 6, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const uint32_t face = uint32_t(kGlowSize) * kGlowSize * 2;
    if (glow_)
      for (uint32_t f = 0; f < 6; ++f) put(copy, glow_, f, kGlowSize, glow_texels_.data() + size_t(f) * face, face);
    pending_glow_ = false;
  }
  if (pending_clouds_) {
    if (!clouds_) clouds_ = texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, kCloudSize, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (clouds_) put(copy, clouds_, 0, kCloudSize, cloud_texels_.data(), static_cast<uint32_t>(cloud_texels_.size()));
    pending_clouds_ = false;
  }
}

SDL_GPUTexture *SkyRenderer::shadow_map() {
  if (!shadow_ && device_)
    shadow_ = texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, kShadowSize, 1,
                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
  return shadow_;
}

LightBinding SkyRenderer::classic_light() {
  LightBinding b;
  LightUniform &u = b.uniform;
  std::memset(&u, 0, sizeof u);
  const float l = std::sqrt(0.62f * 0.62f + 0.68f * 0.68f + 0.40f * 0.40f);
  set4(u.key_dir, 0.62f / l, 0.68f / l, 0.40f / l, 0);
  set4(u.key, 1.04f, 0.98f, 0.88f, 0.84f);
  set4(u.ambient, 0.50f * 1.05f, 0.52f * 1.05f, 0.72f * 1.05f, 0.02f);
  set4(u.eye, 0, 0, 0, 1);
  look_.uniform(u.look);
  b.samplers[0] = {lit_, nearest_};
  b.samplers[1] = {clear_, repeat_};
  return b;
}

LightBinding SkyRenderer::light(const sky::Frame &f, const sky::State &s, const float eye[3], double time,
                                const ShadowView *shadow) {
  LightBinding b;
  LightUniform &u = b.uniform;
  std::memset(&u, 0, sizeof u);
  set4(u.key_dir, f.key_dir, 1);
  set4(u.key, f.key, 0.84f);
  set4(u.ambient, f.ambient, 0.02f);
  set4(u.fog, f.fog, f.fog_density);
  set4(u.eye, eye, f.exposure);
  look_.uniform(u.look);
  const bool shadowed = shadow && shadow_;
  if (shadowed) {
    std::memcpy(u.shadow_vp, shadow->view_proj, sizeof u.shadow_vp);
    set4(u.shadow, 1, shadow->texel_m, shadow->bias, 0.8f);
    set4(u.shadow_at, shadow->centre, shadow->radius);
  }
  const sky::Observatory *o = observatory_.get();
  if (o && clouds_ && !pending_clouds_) {
    set4(u.cloud, std::clamp(s.clouds, 0.0f, 1.0f), 0.25f / std::max(o->cloud_size_m, 1.0f), o->cloud_height_m, 0.72f);
    set4(u.cloud_wind, s.cloud_wind[0], s.cloud_wind[1], static_cast<float>(std::fmod(time, 100000.0)), 0);
  }
  b.samplers[0] = {shadowed ? shadow_ : lit_, nearest_};
  b.samplers[1] = {clouds_ && !pending_clouds_ ? clouds_ : clear_, repeat_};
  return b;
}

SkyUniform SkyRenderer::sky_uniform(const sky::Frame &f, const sky::State &s, const SkyCamera &cam, double time) const {
  SkyUniform u;
  std::memset(&u, 0, sizeof u);
  look_.uniform(u.look);
  const sky::Observatory &o = *observatory_;
  for (int i = 0; i < 3; ++i) {
    u.ray_x[i] = cam.right[i] * cam.tan_x;
    u.ray_y[i] = cam.up[i] * cam.tan_y;
    u.ray_z[i] = cam.forward[i];
  }
  const float t = static_cast<float>(std::fmod(time, 100000.0));
  set4(u.eye, cam.eye, t);
  set4(u.sun, f.sun, 0.5f * o.sun_size_deg * kRad);
  set4(u.sun_color, o.sun_color, o.sun_intensity);
  for (size_t k = 0; k < 2 && k < f.moons.size(); ++k) {
    set4(u.moon_dir[k], f.moons[k].dir, f.moons[k].size_rad);
    set4(u.moon_color[k], f.moons[k].color, f.moons[k].lit);
  }
  const sky::Air &a = o.air;
  set4(u.rayleigh, a.rayleigh, a.rayleigh_km);
  set4(u.mie, a.mie, a.mie_km);
  set4(u.mie_absorb, a.mie_absorb, a.planet_km);
  set4(u.mie_g, a.mie_g, a.top_km);
  set4(u.ozone, a.ozone, f.sky_exposure);
  for (int r = 0; r < 3; ++r) set4(u.to_stars[r], &f.to_stars[r * 3], 0);
  set4(u.stars, 0.22f, 0.22f, std::clamp(s.star_density, 0.0f, sky::kMaxStarDensity), f.sky_scale);
  set4(u.lit_color, o.lit_color, 0);
  set4(u.cloud, std::clamp(s.clouds, 0.0f, 1.0f), 0.25f / std::max(o.cloud_size_m, 1.0f), o.cloud_height_m, 0.72f);
  set4(u.cloud_wind, s.cloud_wind[0], s.cloud_wind[1], t, 0);
  set4(u.cloud_lit, f.cloud_lit, 0);
  set4(u.cloud_shade, f.cloud_shade, 0);
  set4(u.aurora, f.aurora, f.aurora_km, f.aurora_north, 0);
  set4(u.aurora_low, o.aurora_low, 0);
  set4(u.aurora_high, o.aurora_high, 0);
  set4(u.night, f.night, 0);
  float ground[3];
  for (int i = 0; i < 3; ++i) ground[i] = o.ground[i] * (f.ambient[i] + 0.5f * f.key[i] * std::max(f.key_dir[1], 0.0f));
  set4(u.ground, ground, 0);
  return u;
}

void SkyRenderer::sky_samplers(SDL_GPUTextureSamplerBinding out[3]) const {
  out[0] = {stars_, clamp_};
  out[1] = {glow_, clamp_};
  out[2] = {clouds_ ? clouds_ : clear_, repeat_};
}

void SkyRenderer::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const SkyUniform &u) {
  if (!pipeline_ || !ready()) return;
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  SDL_GPUTextureSamplerBinding s[3];
  sky_samplers(s);
  SDL_BindGPUFragmentSamplers(pass, 0, s, 3);
  SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

}  // namespace runtime
