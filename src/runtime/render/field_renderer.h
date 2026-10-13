// A field's tiles on SDL3's GPU API (design/plotter.md, "Fields"): each tile
// build is uploaded once with its per-vertex sway; each frame, the tiles of the
// chunks in view are streamed as instances and drawn, one instanced draw per
// tile build, swayed by the wind in field.hlsl.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/pipeline.h"
#include "render/sky_renderer.h"

namespace runtime {

struct FieldMesh {
  SDL_GPUBuffer *vertices = nullptr, *indices = nullptr;
  uint32_t index_count = 0;
  uint64_t bytes = 0;
};

// Per tile (32 bytes): where it stands and its height scale, the wind (metres per metre of sway),
// its tint (RGBA8, halved), and its turn in sixths (low 3 bits) with a phase for the gusts.
struct FieldInstance {
  float at[3];
  float scale;
  float wind[2];
  uint32_t tint;
  uint32_t turn_phase;
};

struct FieldRange {
  const FieldMesh *mesh;
  uint32_t first, count;
};

class FieldRenderer {
 public:
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format, std::string &error);
  void shutdown();
  FieldMesh upload(SDL_GPUCopyPass *copy, const DrawMesh &mesh);
  void release(FieldMesh &mesh);
  // This frame's tiles: map the staging memory, fill it, then record the upload into a copy pass.
  FieldInstance *begin_frame(uint32_t count);
  void end_frame(SDL_GPUCopyPass *copy);
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<FieldRange> &ranges, const float view_proj[16],
            float time, const LightBinding &light);
  uint64_t gpu_bytes() const { return bytes_; }

 private:
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr;
  uint32_t instance_capacity_ = 0;  // bytes
  SDL_GPUTransferBuffer *staging_ = nullptr;
  uint32_t staging_capacity_ = 0, frame_count_ = 0;
  bool mapped_ = false;
  uint64_t bytes_ = 0;
};

}  // namespace runtime
