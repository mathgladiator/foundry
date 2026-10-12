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
// Starts the export in the background; returns its job, for job_status. One video exports at a
// time (they share the GPU and ffmpeg): while one runs, this returns 0 and sets *blocked_by to it.
int start_video(const forge::Paths &paths, const VideoRequest &request, int *blocked_by = nullptr);
// The video export running now, of any pipeline (0 if none).
int running_video();
// The latest video export of a pipeline, running or done (0 if none).
int latest_video(const std::string &pipeline);
// A stress or video job: its output so far, and once done its result and files.
nlohmann::json job_status(int job);
}
