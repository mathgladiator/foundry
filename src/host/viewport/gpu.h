// The viewport on the GPU (design/submit.md): the same look as the CPU
// renderer in raster.h (cel bands, a rim band, ink on silhouettes and depth
// edges, overlay lines that fade where the mesh hides them), drawn with SDL's
// GPU API at twice the size and box-filtered down, then read back as an image,
// so everything that renders the viewport (the workspace, screenshots, contact
// sheets, a plotter's region) gets it through viewport::render.
//
// A region of thousands of things is millions of triangles: its mesh is
// uploaded once and kept while it is shown, so orbiting it only draws.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <list>
#include <mutex>
#include <string>

#include "render/fire_renderer.h"
#include "render/sky_renderer.h"
#include "viewport/raster.h"

namespace viewport {

class GpuRenderer {
 public:
  bool init(SDL_GPUDevice *device, std::string &error);
  void shutdown();
  // Renders and reads back. Thread-safe (one render at a time). False: draw it on the CPU instead.
  bool render(const Scene &scene, int width, int height, Image &out);
  double last_ms() const { return last_ms_; }

 private:
  struct Mesh {
    const void *positions = nullptr;  // identity: the mesh's arrays and sizes, and a sample of its data
    size_t vertices = 0, indices = 0;
    uint64_t sample = 0;
    SDL_GPUBuffer *position = nullptr, *normal = nullptr, *color = nullptr, *index = nullptr;
    uint64_t bytes = 0;
  };
  struct Targets {
    int w = 0, h = 0;  // the final size
    SDL_GPUTexture *color = nullptr, *albedo = nullptr, *depth = nullptr, *zbuf = nullptr, *inked = nullptr,
                   *final = nullptr;
    SDL_GPUTransferBuffer *download = nullptr;
  };
  Mesh *mesh_for(const runtime::Mesh &m);
  void release(Mesh &m);
  bool targets_for(int w, int h);
  void release_targets();
  SDL_GPUBuffer *upload(const void *data, uint32_t size, SDL_GPUBufferUsageFlags usage);

  SDL_GPUDevice *device_ = nullptr;
  std::mutex mutex_;
  SDL_GPUGraphicsPipeline *mesh_ = nullptr, *ink_ = nullptr, *down_ = nullptr, *lines_ = nullptr, *sky_ = nullptr, *shadow_ = nullptr;
  // An observatory's sky (Scene::sky): its textures, uniforms, and shadow map, and the ground it puts down.
  runtime::SkyRenderer sky_renderer_;
  runtime::FireRenderer fire_renderer_;
  bool fire_ok_ = false;
  runtime::Mesh ground_;
  float ground_key_[4] = {0, 0, 0, 0};  // the radius and colour it was made for
  SDL_GPUSampler *nearest_ = nullptr, *linear_ = nullptr;
  std::list<Mesh> meshes_;  // most recently used first
  Targets t_;
  double last_ms_ = 0;
};

// The renderer viewport::render uses (null: the CPU). Set by the workspace once it has a GPU.
void set_gpu_renderer(GpuRenderer *renderer);
GpuRenderer *gpu_renderer();

}  // namespace viewport
