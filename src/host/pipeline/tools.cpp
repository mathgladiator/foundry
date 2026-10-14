// MCP tools for pipelines: inspect and drive the live instance the workspace
// shows, render a configuration, and render a contact sheet across two
// parameters (species x season, age x seed, ...) at one shared scale.
#include "pipeline/tools.h"

#include "pipeline/compose.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "assets/view.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"
#include "pipeline/config.h"
#include "pipeline/flow.h"
#include "sky/observatory.h"
#include "json/registry.h"
#include "viewport/image.h"

namespace pipeline {

namespace {

using nlohmann::json;

json stage_json(const std::vector<StageRun> &stages) {
  json out = json::array();
  for (const auto &s : stages) {
    json j = {{"stage", s.id}, {"system", s.system}, {"cached", s.cached}, {"output_kb", s.output_bytes / 1024}};
    if (!s.cached) {
      j["milliseconds"] = s.milliseconds;
      j["fuel"] = s.fuel;
    }
    if (s.native_checked) j["native_matches_wasm"] = s.native_match;
    if (!s.error.empty()) j["error"] = s.error;
    out.push_back(j);
  }
  return out;
}

json frame_json(const Frame &f) {
  if (!f.mesh) return json::object();
  json out = {{"triangles", f.mesh->triangles()},
              {"vertices", f.mesh->positions.size()},
              {"height_m", f.height},
              {"bones", f.bones},
              {"settled", f.settled}};
  if (f.conformance_radius > 0)
    out["conformance"] = {{"radius_m", f.conformance_radius}, {"height_m", f.conformance_height}};
  if (f.has_box)
    out["conformance_box"] = {{"min", {f.box_min.x, f.box_min.y, f.box_min.z}}, {"max", {f.box_max.x, f.box_max.y, f.box_max.z}}};
  if (!f.facts.empty()) out["facts"] = f.facts;
  return out;
}

viewport::Camera camera_from(const json &args) {
  viewport::Camera cam;
  cam.view = viewport::parse_view(args.value("view", "orbit"));
  cam.yaw = args.value("yaw", cam.yaw);
  cam.pitch = args.value("pitch", 0.15f);
  cam.zoom = args.value("zoom", cam.zoom);
  cam.quarter = args.value("quarter", 0);
  cam.distance = args.value("distance", 0.0f);
  if (args.contains("eye") && args["eye"].is_array() && args["eye"].size() == 3) {
    // Standing at an eye (a player inside a boundary's region): heading from north (-z) toward
    // east (+x) and elevation, in degrees.
    cam.free = true;
    for (int k = 0; k < 3; ++k) cam.eye[k] = args["eye"][k].get<float>();
    cam.azimuth = args.value("heading", 0.0f) * 0.0174533f;
    cam.elevation = args.value("elevation", 0.0f) * 0.0174533f;
    cam.fov = args.value("fov", 63.0f) * 0.0174533f;
  }
  return cam;
}

// The highest point of a mesh straight under (x, z), or false if nothing is there: an eye can stand
// on the ground (pipeline_view's on_ground).
bool ground_under(const runtime::Mesh &m, float x, float z, float &y) {
  bool hit = false;
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const auto &a = m.positions[m.indices[i]], &b = m.positions[m.indices[i + 1]], &c = m.positions[m.indices[i + 2]];
    const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::fabs(d) < 1e-9f) continue;
    const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
    const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
    if (u < 0 || v < 0 || u + v > 1) continue;
    const float h = u * a.y + v * b.y + (1 - u - v) * c.y;
    if (!hit || h > y) y = h;
    hit = true;
  }
  return hit;
}

assets::ViewContent content_of(const Frame &f, const std::string &label) {
  assets::ViewContent v;
  v.label = label;
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
  if (f.collision) physics::wireframe(*f.collision, v.collision);
  return v;
}

mcp::Result image_result(const viewport::Image &img, const std::string &text) {
  auto png = viewport::encode_png(img);
  mcp::Result r;
  r.content.push_back({"text", text, "", ""});
  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
  return r;
}

}  // namespace

