// Pipelines: a DAG of systems driven by parameters (design/spine.md), run in
// WebAssembly inside the foundry so they can be play tested.
//
// A pipeline file (pipelines/<name>_v<N>.json) names its parameters and its
// stages. Each stage runs one system; its input is either an upstream stage's
// output passed straight through ("@rig"), or a JSON object whose values bind
// parameters ("$age"), upstream fields ("@rig.bones"), or literals. Stages
// marked "phase": "update" run every frame (the simulator); the others build
// what they feed and are cached.
//
// Outputs are cached by system build and input, so a change to wind or time
// reruns only the animator, and a change to the season reruns everything
// downstream of the rig without regrowing anything that didn't change.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "physics/toy.h"
#include "core/pipeline.h"
#include "render/mesh.h"
#include "wasm/runner.h"

namespace pipeline {

struct Param {
  std::string name;
  std::string type;                 // enum, config, u32, f32, time, event
  std::vector<std::string> values;  // enum values, or the entries of a config table
  std::string table;                // config: the table it picks from (config/<table>/), e.g. tree_v1_genomes
  double min = 0, max = 1;
  nlohmann::json fallback;          // the default value
  std::string unit, doc;
  // "when": the parameter applies only where a config parameter's entry sets a bool field
  // (fruit: when species.pickable). Where it doesn't apply, every stage reads it as off.
  std::string when_param, when_field;
  std::vector<std::string> when_on;  // the entries of when_param where it applies
  // "edges": a u32 that packs a value per hex edge (edge_bits each, six edges in the hex
  // directions' order), edited as six choices named by edge_values (road_v1's mask, its fences).
  int edge_bits = 0;
  std::vector<std::string> edge_values;
};

struct Stage {
  std::string id, system, phase;  // phase: "build" (cached) or "update" (every frame)
  nlohmann::json input;           // "@stage", or an object of bindings
  std::string input_channel, output_channel;
};

struct Definition {
  std::string name;  // e.g. tree_v1
  std::string doc;
  std::vector<Param> params;
  std::vector<Stage> stages;
  std::string model_stage, pose_stage;  // what the viewport draws, and what poses it
  // Model fields giving the conformance capsule's radius and height (draw.conformance), if any.
  std::string conformance_radius, conformance_height;
  // Model fields giving a box conformance (draw.conformance.box: its min and max corners), if any.
  std::string conformance_box_min, conformance_box_max;
  // Model fields giving the footprint at the largest age (draw.footprint), if any.
  std::string footprint_radius, footprint_cells;
  // The model field listing the cells it stands on, as q, r pairs (draw.footprint.lot), if any.
  std::string footprint_lot;
  // The model field listing which of those cells are water (draw.footprint.wet), if any.
  std::string footprint_wet;
  std::string footprint_doors;  // the cells just outside its doors (draw.footprint.doors)
  // Where a model asks for other things to go (draw.places: positions, yaws, sizes), such as a
  // wall's gates: vec3, degree, and metre fields of the model.
  std::string places_positions, places_yaws, places_sizes;
  std::string fire_points, fire_configs, fire_lit;  // draw.fires
  // Model fields giving cells to outline on the ground (draw.guides: group, q, r triples, and a tone
  // per group: 0 bright, 1 medium, 2 faint), such as every lot a building can stand on.
  std::string guide_cells, guide_tones;
  // Guide segments the model carries for the viewer (draw.guides.lines: vec3 pairs; colors: packed
  // RGBA per segment): a road cell's neighbours and flow, a wall piece's line.
  std::string guide_lines, guide_colors;
  // > 0: the viewer frames this many metres for every model (draw.frame_m), so sizes compare.
  float frame_m = 0;
  // draw.terrain (design/boundary.md): the model's terrain quadtree's field names; empty: none.
  nlohmann::json terrain = nlohmann::json::object();
  // What the viewer composes round the model, each naming model fields (empty: none):
  // draw.nature (a plotter grown over the model's open land: regions, plotter), draw.surround (a
  // second pipeline built round it, such as a boundary round a terrain: pipeline, hull, shared,
  // params), and draw.figures (stand-ins the model leaves for the game, filled with builds: field,
  // kinds).
  nlohmann::json nature = nlohmann::json::object(), surround = nlohmann::json::object(), figures = nlohmann::json::object();
  const Param *param(const std::string &n) const;
  const Stage *stage(const std::string &id) const;
  // Parameters that only update stages read (time, wind): changing them never rebuilds.
  bool is_update_param(const std::string &n) const;
  // Whether parameter p applies under values (its "when" gate, if any, is on).
  bool applies(const Param &p, const nlohmann::json &values) const;
  // How many combinations the discrete choices give (param -> values chosen), counting a
  // gated parameter once wherever it doesn't apply. Parameters not in choices count 1.
  double combinations(const std::map<std::string, std::vector<std::string>> &choices) const;
};

// A parameter's off value, which stages read where it doesn't apply: false, the first
// value, or the minimum.
nlohmann::json off_value(const Param &p);

// Loads a pipeline and fills in config choices and the systems' channels.
bool load(const forge::Paths &paths, const std::string &name, Definition &def, std::string &error);

struct StageRun {
  std::string id, system;
  bool cached = false;
  int status = 0;
  std::string error;
  double milliseconds = 0;
  uint64_t fuel = 0;
  size_t output_bytes = 0;
  bool native_checked = false, native_match = false;
};

// A posed, drawable result.
struct Frame {
  std::shared_ptr<const runtime::Mesh> mesh;  // skinned with the pose
  float conformance_radius = 0, conformance_height = 0;
  float height = 0;
  size_t bones = 0;
  bool settled = false;
  nlohmann::json facts;  // the model's and pose's scalar fields, and the sizes of their other arrays
  int model_version = 0;  // bumps when the model (not just the pose) changes
  std::shared_ptr<const physics::Collision> collision;  // what collides, checked in the toy Jolt world
  float footprint_radius = 0;   // the ground contact at the largest age (0: none)
  uint32_t footprint_cells = 0;  // and its footprint on the hex grid
  std::vector<int> lot_cells;    // the cells it stands on, q, r pairs (draw.footprint.lot)
  std::vector<int> wet_cells;    // which of them are water (draw.footprint.wet)
  std::vector<int> door_cells;   // the cells just outside its doors (draw.footprint.doors)
  struct Place {
    foundry::vec3 at;
    float yaw_deg = 0, size = 0;
  };
  std::vector<Place> places;     // what it asks to have placed (draw.places)
  bool has_box = false;          // a box conformance (draw.conformance.box)
  foundry::vec3 box_min{0, 0, 0}, box_max{0, 0, 0};
  std::vector<viewport::Line> guides;  // cells outlined on the ground (draw.guides)
  float frame_m = 0;                   // the definition's fixed framing, if any
  bool terrain = false;                // it draws its own ground out to the horizon (draw.terrain)
  std::vector<runtime::FirePoint> fires;  // its fire points (draw.fires; design/submit.md, "Fire")
  // The values the fire system reads by name, as the foundry shares wind: the pipeline's clock
  // (its time parameter) and the wind (wind_speed, wind_heading).
  float time = 0, wind_speed = 0, wind_heading = 0;
  // The model fields draw.nature, draw.surround, and draw.figures name, as JSON, for the composer.
  std::shared_ptr<const nlohmann::json> extras;
};

struct Model;  // a decoded model: rest mesh plus skinning

// Runs pipelines with a shared output cache. Thread-safe.
class Runner {
 public:
  Runner(forge::Paths paths, wasm::Runner &wasm);

