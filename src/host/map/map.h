// Maps (design/map.md): the regional executor's workspace primitive, maps/<name>.json, the high-level
// plan of a region a player plays in (its shape, biome, parcels, sites, links, and rivers); the macro
// plan map_plan makes from it (in WebAssembly); one chunk's spec cut from that plan; and the atlas,
// the plan drawn from above for the human and the agent to talk over.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "viewport/image.h"
#include "wasm/runner.h"

namespace map {

std::vector<std::string> names(const forge::Paths &paths);
bool load(const forge::Paths &paths, const std::string &name, nlohmann::json &file, std::string &error);
bool save(const forge::Paths &paths, const std::string &name, const nlohmann::json &file, std::string &error);
// A new map's file: a hexagon of the radius (cells) with forest sides and one plains parcel.
nlohmann::json blank(const std::string &name, uint32_t radius);

// Applies edits to a map file, in order. Each is an object with "op":
//   parcel {name, at|q,r|x,z, kind, plotter, reach, height, relief, hill_size, roughness, ridges, cliffs,
//          caves, moisture}: adds or changes a parcel (by name); remove_parcel {name}
//   site {name, kind, at|q,r|x,z, radius, level, height, planner, pipeline, params, seed, yaw}; remove_site {name}
//   link {from, to, kind, via}: adds (or changes the kind of) a link; unlink {from, to}
//   river {name, at|q,r|x,z, to, width}; remove_river {name}
//   shape {...terrain_shape fields}, seed {value}, biome {value}, doc {value}, chunk_size {value}
// "at" is [q, r]; x, z are metres (converted to the nearest cell).
bool apply(nlohmann::json &file, const nlohmann::json &ops, std::string &error);

// The map_spec channel's value for a map file.
nlohmann::json spec_of(const nlohmann::json &file);

struct PlanRun {
  bool ok = false;
  std::string error;
  nlohmann::json plan;  // map_plan's output
  double milliseconds = 0;
  uint64_t fuel = 0;
  bool native_checked = false, native_match = false;
};
// Runs map_plan on a map file's spec.
PlanRun plan(const forge::Paths &paths, wasm::Runner &runner, const nlohmann::json &file, bool verify_native);
// The report part of a plan (everything but its big arrays).
nlohmann::json report(const nlohmann::json &plan);

// One chunk's spec (map_chunk) cut from a plan: its window of the grid, the segments and flats near
// it, the parcels, and the outline. Copies only, so it is the same wherever it is cut.
nlohmann::json chunk_spec(const nlohmann::json &plan, size_t chunk, uint32_t seed);

// The far land's spec (map_far_spec) for a plan: the plan, the map's biome resolved, the season, and
// how far it sinks below the ground.
nlohmann::json far_spec(const forge::Paths &paths, const nlohmann::json &file, const nlohmann::json &plan, const std::string &season,
                        float sink);

// The atlas: the plan from above, north up. Parcels coloured by kind and shaded by the land, water,
// parcel borders, roads by kind, rivers, sites with their names, parcels' names, chunk lines, and a
// scale bar.
struct AtlasOptions {
  int size = 1024;
  bool labels = true, chunks = false, grid = true;
};
viewport::Image atlas(const nlohmann::json &file, const nlohmann::json &plan, const AtlasOptions &options);

}  // namespace map
