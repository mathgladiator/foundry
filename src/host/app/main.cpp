// The foundry application: the workspace UI plus the MCP server the agent
// drives it through.
#include <SDL3/SDL.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "app/capture.h"
#include "app/services.h"
#include "assets/mesh_item.h"
#include "assets/tools.h"
#include "assets/view.h"
#include "core/log.h"
#include "core/util.h"
#include "forge/tools.h"
#include "meshy/tools.h"
#include "perf/stress.h"
#include "pipeline/tools.h"
#include "workspace/tools.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void usage() {
  std::printf(
      "usage: foundry [--headless] [--root DIR] [--workspace DIR] [--build DIR] [--port N]\n"
      "  --headless  run the MCP server without a window (for automation)\n"
      "  --root      the source root (default: the source tree this was built from)\n"
      "  --workspace the workspace root: the project's data and its foundry.json (default: demo/ in the source root)\n"
      "  --build     the CMake build directory (default: the one this was built in)\n"
      "  --port      the MCP port (default: mcp.port in the workspace's foundry.json)\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  bool headless = false;
  std::string root = FOUNDRY_SOURCE_DIR, build = FOUNDRY_BUILD_DIR, workspace_arg;
  int port = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--headless") headless = true;
    else if (a == "--root" && i + 1 < argc) root = argv[++i];
    else if (a == "--build" && i + 1 < argc) build = argv[++i];
    else if (a == "--workspace" && i + 1 < argc) workspace_arg = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else {
      usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }

  app::Services s;
  std::string error;
  const std::string workspace = workspace_arg.empty() ? root + "/demo" : workspace_arg;
  if (!core::load_config(root, build, workspace, s.config, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (port > 0) s.config.mcp_port = port;
  s.paths = {.root = root, .workspace = s.config.workspace, .build = build};
  core::log().add("app", "workspace: " + s.config.workspace);

  workspace::Workspace ws(root, s.config.workspace);
  ws.scan();
  wasm::Runner runner;
  meshy::Client meshy(s.config);
  if (!meshy.has_key()) core::log().add("meshy", "no API key found in " + s.config.meshy_keys_file);

  mcp::Registry registry;
  workspace::register_tools(registry, ws);
  forge::register_tools(registry, s.paths, runner, ws);
  assets::register_tools(registry, s.paths, runner, ws);
  meshy::register_tools(registry, meshy, ws);
  pipeline::Runner pipelines(s.paths, runner);
  pipeline::Live live(pipelines);
  pipeline::register_tools(registry, pipelines, live, ws);
  perf::register_stress_tools(registry, s.paths, pipelines);
  // Selecting a pipeline or a config entry shows the live instance, posed for now.
  assets::set_live_provider([&live](const workspace::Item &item, assets::ViewContent &v) {
    std::string error;
    if (item.kind == "pipeline" ? !live.show(item.name, error) : !live.show_config_file(item.path, error)) return false;
    double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
    pipeline::Frame f = live.frame(now);
    if (!f.mesh) {
      // The first build runs in the background; wait for it briefly (screenshots need a model).
      for (int i = 0; i < 300 && !f.mesh && live.error().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f = live.frame(static_cast<double>(SDL_GetTicks()) / 1000.0);
      }
      if (!f.mesh) return false;
    }
    v.mesh = f.mesh;
    v.has_conformance = f.conformance_radius > 0;
    v.radius = f.conformance_radius;
    v.height = f.conformance_height;
    return true;
  });
  registry.add({"ui_screenshot",
                "Capture the whole foundry window (workspace tree, viewport, inspector, activity) as a PNG, to see "
                "what the human sees. Fails in --headless mode.",
                mcp::schema(nlohmann::json::object()),
                [](const nlohmann::json &) {
                  viewport::Image img;
                  if (!app::capture().request(img, 3000))
                    return mcp::Result::error("no window to capture (running headless, or the UI is busy)");
                  auto png = viewport::encode_png(img);
                  mcp::Result r;
                  r.content.push_back({"text", std::to_string(img.width) + "x" + std::to_string(img.height), "", ""});
                  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  return r;
                }});

  // Approving a Meshy mesh item stamps its levels of detail into fixed/.
  ws.set_on_approved([&ws, root = s.config.workspace](const workspace::Item &item) {
    if (item.kind != "meshy" || item.data.value("kind", "") != "mesh") return;
    std::string name = item.name;
    std::thread([&ws, root, name] {
      std::string error;
      if (!assets::stamp(root, name, error)) core::log().add("mesh", "stamp failed: " + error);
      ws.scan();
    }).detach();
  });

  mcp::Server server(registry);
  if (!server.start(s.config.mcp_host, s.config.mcp_port)) {
    std::fprintf(stderr, "can't listen on %s:%d (is another foundry running?)\n", s.config.mcp_host.c_str(),
                 s.config.mcp_port);
    return 1;
  }
  s.ws = &ws;
  s.runner = &runner;
  s.meshy = &meshy;
  s.server = &server;
  s.live = &live;

  int rc = 0;
  if (headless) {
    // The GPU benchmark needs SDL's video subsystem even off screen; without
    // a display, fall back to SDL's offscreen video driver.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
      if (!SDL_Init(SDL_INIT_VIDEO)) core::log().add("app", std::string("no SDL video; GPU benchmarks unavailable: ") + SDL_GetError());
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    core::log().add("app", "headless; MCP at " + server.url() + " (Ctrl-C to stop)");
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  } else {
    rc = app::run_ui(s);
  }
  server.stop();
  core::log().add("app", "exiting with code " + std::to_string(rc));
  return rc;
}