  struct Result {
    bool ok = false;
    std::string error;
    std::vector<StageRun> stages;
    Frame frame;
    std::map<std::string, uint64_t> output_hashes;  // per stage id, with hash_outputs: FNV-1a of the packed output
  };
  // Runs every stage for these parameter values (missing ones take defaults).
  // With verify_native, stages that run (not cached) are also run natively and compared.
  // With hash_outputs, the result carries each stage's output hash (the stress tool's reference).
  Result run(const Definition &def, const nlohmann::json &values, bool verify_native = false, bool log = true,
             bool hash_outputs = false);

  // The values with defaults filled in and config entries resolved to their names.
  nlohmann::json complete(const Definition &def, const nlohmann::json &values) const;

  const forge::Paths &paths() const { return paths_; }

 private:
  struct Entry {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::shared_ptr<const nlohmann::json> json;  // filled on first use
    std::shared_ptr<const Model> model;          // filled when drawn
  };
  bool resolve(const Definition &def, const nlohmann::json &values, const nlohmann::json &binding,
               std::map<std::string, std::string> &keys, nlohmann::json &out, std::string &error);
  std::shared_ptr<const nlohmann::json> json_of(const std::string &key, const std::string &channel);
  void remember(const std::string &key, Entry e);
  int64_t module_time(const std::string &system);

  forge::Paths paths_;
  wasm::Runner &wasm_;
  std::mutex mutex_;
  std::map<std::string, Entry> cache_;
  std::vector<std::string> order_;  // insertion order, for eviction
  size_t cache_bytes_ = 0;
  std::map<std::string, int> model_versions_;
  int next_model_version_ = 1;
};

// Skins a model's rest mesh with column-major 4x4 bone matrices.
std::shared_ptr<const runtime::Mesh> skin(const Model &model, const std::vector<float> &matrices);

}  // namespace pipeline
