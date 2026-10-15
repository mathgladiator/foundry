// Plotters (design/plotter.md): a workspace primitive, plotters/<name>.json, that
// binds a plot system to a biome config. Loading one checks its kinds against
// their pipelines, measures any kind's missing footprint from its pipeline at its
// largest age (and writes it back, so the native runtime can compile it in), and
// compiles the config into the plot system's channel (plot_config).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "pipeline/pipeline.h"

namespace plot {

// The grounds a cell can have (plot_region's ground enum).
const std::vector<std::string> &grounds();

struct Kind {
  uint32_t id = 0;
  std::string layer, pipeline;
  bool regrow_only = false;     // listed only in a layer's regrow_kinds
  nlohmann::json params;        // fixed parameters
  nlohmann::json vary;          // parameter -> {value: weight}
  nlohmann::json on_regrow;     // parameters a regrown placement starts with
  float weight = 1, max_share = 1;
  float footprint_m = 0;
  uint32_t footprint_cells = 0;
  std::string label() const;    // e.g. "tree oak"
};

struct Plotter {
  std::string name, system, doc, state;
  nlohmann::json file;          // the plotter as written
  std::vector<Kind> kinds;      // every kind, by id
  std::vector<std::string> layers;
  nlohmann::json config;        // the plot_config value
  uint32_t distinct_seeds = 6;  // seeds per kind and parameters (the quality cap)
  // The parameters shared by its kinds' pipelines (season, wind...), with the values to show them at.
  nlohmann::json shared = nlohmann::json::object();
};

std::vector<std::string> names(const forge::Paths &paths);

// Loads plotters/<name>.json. Measures missing footprints with the runner (writing them back).
bool load(const forge::Paths &paths, pipeline::Runner &runner, const std::string &name, Plotter &out, std::string &error);

// A placement's pipeline values: the kind's parameters, its varied ones picked by the placement's
// seed, its on_regrow ones when regrown, a pipeline seed from the distinct-seed cap, and the shared ones.
nlohmann::json values_for(const Plotter &p, const Kind &k, uint32_t seed, bool regrown, const nlohmann::json &shared);

// The footprint for a radius: the smallest arrangement whose inner circle, inset 0.1 m, holds it.
uint32_t footprint_cells_for(float radius);

}  // namespace plot
