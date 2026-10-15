// A planner's settlement as a list of placements (design/map.md, "The pieces round it"): its plan
// run for a seed with overrides (a map's town plans on flat ground and stands on its site's flat),
// and what each plot and line asks to have built, by pipeline, values, place, and turn, for the map's
// executor to spawn natively. The same decisions as the planner view's preview fill (view.cpp), kept
// as a second copy (spine.md: write everything twice); walls are built without their gates.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "pipeline/pipeline.h"
#include "wasm/runner.h"

namespace plan {

struct Fill {
  std::string pipeline;
  nlohmann::json values;   // its parameters by name
  foundry::vec3 at{0, 0, 0};  // in the settlement's frame (its middle at the origin), metres
  float yaw = 0;           // radians
  std::string what;        // road_cells, buildings, civic, castle_parts, fields, waterfronts, walls
};

struct Settlement {
  nlohmann::json plan;     // plan_plan
  std::vector<Fill> fills;
  float radius_m = 0;      // the working site's radius
  // The cells it occupies, as q, r pairs in its frame (its middle is cell 0, 0): every cell something
  // stands on (buildings, streets, water, walls), and its streets with each one's surface (the road
  // kind's: dirt, gravel, cobble, flagstone, planks...), for a map's ground to draw.
  std::vector<int32_t> built, streets;
  std::vector<std::string> street_surfaces;
  std::string error;
};

// Plans planner's settlement at seed (overrides: plan_config fields; shared: season, wind, theme...).
bool settle(const forge::Paths &paths, pipeline::Runner &runner, wasm::Runner &wasm, const std::string &planner, uint32_t seed,
            const nlohmann::json &overrides, const nlohmann::json &shared, Settlement &out, std::string &error);

}  // namespace plan
