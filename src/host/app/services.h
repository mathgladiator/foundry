// The host's long-lived services, created once in main.
#pragma once

#include "core/config.h"
#include "forge/forge.h"
#include "mcp/handler.h"
#include "mcp/server.h"
#include "meshy/meshy.h"
#include "pipeline/live.h"
#include "plan/view.h"
#include "plot/view.h"
#include "wasm/runner.h"
#include "workspace/workspace.h"

namespace app {

struct Services {
  core::Config config;
  forge::Paths paths;
  workspace::Workspace *ws = nullptr;
  wasm::Runner *runner = nullptr;
  meshy::Client *meshy = nullptr;
  mcp::Server *server = nullptr;
  pipeline::Live *live = nullptr;  // the pipeline instance the workspace shows
  plot::View *plot = nullptr;      // the plotter region the workspace shows
  plan::View *plan = nullptr;      // the planner the workspace shows
};

// Runs the windowed workspace UI until the window closes. Returns an exit code.
int run_ui(Services &services);

}  // namespace app
