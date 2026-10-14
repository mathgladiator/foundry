// The fire system's sprites on SDL3's GPU API (design/submit.md, "Fire"): each frame the
// particles' sprites are streamed as instances and drawn in two instanced draws after everything
// solid: smoke blended over, then flames, sparks, and glows added (fire.hlsl).
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include "fire/fire.h"
#include "render/sky_renderer.h"

namespace runtime {

class FireRenderer {
 public:
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format, std::string &error);
  void shutdown();
  // This frame's sprites (smoke first, then light, as fire::Sim::sprites gives them).
  void upload(SDL_GPUCopyPass *copy, const std::vector<fire::Sprite> &sprites);
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const float view_proj[16], const float view[16],
            const LightBinding &light);
  uint64_t gpu_bytes() const { return capacity_; }
  uint32_t drawn() const { return smoke_ + light_; }

 private:
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *blend_ = nullptr, *add_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr;
  SDL_GPUTransferBuffer *staging_ = nullptr;
  uint32_t capacity_ = 0, staging_capacity_ = 0;
  uint32_t smoke_ = 0, light_ = 0;
};

}  // namespace runtime
