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