void register_tools(mcp::Registry &registry, Runner &runner, Live &live, workspace::Workspace &ws) {
  registry.add({"pipeline_get",
                "Describe a pipeline (default: the one the workspace shows): its parameters with their choices and "
                "ranges, its stages and systems, the live instance's values, and the last build's stage stats.",
                mcp::schema({{"pipeline", {{"type", "string"}, {"description", "e.g. tree_v1"}}}}),
                [&live](const json &args) {
                  std::string name = args.value("pipeline", live.name()), error;
                  if (name.empty()) return mcp::Result::error("no pipeline shown; pass pipeline");
                  if (!live.show(name, error)) return mcp::Result::error(error);
                  Definition def = live.definition();
                  json params = json::array();
                  for (const auto &p : def.params) {
                    json j = {{"name", p.name}, {"type", p.type}, {"default", p.fallback}};
                    if (!p.values.empty()) j["values"] = p.values;
                    if (p.type == "u32" || p.type == "f32") j["range"] = {p.min, p.max};
                    if (!p.unit.empty()) j["unit"] = p.unit;
                    if (p.edge_bits > 0) j["edges"] = {{"bits", p.edge_bits}, {"values", p.edge_values}, {"order", "edge d at bits d * bits: directions 0, 300, 240, 180, 120, 60 degrees from +x toward +z"}};
                    if (!p.doc.empty()) j["doc"] = p.doc;
                    if (!p.when_param.empty())
                      j["when"] = {{"param", p.when_param}, {"field", p.when_field}, {"applies_in", p.when_on}};
                    j["update_only"] = def.is_update_param(p.name);
                    params.push_back(j);
                  }
                  json stages = json::array();
                  for (const auto &s : def.stages)
                    stages.push_back({{"id", s.id}, {"system", s.system}, {"phase", s.phase},
                                      {"in", s.input_channel}, {"out", s.output_channel}, {"input", s.input}});
                  json out = {{"pipeline", def.name}, {"doc", def.doc}, {"params", params}, {"stages", stages},
                              {"values", live.values()}, {"last_build", stage_json(live.build_stages())},
                              {"flow", analyze(def).to_json()}};
                  if (!live.error().empty()) out["error"] = live.error();
                  return mcp::Result::json(out);
                }});

  registry.add({"pipeline_flow",
                "How information flows through a pipeline and where caching holds: for each stage, the parameters "
                "that reach it (its cache key), what it reads upstream, and how many distinct outputs it can have; for "
                "each parameter, what reruns when it changes. With probe, also varies each parameter from values (or "
                "the live instance's) and reports which stages' outputs actually changed: a declared dependency that "
                "changes nothing is a caching opportunity.",
                mcp::schema({{"pipeline", {{"type", "string"}}},
                             {"probe", {{"type", "boolean"}}},
                             {"samples", {{"type", "integer"}, {"description", "values tried per parameter (default 3)"}}},
                             {"values", {{"type", "object"}}}},
                            {"pipeline"}),
                [&runner, &live](const json &args) {
                  std::string name = args.at("pipeline"), error;
                  Definition def;
                  if (!load(runner.paths(), name, def, error)) return mcp::Result::error(error);
                  json out = analyze(def).to_json();
                  if (args.value("probe", false)) {
                    json base = live.name() == name ? live.values() : json::object();
                    const json overrides = args.value("values", json::object());
                    for (auto it = overrides.begin(); it != overrides.end(); ++it) base[it.key()] = it.value();
                    core::Job job("Probe: " + name);
                    out["probe"] = probe(runner, def, base, args.value("samples", 3));
                  }
                  return mcp::Result::json(out);
                }});

  registry.add({"config_get",
                "Read a config table (config/<table>/, e.g. tree_v1_genomes: named records of one channel that a "
                "pipeline picks from by name): without entry, its doc, channel, and entries; with entry, that entry's "
                "value, state, and notes. With schema, also every field's type, range, choices, and doc.",
                mcp::schema({{"table", {{"type", "string"}, {"description", "e.g. tree_v1_genomes"}}},
                             {"entry", {{"type", "string"}, {"description", "e.g. oak"}}},
                             {"schema", {{"type", "boolean"}}}},
                            {"table"}),
                [&runner](const json &args) {
                  const std::string table = args.at("table");
                  json t = config::table(runner.paths(), table);
                  if (t.is_null()) return mcp::Result::error("no config/" + table + "/table.json");
                  json out;
                  if (args.contains("entry")) {
                    const std::string entry = args["entry"];
                    out = config::entry(runner.paths(), table, entry);
                    if (out.is_null()) return mcp::Result::error("no config/" + table + "/" + entry + ".json");
                    out["entry"] = entry;
                  } else {
                    out = {{"doc", t.value("doc", "")}, {"entries", config::entries(runner.paths(), table)}};
                  }
                  out["table"] = table;
                  out["channel"] = t.value("channel", "");
                  if (args.value("schema", false)) out["schema"] = config::fields(t);
                  return mcp::Result::json(out);
                }});

  registry.add({"config_set",
                "Change a config entry's fields (values: {field: value}; enums by name), or create one: a new entry "
                "with from copies that entry first. Checks every value against the table's channel (types and ranges), "
                "saves it, rebuilds the live instance, and regenerates the native runtime, which compiles every entry "
                "in. Final entries can't change. reference links the entry to photos under reference/ (workspace-relative "
                "paths), which the viewport can show in a corner beside the live instance. Pair it with pipeline_view "
                "or pipeline_sheet to see the result.",
                mcp::schema({{"table", {{"type", "string"}, {"description", "e.g. tree_v1_genomes"}}},
                             {"entry", {{"type", "string"}, {"description", "e.g. teak"}}},
                             {"values", {{"type", "object"}}},
                             {"from", {{"type", "string"}, {"description", "an existing entry to copy, to create a new one"}}},
                             {"doc", {{"type", "string"}}},
                             {"reference", {{"type", "array"}, {"items", {{"type", "string"}}},
                                            {"description", "e.g. [\"reference/trees/pine/01.jpg\"]; replaces the list"}}}},
                            {"table", "entry"}),
                [&runner, &live, &ws](const json &args) {
                  const std::string table = args.at("table"), entry = args.at("entry");
                  const json t = config::table(runner.paths(), table);
                  if (t.is_null()) return mcp::Result::error("no config/" + table + "/table.json");
                  if (!core::valid_name(entry) || entry == "table") return mcp::Result::error("bad entry name");
                  json file = config::entry(runner.paths(), table, entry);
                  const bool created = file.is_null();
                  if (created) {
                    if (!args.contains("from")) return mcp::Result::error("no entry " + entry + "; pass from to create it");
                    file = config::entry(runner.paths(), table, args["from"].get<std::string>());
                    if (file.is_null()) return mcp::Result::error("no entry " + args["from"].get<std::string>() + " to copy");
                    file["state"] = "draft";
                    file["notes"] = json::array();
                    file["created_at"] = core::now_iso();
                    for (const char *k : {"approved_at", "approved_by", "rejected_reason", "reference"}) file.erase(k);
                  }
                  if (file.value("state", "") == "final") return mcp::Result::error(entry + " is final; create a new entry instead");
                  if (args.contains("doc")) file["doc"] = args["doc"];
                  if (args.contains("reference")) {
                    json list = json::array();
                    for (const auto &r : args["reference"]) {
                      const std::string path = r.is_string() ? r.get<std::string>() : "";
                      if (path.rfind("reference/", 0) != 0 || path.find("..") != std::string::npos ||
                          !std::filesystem::exists(runner.paths().workspace + "/" + path))
                        return mcp::Result::error("no reference photo " + path + " (paths start with reference/)");
                      list.push_back(path);
                    }
                    file["reference"] = list;
                  }
                  const json values = args.value("values", json::object());
                  std::string error;
                  if (!config::apply(t, file["value"], values, error)) return mcp::Result::error(error);
                  if (!core::write_json(config::entry_path(runner.paths(), table, entry), file))
                    return mcp::Result::error("can't write the entry");
                  ws.scan();
                  live.reload();
                  core::log().add("config", (created ? "created " : "changed ") + table + "/" + entry +
                                                (values.empty() ? "" : ": " + values.dump()));
                  // Config is compiled into the native runtime: keep its generated code current.
                  std::string gen_out;
                  const bool regenerated = core::run_process({"python3", runner.paths().root + "/tools/gen_pipelines.py",
                                                              "--workspace", runner.paths().workspace},
                                                             gen_out) == 0;
                  json out = {{"table", table}, {"entry", entry}, {"created", created}, {"value", file["value"]}};
                  if (!regenerated) out["warning"] = "tools/gen_pipelines.py failed: " + gen_out;
                  return mcp::Result::json(out);
                }});

  registry.add({"pipeline_set",
                "Set parameters on the live pipeline instance the workspace shows (the human sees the change), build "
                "it, and return each stage's time, fuel, and cache hit, plus the model's stats. Clocks and events set "
                "here stop running; list them in start to run them from now (e.g. start: [\"break_time\"] fires the event and "
                "breaks the tree). verify_native also runs each stage natively and compares the bytes.",
                mcp::schema({{"pipeline", {{"type", "string"}}},
                             {"values", {{"type", "object"}}},
                             {"start", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"verify_native", {{"type", "boolean"}}}},
                            {"pipeline"}),
                [&live, &ws](const json &args) {
                  std::string name = args.value("pipeline", ""), error;
                  if (!live.show(name, error)) return mcp::Result::error(error);
                  ws.select("pipeline:" + name);
                  if (args.contains("values")) live.set(args["values"]);
                  const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
                  for (const auto &p : args.value("start", json::array())) live.start(p.get<std::string>(), now);
                  Runner::Result r = live.build_now(args.value("verify_native", false));
                  json out = {{"values", live.values()}, {"stages", stage_json(r.stages)}, {"model", frame_json(r.frame)}};
                  if (!r.ok) {
                    out["error"] = r.error;
                    return mcp::Result{{{"text", out.dump(2), "", ""}}, true};
                  }
                  return mcp::Result::json(out);
                }});

  registry.add({"pipeline_view",
                "Render one configuration of a pipeline as a PNG without changing the live instance: values override "
                "the live values (e.g. {\"species\": \"oak\", \"season\": \"fall\", \"time\": 3.5}). Views as "
                "view_screenshot; frame (metres) fixes the scale instead of fitting the model.",
                mcp::schema({{"pipeline", {{"type", "string"}}},
                             {"values", {{"type", "object"}}},
                             {"view", {{"type", "string"}, {"enum", {"orbit", "front", "side", "top", "iso"}}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"zoom", {{"type", "number"}}},
                             {"distance", {{"type", "number"}}},
                             {"frame", {{"type", "number"}}},
                             {"silhouette", {{"type", "boolean"}}},
                             {"eye", {{"type", "array"}, {"description", "stand here (x, y, z metres) instead of orbiting: a player's view (a boundary's region)"}}},
                             {"heading", {{"type", "number"}, {"description", "with eye: degrees from north (-z) toward east (+x)"}}},
                             {"elevation", {{"type", "number"}, {"description", "with eye: degrees above the horizon"}}},
                             {"on_ground", {{"type", "boolean"}, {"description", "with eye: its y is above the model's ground there (1.7 stands a player on it)"}}},
                             {"fov", {{"type", "number"}, {"description", "with eye: vertical field of view, degrees (63)"}}},
                             {"overlays", {{"type", "boolean"}, {"description", "hero, hex, conformance, grid, collision (default: true, false with eye)"}}},
                             {"burning", {{"type", "number"}, {"description", "how much it burns (design/submit.md, Fire): 0 only its lit fires and chimneys, 1 its blaze points alight, up to 2"}}},
                             {"observatory", {{"type", "string"},
                                              {"description", "under this observatory's sky (design/observatory.md), or none; "
                                                              "default: what the pipeline's viewer is under"}}},
                             {"compose", {{"type", "object"},
                                          {"description", "what is composed round the model, where its draw asks (a terrain's boundary and plotter, "
                                                          "a boundary's figures): nature (bool), plotter (name), surround (bool), figures "
                                                          "(bool), surround_values (object). Default: all on; false turns all off"}}},
                             {"size", {{"type", "integer"}}}},
                            {"pipeline"}),
                [&runner, &live](const json &args) {
                  std::string name = args.value("pipeline", ""), error;
                  Definition def;
                  if (!load(runner.paths(), name, def, error)) return mcp::Result::error(error);
                  json values = live.name() == name ? live.values() : json::object();
                  const json overrides = args.value("values", json::object());
                  for (auto it = overrides.begin(); it != overrides.end(); ++it) values[it.key()] = it.value();
                  Runner::Result r = runner.run(def, values, false, true);
                  if (!r.ok) return mcp::Result::error(r.error);
                  viewport::Camera cam = camera_from(args);
                  cam.frame = args.value("frame", r.frame.frame_m);
                  if (cam.free && args.value("on_ground", false) && r.frame.mesh) {
                    float y = 0;
                    if (ground_under(*r.frame.mesh, cam.eye[0], cam.eye[2], y)) cam.eye[1] += y;
                  }
                  int size = std::clamp(args.value("size", 512), 64, 1024);
                  viewport::SkyScene sky_scene;
                  const std::string under = args.value("observatory", sky::observatories().view_of("pipeline:" + name));
                  const bool has_sky = under != "none" && sky::observatories().scene(under, sky_scene);
                  assets::ViewContent content = content_of(r.frame, name);
                  content.burning = args.value("burning", 0.0f);
                  json composed;
                  if (composes(def) && !(args.contains("compose") && args["compose"].is_boolean() && !args["compose"].get<bool>())) {
                    const json c = args.value("compose", json::object()).is_object() ? args.value("compose", json::object()) : json::object();
                    ComposeOptions o;
                    o.nature = c.value("nature", true);
                    o.plotter = c.value("plotter", "");
                    o.surround = c.value("surround", true);
                    o.figures = c.value("figures", true);
                    o.surround_values = c.value("surround_values", json::object());
                    Composite comp = compose(runner, def, r.frame, values, o);
                    if (comp.mesh) {
                      content.mesh = comp.mesh;
                      content.lines.insert(content.lines.end(), comp.guides.begin(), comp.guides.end());
                      content.collision.insert(content.collision.end(), comp.wire.begin(), comp.wire.end());
                      content.fires.insert(content.fires.end(), comp.fires.begin(), comp.fires.end());
                      content.own_ground = content.own_ground || comp.own_ground;
                    }
                    composed = comp.report;
                  }
                  // Overlays (hero, hex, conformance, grid, collision) by default, but not for a player's eye.
                  viewport::Overlays overlays;
                  if (!args.value("overlays", !cam.free)) {
                    overlays.conformance = overlays.collision = overlays.hero = overlays.hex = overlays.grid = false;
                    content.lines.clear();
                  }
                  viewport::Image img = assets::render_view(content, cam, overlays, args.value("silhouette", false), size, size,
                                                            0, has_sky ? &sky_scene : nullptr);
                  json v = runner.complete(def, values);
                  json facts = frame_json(r.frame);
                  if (!composed.is_null()) facts["composed"] = composed;
                  return image_result(img, name + " " + v.dump() + "\n" + facts.dump());
                }});

  registry.add({"pipeline_sheet",
                "Render a contact sheet: one cell per combination of a row parameter and a column parameter (e.g. "
                "rows species, cols season), all at one scale so sizes compare. Other values override the live "
                "instance's. Returns the PNG and a legend of rows, columns, and per-cell stats.",
                mcp::schema({{"pipeline", {{"type", "string"}}},
                             {"rows", {{"type", "string"}, {"description", "parameter name"}}},
                             {"row_values", {{"type", "array"}}},
                             {"cols", {{"type", "string"}, {"description", "parameter name"}}},
                             {"col_values", {{"type", "array"}}},
                             {"values", {{"type", "object"}}},
                             {"view", {{"type", "string"}, {"enum", {"orbit", "front", "side", "top", "iso"}}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"frame", {{"type", "number"}, {"description", "metres to frame; default: the tallest cell"}}},
                             {"cell", {{"type", "integer"}, {"description", "cell size in pixels, default 192"}}},
                             {"overlays", {{"type", "boolean"}, {"description", "hero, hex, conformance, grid (default true)"}}},
                             {"burning", {{"type", "number"}, {"description", "how much each cell burns (0 to 2)"}}}},
                            {"pipeline", "rows", "cols"}),
                [&runner, &live](const json &args) {
                  std::string name = args.value("pipeline", ""), error;
                  Definition def;
                  if (!load(runner.paths(), name, def, error)) return mcp::Result::error(error);
                  auto choices = [&](const std::string &param, const char *key) {
                    std::vector<json> out;
                    if (args.contains(key)) {
                      for (const auto &v : args[key]) out.push_back(v);
                      return out;
                    }
                    if (const Param *p = def.param(param))
                      for (const auto &v : p->values) out.push_back(v);
                    return out;
                  };
                  const std::string rp = args.value("rows", ""), cp = args.value("cols", "");
                  if (!def.param(rp) || !def.param(cp)) return mcp::Result::error("rows and cols must name parameters");
                  std::vector<json> rows = choices(rp, "row_values"), cols = choices(cp, "col_values");
                  if (rows.empty() || cols.empty() || rows.size() * cols.size() > 64)
                    return mcp::Result::error("give 1 to 64 cells (enum and config parameters list their own values)");
                  json base = live.name() == name ? live.values() : json::object();
                  const json overrides = args.value("values", json::object());
                  for (auto it = overrides.begin(); it != overrides.end(); ++it) base[it.key()] = it.value();
                  core::Job job("Contact sheet: " + name);
                  std::vector<Runner::Result> cells;
                  json legend = {{"rows", {{"param", rp}, {"values", rows}}}, {"cols", {{"param", cp}, {"values", cols}}}};
                  json stats = json::array();
                  float tallest = 0;
                  for (size_t r = 0; r < rows.size(); ++r)
                    for (size_t c = 0; c < cols.size(); ++c) {
                      json v = base;
                      v[rp] = rows[r];
                      v[cp] = cols[c];
                      cells.push_back(runner.run(def, v, false, false));
                      job.update(static_cast<float>(cells.size()) / static_cast<float>(rows.size() * cols.size()), "");
                      const auto &res = cells.back();
                      if (res.ok) {
                        tallest = std::max(tallest, res.frame.height);
                        stats.push_back({{"row", rows[r]}, {"col", cols[c]}, {"triangles", res.frame.mesh->triangles()},
                                         {"height_m", res.frame.height}});
                      } else {
                        stats.push_back({{"row", rows[r]}, {"col", cols[c]}, {"error", res.error}});
                      }
                    }
                  viewport::Camera cam = camera_from(args);
                  cam.frame = args.value("frame", tallest * 1.05f);
                  viewport::Overlays ov;
                  if (!args.value("overlays", true)) ov = {false, false, false, false};
                  const int cell = std::clamp(args.value("cell", 192), 64, 512);
                  const int gap = 4;
                  viewport::Image sheet;
                  sheet.width = static_cast<int>(cols.size()) * (cell + gap) + gap;
                  sheet.height = static_cast<int>(rows.size()) * (cell + gap) + gap;
                  sheet.rgba.assign(static_cast<size_t>(sheet.width) * sheet.height * 4, 18);
                  for (size_t k = 0; k < cells.size(); ++k) {
                    if (!cells[k].ok) continue;
                    assets::ViewContent content = content_of(cells[k].frame, name);
                    content.burning = args.value("burning", 0.0f);
                    viewport::Image img = assets::render_view(content, cam, ov, false, cell, cell);
                    int ox = gap + static_cast<int>(k % cols.size()) * (cell + gap);
                    int oy = gap + static_cast<int>(k / cols.size()) * (cell + gap);
                    for (int y = 0; y < img.height && y < cell; ++y)
                      std::copy(img.rgba.begin() + static_cast<size_t>(y) * img.width * 4,
                                img.rgba.begin() + static_cast<size_t>(y) * img.width * 4 + static_cast<size_t>(std::min(img.width, cell)) * 4,
                                sheet.rgba.begin() + (static_cast<size_t>(oy + y) * sheet.width + ox) * 4);
                  }
                  legend["cells"] = stats;
                  legend["frame_m"] = cam.frame;
                  legend["values"] = runner.complete(def, base);
                  core::log().add("pipeline", name + ": contact sheet " + rp + " x " + cp);
                  return image_result(sheet, legend.dump());
                }});
}

}  // namespace pipeline
