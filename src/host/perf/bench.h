// The performance phase for fixed assets: how many instances of a mesh can be
// drawn off screen while staying above 60 FPS, using the runtime's instanced
// cel renderer on the GPU.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/mesh.h"

namespace perf {

struct Options {
  uint32_t width = 1920, height = 1080;
  uint32_t frames = 30;              // frames timed per sample
  double fps_floor = 60;             // stop once below this
  uint32_t max_instances = 1u << 21;
  int refine_steps = 5;              // bisection steps between the last good and first bad counts
};

struct Sample {
  uint32_t instances = 0;
  double fps = 0, ms = 0;
  uint32_t frames = 0;
  bool refine = false;
};

struct Result {
  bool ok = false;
  std::string error;
  std::string driver, device;
  std::vector<Sample> samples;
  uint32_t budget = 0;  // the most instances measured at or above fps_floor
  bool budget_is_lower_bound = false;  // never dropped below fps_floor
};

// Doubles the instance count (1, 2, 4, ...) until FPS drops below the floor,
// then bisects between the last good and first bad counts.
Result benchmark(const runtime::Mesh &mesh, float footprint_radius, const Options &options);

nlohmann::json to_json(const Result &r, const Options &o);

}  // namespace perf
