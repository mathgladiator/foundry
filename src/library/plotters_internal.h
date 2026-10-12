// The compiled plotters (tools/gen_pipelines.py writes src/library/plotters.cpp):
// each one's bound config, its plot system called natively, and its kinds with
// the spawn function that turns a placement into an instance. Never included by
// runtime.h.
#pragma once

#include <cstdint>
#include <string>

#include "channels/plot_v1_request.h"
#include "channels/plot_v1_result.h"
#include "runtime.h"

namespace plotters {

struct Kind {
  const char *label;  // "pipeline param...", e.g. "tree_v1 oak"
  // Spawns the kind with its parameters: varied ones picked by seed, on_regrow ones when regrown.
  foundry_instance (*spawn)(foundry_runtime *rt, uint32_t seed, bool regrown, const float transform[16]);
};

struct Info {
  const char *name;
  const channels::plot_v1_config &(*config)();
  bool (*run)(const channels::plot_v1_request &in, channels::plot_v1_result &out, std::string &error);
  const Kind *kinds;
  uint32_t kind_count;
};

uint32_t count();
const Info *info(foundry_plotter plotter);  // null when out of range

}  // namespace plotters
