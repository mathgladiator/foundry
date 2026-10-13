// Skinned, instanced meshes on SDL3's GPU API (design/submit.md): every
// instance of a mesh is one draw, with its transform in an instance buffer
// streamed each frame, and its bones in a persistent storage buffer where each
// instance keeps a slot: only the poses that changed are uploaded, packed, and
// a compute pass scatters them into their slots.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
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

  // Bone slots: a persistent range of the bone buffer per instance, in bones (3 float4 rows each).
  uint32_t alloc_bones(uint32_t count);
  void free_bones(uint32_t offset, uint32_t count);

  // One frame's uploads: every drawn instance, and the bones that changed (their rows, packed, and
  // the slot each goes to). begin_frame maps the staging memory to fill (from any thread);
  // end_frame records the uploads into a copy pass; scatter, after the copy pass ends, records the
  // compute pass that writes the changed bones into their slots.
  struct Frame {
    SkinnedInstance *instances = nullptr;
    float *rows = nullptr;      // 12 floats per changed bone
    uint32_t *slots = nullptr;  // per changed bone, its place in the bone buffer
  };
  Frame begin_frame(uint32_t instances, uint32_t changed_bones);
  void end_frame(SDL_GPUCopyPass *copy);
  void scatter(SDL_GPUCommandBuffer *cmd);

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
  SDL_GPUComputePipeline *scatter_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr, *bones_ = nullptr, *rows_ = nullptr, *slots_ = nullptr;
  uint32_t instance_capacity_ = 0, bone_capacity_ = 0, rows_capacity_ = 0, slots_capacity_ = 0;  // bytes
  SDL_GPUTransferBuffer *staging_ = nullptr;
  uint32_t staging_capacity_ = 0;
  // The bone allocator: the high-water mark, and freed ranges by size.
  uint32_t bone_top_ = 0;
  std::unordered_map<uint32_t, std::vector<uint32_t>> bone_free_;
  // This frame's uploads, between begin_frame and scatter.
  uint32_t frame_instances_ = 0, frame_bones_ = 0;
  bool mapped_ = false;
  uint64_t created_ = 0, released_ = 0, bytes_ = 0;
};

}  // namespace runtime
