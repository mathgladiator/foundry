// What the viewer composes round a pipeline's model, as its draw block names it (design/terrain.md,
// "In the foundry"; design/boundary.md, "Figures"): a plotter grown over the model's open land
// (draw.nature), a second pipeline built round it, such as a boundary round a terrain
// (draw.surround), and the figures a model leaves for the game, filled with builds (draw.figures,
// the model's own or the surround's). The host names no pipeline: every name comes from the draw
// blocks and the options.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/pipeline.h"

namespace pipeline {

// Grows a plotter over hex regions (plot_v1_region JSON) into mesh, returning how many stood. The plot
// slice provides it (plot::grow), so this slice needn't know plotters.
using NatureGrower = std::function<uint32_t(const std::string &plotter, const nlohmann::json &regions, uint32_t seed,
                                            const nlohmann::json &shared, runtime::Mesh &mesh, std::string &error)>;
void set_nature_grower(NatureGrower grower);

struct ComposeOptions {
  bool nature = true;         // grow the plotter (draw.nature)
  std::string plotter;        // which; empty: draw.nature's
  bool surround = true;       // build the surround (draw.surround)
  bool figures = true;        // fill the figures (draw.figures)
  nlohmann::json surround_values = nlohmann::json::object();  // the surround's parameters, over draw.surround.params
  bool operator==(const ComposeOptions &o) const {
    return nature == o.nature && plotter == o.plotter && surround == o.surround && figures == o.figures && surround_values == o.surround_values;
  }
};

// Collision to add for a play test: bodies at a place.
struct PlacedBodies {
  std::vector<runtime::CollisionBody> bodies;
  foundry::vec3 at{0, 0, 0};
  float yaw = 0;  // radians
};

struct Composite {
  int version = 0;  // bumps as each compose finishes
  std::shared_ptr<const runtime::Mesh> mesh;  // the model with everything round it
  std::vector<viewport::Line> guides;         // the surround's guides
  std::vector<viewport::Line> wire;           // its collision, as wireframe
  std::vector<PlacedBodies> bodies;           // the surround's and the figures' collision
  std::vector<runtime::FirePoint> fires;      // the surround's
  bool own_ground = false;                    // the surround draws the land to the horizon
  float reach = 0;                            // how far it reaches, metres
  nlohmann::json report = nlohmann::json::object();  // what stood, and any errors
};

// Fills a model's figures (its draw.figures block and the figures' JSON) with builds placed into
// mesh, their collision into bodies; parent: the model's values, for what the figures share (season).
// Counts go into report (figures, figure_builds, figures_unknown). Returns how many stood.
uint32_t fill_figures(Runner &runner, const nlohmann::json &block, const nlohmann::json &figures, const nlohmann::json &parent,
                      runtime::Mesh &mesh, std::vector<PlacedBodies> &bodies, nlohmann::json &report);

// Whether a definition composes anything.
bool composes(const Definition &def);

// Composes now, in the calling thread.
Composite compose(Runner &runner, const Definition &def, const Frame &frame, const nlohmann::json &values, const ComposeOptions &options);

// Composes in the background for the live view: want() starts a compose when the model, values, or
// options change; latest() is the last finished one.
class Composer {
 public:
  explicit Composer(Runner &runner) : runner_(runner) {}
  ~Composer();
  void want(const Definition &def, const Frame &frame, const nlohmann::json &values, const ComposeOptions &options);
  Composite latest() const;
  bool busy() const { return busy_; }

 private:
  Runner &runner_;
  mutable std::mutex mutex_;
  std::string key_;
  Composite latest_;
  std::thread worker_;
  std::atomic<bool> busy_{false};
  int generation_ = 0;
};

}  // namespace pipeline
