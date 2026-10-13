// The sky on SDL3's GPU API (design/observatory.md): an observatory's star
// chart and constellation glow as cube maps, the cloud map, and the key light's
// shadow map; the uniforms of the light every mesh shader shares (light.hlsli)
// and of the sky pass (sky.hlsli); and the sky pass itself, drawn last at the
// far plane. Without an observatory the light is the classic fixed one.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sky/sky.h"

namespace runtime {

// light.hlsli's LightData.
struct LightUniform {
  float key_dir[4], key[4], ambient[4], fog[4], eye[4];
  float shadow_vp[16];
  float shadow[4], shadow_at[4], cloud[4], cloud_wind[4];
};

// sky.hlsli's SkyData.
struct SkyUniform {
  float ray_x[4], ray_y[4], ray_z[4], eye[4], sun[4], sun_color[4];
  float moon_dir[2][4], moon_color[2][4];
  float rayleigh[4], mie[4], mie_absorb[4], mie_g[4], ozone[4];
  float to_stars[3][4];
  float stars[4], lit_color[4], cloud[4], cloud_wind[4], cloud_lit[4], cloud_shade[4];
  float aurora[4], aurora_low[4], aurora_high[4], night[4], ground[4];
};

// What a mesh pass binds for its light: the uniform, and the shadow map and cloud map.
struct LightBinding {
  LightUniform uniform;
  SDL_GPUTextureSamplerBinding samplers[2] = {};
};

// The camera, for the sky's rays: where it stands, its axes, and the tangents of its half-angles.
struct SkyCamera {
  float eye[3] = {0, 0, 0};
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, forward[3] = {0, 0, -1};
  float tan_x = 1, tan_y = 1;
};
// From a view matrix and a projection (column-major, as foundry_set_camera takes them).
SkyCamera sky_camera(const float view[16], const float projection[16]);

// The key light's view for its shadow map: an orthographic box around a centre, snapped to its
// texels so it doesn't shimmer as the centre moves.
struct ShadowView {
  float view_proj[16];
  float texel_m = 0.05f, bias = 0, centre[3] = {0, 0, 0}, radius = 0;
};
ShadowView shadow_view(const float key_dir[3], const float centre[3], float radius, uint32_t size);

class SkyRenderer {
 public:
  static constexpr uint32_t kShadowSize = 2048;
  static constexpr int kStarSize = 1024, kGlowSize = 512, kCloudSize = 512;
  static constexpr float kMilkyWayMax = 0.06f;  // the galaxy's glow at 255 in the glow cube's G (sky.hlsli)

  // color_format INVALID: no sky pass of its own (the foundry's viewport draws it with its own).
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format, std::string &error);
  void shutdown();

  // The observatory (null: none) and its constellations' states. Bakes what changed (the star
  // chart and clouds per observatory, the glow per states), uploaded by the next upload().
  void set(std::shared_ptr<const sky::Observatory> observatory, const std::vector<uint8_t> &states);
  const sky::Observatory *observatory() const { return observatory_.get(); }
  // Records the uploads of anything baked into a copy pass.
  void upload(SDL_GPUCopyPass *copy);
  bool ready() const { return observatory_ && stars_ && glow_ && clouds_ && !pending_stars_ && !pending_glow_ && !pending_clouds_; }

  // The shadow map, made on first use.
  SDL_GPUTexture *shadow_map();

  // The light every mesh shader shares: the classic fixed one, or the sky's (with the shadow map
  // when shadow is given). Its samplers are bound either way.
  LightBinding classic_light();
  LightBinding light(const sky::Frame &f, const sky::State &s, const float eye[3], double time, const ShadowView *shadow);
  SkyUniform sky_uniform(const sky::Frame &f, const sky::State &s, const SkyCamera &cam, double time) const;
  void sky_samplers(SDL_GPUTextureSamplerBinding out[3]) const;

  // The sky pass, into an open render pass whose depth was cleared to 1.
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const SkyUniform &u);

 private:
  SDL_GPUTexture *texture(SDL_GPUTextureType type, SDL_GPUTextureFormat format, uint32_t size, uint32_t layers,
                          SDL_GPUTextureUsageFlags usage);
  void put(SDL_GPUCopyPass *copy, SDL_GPUTexture *tex, uint32_t layer, uint32_t size, const void *data, uint32_t bytes);

  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUSampler *repeat_ = nullptr, *clamp_ = nullptr, *nearest_ = nullptr;
  SDL_GPUTexture *stars_ = nullptr, *glow_ = nullptr, *clouds_ = nullptr, *shadow_ = nullptr;
  SDL_GPUTexture *lit_ = nullptr, *clear_ = nullptr;  // 1 x 1: no shadow, no clouds
  bool dummies_uploaded_ = false;
  std::shared_ptr<const sky::Observatory> observatory_;
  std::vector<uint8_t> states_;
  std::vector<uint16_t> star_texels_;  // half floats, waiting to upload
  std::vector<uint8_t> glow_texels_, milky_texels_, cloud_texels_;
  bool pending_stars_ = false, pending_glow_ = false, pending_clouds_ = false;
};

}  // namespace runtime
