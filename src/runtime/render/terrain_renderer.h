// A terrain's quadtree on SDL3's GPU API (design/boundary.md, "Levels"; design/submit.md): each
// terrain build is uploaded once (its vertices with their morph, its triangles, and every node's
// image packed into one atlas); each frame, the World picks each terrain instance's cut by
// distance and streams one record per node drawn (the instance's transform, the node's tile in the
// atlas, and the distances its vertices morph over), and terrain.hlsl morphs, textures, and lights
// them.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/pipeline.h"
#include "render/sky_renderer.h"

namespace runtime {

struct TerrainMesh {
  SDL_GPUBuffer *vertices = nullptr, *indices = nullptr;
  SDL_GPUTexture *atlas = nullptr;
  uint32_t columns = 1, tile = 0;  // tiles a row in the atlas, and texels a tile's side
  uint32_t width = 0, height = 0;  // the atlas, texels
  uint64_t bytes = 0;
};

// Per node drawn (80 bytes): the instance's transform rows, the node's tile in the atlas (origin
// and size, 0 to 1), and its morph (where it starts, metres from the eye, and 1 / its length).
struct TerrainInstance {
  float row0[4], row1[4], row2[4];
  float tile[4];
  float morph[4];
};

struct TerrainDraw {
  const TerrainMesh *mesh;
  uint32_t first, count;  // its node's triangles' indices
};

class TerrainRenderer {
 public:
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format, std::string &error);
  void shutdown();
  TerrainMesh upload(SDL_GPUCopyPass *copy, const Terrain &terrain);
  void release(TerrainMesh &mesh);
  // This frame's nodes: map the staging memory, fill a record per draw, then record the upload.
  TerrainInstance *begin_frame(uint32_t count);
  void end_frame(SDL_GPUCopyPass *copy);
  // eye: the camera, for the morph; the haze colours the classic light's distance (a sky has its fog).
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<TerrainDraw> &draws, const float view_proj[16],
            const float eye[3], const LightBinding &light);
  uint64_t gpu_bytes() const { return bytes_; }

 private:
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUSampler *sampler_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr;
  uint32_t instance_capacity_ = 0;  // bytes
  SDL_GPUTransferBuffer *staging_ = nullptr;
  uint32_t staging_capacity_ = 0, frame_count_ = 0;
  bool mapped_ = false;
  uint64_t bytes_ = 0;
};

}  // namespace runtime
