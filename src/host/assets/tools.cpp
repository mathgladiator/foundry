// MCP tools for Meshy mesh items (sizing, recommendation, previews),
// conformance shapes, and screenshots.
#include "assets/tools.h"

#include <algorithm>
#include <cstdio>

#include "assets/lod.h"
#include "assets/mesh_item.h"
#include "assets/quality.h"
#include "assets/sizing.h"
#include "assets/view.h"
#include "core/log.h"
#include "core/util.h"

namespace assets {

namespace {

using nlohmann::json;

json lod_summary(const json &j) {
  json out = json::array();
  if (!j.contains("recommended")) return out;
  for (const auto &l : j["recommended"].value("lods", json::array()))
    out.push_back({{"level", l["level"]}, {"triangles", l["triangles"]}, {"vertices", l["vertices"]},
                   {"error", l["error"]}, {"max_error", l.value("max_error", json())},
                   {"volume_m3", l.value("volume_m3", 0.0)}, {"boundary_edges", l["boundary_edges"]},
                   {"switch_distance_m", l.contains("switch") ? l["switch"].value("distance_m", 0.0) : 0.0},
                   {"switch_pixels", l.contains("switch") ? l["switch"].value("pixels", 0.0) : 0.0}});
  return out;
}

json perf_summary(const json &j) {
  json out = json::array();
  if (!j.contains("perf")) return out;
  for (const auto &r : j["perf"].value("runs", json::array()))
    out.push_back({{"label", r["label"]}, {"triangles", r["triangles"]}, {"index_count", r["index_count"]},
                   {"budget_instances", r["budget_instances"]}, {"budget_is_lower_bound", r["budget_is_lower_bound"]}});
  return out;
}

}  // namespace

void register_tools(mcp::Registry &registry, const forge::Paths &paths, wasm::Runner &, workspace::Workspace &ws) {
  const std::string root = paths.workspace;
  registry.add({"conformance_list", "List conformance shapes (conformance/*.json) that assets are sized against.",
                mcp::schema(json::object()),
                [&ws](const json &) {
                  ws.scan();
                  json out = json::array();
                  for (const auto &i : ws.items())
                    if (i.kind == "conformance") out.push_back(i.data);
                  return mcp::Result::json(out);
                }});

  registry.add({"mesh_info",
                "The hull estimator's report on a Meshy mesh item (e.g. garden_gnome_mesh) after its sizing ops: size, "
                "height, lowest point (grounded?), footprint centre and smallest-enclosing-circle radius (centred?), "
                "volume, topology, and for each conformance shape whether its size fits and the largest uniform "
                "scale that would fit. Also the item's ops, fit, and recommendation.",
                mcp::schema({{"name", {{"type", "string"}}}}, {"name"}),
                [root](const json &args) {
                  const std::string name = args.value("name", "");
                  runtime::Mesh mesh;
                  std::string error;
                  if (!load_full(root, name, mesh, error)) return mcp::Result::error(error);
                  json j = core::read_json(item_dir(root, name) + "/item.json");
                  return mcp::Result::json({{"ops", j.value("ops", json::array())},
                                            {"conformance", j.value("conformance", "")},
                                            {"fit", j.value("fit", json())},
                                            {"hull", hull_report(root, mesh)},
                                            {"recommended", lod_summary(j)},
                                            {"perf", perf_summary(j)},
                                            {"state", j.value("state", "draft")}});
                }});
  registry.add({"mesh_transform",
                "Apply sizing transforms to a Meshy mesh item (replayed from the full-fidelity import, so they're "
                "reproducible). ops is a list of {\"op\": ...}: scale {by: s or [x,y,z]}, rotate {axis, degrees}, "
                "translate {by: [x,y,z]}, ground (lowest point to y = 0), center (footprint's smallest enclosing "
                "circle to the origin), height {meters} (uniform scale to that height, grounded), fit {conformance, "
                "fill = 0.95, height optional} (center, ground, and scale into the conformance). reset: true "
                "replaces the existing ops. Clears any LOD recommendation. Refused once the item is approved.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"ops", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                             {"reset", {{"type", "boolean"}}}},
                            {"name", "ops"}),
                [root, &ws](const json &args) {
                  json j;
                  std::string error;
                  if (!mesh_transform(root, args.value("name", ""), args.value("ops", json::array()),
                                      args.value("reset", false), j, error))
                    return mcp::Result::error(error);
                  ws.scan();
                  ws.select("meshy:" + args.value("name", ""));
                  runtime::Mesh mesh;
                  load_full(root, args.value("name", ""), mesh, error);
                  return mcp::Result::json({{"ops", j["ops"]}, {"fit", j["fit"]}, {"hull", hull_report(root, mesh)}});
                }});
  registry.add({"mesh_recommend",
                "Run the transforms on a sized Meshy mesh item: the volume-preserving progressive mesh, then three "
                "recommended LODs by image error (by default LOD0 < 1%, LOD1 < 5%, LOD2 < 20%, or the item's own "
                "limits, or thresholds given here; off-screen renders from 14 "
                "views counting silhouette changes and color changes), each as a vertex-cache-optimized triangle "
                "list, and the GPU benchmark of each. triangles overrides the "
                "error search with explicit counts. Takes about half a minute; watch the status bar. When the human "
                "approves the item, these LODs are stamped into fixed/<name>/.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"thresholds", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "three ascending error limits as fractions, e.g. [0.01, 0.05, 0.2]"}}},
                             {"triangles", {{"type", "array"}, {"items", {{"type", "integer"}}}}}},
                            {"name"}),
                [root, &ws](const json &args) {
                  if (args.contains("thresholds")) {
                    const auto &t = args["thresholds"];
                    if (!t.is_array() || t.size() != 3 || !(t[0] < t[1] && t[1] < t[2]) || t[0].get<double>() <= 0)
                      return mcp::Result::error("thresholds must be three ascending fractions, e.g. [0.01, 0.05, 0.2]");
                    const std::string path = item_dir(root, args.value("name", "")) + "/item.json";
                    json j = core::read_json(path);
                    if (!j.is_object()) return mcp::Result::error("no mesh item " + args.value("name", ""));
                    j["lod_thresholds"] = t;
                    core::write_json(path, j);
                  }
                  std::vector<uint32_t> targets;
                  for (const auto &t : args.value("triangles", json::array())) targets.push_back(t.get<uint32_t>());
                  json j;
                  std::string error;
                  if (!mesh_recommend(root, args.value("name", ""), targets.empty() ? nullptr : &targets, j, error))
                    return mcp::Result::error(error);
                  ws.scan();
                  ws.select("meshy:" + args.value("name", ""));
                  return mcp::Result::json({{"progressive", j.value("progressive", json())},
                                            {"recommended", lod_summary(j)},
                                            {"perf", perf_summary(j)},
                                            {"perf_device", j.contains("perf") ? j["perf"].value("device", "") : ""}});
                }});
  registry.add({"mesh_regenerate_lod",
                "Change one LOD's error limit on a Meshy mesh item (a fraction, e.g. 0.2 for 20%). Limits always "
                "stack: the others are pushed out of its way (later ones up, earlier ones down, 0.1% apart, within "
                "0.1%..60%). Every LOD whose limit or level changed is searched again, rebaked, and re-benchmarked; "
                "the rest stay as they are.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"lod", {{"type", "integer"}}},
                             {"max_error", {{"type", "number"}}}},
                            {"name", "lod", "max_error"}),
                [root, &ws](const json &args) {
                  json j;
                  std::string error;
                  if (!mesh_regenerate_lod(root, args.value("name", ""), args.value("lod", 0u),
                                           args.value("max_error", 0.0), j, error))
                    return mcp::Result::error(error);
                  ws.scan();
                  return mcp::Result::json({{"lod_thresholds", j["lod_thresholds"]}, {"recommended", lod_summary(j)},
                                            {"perf", perf_summary(j)}});
                }});
  registry.add({"mesh_switch_threshold",
                "Set a Meshy mesh item's switch threshold: the share of pixels (a fraction, default 0.1 for 10%) "
                "allowed to visibly disagree with the full mesh at a LOD's switch distance. Recomputes every LOD's "
                "switch distance; the LODs and benchmarks don't change. Clamped to 0.001..0.5.",
                mcp::schema({{"name", {{"type", "string"}}}, {"share", {{"type", "number"}}}}, {"name", "share"}),
                [root, &ws](const json &args) {
                  json j;
                  std::string error;
                  if (!mesh_set_switch_share(root, args.value("name", ""), args.value("share", 0.1), j, error))
                    return mcp::Result::error(error);
                  ws.scan();
                  return mcp::Result::json({{"switch_share", j["switch_share"]}, {"recommended", lod_summary(j)}});
                }});
  registry.add({"mesh_lod_preview",
                "Render a Meshy mesh item at several triangle counts in one call (snippets for judging LODs). Each "
                "image is labelled with the triangle count shown, its error (silhouette and color), and volume. "
                "Needs the progressive mesh (run mesh_recommend first).",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"triangles", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                             {"view", {{"type", "string"}, {"enum", {"orbit", "front", "side", "top", "iso"}}}},
                             {"zoom", {{"type", "number"}}},
                             {"size", {{"type", "integer"}, {"description", "pixels, default 320"}}}},
                            {"name", "triangles"}),
                [&ws](const json &args) {
                  ws.scan();
                  ViewContent v = load_view(ws, "meshy:" + args.value("name", ""));
                  if (!v.mesh) return mcp::Result::error("no mesh item " + args.value("name", ""));
                  if (!v.progressive) return mcp::Result::error("no progressive mesh yet; run mesh_recommend first");
                  QualityJudge judge(*v.mesh);
                  viewport::Camera cam;
                  cam.view = viewport::parse_view(args.value("view", "orbit"));
                  cam.zoom = args.value("zoom", 1.0f);
                  int size = std::clamp(args.value("size", 320), 64, 768);
                  viewport::Overlays overlays;  // frame the asset and its conformance only
                  overlays.hero = false;
                  overlays.hex = false;
                  mcp::Result r;
                  for (const auto &t : args.value("triangles", json::array())) {
                    uint32_t tris = t.get<uint32_t>();
                    auto shown = mesh_at(v, tris);
                    ImageError e = judge.error(*shown);
                    json st = mesh_stats(*shown);
                    char buf[220];
                    std::snprintf(buf, sizeof buf,
                                  "asked %u -> %zu triangles; error %.2f%% (silhouette %.2f%%, color %.2f%%); volume %.5f m3",
                                  tris, shown->triangles(), e.total() * 100, e.silhouette * 100, e.color * 100,
                                  st.value("volume_m3", 0.0));
                    viewport::Image img = render_view(v, cam, overlays, false, size, size, tris);
                    auto png = viewport::encode_png(img);
                    r.content.push_back({"text", buf, "", ""});
                    r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  }
                  return r;
                }});
  registry.add({"view_screenshot",
                "Render a workspace item (default: the current selection) as a PNG: a mesh item or stamped fixed "
                "asset in the cel look with its conformance capsule, the 1.8 m hero, and the hex; or concept art as "
                "is. Views: orbit, front, side, top, iso (isometric); quarter rotates by 90 degrees. silhouette draws "
                "the battle-distance black-shape check.",
                mcp::schema({{"id", {{"type", "string"}}},
                             {"view", {{"type", "string"}, {"enum", {"orbit", "front", "side", "top", "iso"}}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"zoom", {{"type", "number"}}},
                             {"silhouette", {{"type", "boolean"}}},
                             {"quarter", {{"type", "integer"}, {"description", "quarter turns (90 degrees) around +Y"}}},
                             {"distance", {{"type", "number"}, {"description", "orbit view: camera distance in metres with the game camera's 0.9 rad field of view (e.g. a LOD's switch distance)"}}},
                             {"triangles", {{"type", "integer"}, {"description", "mesh items: show the progressive level at or under this triangle count"}}},
                             {"size", {{"type", "integer"}, {"description", "pixels, default 512"}}}}),
                [&ws](const json &args) {
                  ws.scan();
                  std::string id = args.value("id", ws.selected());
                  if (id.empty()) return mcp::Result::error("nothing selected; pass an id");
                  ViewContent v = load_view(ws, id);
                  if (v.empty()) return mcp::Result::error("nothing to show for " + id);
                  viewport::Camera cam;
                  cam.view = viewport::parse_view(args.value("view", "orbit"));
                  cam.yaw = args.value("yaw", cam.yaw);
                  cam.pitch = args.value("pitch", cam.pitch);
                  cam.zoom = args.value("zoom", cam.zoom);
                  cam.quarter = args.value("quarter", 0);
                  cam.distance = args.value("distance", 0.0f);
                  int size = std::clamp(args.value("size", 512), 64, 1024);
                  uint32_t tris = args.value("triangles", 0u);
                  viewport::Image img = render_view(v, cam, {}, args.value("silhouette", false), size, size, tris);
                  auto png = viewport::encode_png(img);
                  mcp::Result r;
                  auto shown = mesh_at(v, tris);
                  std::string label = id + " (" + viewport::view_name(cam.view) + ")";
                  if (shown) label += ", " + std::to_string(shown->triangles()) + " triangles";
                  r.content.push_back({"text", label, "", ""});
                  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  return r;
                }});
}

}  // namespace assets
