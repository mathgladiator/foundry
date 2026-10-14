// The foundry application: the workspace UI plus the MCP server the agent
// drives it through.
#include <SDL3/SDL.h>

#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "app/capture.h"
#include "app/services.h"
#include "plan/tools.h"
#include "viewport/tools.h"
#include "plot/tools.h"
#include "assets/mesh_item.h"
#include "assets/tools.h"
#include "assets/view.h"
#include "core/log.h"
#include "core/util.h"
#include "forge/tools.h"
#include "meshy/tools.h"
#include "perf/stress.h"
#include "pipeline/compose.h"
#include "pipeline/tools.h"
#include "plot/grow.h"
#include "sky/observatory.h"
#include "sky/tools.h"
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

// A crash writes its stack (addresses in the binary, for addr2line) to stderr, the foundry's log.
void on_crash(int sig) {
  void *frames[64];
  const int n = backtrace(frames, 64);
  const char head[] = "crash: signal; the stack follows\n";
  (void)!write(2, head, sizeof head - 1);
  backtrace_symbols_fd(frames, n, 2);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGSEGV, on_crash);
  std::signal(SIGABRT, on_crash);
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
  assets::set_fire_root(ws.root());
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
  // The pipeline view's plotter fill (draw.nature) grows through the plot slice.
  pipeline::set_nature_grower([&s, &pipelines, &runner](const std::string &plotter, const nlohmann::json &regions, uint32_t seed,
                                                        const nlohmann::json &shared, runtime::Mesh &mesh, std::string &error) {
    return plot::grow(s.paths, pipelines, runner, plotter, regions, seed, shared, mesh, error);
  });
  pipeline::Live live(pipelines);
  pipeline::register_tools(registry, pipelines, live, ws);
  plot::View plot_view(s.paths, pipelines, runner);
  plot::register_tools(registry, plot_view);
  plan::View plan_view(s.paths, pipelines, runner);
  plan::register_tools(registry, plan_view);
  viewport::register_tools(registry);
  sky::observatories().set_root(s.config.workspace);
  sky::register_tools(registry);
  perf::register_stress_tools(registry, s.paths, pipelines);
  // Selecting a pipeline or a config entry shows the live instance, posed for now.
  assets::set_live_provider([&live, &plot_view, &plan_view](const workspace::Item &item, assets::ViewContent &v) {
    std::string error;
    if (item.kind == "planner") {
      // A planner shows its plan, planned and composed in the background.
      if (!plan_view.show(item.name, error)) return false;
      return plan::content(plan_view, v, 180000);
    }
    if (item.kind == "plotter") {
      // A plotter shows its region, composed in the background.
      if (!plot_view.show(item.name, error)) return false;
      return plot::content(plot_view, v, 120000);
    }
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
    v.has_box = f.has_box;
    v.box_min = f.box_min;
    v.box_max = f.box_max;
    v.lines = f.guides;
    v.fires = f.fires;
    v.own_ground = f.terrain;
    v.fire_time = f.time;
    v.wind_speed = f.wind_speed;
    v.wind_heading = f.wind_heading;
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
  s.plot = &plot_view;
  s.plan = &plan_view;

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
