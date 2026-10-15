#include "map/tools.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

#include "core/util.h"
#include "viewport/image.h"

namespace map {

using json = nlohmann::json;

namespace {

bool ensure(View &view, const json &args, std::string &error) {
  const std::string name = args.value("map", view.name());
  if (name.empty()) {
    error = "no map shown; pass map (map_list names them)";
    return false;
  }
  return view.show(name, error);
}

mcp::Result image_result(const viewport::Image &img, const std::string &text) {
  auto png = viewport::encode_png(img);
  mcp::Result r;
  r.content.push_back({"text", text, "", ""});
  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
  return r;
}

}  // namespace

void register_tools(mcp::Registry &registry, View &view) {
  registry.add({"map_list", "List the workspace's maps (maps/<name>.json; design/map.md).", mcp::schema({}, {}),
                [&view](const json &) {
                  (void)view;
                  return mcp::Result::json({{"maps", names(view.paths())}});
                }});
  registry.add({"map_get",
                "Show a map (the regional executor's plan; design/map.md) and return its file: shape, biome, seed, "
                "parcels (the partition), sites, links (the network), and rivers. Cells are axial (q, r): x = 1.732 (q + r/2), "
                "z = 1.5 r metres, north is -z.",
                mcp::schema({{"map", {{"type", "string"}}}}, {}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  return mcp::Result::json(view.file());
                }});
  registry.add({"map_set",
                "Edit a map, then save it. `create` (with `radius` in cells) makes a new hexagonal map. `ops` is a list, "
                "applied in order: {op: parcel, name, at:[q,r] | x,z (metres), kind (plains, hills, highlands, mountains, "
                "valley, lake, marsh, coast), plotter, reach (m), height, relief, hill_size, roughness, ridges, cliffs, caves, "
                "moisture}, {op: remove_parcel, name}, {op: site, name, kind (town, village, hamlet, keep, castle, house, farm, "
                "ruin, camp, shrine, tower, landmark), at | x,z, radius (cells), planner | pipeline + params, seed, yaw}, "
                "{op: remove_site, name}, {op: link, from, to (site names or exit:<n>), kind (road, lane, path), via [q,r,...]}, "
                "{op: unlink, from, to}, {op: river, name, at | x,z, to (exit:<n>, a lake parcel, or empty), width}, "
                "{op: remove_river, name}, {op: shape, ...terrain_shape fields}, {op: seed|biome|doc|chunk_size, value}.",
                mcp::schema({{"map", {{"type", "string"}}},
                             {"create", {{"type", "boolean"}}},
                             {"radius", {{"type", "integer"}}},
                             {"ops", {{"type", "array"}}}},
                            {}),
                [&view](const json &args) {
                  std::string error;
                  if (args.value("create", false)) {
                    if (!view.create(args.value("map", ""), args.value("radius", 1000u), error)) return mcp::Result::error(error);
                  } else if (!ensure(view, args, error)) {
                    return mcp::Result::error(error);
                  }
                  if (args.contains("ops") && !view.edit(args["ops"], error)) return mcp::Result::error(error);
                  return mcp::Result::json({{"map", view.name()}, {"saved", true}});
                }});
  registry.add({"map_plan",
                "Run the map's macro plan (map_plan, in WebAssembly; verify_native compares the native build) and report: "
                "parcels' areas, places (each site on its flat, turned to its road), road length, bridges, the steepest grade, "
                "links unrouted (must be 0), chunks, time, and fuel.",
                mcp::schema({{"map", {{"type", "string"}}},
                             {"verify_native", {{"type", "boolean"}}},
                             {"check_seams", {{"type", "integer"}, {"description", "build this many pairs of neighbouring chunks and check every apron cell's column matches its neighbour's own (seams)"}}},
                             {"hull", {{"type", "boolean"}, {"description", "also return the boundary's hull (its cells as q, r pairs, edges, heights, rises, openings)"}}},
                             {"export_far", {{"type", "boolean"}, {"description", "write the far land's spec as config/map_fars/<map>.json (a sample for the map_far pipeline; keep it to small maps)"}}},
                             {"export_chunk", {{"type", "array"}, {"description", "[q, r]: write the chunk holding this cell as config/map_chunks/<map>_<q>_<r>.json (a sample for the map_chunk pipeline)"}}}},
                            {}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  PlanRun r = view.plan(args.value("verify_native", false));
                  if (!r.ok) return mcp::Result::error(r.error);
                  json out = report(r.plan);
                  if (args.value("check_seams", 0) > 0) {
                    // Neighbouring chunks agree at their seam: each apron cell's column is its neighbour's own.
                    const int S = r.plan.value("chunk_size", 64);
                    const json &cq = r.plan["chunk_q"], &cr = r.plan["chunk_r"];
                    std::map<std::pair<int, int>, size_t> at;
                    for (size_t c = 0; c < cq.size(); ++c) at[{cq[c].get<int>(), cr[c].get<int>()}] = c;
                    const int want = args.value("check_seams", 0);
                    int pairs = 0, cells = 0, differ = 0;
                    const uint32_t seed = view.file().value("seed", 1u);
                    auto columns = [&](const json &g) {
                      std::map<std::pair<int, int>, std::pair<json, uint32_t>> m;  // spans, and whether it is apron
                      const auto &qr = g["cells"], &first = g["span_first"];
                      for (size_t i = 0; i + 1 < qr.size(); i += 2) {
                        const size_t k = i / 2;
                        json spans = json::array();
                        for (uint32_t s = first[k].get<uint32_t>(); s < first[k + 1].get<uint32_t>(); ++s)
                          spans.push_back({g["span_hi"][s], g["span_material"][s]});
                        m[{qr[i].get<int>(), qr[i + 1].get<int>()}] = {spans, g["flags"][k].get<uint32_t>() & 512u};
                      }
                      return m;
                    };
                    for (size_t c = 0; c < cq.size() && pairs < want; c += 37) {
                      const auto n = at.find({cq[c].get<int>() + S, cr[c].get<int>()});
                      if (n == at.end()) continue;
                      forge::RunResult a = forge::run(view.paths(), view.runner(), "map_ground", chunk_spec(r.plan, c, seed), false);
                      forge::RunResult b = forge::run(view.paths(), view.runner(), "map_ground", chunk_spec(r.plan, n->second, seed), false);
                      if (a.status != 0 || b.status != 0) continue;
                      const auto ca = columns(a.output), cb = columns(b.output);
                      for (const auto &[cell, v] : ca) {
                        if (!v.second) continue;
                        auto o = cb.find(cell);
                        if (o == cb.end() || o->second.second) continue;
                        ++cells;
                        if (o->second.first[0][0] != v.first[0][0] && o->second.first.back()[0] != v.first.back()[0]) ++differ;
                      }
                      ++pairs;
                    }
                    out["seams"] = {{"pairs", pairs}, {"apron_cells", cells}, {"differ", differ}};
                  }
                  if (args.value("export_far", false)) {
                    json file = {{"doc", "The far land of map " + view.name() + ", from its plan (a sample for the map_far pipeline)."},
                                 {"state", "draft"}, {"notes", json::array()}, {"created_at", core::now_iso()},
                                 {"value", far_spec(view.paths(), view.file(), r.plan, "summer", 2.0f)}};
                    core::write_json(view.paths().workspace + "/config/map_fars/" + view.name() + ".json", file);
                    out["exported_far"] = view.name();
                  }
                  if (args.contains("export_chunk")) {
                    const int q = args["export_chunk"][0].get<int>(), cr = args["export_chunk"][1].get<int>();
                    const int S = r.plan.value("chunk_size", 64);
                    for (size_t c = 0; c < r.plan["chunk_q"].size(); ++c) {
                      const int cq0 = r.plan["chunk_q"][c].get<int>(), cr0 = r.plan["chunk_r"][c].get<int>();
                      if (q < cq0 || q >= cq0 + S || cr < cr0 || cr >= cr0 + S) continue;
                      // Entry names become identifiers in the native runtime: a minus is spelt m.
                      auto num = [](int v) { return v < 0 ? "m" + std::to_string(-v) : std::to_string(v); };
                      const std::string entry = view.name() + "_" + num(cq0) + "_" + num(cr0);
                      json file = {{"doc", "A chunk of map " + view.name() + " at (" + std::to_string(cq0) + ", " + std::to_string(cr0) + "), cut from its plan."},
                                   {"state", "draft"}, {"notes", json::array()}, {"created_at", core::now_iso()},
                                   {"value", chunk_spec(r.plan, c, view.file().value("seed", 1u))}};
                      core::write_json(view.paths().workspace + "/config/map_chunks/" + entry + ".json", file);
                      out["exported"] = entry;
                    }
                  }
                  if (args.value("hull", false)) out["hull"] = r.plan.value("hull", json::object());
                  out["milliseconds"] = r.milliseconds;
                  out["fuel"] = r.fuel;
                  if (r.native_checked) out["native_matches_wasm"] = r.native_match;
                  return mcp::Result::json(out);
                }});
  registry.add({"map_look",
                "Show the map live in the workspace (the native runtime, streaming round the camera) and fly the camera to a "
                "place: x, z in metres (y: the height above the ground there, default 2), heading (degrees from north toward "
                "east), pitch (degrees, negative looks down). play: true starts walking there. Then ui_screenshot shows it.",
                mcp::schema({{"map", {{"type", "string"}}},
                             {"x", {{"type", "number"}}},
                             {"z", {{"type", "number"}}},
                             {"y", {{"type", "number"}}},
                             {"heading", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"play", {{"type", "boolean"}}},
                             {"probe", {{"type", "boolean"}, {"description", "report what the walker's world holds under x, z (in the next call's result)"}}},
                             {"grass", {{"type", "boolean"}, {"description", "grass fields on the nature (default on); the live view rebuilds"}}}},
                            {"x", "z"}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  View::Look l;
                  l.eye[0] = args.value("x", 0.0f);
                  l.eye[1] = args.value("y", 2.0f);
                  l.eye[2] = args.value("z", 0.0f);
                  l.heading = args.value("heading", 0.0f) * 0.0174533f;
                  l.pitch = args.value("pitch", -10.0f) * 0.0174533f;
                  l.play = args.value("play", false);
                  l.grass = args.value("grass", true);
                  l.probe = args.value("probe", false);
                  const std::string last = view.probe;
                  view.request_look(l);
                  return mcp::Result::json({{"map", view.name()}, {"looking", true}, {"last_probe", last}});
                }});
  registry.add({"map_video",
                "Record a video of the map live (the native runtime, as a game runs it) into qa/videos/ as H.264 MP4. The map "
                "must be selected in the workspace (workspace_select map:<name>), live. tour: sites (an orbit over each site, "
                "all or those named in sites, flying between them), road (along the link at index link, at a rider's eye), "
                "or keys ([{x, z, y above the ground, heading, pitch in degrees, seconds from the key before}]). Every frame "
                "waits for the ground, nature, and sites round it to be built (at most max_wait_s), so the video is smooth "
                "whatever the frame rate. Blocks until done (wait: false returns at once; status: true reports progress; "
                "cancel: true stops and keeps what was recorded).",
                mcp::schema({{"map", {{"type", "string"}}},
                             {"tour", {{"type", "string"}, {"enum", {"sites", "road", "keys"}}}},
                             {"sites", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"link", {{"type", "integer"}}},
                             {"keys", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                             {"orbit_s", {{"type", "number"}, {"description", "seconds over each site (default 8)"}}},
                             {"speed", {{"type", "number"}, {"description", "m/s between sites (default 90) or along a road (default 12)"}}},
                             {"sweep_deg", {{"type", "number"}, {"description", "how far each orbit goes round (default 100)"}}},
                             {"width", {{"type", "integer"}}},
                             {"height", {{"type", "integer"}}},
                             {"fps", {{"type", "integer"}}},
                             {"grass", {{"type", "boolean"}}},
                             {"max_wait_s", {{"type", "number"}}},
                             {"reach", {{"type", "number"}, {"description", "times the streaming radii while recording (default 1.8 for sites and keys, 1 for a road): forests and towns farther out"}}},
                             {"wait", {{"type", "boolean"}}},
                             {"status", {{"type", "boolean"}}},
                             {"cancel", {{"type", "boolean"}}}},
                            {}),
                [&view](const json &args) {
                  auto report = [](const View::VideoStatus &st) {
                    json o = {{"running", st.running}, {"done", st.done}, {"frames", st.frames}, {"total", st.total},
                              {"seconds", st.seconds}, {"wall_s", st.wall_s}, {"frames_before_settled", st.waited}};
                    if (!st.path.empty()) o["path"] = st.path;
                    if (!st.error.empty()) o["error"] = st.error;
                    return o;
                  };
                  if (args.value("status", false)) return mcp::Result::json(report(view.video_status()));
                  if (args.value("cancel", false)) {
                    view.cancel_video();
                    return mcp::Result::json(report(view.video_status()));
                  }
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  View::Video v;
                  v.mode = args.value("tour", std::string("sites"));
                  if (args.contains("sites"))
                    for (const auto &n : args["sites"]) v.sites.push_back(n.get<std::string>());
                  v.link = args.value("link", 0);
                  v.keys = args.value("keys", json::array());
                  v.tour.orbit_s = args.value("orbit_s", 8.0f);
                  v.tour.speed = args.value("speed", v.mode == "road" ? 12.0f : 90.0f);
                  v.tour.sweep_deg = args.value("sweep_deg", 100.0f);
                  v.width = std::clamp(args.value("width", 1280), 160, 3840);
                  v.height = std::clamp(args.value("height", 720), 120, 2160);
                  v.fps = std::clamp(args.value("fps", 30), 10, 60);
                  v.grass = args.value("grass", true);
                  v.max_wait_s = std::clamp(args.value("max_wait_s", 4.0), 0.0, 60.0);
                  v.reach = std::clamp(args.value("reach", v.mode == "road" ? 1.0f : 1.8f), 0.5f, 4.0f);
                  const uint64_t serial = view.request_video(v);
                  if (!args.value("wait", true)) return mcp::Result::json({{"started", true}});
                  // Wait for the live view to take it, then for it to finish.
                  const auto start = std::chrono::steady_clock::now();
                  for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    const View::VideoStatus st = view.video_status();
                    if (st.serial != serial) return mcp::Result::error("another video replaced this one");
                    if (st.done) return st.error.empty() ? mcp::Result::json(report(st)) : mcp::Result::error(st.error);
                    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    if (!st.running && waited > 120)
                      return mcp::Result::error("the live view didn't start the video: select the map (workspace_select map:" + view.name() +
                                                ") with Live on");
                    if (waited > 3600) return mcp::Result::json(report(st));
                  }
                }});
  registry.add({"map_view",
                "Draw a map's atlas: the plan from above, north up, parcels by kind and shaded by the land, water, roads, "
                "rivers, sites with their names, a 250 m grid (bolder through the origin), and a 500 m scale bar.",
                mcp::schema({{"map", {{"type", "string"}}},
                             {"size", {{"type", "integer"}}},
                             {"labels", {{"type", "boolean"}}},
                             {"chunks", {{"type", "boolean"}}}},
                            {}),
                [&view](const json &args) {
                  std::string error;
                  if (!ensure(view, args, error)) return mcp::Result::error(error);
                  PlanRun r = view.plan(false);
                  if (!r.ok) return mcp::Result::error(r.error);
                  AtlasOptions o;
                  o.size = args.value("size", 1024);
                  o.labels = args.value("labels", true);
                  o.chunks = args.value("chunks", false);
                  return image_result(atlas(view.file(), r.plan, o), view.name() + " " + report(r.plan).dump());
                }});
}

}  // namespace map
