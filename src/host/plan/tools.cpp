#include "plan/tools.h"

#include <chrono>
#include <optional>
#include <thread>

#include "core/log.h"
#include "core/util.h"
#include "plot/tools.h"
#include "sky/observatory.h"
#include "viewport/image.h"

namespace plan {

using json = nlohmann::json;

bool content(View &view, assets::ViewContent &out, int timeout_ms) {
  Picture pic = view.picture();
  for (int waited = 0; (pic.building || !pic.mesh) && waited < timeout_ms; waited += 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pic = view.picture();
  }
  if (!pic.mesh) return false;
  out.label = "planner:" + view.name();
  out.mesh = pic.mesh;
  out.lines = pic.lines;
  out.collision = pic.collision;
  out.has_conformance = false;
  out.fires = pic.fires;
  const json shared = view.settings().shared;
  out.wind_speed = shared.value("wind_speed", 0.0f);
  out.wind_heading = shared.value("wind_heading", 90.0f);
  return true;
}

namespace {

bool ensure(View &view, const json &args, std::string &error) {
  const std::string name = args.value("planner", view.name());
  if (name.empty()) {
    error = "no planner shown; pass planner (e.g. market_town_v1)";
    return false;
  }
  return view.show(name, error);
}

json settings_json(const Settings &s) {
  return {{"seed", s.seed},       {"plots", s.plots},           {"tier", tiers()[s.tier]}, {"fill", s.fill}, {"lines", s.lines},
          {"nature", s.nature},   {"outlines", s.outlines},     {"nav", s.nav},            {"collision", s.collision},
          {"colour", s.colour},   {"shared", s.shared},         {"overrides", s.overrides}};
}

json state(View &view) {
  const Planner p = view.planner();
  json lots = json::array();
  for (const auto &l : p.lots)
    lots.push_back({{"class", classes()[l.cls]}, {"shape", shapes()[l.shape]}, {"name", l.name}, {"cells", l.cells.size() / 2},
                    {"builds", l.builds.size()}});
  json cfg = view.config();
  cfg.erase("lot_class");
  cfg.erase("lot_shape");
  cfg.erase("lot_cells");
  cfg.erase("lot_wet");
  cfg.erase("lot_doors");
  return {{"planner", p.name}, {"system", p.system}, {"doc", p.doc}, {"config", cfg}, {"lots", lots},
          {"settings", settings_json(view.settings())}, {"report", view.report()}};
}

}  // namespace

void register_tools(mcp::Registry &registry, View &view) {
  registry.add({"planner_get",
                "Describe a planner (design/planner.md; default: the one the workspace shows): its plan system, its "
                "config (the scenario's ranges and rules), its lots (measured from the fill's building pipeline, plus "
                "and the waterfront, civic, centre, and castle lots measured from theirs), the view's settings (seed, plots, tier, "
                "what's drawn), and the last plan's report (plots by kind, class, shape, use, and tier; road cells by class; "
                "halves and fenced edges; lines; open cells; reachability; grades; timing).",
                mcp::schema({{"planner", {{"type", "string"}, {"description", "e.g. market_town_v1"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  return mcp::Result::json(state(view));
                }});
  registry.add({"planner_set",
                "Change the shown planner's view and plan: seed, plots (0: the middle of the config's range), tier "
                "(hamlet, village, town), what's drawn (fill: houses, civic buildings, castle parts, road pieces, and waterfronts on "
                "plots; lines: walls, the keep's wall, and fences laid by the fill's line system with their gates; nature: a plotter "
                "(e.g. happy_glen_v1) grown on the open land, or \"\" for none; outlines; nav: the navigation "
                "mesh; collision; colour: use, height, tier, or wealth), shared values for every building (season, "
                "wind_speed), and config fields to try (merged over the file's; null removes one). save: true writes the "
                "tried config, seed, plots, and tier into the planner's file. Re-plans, and returns the report.",
                mcp::schema({{"planner", {{"type", "string"}}},
                             {"seed", {{"type", "integer"}}},
                             {"plots", {{"type", "integer"}}},
                             {"tier", {{"type", "string"}}},
                             {"fill", {{"type", "boolean"}}},
                             {"lines", {{"type", "boolean"}}},
                             {"nature", {{"type", "string"}}},
                             {"outlines", {{"type", "boolean"}}},
                             {"nav", {{"type", "boolean"}}},
                             {"collision", {{"type", "boolean"}}},
                             {"colour", {{"type", "string"}}},
                             {"shared", {{"type", "object"}}},
                             {"config", {{"type", "object"}}},
                             {"save", {{"type", "boolean"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  json patch = args;
                  patch.erase("planner");
                  patch.erase("save");
                  if (!view.set(patch, error)) return mcp::Result::error(error);
                  json report;
                  if (!view.plan(false, report, error)) return mcp::Result::error(error);
                  if (args.value("save", false) && !view.save(error)) return mcp::Result::error(error);
                  return mcp::Result::json({{"settings", settings_json(view.settings())}, {"report", report}});
                }});
  registry.add({"planner_plan",
                "Plan the shown planner now with its settings, in WebAssembly; verify_native also runs the plan system "
                "natively and compares the bytes. Returns the report; plan: true also returns the whole plan (cells, "
                "plots, nature ground, navigation mesh), which is large.",
                mcp::schema({{"planner", {{"type", "string"}}},
                             {"verify_native", {{"type", "boolean"}}},
                             {"plan", {{"type", "boolean"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  json report;
                  if (!view.plan(args.value("verify_native", false), report, error)) return mcp::Result::error(error);
                  json out = {{"report", report}};
                  if (args.value("plan", false)) out["plan"] = view.plan_json();
                  return mcp::Result::json(out);
                }});
  registry.add({"planner_view",
                "Render the shown planner's plan: the village's shape (its cells coloured by the settings' colour), water, "
                "wall and gate tiles (flat, outlined), the fill (houses, civic buildings, castle parts, road pieces, waterfronts; "
                "a plot with no fill is only outlined), its lines laid with walls, fences, and gates, nature from "
                "the settings' plotter on the open land, plot outlines (gold, waterfronts blue; red where unreached), "
                "entrance tiles (cyan rings), and with nav "
                "the navigation mesh (cyan where carts go). view: orbit (default), top, front, side, iso; yaw, pitch, "
                "zoom; distance > 0 puts the game camera that far away.",
                mcp::schema({{"planner", {{"type", "string"}}},
                             {"view", {{"type", "string"}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"zoom", {{"type", "number"}}},
                             {"distance", {{"type", "number"}}},
                             {"observatory", {{"type", "string"}}},
                             {"size", {{"type", "integer"}, {"description", "pixels (default 900)"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  assets::ViewContent c;
                  if (!content(view, c, 180000)) return mcp::Result::error("the plan's picture isn't ready");
                  viewport::Camera cam;
                  cam.view = viewport::parse_view(args.value("view", "orbit"));
                  cam.yaw = args.value("yaw", 0.7f);
                  cam.pitch = args.value("pitch", 0.6f);
                  cam.zoom = args.value("zoom", 1.0f);
                  cam.distance = args.value("distance", 0.0f);
                  const int size = std::clamp(args.value("size", 900), 128, 2048);
                  viewport::SkyScene sky_scene;
                  const std::string under = args.value("observatory", sky::observatories().view_of("planner:" + view.name()));
                  const bool has_sky = under != "none" && sky::observatories().scene(under, sky_scene);
                  viewport::Image img = assets::render_view(c, cam, plot::overlays(view.settings().collision), false, size, size, 0,
                                                            has_sky ? &sky_scene : nullptr);
                  auto png = viewport::encode_png(img);
                  Picture pic = view.picture();
                  mcp::Result r;
                  json text = {{"triangles", c.mesh ? c.mesh->triangles() : 0}, {"renderer", viewport::last_renderer()}, {"report", pic.report}};
                  if (!pic.error.empty()) text["error"] = pic.error;
                  r.content.push_back({"text", text.dump(), "", ""});
                  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  return r;
                }});
}

}  // namespace plan
