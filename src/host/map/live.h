// A map, live (design/map.md, "In the foundry and the runtime"): the native runtime a game links,
// running the map as a game would. The far land and the boundary stand for the whole map; the chunks
// within a few hundred metres of the camera are spawned as it moves (nearest first, a few a frame)
// and destroyed behind it, each parcel's plotter growing its nature on each chunk once it is built;
// settlements (their planners' fills) and points of interest (their pipelines) spawn when the
// camera comes near.
#pragma once

#include "render/look.h"

#include <SDL3/SDL_gpu.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "physics/walker.h"
#include "plan/fill.h"
#include "runtime.h"
#include "sky/sky.h"

namespace map {

struct LiveStats {
  double fps = 0, update_ms = 0, prepare_ms = 0;
  uint64_t instances = 0, drawn = 0, triangles = 0, builds_pending = 0, terrain_nodes = 0, field_tiles = 0;
  double gpu_ms = 0, stream_ms = 0;
  uint32_t chunks = 0, chunks_total = 0, chunks_filled = 0, sites = 0, nature = 0, pieces = 0;
  double start_ms = 0;
  std::string error;
};

class Live {
 public:
  ~Live();
  // (Re)starts with this map: its file, its plan, and its settlements; the far land and the boundary.
  bool start(SDL_GPUDevice *device, const nlohmann::json &file, const nlohmann::json &plan, const nlohmann::json &far_spec,
             const std::map<std::string, std::shared_ptr<const plan::Settlement>> &settlements, std::string &error);
  void stop();
  bool running() const { return rt_ != nullptr; }
  const std::string &key() const { return key_; }
  void set_key(const std::string &k) { key_ = k; }
  // Streams round focus (the camera, or the walker), then updates and draws from eye toward target.
  SDL_GPUTexture *frame(SDL_GPUCommandBuffer *cmd, int width, int height, const float eye[3], const float target[3],
                        const float focus[3], double dt);
  void set_look(const runtime::Look &look);
  void set_sky(const std::shared_ptr<const runtime::sky::Observatory> &observatory, const runtime::sky::State &state,
               const float centre[3], float radius);
  // The ground under a place, from the far land's grid (what the camera starts from), metres.
  float ground_at(float x, float z) const;
  LiveStats stats() const { return stats_; }
  // Everything within reach of the focus is spawned and built (chunks, nature, grass, sites): what a
  // recording waits for before it keeps a frame.
  bool settled() const { return pending_ == 0 && stats_.builds_pending == 0; }
  // Grass fields on the chunks' nature (they cost the most on the GPU near the ground).
  bool grass = true;
  // Times the streaming radii (chunks, nature, sites; not grass): a recording, which waits for what
  // it streams, sees farther from the air.
  float reach = 1;
  physics::Walker &walker() { return *walker_; }

 private:
  struct Chunk {
    float x = 0, z = 0;  // its middle, metres
    foundry_instance ground = FOUNDRY_NO_INSTANCE;
    std::vector<foundry_region> regions;
    std::vector<foundry_plotter> region_plotters;
    std::vector<foundry_instance> things, fields;
    bool filled = false;
  };
  struct Site {
    float x = 0, y = 0, z = 0, yaw = 0, radius = 0;
    std::string name, pipeline, params;
    std::shared_ptr<const plan::Settlement> town;
    std::vector<foundry_instance> things;
  };
  void stream(const float focus[3]);
  void fill(Chunk &c, size_t index);
  void clear(Chunk &c);
  SDL_GPUDevice *device_ = nullptr;
  std::unique_ptr<physics::Walker> walker_ = std::make_unique<physics::Walker>();
  foundry_runtime *rt_ = nullptr;
  nlohmann::json plan_, file_;
  uint32_t seed_ = 1;
  std::vector<Chunk> chunks_;
  std::vector<Site> sites_;
  std::vector<std::string> plotters_;  // each parcel's plotter
  foundry_instance far_ = FOUNDRY_NO_INSTANCE, boundary_ = FOUNDRY_NO_INSTANCE;
  float far_reach_ = 0;
  SDL_GPUTexture *color_ = nullptr, *depth_ = nullptr;
  int w_ = 0, h_ = 0;
  std::string key_;
  std::shared_ptr<const runtime::sky::Observatory> sky_;
  LiveStats stats_;
  uint64_t frames_ = 0, fps_frames_ = 0;
  size_t pending_ = 1;  // what streaming still has to do near the focus
  double fps_window_ = 0;
};

}  // namespace map
