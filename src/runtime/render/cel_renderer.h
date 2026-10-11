// The first piece of the engine (design/submit.md): instanced meshes drawn
// with the cel shader on SDL3's GPU API. One draw call per mesh, with
// per-instance 3x4 transforms in a vertex buffer.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include "render/mesh.h"

namespace runtime {

struct GpuMesh {
  SDL_GPUBuffer *vertices = nullptr;
  SDL_GPUBuffer *indices = nullptr;
  uint32_t index_count = 0;
};

// The rows of a 3x4 transform: rotation and scale in xyz, translation in w.
struct Instance {
  float row0[4], row1[4], row2[4];
};

class CelRenderer {
 public:
  bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format,
            std::string &error);
  void shutdown();

  // Uploads a mesh (positions, normals, colors) into GPU buffers.
  GpuMesh upload(const runtime::Mesh &mesh);
  void release(GpuMesh &mesh);

  // Uploads instances into a buffer (re-created when it must grow).
  void set_instances(const std::vector<Instance> &instances);

  // Draws `count` instances of the mesh into an open render pass.
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const GpuMesh &mesh, uint32_t count,
            const float view_proj[16]);

 private:
  void upload_buffer(SDL_GPUBuffer *buffer, const void *data, uint32_t size);
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
  SDL_GPUBuffer *instances_ = nullptr;
  uint32_t instance_capacity_ = 0;
};

// Builds a column-major perspective * look-at matrix.
void view_projection(const float eye[3], const float target[3], float fov_y, float aspect, float near_z,
                     float far_z, float out[16]);

}  // namespace runtime
