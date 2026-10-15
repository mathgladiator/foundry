// A plotter's region, live (design/plotter.md, "In the foundry"): the native
// runtime a game links, on the workspace's GPU, growing the region the plotter
// view shows and simulating and drawing it every frame, with the runtime's cost
// decisions (design/submit.md, "Cost"), into a texture the viewport shows.
//
// The picture the plot view composes runs every pipeline in WebAssembly, once
// per change; this is the whole simulation, as a game would run it, at the
// frame rate it would get. It shows the native runtime as last built.
#pragma once

#include "render/look.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "physics/walker.h"
#include "runtime.h"
#include "sky/sky.h"

namespace plot {

struct LiveStats {
  double fps = 0;  // frames the workspace drew per second, over the last second
  double update_ms = 0, prepare_ms = 0, draw_ms = 0;  // the runtime's CPU, per frame
  uint64_t instances = 0, drawn = 0, triangles = 0, draws = 0, animated = 0, builds_pending = 0;
  uint64_t field_tiles = 0;  // grass tiles drawn (fields)
  uint64_t terrain_nodes = 0, terrain_triangles = 0;  // the boundary's terrain drawn (design/boundary.md)
  uint64_t trees = 0;  // drawn, of the kinds whose pipeline starts with tree_
  double grow_ms = 0;  // growing the region to full, when it started
  std::string error;
};

// Hands the runtime an observatory (when it differs from what it was last handed, kept in handed) and
// its state, its shadow map looking at centre (the live plotter's and the live map's).
void hand_sky(foundry_runtime *rt, std::shared_ptr<const runtime::sky::Observatory> &handed,
              const std::shared_ptr<const runtime::sky::Observatory> &observatory, const runtime::sky::State &state,
              const float centre[3], float radius);

class Live {
 public:
  ~Live();
  // (Re)starts with this plotter, ground (plot_region values), and world seed: a fresh
  // runtime, the region grown to full, every placement spawned.
  bool start(SDL_GPUDevice *device, const std::string &plotter, const nlohmann::json &regions, uint32_t seed,
             std::string &error);
  void stop();
  bool running() const { return rt_ != nullptr; }
  const std::string &key() const { return key_; }
  void set_key(const std::string &k) { key_ = k; }

  // One frame: update by dt, then prepare and draw into the live texture (width x height) seen
  // from eye toward target. Records into cmd; returns the texture to show.
  SDL_GPUTexture *frame(SDL_GPUCommandBuffer *cmd, int width, int height, const float eye[3], const float target[3],
                        double dt);
  // The plot view's shared values (season, wind) on every instance and field that has them, by
  // name, whenever they change (a build parameter such as season rebuilds; wind only sways).
  void share(const nlohmann::json &shared);
  // The boundary (design/boundary.md), when the plotter has one: shown or not, and values for its
  // pipeline's parameters by name (its hull's sectors, biome, detail...), as a game would set them.
  void set_boundary(bool show, const nlohmann::json &values);
  // The game's look (foundry_set_look), as the viewport's.
  void set_look(const runtime::Look &look);
  // The sky over the region (design/observatory.md), through the game's API: the observatory (null:
  // none) is handed in when it changes, its state every call; the shadow map looks at centre.
  void set_sky(const std::shared_ptr<const runtime::sky::Observatory> &observatory, const runtime::sky::State &state,
               const float centre[3], float radius);
  LiveStats stats() const { return stats_; }
  float extent() const { return extent_; }  // the region's radius, metres
  size_t count() const { return instances_.size(); }  // instances spawned (things and fields)
  // The Jolt world the runtime keeps every instance's collision in, as a game's: a play test walks in it.
  physics::Walker &walker() { return *walker_; }

 private:
  SDL_GPUDevice *device_ = nullptr;
  std::unique_ptr<physics::Walker> walker_ = std::make_unique<physics::Walker>();  // outlives each runtime
  foundry_runtime *rt_ = nullptr;
  foundry_region region_ = FOUNDRY_NO_REGION;
  std::vector<foundry_instance> instances_;
  foundry_instance boundary_ = FOUNDRY_NO_INSTANCE;
  bool boundary_shown_ = true;
  std::string boundary_values_;
  float far_ = 0;  // the boundary's reach, for the camera's far plane
  std::vector<foundry_instance> figures_;  // the boundary's figures, filled once it is built
  bool figures_done_ = false;
  std::vector<bool> tree_;
  SDL_GPUTexture *color_ = nullptr, *depth_ = nullptr;
  int w_ = 0, h_ = 0;
  float extent_ = 10;
  std::string key_, shared_;
  std::shared_ptr<const runtime::sky::Observatory> sky_;  // what the runtime was handed
  LiveStats stats_;
  uint64_t frames_ = 0;
  double fps_window_ = 0;
  uint64_t fps_frames_ = 0;
};

}  // namespace plot
