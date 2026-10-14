// Planners (design/planner.md): a workspace primitive, planners/<name>.json, that binds a plan
// system to a settlement config. Loading one measures its lots (a building pipeline's footprint
// for each size class and shape, from the config entries that have that shape), and the
// waterfront, civic, centre, and castle lots (each size built once from its pipeline), and compiles the config into the plan system's channel
// (plan_v1_config). The preview fill (what the foundry shows standing on plots) is data in the
// file too; the host names no pipeline.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "pipeline/pipeline.h"

namespace plan {

// The names the plan channels use, in their enum order.
const std::vector<std::string> &classes();    // solo, family, extended, civic, castle, waterfront, centre
const std::vector<std::string> &shapes();     // wide, deep, round, long, broad
const std::vector<std::string> &uses();       // home, store, workshop, inn, farm, civic, keep, harbour, way, water, castle, centre
const std::vector<std::string> &tiers();      // hamlet, village, town

// One lot a plot can be: its class, shape, cells (q, r pairs, facing +z), and, for lots measured from
// a building pipeline, the values that build on it for each config entry with that shape.
struct Lot {
  uint32_t cls = 0, shape = 0;
  std::string name;                // e.g. rect6x5, flower91
  std::vector<int> cells, wet;      // wet: which cells must be water (a waterfront's), q, r pairs
  std::vector<int> doors;          // the cells just outside its doors, q, r pairs
  std::vector<nlohmann::json> builds;  // pipeline values (type, household, footprint) that stand on it
};

struct Planner {
  std::string name, system, doc, state;
  nlohmann::json file;     // the planner as written
  nlohmann::json config;   // the plan_v1_config value, with its lots
  nlohmann::json fill;     // the preview fill
  std::vector<Lot> lots;
};

std::vector<std::string> names(const forge::Paths &paths);

// Loads planners/<name>.json and measures its lots with the runner.
bool load(const forge::Paths &paths, pipeline::Runner &runner, const std::string &name, Planner &out, std::string &error);

}  // namespace plan
