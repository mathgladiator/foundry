// A plotter grown over hex regions and built (design/plotter.md): its plot system run over the
// regions, each placement built by its kind's pipeline (once per distinct set of values) and placed
// into a mesh. The planner's nature fill and the pipeline view's (draw.nature) both use it.
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "forge/forge.h"
#include "pipeline/pipeline.h"
#include "wasm/runner.h"

namespace plot {

// Returns how many placements stood; regions is a plot_region array.
uint32_t grow(const forge::Paths &paths, pipeline::Runner &runner, wasm::Runner &wasm, const std::string &plotter,
              const nlohmann::json &regions, uint32_t seed, const nlohmann::json &shared, runtime::Mesh &mesh, std::string &error);

}  // namespace plot
