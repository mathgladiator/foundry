// The stress tool's MCP front (design/quality.perf.md): perf_stress and
// perf_stress_status.
#pragma once

#include "forge/forge.h"
#include "mcp/handler.h"
#include "pipeline/pipeline.h"

namespace perf {
void register_stress_tools(mcp::Registry &registry, const forge::Paths &paths, pipeline::Runner &runner);

// The QA video (design/quality.perf.md, "The QA video"), exported by the pipeline's stress binary.
struct VideoRequest {
  std::string pipeline;
  int floats = 1;          // samples of the continuous parameters per discrete combination
  double seconds = 1.0;    // per variation
  int fps = 30;
  std::string size = "1280x720";
  nlohmann::json fixed = nlohmann::json::object();   // parameters pinned to one value
  nlohmann::json ranges = nlohmann::json::object();  // enums narrowed to a list
};
// Starts the export in the background; returns its job, for job_status.
int start_video(const forge::Paths &paths, const VideoRequest &request);
// A stress or video job: its output so far, and once done its result and files.
nlohmann::json job_status(int job);
}
