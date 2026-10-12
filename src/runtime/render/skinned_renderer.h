// Skinned, instanced meshes on SDL3's GPU API (design/submit.md): every
// instance of a mesh is one draw, with its transform in an instance buffer
// and its bones in one storage buffer streamed each frame.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/pipeline.h"

namespace runtime {

struct SkinnedMesh {
  SDL_GPUBuffer *vertices = nullptr;
  SDL_GPUBuffer *indices = nullptr;
  uint32_t index_count = 0;
  uint64_t bytes = 0;
};

// Per instance: the rows of a 3x4 transform, and the first of its bones.
struct SkinnedInstance {
  float row0[4], row1[4], row2[4];
  uint32_t bone_offset;
  uint32_t pad[3];
};

struct DrawRange {
  const SkinnedMesh *mesh;
  uint32_t first, count;
};

class SkinnedRenderer {
 public:
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
            std::string &error);
  void shutdown();

  // Records the upload of a mesh into a copy pass.
  SkinnedMesh upload(SDL_GPUCopyPass *copy, const DrawMesh &mesh);
  void release(SkinnedMesh &mesh);

  // Records this frame's instances and bones (3 float4 rows per bone) into a copy pass.
  void stream(SDL_GPUCopyPass *copy, const std::vector<SkinnedInstance> &instances, const std::vector<float> &bone_rows);

  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const std::vector<DrawRange> &ranges,
            const float view_proj[16]);

  uint64_t buffers_created() const { return created_; }
  uint64_t buffers_released() const { return released_; }
  uint64_t gpu_bytes() const { return bytes_; }

 private:
  SDL_GPUBuffer *make(SDL_GPUBufferUsageFlags usage, uint32_t size);
  void drop(SDL_GPUBuffer *&buffer, uint32_t size);
  void put(SDL_GPUCopyPass *copy, SDL_GPUBuffer *dst, const void *data, uint32_t size);
  void grow(SDL_GPUBuffer *&buffer, uint32_t &capacity, SDL_GPUBufferUsageFlags usage, uint32_t need);

  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr, *bones_ = nullptr;
  uint32_t instance_capacity_ = 0, bone_capacity_ = 0;  // bytes
  SDL_GPUTransferBuffer *staging_ = nullptr;
  uint32_t staging_capacity_ = 0;
  uint64_t created_ = 0, released_ = 0, bytes_ = 0;
};

}  // namespace runtime
