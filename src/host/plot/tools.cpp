#include "plot/tools.h"

#include <chrono>
#include <thread>

#include "core/util.h"
#include "viewport/image.h"

namespace plot {

using json = nlohmann::json;

viewport::Overlays overlays(bool collision) {
  viewport::Overlays o;
  o.conformance = false;
  o.hero = false;
  o.hex = false;
  o.grid = false;
  o.collision = collision;
  return o;
}

bool content(View &view, assets::ViewContent &out, int timeout_ms) {
  Picture pic = view.picture();
  for (int waited = 0; (pic.building || !pic.mesh) && waited < timeout_ms; waited += 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pic = view.picture();
  }
  if (!pic.mesh) return false;
  out.label = "plotter:" + view.name();
  out.mesh = pic.mesh;
  out.lines = pic.lines;
  out.collision = pic.collision;
  out.has_conformance = false;
  return true;
}

namespace {

bool ensure(View &view, const json &args, std::string &error) {
  const std::string name = args.value("plotter", view.name());
  if (name.empty()) {
    error = "no plotter shown; pass plotter (e.g. happy_glen_v1)";
    return false;
  }
  return view.show(name, error);
}

}  // namespace

void register_tools(mcp::Registry &registry, View &view) {
  registry.add({"plotter_get",
                "Describe a plotter (design/plotter.md; default: the one the workspace shows) and its region: the "
                "plot system, its kinds with their measured footprints, the region's ground (prioritized hex regions), "
                "seed, shared values (season, wind), the last grow, and metrics (things per layer and kind, per hectare, "
                "nearest neighbours against the spacing, footprint overlaps, distinct builds).",
                mcp::schema({{"plotter", {{"type", "string"}, {"description", "e.g. happy_glen_v1"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  json out = view.state();
                  out["presets"] = View::presets();
                  return mcp::Result::json(out);
                }});
  registry.add({"plotter_set",
                "Change the shown plotter's region: a ground preset (glen, hex, ring, strip, union, pond, path, slope, "
                "ragged, single) with a radius in cells, or regions directly (plot_v1_region values: shape cells/hex/"
                "ring/line, q, r, q2, r2, radius, priority, cut, ground soil/water/rock/sand/path, available, "
                "keep_clear_m, height, moisture); the world seed (a new seed clears the region); and shared values for "
                "every occupant (season, wind_speed...). Occupants on lost cells are removed; the rest never move.",
                mcp::schema({{"plotter", {{"type", "string"}}},
                             {"preset", {{"type", "string"}}},
                             {"radius", {{"type", "integer"}, {"description", "cells (default 12)"}}},
                             {"regions", {{"type", "array"}}},
                             {"seed", {{"type", "integer"}}},
                             {"shared", {{"type", "object"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  json out = json::object();
                  if (args.contains("seed")) view.set_seed(args["seed"].get<uint32_t>());
                  if (args.contains("shared")) view.set_shared(args["shared"]);
                  json regions;
                  if (args.contains("regions")) regions = args["regions"];
                  else if (args.contains("preset"))
                    regions = View::preset(args["preset"].get<std::string>(), args.value("radius", 12), view.seed());
                  if (!regions.is_null()) {
                    json r;
                    if (!view.set_regions(regions, r, error)) return mcp::Result::error(error);
                    out["region"] = r;
                  }
                  out["metrics"] = view.metrics();
                  return mcp::Result::json(out);
                }});
  registry.add({"plotter_grow",
                "Grow the shown plotter's region (design/plotter.md, Growing): the plot system returns up to max "
                "placements that work now, in seed mode (a new region) or regrow mode (filling what was cleared, "
                "starting young), clear of exclusions ([{position: [x, y, z], radius}]); they are added as occupants. "
                "Returns how many were placed, by kind, and the volume left (0: the region is full and stable). "
                "verify_native also runs the plot system natively and compares the bytes.",
                mcp::schema({{"plotter", {{"type", "string"}}},
                             {"mode", {{"type", "string"}, {"description", "seed (default) or regrow"}}},
                             {"max", {{"type", "integer"}, {"description", "default 100000 (everything)"}}},
                             {"exclusions", {{"type", "array"}}},
                             {"focus", {{"type", "array"}, {"description", "[x, y, z]: nearest first"}}},
                             {"verify_native", {{"type", "boolean"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  float focus[3] = {0, 0, 0};
                  const bool use_focus = args.contains("focus");
                  if (use_focus)
                    for (int i = 0; i < 3; ++i) focus[i] = args["focus"][i].get<float>();
                  json out;
                  if (!view.grow(args.value("mode", std::string("seed")) == "regrow", args.value("max", 100000u),
                                 args.value("exclusions", json::array()), use_focus, focus, args.value("verify_native", false),
                                 out, error))
                    return mcp::Result::error(error);
                  out["metrics"] = view.metrics();
                  return mcp::Result::json(out);
                }});
  registry.add({"plotter_clear",
                "Clear the shown plotter's region the way a game would: all occupants, those within radius metres of "
                "center [x, z], or a fraction of one layer (a harvest). fell: true turns trees into stumps instead "
                "(they keep their footprints until cleared again); stumps: true clears the stumps felled trees left.",
                mcp::schema({{"plotter", {{"type", "string"}}},
                             {"all", {{"type", "boolean"}}},
                             {"center", {{"type", "array"}}},
                             {"radius", {{"type", "number"}}},
                             {"layer", {{"type", "string"}}},
                             {"fraction", {{"type", "number"}}},
                             {"fell", {{"type", "boolean"}}},
                             {"stumps", {{"type", "boolean"}}},
                             {"seed", {{"type", "integer"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  json out = view.clear(args);
                  out["metrics"] = view.metrics();
                  return mcp::Result::json(out);
                }});
  registry.add({"plotter_view",
                "Render the shown plotter's region: the ground, every occupant, and the cells' outlines (an unavailable "
                "cell's in red). collision: true also draws every occupant's collision and the footprint it reserves. "
                "view: orbit (default), top, front, side, iso; yaw, pitch, zoom; distance > 0 puts the game camera that "
                "far away.",
                mcp::schema({{"plotter", {{"type", "string"}}},
                             {"view", {{"type", "string"}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"zoom", {{"type", "number"}}},
                             {"distance", {{"type", "number"}}},
                             {"collision", {{"type", "boolean"}}},
                             {"size", {{"type", "integer"}, {"description", "pixels (default 900)"}}}}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  assets::ViewContent c;
                  if (!content(view, c, 120000)) return mcp::Result::error("the region's picture isn't ready");
                  viewport::Camera cam;
                  cam.view = viewport::parse_view(args.value("view", "orbit"));
                  cam.yaw = args.value("yaw", 0.7f);
                  cam.pitch = args.value("pitch", 0.55f);
                  cam.zoom = args.value("zoom", 1.0f);
                  cam.distance = args.value("distance", 0.0f);
                  const int size = std::clamp(args.value("size", 900), 128, 2048);
                  viewport::Image img = assets::render_view(c, cam, overlays(args.value("collision", false)), false, size, size);
                  auto png = viewport::encode_png(img);
                  Picture pic = view.picture();
                  mcp::Result r;
                  json text = {{"triangles", c.mesh ? c.mesh->triangles() : 0}, {"metrics", pic.metrics}};
                  if (!pic.error.empty()) text["error"] = pic.error;
                  r.content.push_back({"text", text.dump(), "", ""});
                  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  return r;
                }});
}

}  // namespace plot
