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

// A plotter's scale plan (design/quality.perf.md, "Scale"): builds stress_plotter_<name> and runs it
// with --plan scale in the background (sizes: grid sides, e.g. "1,2,4,8"; frames per measurement).
// Returns its job, or 0 when one is already running (blocked_by is set to it).
int start_plotter_scale(const forge::Paths &paths, const std::string &plotter, const std::string &sizes, int frames,
                        bool report, int *blocked_by = nullptr);
// The latest scale job of a plotter, running or done (0 if none).
int latest_plotter_scale(const std::string &plotter);
// The newest scale report of a plotter in the workspace's perf/ (null if none), and its path.
nlohmann::json latest_scale_report(const forge::Paths &paths, const std::string &plotter, std::string *path = nullptr);
}
