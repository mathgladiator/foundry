// The plotter the workspace shows (design/plotter.md, "In the foundry"): one
// region, its ground (prioritized hex regions), its occupants, and a composed
// picture of it. The human (UI) and the agent (MCP) drive the same region.
//
// Growing runs the plotter's plot system in WebAssembly (and, when asked,
// natively, comparing the bytes). Each occupant is shown by running its kind's
// pipeline with its values; the picture composes them over the ground in the
// background, so the UI stays responsive while builds run.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "plot/plotter.h"
#include "viewport/raster.h"
#include "wasm/runner.h"

namespace plot {

struct Occupant {
  nlohmann::json placement;  // a plot_v1_placement, as JSON
  uint32_t kind = 0;
  nlohmann::json extra = nlohmann::json::object();  // values the view set on it (a felled tree's stump)
};

struct Picture {
  std::shared_ptr<const runtime::Mesh> mesh;  // the ground and every occupant, composed
  std::vector<viewport::Line> lines;          // the cells' outlines, coloured by ground
  std::vector<viewport::Line> collision;      // every occupant's collision
  nlohmann::json metrics = nlohmann::json::object();
  int version = 0;     // the state it shows
  bool building = false;
  std::string error;
};

// Terrain over a ground (heights and moisture, from noise over the cells): a style (flat, hills,
// ridges, terraces, dunes, mesas, wild), its height, the size of its features, how rough it is,
// and a random height per cell on top, then smoothed (no cliffs at 1). Valleys come out wetter; a pond
// stays flat, below its shore.
struct Terrain {
  std::string style = "flat";
  float height_m = 0;      // from the lowest cell to the highest
  float scale_cells = 12;  // the size of a hill
  float roughness = 0.5f;  // 0 smooth to 1 broken
  float jitter_m = 0;      // each cell's own height, up and down
  float smooth = 0;        // 0 as made; 1 blurred, and no step between neighbours over kSmoothStep
};
constexpr float kSmoothStep = 0.25f;  // metres, between neighbouring cells at smooth 1

class View {
 public:
  View(forge::Paths paths, pipeline::Runner &runner, wasm::Runner &wasm) : paths_(std::move(paths)), runner_(runner), wasm_(wasm) {}
  ~View();

  // Shows a plotter (loading it if it isn't the current one): a fresh region from its preset.
  bool show(const std::string &name, std::string &error);
  std::string name() const;
  Plotter plotter() const;

  // The ground: prioritized hex regions (plot_v1_region values), replaced whole. Occupants on lost cells are removed.
  bool set_regions(const nlohmann::json &regions, nlohmann::json &out, std::string &error);
  nlohmann::json regions() const;
  using Terrain = plot::Terrain;
  static const std::vector<std::string> &terrains();
  static Terrain terrain_of(const nlohmann::json &j);  // {style, height_m, scale_cells, roughness, jitter_m, smooth}
  // A ready-made ground: hex, ring, strip, union, pond, path, slope, ragged, single, or glen, with a terrain.
  static nlohmann::json preset(const std::string &name, int radius, uint32_t seed, const Terrain &terrain = Terrain());
  static const std::vector<std::string> &presets();
  void set_seed(uint32_t seed);
  uint32_t seed() const;
  // Shared values (season, wind) for every occupant shown.
  void set_shared(const nlohmann::json &patch);
  nlohmann::json shared() const;

  // Grows the region: up to max placements in seed or regrow mode, keeping clear of exclusions
  // ([{position:[x,y,z], radius}]); verify_native also runs the plot system natively and compares.
  bool grow(bool regrow, uint32_t max, const nlohmann::json &exclusions, bool use_focus, const float focus[3],
            bool verify_native, nlohmann::json &out, std::string &error);
  // Clears occupants: all, those within radius of a point, or a fraction of a layer. `fell` turns
  // trees into stumps (they keep their slots) where their pipeline has a stump; otherwise they go.
  nlohmann::json clear(const nlohmann::json &how);

  nlohmann::json state() const;  // the region, its occupants by layer and kind, the last grow
  nlohmann::json metrics() const;
  // The ground as triangles (xyz per vertex, three per triangle): what a play test walks on.
  std::vector<float> ground_triangles() const;
  // The latest composed picture; starts composing in the background when the state changed.
  Picture picture();

 private:
  bool run_plot(const nlohmann::json &request, nlohmann::json &result, bool verify_native, bool &native_match,
                double &ms, std::string &error);
  nlohmann::json request(bool regrow, uint32_t max, const nlohmann::json &exclusions, bool use_focus, const float focus[3],
                         bool want_cells) const;
  bool resolve_cells(std::string &error);  // runs the plot system with max 0 for the resolved cells
  nlohmann::json metrics_locked() const;
  void compose(int version);

  forge::Paths paths_;
  pipeline::Runner &runner_;
  wasm::Runner &wasm_;
  mutable std::mutex mutex_;
  bool loaded_ = false;
  Plotter plotter_;
  std::map<std::string, pipeline::Definition> defs_;
  nlohmann::json regions_ = nlohmann::json::array();
  nlohmann::json cells_ = nlohmann::json::array();  // the resolved cells
  uint32_t seed_ = 1;
  nlohmann::json shared_ = nlohmann::json::object();
  std::vector<Occupant> occupants_;
  nlohmann::json last_grow_ = nlohmann::json::object();
  int version_ = 1;
  Picture picture_;
  int composing_ = 0;  // the version being composed (0: none)
  std::thread worker_;
};

}  // namespace plot
