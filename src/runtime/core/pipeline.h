// What the runtime knows about a pipeline (design/spine.md): a type that can
// build what instances share from their build parameters, and pose one
// instance each frame from its update parameters and clocks. The generated
// code in src/library/<pipeline>.cpp implements it on top of the systems,
// passing channels as plain structures (no packers on this path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "boundary.h"

namespace runtime {

// The draw stage's output, in the form the renderer takes.
struct DrawMesh {
  std::vector<foundry::vec3> positions, normals;
  std::vector<uint32_t> colors;  // packed RGBA, red in the lowest byte
  std::vector<uint32_t> indices;
  std::vector<uint32_t> bone_a, bone_b;  // empty for a rigid mesh
  std::vector<float> bone_w;
  void clear() { *this = DrawMesh{}; }
};

// What a pipeline's build stages leave behind, shared by every instance with
// the same build parameters. Generated code subclasses it to keep the stage
// outputs that update stages read (the tree keeps its rig).
struct Build {
  virtual ~Build() = default;
  DrawMesh mesh;          // released once uploaded, or right away without a GPU
  uint32_t bones = 1;     // pose matrices per instance
  uint32_t triangles = 0;
  float height = 0, radius = 0;  // bounding sphere, centred at half height
  size_t bytes = 0;              // CPU bytes kept after the mesh is released
  uint64_t model_hash = 0;       // the packed draw output (diagnostics)
  std::vector<double> stage_ms;  // per build stage
  std::string problem;           // a failed geometry check, if any
};

// One instance's update state: the update stages' persistent inputs (filled
// once from the build) and the latest pose.
struct PoseState {
  virtual ~PoseState() = default;
  std::vector<float> matrices;  // column-major 4x4 per bone
  bool settled = false;
  std::vector<double> stage_ms;  // the last update, per update stage
};

struct StageInfo {
  std::string label;  // pipeline.stage, e.g. tree_v1.rig
  bool update = false;
};

class PipelineType {
 public:
  virtual ~PipelineType() = default;
  virtual const char *name() const = 0;
  virtual size_t params_size() const = 0;
  // Clocks: [0] is the instance's time, then one per event (seconds since it
  // fired, negative before).
  virtual size_t event_count() const = 0;
  virtual bool has_update() const = 0;
  virtual const std::vector<StageInfo> &stages() const = 0;
  // A key for the build parameters, including the pipeline's identity.
  virtual uint64_t build_key(const void *params) const = 0;
  // Runs the build stages. Thread-safe; runs on the generation threads.
  virtual std::shared_ptr<Build> build(const void *params, std::string &error) const = 0;
  // Fills an instance's update state from a finished build.
  virtual std::unique_ptr<PoseState> bind(const Build &build) const = 0;
  // Runs the update stages for one instance.
  virtual bool update(const Build &build, PoseState &state, const void *params, const float *clocks,
                      std::string &error) const = 0;
  // The packed pose output, hashed (diagnostics).
  virtual uint64_t pose_hash(const PoseState &state) const = 0;
};

// FNV-1a, 64 bits: the same hash the foundry uses for stage outputs.
inline uint64_t fnv64(const void *data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace runtime
