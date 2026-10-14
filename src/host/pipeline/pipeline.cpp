#include "pipeline/pipeline.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <filesystem>
#include <set>

#include "core/log.h"
#include "core/util.h"
#include "json/registry.h"
#include "pipeline/config.h"
#include "pipeline/terrain.h"

namespace fs = std::filesystem;

namespace pipeline {

// A decoded model: the rest mesh plus what skinning needs.
struct Model {
  runtime::Mesh rest;
  std::vector<uint32_t> bone_a, bone_b;
  std::vector<float> bone_w;
  size_t bones = 0;
  float radius = 0, conformance_height = 0, height = 0;  // radius: the conformance capsule's
  nlohmann::json facts;  // scalar fields, and the sizes of other arrays
  std::shared_ptr<const physics::Collision> collision;
  float footprint_radius = 0;
  uint32_t footprint_cells = 0;
  bool has_box = false;
  foundry::vec3 box_min{0, 0, 0}, box_max{0, 0, 0};
  std::vector<viewport::Line> guides;
  std::vector<int> lot_cells, wet_cells, door_cells;
  std::vector<Frame::Place> places;
  std::vector<runtime::FirePoint> fires;
  std::shared_ptr<const nlohmann::json> extras;  // the fields draw.nature, surround, and figures name
};

namespace {

constexpr size_t kCacheEntries = 4096;  // a plotter's region shows dozens of builds, four stages each
constexpr size_t kCacheBytes = size_t(1536) << 20;

uint64_t fnv64(const std::vector<uint8_t> &b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// Drawable channels follow a convention, not a type (as the native generator's do,
// tools/gen_pipelines.py): a model has positions and indices, and may have normals,
// colors (packed RGBA), and skinning (bone_a, bone_b, bone_w); a pose has matrices
// (column-major 4x4 per bone) and may say it has settled. Everything else the
// channel carries is reported as facts (a nested channel's scalars as "field.name").
const std::set<std::string> kMeshFields = {"positions", "normals", "colors", "indices", "bone_a", "bone_b", "bone_w",
                                           "collision"};

nlohmann::json facts_of(const nlohmann::json &j, const std::set<std::string> &skip) {
  nlohmann::json out = nlohmann::json::object();
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (skip.count(it.key())) continue;
    if (it.value().is_array()) out[it.key() + "_count"] = it.value().size();
    else if (!it.value().is_object()) out[it.key()] = it.value();
    else  // a nested channel's own scalars (a terrain's ground report), one level down
      for (auto f = it.value().begin(); f != it.value().end(); ++f)
        if (!f.value().is_array() && !f.value().is_object()) out[it.key() + "." + f.key()] = f.value();
  }
  return out;
}

bool unpack_json(const std::string &channel, const std::vector<uint8_t> &bytes, nlohmann::json &out, std::string &error) {
  const auto *info = foundry::json::find_channel(channel);
  if (!info) {
    error = "unknown channel " + channel;
    return false;
  }
  if (!info->to_json(bytes.data(), bytes.size(), out)) {
    error = "output doesn't unpack as " + channel;
    return false;
  }
  return true;
}

template <typename T>
std::vector<T> numbers(const nlohmann::json &j, const char *field) {
  std::vector<T> out;
  if (!j.contains(field)) return out;
  out.reserve(j[field].size());
  for (const auto &x : j[field]) out.push_back(x.get<T>());
  return out;
}

std::vector<foundry::vec3> vec3s(const nlohmann::json &j, const char *field) {
  std::vector<foundry::vec3> out;
  if (!j.contains(field)) return out;
  out.reserve(j[field].size());
  for (const auto &x : j[field]) out.push_back({x[0].get<float>(), x[1].get<float>(), x[2].get<float>()});
  return out;
}

// The outlines of groups of hex cells (pointy-top, circumradius 1 m): each group's border edges,
// coloured by its tone, and for tone 0 its inner edges too, faintly.
std::vector<viewport::Line> guide_lines(const nlohmann::json &cells, const nlohmann::json &tones) {
  std::vector<viewport::Line> out;
  if (!cells.is_array() || !tones.is_array()) return out;
  constexpr float kS3 = 1.7320508f, kPi = 3.14159265f;
  std::map<int, std::set<std::pair<int, int>>> groups;
  for (size_t i = 0; i + 2 < cells.size(); i += 3) groups[cells[i].get<int>()].insert({cells[i + 1].get<int>(), cells[i + 2].get<int>()});
  // Fainter groups first, so the brightest draws on top.
  for (int tone = 2; tone >= 0; --tone)
    for (const auto &[g, set] : groups) {
      if (g < 0 || static_cast<size_t>(g) >= tones.size() || tones[g].get<int>() != tone) continue;
      const float lift = 0.015f + 0.01f * static_cast<float>(2 - tone);
      for (const auto &[q, r] : set) {
        const float x = kS3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)), z = 1.5f * static_cast<float>(r);
        for (int k = 0; k < 6; ++k) {
          const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k), a1 = a0 + kPi / 3.0f;
          const float mx = x + std::cos((a0 + a1) * 0.5f) * kS3, mz = z + std::sin((a0 + a1) * 0.5f) * kS3;
          const int nq = static_cast<int>(std::lround(kS3 / 3.0f * mx - mz / 3.0f)), nr = static_cast<int>(std::lround(2.0f / 3.0f * mz));
          const bool border = !set.count({nq, nr});
          if (!border && tone != 0) continue;
          viewport::Line l{{x + std::cos(a0), lift, z + std::sin(a0)}, {x + std::cos(a1), lift, z + std::sin(a1)}, 1, 1, 1, 1};
          if (tone == 0) {
            l.r = 1.0f, l.g = 0.82f, l.bl = 0.3f, l.alpha = border ? 0.95f : 0.22f;
          } else if (tone == 1) {
            l.r = 0.9f, l.g = 0.92f, l.bl = 1.0f, l.alpha = 0.6f;
          } else {
            l.r = 0.55f, l.g = 0.8f, l.bl = 1.0f, l.alpha = 0.42f;
          }
          out.push_back(l);
        }
      }
    }
  return out;
}

std::shared_ptr<const Model> decode_model(const Definition &def, const std::string &channel,
                                          const std::vector<uint8_t> &bytes, std::string &error) {
  nlohmann::json m;
  if (!unpack_json(channel, bytes, m, error)) return nullptr;
  if (!m.contains("positions") || !m.contains("indices")) {
    error = "the viewport can't draw " + channel + ": a drawable model needs positions and indices";
    return nullptr;
  }
  auto out = std::make_shared<Model>();
  out->rest.positions = vec3s(m, "positions");
  out->rest.normals = vec3s(m, "normals");
  out->rest.colors = numbers<uint32_t>(m, "colors");
  out->rest.indices = numbers<uint32_t>(m, "indices");
  out->bone_a = numbers<uint32_t>(m, "bone_a");
  out->bone_b = numbers<uint32_t>(m, "bone_b");
  out->bone_w = numbers<float>(m, "bone_w");
  size_t bones = 1;
  for (uint32_t b : out->bone_a) bones = std::max<size_t>(bones, b + 1);
  for (uint32_t b : out->bone_b) bones = std::max<size_t>(bones, b + 1);
  out->bones = bones;
  for (const auto &p : out->rest.positions) out->height = std::max(out->height, p.y);
  auto scalar = [&](const std::string &field) {
    return !field.empty() && m.contains(field) && m[field].is_number() ? m[field].get<float>() : 0.0f;
  };
  out->radius = scalar(def.conformance_radius);
  out->conformance_height = scalar(def.conformance_height);
  out->facts = facts_of(m, kMeshFields);
  {  // what the composer needs (draw.nature, draw.surround, draw.figures)
    auto extras = std::make_shared<nlohmann::json>(nlohmann::json::object());
    for (const std::string f : {def.nature.value("regions", ""), def.surround.value("hull", ""), def.figures.value("field", "")})
      if (!f.empty() && m.contains(f)) (*extras)[f] = m[f];
    if (!extras->empty()) out->extras = extras;
  }
  out->footprint_radius = scalar(def.footprint_radius);
  out->footprint_cells = static_cast<uint32_t>(scalar(def.footprint_cells));
  auto vec = [&](const std::string &field, foundry::vec3 &v) {
    if (field.empty() || !m.contains(field) || !m[field].is_array() || m[field].size() != 3) return false;
    v = {m[field][0].get<float>(), m[field][1].get<float>(), m[field][2].get<float>()};
    return true;
  };
  out->has_box = vec(def.conformance_box_min, out->box_min) && vec(def.conformance_box_max, out->box_max);
  if (!def.footprint_lot.empty()) out->lot_cells = numbers<int>(m, def.footprint_lot.c_str());
  if (!def.footprint_wet.empty()) out->wet_cells = numbers<int>(m, def.footprint_wet.c_str());
  if (!def.footprint_doors.empty()) out->door_cells = numbers<int>(m, def.footprint_doors.c_str());
  if (!def.places_positions.empty()) {
    const auto at = vec3s(m, def.places_positions.c_str());
    const auto yaw = numbers<float>(m, def.places_yaws.c_str()), size = numbers<float>(m, def.places_sizes.c_str());
    for (size_t i = 0; i < at.size(); ++i) out->places.push_back({at[i], i < yaw.size() ? yaw[i] : 0.0f, i < size.size() ? size[i] : 0.0f});
  }
  if (!def.fire_points.empty()) {
    const auto at = vec3s(m, def.fire_points.c_str());
    const auto cfg = numbers<uint32_t>(m, def.fire_configs.c_str()), lit = numbers<uint32_t>(m, def.fire_lit.c_str());
    for (size_t i = 0; i < at.size() && i < cfg.size(); ++i) {
      runtime::FirePoint f;
      f.pos[0] = at[i].x;
      f.pos[1] = at[i].y;
      f.pos[2] = at[i].z;
      f.config = cfg[i];
      f.lit = i < lit.size() ? lit[i] : runtime::FirePoint::kAlways;
      out->fires.push_back(f);
    }
    for (const auto &f : {def.fire_points, def.fire_configs, def.fire_lit}) out->facts.erase(f + "_count");
    out->facts["fires"] = out->fires.size();
  }
  if (!def.guide_cells.empty() && m.contains(def.guide_cells) && m.contains(def.guide_tones))
    out->guides = guide_lines(m[def.guide_cells], m[def.guide_tones]);
  if (!def.guide_lines.empty() && m.contains(def.guide_lines) && m.contains(def.guide_colors)) {
    const auto pts = vec3s(m, def.guide_lines.c_str());
    const auto cols = numbers<uint32_t>(m, def.guide_colors.c_str());
    for (size_t i = 0; i + 1 < pts.size() && i / 2 < cols.size(); i += 2) {
      const uint32_t c = cols[i / 2];
      out->guides.push_back({pts[i], pts[i + 1], static_cast<float>(c & 255u) / 255.0f, static_cast<float>((c >> 8) & 255u) / 255.0f,
                             static_cast<float>((c >> 16) & 255u) / 255.0f, 0.9f});
    }
  }
  for (const auto &f : {def.guide_cells, def.guide_tones, def.guide_lines, def.guide_colors})
    if (!f.empty()) out->facts.erase(f + "_count");
  if (!def.terrain.empty()) {
    // A terrain (draw.terrain): one cut through its quadtree, from a player's eye in the middle of
    // the region its guides outline.
    float ground_y = 0, sum = 0;
    size_t n = 0;
    for (const auto &g : out->guides) sum += g.a.y, ++n;
    if (n) ground_y = sum / static_cast<float>(n) - 0.05f;  // the guides float 5 cm over the cells
    flatten_terrain(m, def.terrain, ground_y, out->rest, out->facts);
    for (auto it = def.terrain.begin(); it != def.terrain.end(); ++it)
      if (it.value().is_string()) out->facts.erase(it.value().get<std::string>() + "_count");
  }
  // Collision (the physics_v1_body convention): checked in the toy Jolt world once per model.
  out->collision = physics::analyze(physics::bodies_from_json(m.value("collision", nlohmann::json::array())),
                                    def.conformance_radius.empty() ? 0.0f : out->radius, out->conformance_height,
                                    out->has_box ? &out->box_min : nullptr, out->has_box ? &out->box_max : nullptr);
  out->facts["collision"] = out->collision->report;
  return out;
}

bool decode_pose(const std::string &channel, const std::vector<uint8_t> &bytes, std::vector<float> &matrices,
                 bool &settled, nlohmann::json &facts) {
  nlohmann::json p;
  std::string error;
  if (!unpack_json(channel, bytes, p, error) || !p.contains("matrices")) return false;
  matrices = numbers<float>(p, "matrices");
  settled = p.value("settled", false);
  facts = facts_of(p, {"matrices", "settled"});
  return true;
}

}  // namespace

const Param *Definition::param(const std::string &n) const {
  for (const auto &p : params)
    if (p.name == n) return &p;
  return nullptr;
}

const Stage *Definition::stage(const std::string &id) const {
  for (const auto &s : stages)
    if (s.id == id) return &s;
  return nullptr;
}

namespace {
void collect_params(const nlohmann::json &b, std::vector<std::string> &out) {
  if (b.is_string()) {
    std::string s = b.get<std::string>();
    if (!s.empty() && s[0] == '$') out.push_back(s.substr(1));
  } else if (b.is_object() || b.is_array()) {
    for (const auto &v : b) collect_params(v, out);
  }
}
}  // namespace

bool Definition::is_update_param(const std::string &n) const {
  for (const auto &s : stages) {
    if (s.phase == "update") continue;
    std::vector<std::string> used;
    collect_params(s.input, used);
    if (std::find(used.begin(), used.end(), n) != used.end()) return false;
  }
  return true;
}

bool Definition::applies(const Param &p, const nlohmann::json &values) const {
  if (p.when_param.empty()) return true;
  const Param *gate = param(p.when_param);
  std::string v = values.is_object() && values.contains(p.when_param) && values[p.when_param].is_string()
                      ? values[p.when_param].get<std::string>()
                      : "";
  if (gate && std::find(gate->values.begin(), gate->values.end(), v) == gate->values.end())
    v = gate->fallback.is_string() ? gate->fallback.get<std::string>() : "";
  return std::find(p.when_on.begin(), p.when_on.end(), v) != p.when_on.end();
}

double Definition::combinations(const std::map<std::string, std::vector<std::string>> &choices) const {
  auto chosen = [&](const std::string &n) -> const std::vector<std::string> * {
    auto it = choices.find(n);
    return it == choices.end() ? nullptr : &it->second;
  };
  double n = 1;
  for (const auto &p : params) {
    const auto *c = chosen(p.name);
    if (!c) continue;
    bool gated = false;
    if (!p.when_param.empty() && chosen(p.when_param)) gated = true;
    bool gate = false;
    for (const auto &q : params)
      if (q.when_param == p.name && chosen(q.name)) gate = true;
    if (gated) continue;  // counted with its gate
    if (!gate) {
      n *= static_cast<double>(c->size());
      continue;
    }
    // A gate: per chosen entry, the gated parameters' choices where they apply, else one.
    double sum = 0;
    for (const auto &e : *c) {
      double per = 1;
      for (const auto &q : params) {
        if (q.when_param != p.name || !chosen(q.name)) continue;
        const bool on = std::find(q.when_on.begin(), q.when_on.end(), e) != q.when_on.end();
        per *= on ? static_cast<double>(chosen(q.name)->size()) : 1.0;
      }
      sum += per;
    }
    n *= sum;
  }
  return n;
}

nlohmann::json off_value(const Param &p) {
  if (p.type == "bool") return false;
  if (!p.values.empty()) return p.values.front();
  if (p.type == "u32") return static_cast<uint32_t>(p.min);
  return p.min;
}

bool load(const forge::Paths &paths, const std::string &name, Definition &def, std::string &error) {
  nlohmann::json j = core::read_json(paths.workspace + "/pipelines/" + name + ".json");
  if (!j.is_object()) {
    error = "no pipeline " + name + " (pipelines/" + name + ".json)";
    return false;
  }
  def = {};
  def.name = name;
  def.doc = j.value("doc", "");
  try {
    for (const auto &p : j.value("params", nlohmann::json::array())) {
      Param param;
      param.name = p.at("name").get<std::string>();
      param.type = p.at("type").get<std::string>();
      param.min = p.value("min", 0.0);
      param.max = p.value("max", 1.0);
      param.unit = p.value("unit", "");
      param.doc = p.value("doc", "");
      param.fallback = p.value("default", nlohmann::json());
      static const char *const kTypes[] = {"enum", "config", "bool", "u32", "f32", "time", "event"};
      if (std::find(std::begin(kTypes), std::end(kTypes), param.type) == std::end(kTypes)) {
        error = "parameter " + param.name + " has unknown type " + param.type;
        return false;
      }
      if (param.type == "enum") param.values = p.at("values").get<std::vector<std::string>>();
      if (param.type == "bool") param.values = {"false", "true"};  // enumerated like an enum by sheets and videos
      if (param.type == "config") {
        param.table = p.at("table").get<std::string>();
        if (config::table(paths, param.table).is_null()) {
          error = "parameter " + param.name + ": no config/" + param.table + "/table.json";
          return false;
        }
        param.values = config::entries(paths, param.table);
        if (param.values.empty()) {
          error = "config/" + param.table + " has no entries for parameter " + param.name;
          return false;
        }
      }
      if (p.contains("edges")) {
        param.edge_bits = p["edges"].value("bits", 0);
        param.edge_values = p["edges"].value("values", std::vector<std::string>());
      }
      if (p.contains("when")) {
        param.when_param = p["when"].at("param").get<std::string>();
        param.when_field = p["when"].at("field").get<std::string>();
      }
      def.params.push_back(param);
    }
    // Gates: the entries of a config parameter whose bool field is set.
    for (auto &param : def.params) {
      if (param.when_param.empty()) continue;
      const Param *gate = def.param(param.when_param);
      if (!gate || gate->type != "config") {
        error = "parameter " + param.name + ": when names " + param.when_param + ", which is not a config parameter";
        return false;
      }
      bool is_bool = false;
      for (const auto &f : config::fields(config::table(paths, gate->table)))
        if (f.value("name", "") == param.when_field && f.value("type", "") == "bool") is_bool = true;
      if (!is_bool) {
        error = "parameter " + param.name + ": config/" + gate->table + " has no bool field " + param.when_field;
        return false;
      }
      for (const auto &e : gate->values) {
        const nlohmann::json v = config::entry(paths, gate->table, e).value("value", nlohmann::json::object());
        if (v.value(param.when_field, false)) param.when_on.push_back(e);
      }
    }
    for (const auto &s : j.at("stages")) {
      Stage st;
      st.id = s.at("id").get<std::string>();
      st.system = s.at("system").get<std::string>();
      st.phase = s.value("phase", "build");
      st.input = s.at("input");
      nlohmann::json m = core::read_json(paths.root + "/src/systems/" + st.system + "/system.json");
      if (!m.is_object()) {
        error = "stage " + st.id + ": no system " + st.system;
        return false;
      }
      st.input_channel = m.value("input", "");
      st.output_channel = m.value("output", "");
      def.stages.push_back(st);
    }
    def.model_stage = j.at("draw").value("model", "");
    def.pose_stage = j.at("draw").value("pose", "");
    const nlohmann::json conf = j.at("draw").value("conformance", nlohmann::json::object());
    def.conformance_radius = conf.value("radius", "");
    def.conformance_height = conf.value("height", "");
    const nlohmann::json fp = j.at("draw").value("footprint", nlohmann::json::object());
    def.footprint_radius = fp.value("radius", "");
    def.footprint_cells = fp.value("cells", "");
    def.footprint_lot = fp.value("lot", "");
    def.footprint_wet = fp.value("wet", "");
    def.footprint_doors = fp.value("doors", "");
    const nlohmann::json places = j.at("draw").value("places", nlohmann::json::object());
    def.places_positions = places.value("positions", "");
    def.places_yaws = places.value("yaws", "");
    def.places_sizes = places.value("sizes", "");
    const nlohmann::json fires = j.at("draw").value("fires", nlohmann::json::object());
    def.fire_points = fires.value("points", "");
    def.fire_configs = fires.value("configs", "");
    def.fire_lit = fires.value("lit", "");
    const nlohmann::json box = conf.value("box", nlohmann::json::object());
    def.conformance_box_min = box.value("min", "");
    def.conformance_box_max = box.value("max", "");
    const nlohmann::json guides = j.at("draw").value("guides", nlohmann::json::object());
    def.guide_cells = guides.value("cells", "");
    def.guide_tones = guides.value("tones", "");
    def.guide_lines = guides.value("lines", "");
    def.guide_colors = guides.value("colors", "");
    def.frame_m = j.at("draw").value("frame_m", 0.0f);
    def.terrain = j.at("draw").value("terrain", nlohmann::json::object());
    def.nature = j.at("draw").value("nature", nlohmann::json::object());
    def.surround = j.at("draw").value("surround", nlohmann::json::object());
    def.figures = j.at("draw").value("figures", nlohmann::json::object());
  } catch (const std::exception &e) {
    error = std::string("bad pipeline file: ") + e.what();
    return false;
  }
  if (!def.stage(def.model_stage)) {
    error = "draw.model names no stage";
    return false;
  }
  return true;
}

Runner::Runner(forge::Paths paths, wasm::Runner &wasm) : paths_(std::move(paths)), wasm_(wasm) {}

nlohmann::json Runner::complete(const Definition &def, const nlohmann::json &values) const {
  nlohmann::json out = nlohmann::json::object();
  for (const auto &p : def.params) {
    nlohmann::json v = values.is_object() && values.contains(p.name) ? values[p.name] : p.fallback;
    if (p.type == "config" && v.is_object()) {
      // An entry given inline (a planner's own network, say), as the table's channel would hold it.
      out[p.name] = v;
      continue;
    }
    if (p.type == "enum" || p.type == "config") {
      std::string s = v.is_string() ? v.get<std::string>() : "";
      if (std::find(p.values.begin(), p.values.end(), s) == p.values.end())
        s = p.fallback.is_string() && std::find(p.values.begin(), p.values.end(), p.fallback.get<std::string>()) != p.values.end()
                ? p.fallback.get<std::string>()
                : p.values.front();
      v = s;
    } else if (p.type == "bool") {
      if (v.is_string()) v = v.get<std::string>() == "true";
      else if (v.is_number()) v = v.get<double>() != 0;
      else if (!v.is_boolean()) v = p.fallback.is_boolean() && p.fallback.get<bool>();
    } else if (p.type == "u32") {
      double d = v.is_number() ? v.get<double>() : 0;
      v = static_cast<uint32_t>(std::clamp(d, p.min, p.max));
    } else {
      v = v.is_number() ? v.get<double>() : (p.type == "event" ? -1.0 : p.type == "time" ? 0.0 : p.min);
    }
    out[p.name] = v;
  }
  return out;
}

int64_t Runner::module_time(const std::string &system) {
  struct stat st;
  if (stat(forge::wasm_path(paths_, system).c_str(), &st) != 0) return 0;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
}

std::shared_ptr<const nlohmann::json> Runner::json_of(const std::string &key, const std::string &channel) {
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return nullptr;
    if (it->second.json) return it->second.json;
    bytes = it->second.bytes;
  }
  const auto *info = foundry::json::find_channel(channel);
  auto j = std::make_shared<nlohmann::json>();
  if (!info || !info->to_json(bytes->data(), bytes->size(), *j)) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(key);
  if (it != cache_.end()) it->second.json = j;
  return j;
}

void Runner::remember(const std::string &key, Entry e) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cache_.count(key)) return;
  cache_bytes_ += e.bytes->size();
  cache_[key] = std::move(e);
  order_.push_back(key);
  while (order_.size() > kCacheEntries || (cache_bytes_ > kCacheBytes && order_.size() > 1)) {
    auto it = cache_.find(order_.front());
    if (it != cache_.end()) {
      cache_bytes_ -= it->second.bytes->size();
      cache_.erase(it);
    }
    order_.erase(order_.begin());
  }
}

bool Runner::resolve(const Definition &def, const nlohmann::json &values, const nlohmann::json &binding,
                     std::map<std::string, std::string> &keys, nlohmann::json &out, std::string &error) {
  if (binding.is_object()) {
    out = nlohmann::json::object();
    for (auto it = binding.begin(); it != binding.end(); ++it)
      if (!resolve(def, values, it.value(), keys, out[it.key()], error)) return false;
    return true;
  }
  if (binding.is_array()) {
    out = nlohmann::json::array();
    for (const auto &v : binding) {
      nlohmann::json r;
      if (!resolve(def, values, v, keys, r, error)) return false;
      out.push_back(r);
    }
    return true;
  }
  if (binding.is_string()) {
    const std::string s = binding.get<std::string>();
    if (!s.empty() && s[0] == '$') {
      const Param *p = def.param(s.substr(1));
      if (!p) {
        error = "unknown parameter " + s;
        return false;
      }
      const nlohmann::json &v = values[p->name];
      if (p->type == "config" && v.is_object()) {
        out = v;
        return true;
      }
      if (p->type == "config") {
        nlohmann::json e = config::entry(paths_, p->table, v.get<std::string>());
        if (e.is_null()) {
          error = "can't read config/" + p->table + "/" + v.get<std::string>() + ".json";
          return false;
        }
        out = e["value"];
      } else {
        out = v;
      }
      return true;
    }
    if (!s.empty() && s[0] == '@') {
      std::string path = s.substr(1), id = path, rest;
      size_t dot = path.find('.');
      if (dot != std::string::npos) {
        id = path.substr(0, dot);
        rest = path.substr(dot + 1);
      }
      const Stage *st = def.stage(id);
      if (!st || !keys.count(id)) {
        error = "binding " + s + " names a stage that hasn't run";
        return false;
      }
      auto j = json_of(keys[id], st->output_channel);
      if (!j) {
        error = "stage " + id + "'s output is gone";
        return false;
      }
      const nlohmann::json *cur = j.get();
      while (!rest.empty()) {
        size_t d = rest.find('.');
        std::string field = rest.substr(0, d);
        if (!cur->is_object() || !cur->contains(field)) {
          error = "binding " + s + ": no field " + field;
          return false;
        }
        cur = &(*cur)[field];
        rest = d == std::string::npos ? "" : rest.substr(d + 1);
      }
      out = *cur;
      return true;
    }
  }
  out = binding;
  return true;
}

Runner::Result Runner::run(const Definition &def, const nlohmann::json &input_values, bool verify_native, bool log,
                           bool hash_outputs) {
  Result res;
  nlohmann::json values = complete(def, input_values);
  // A parameter that doesn't apply reads as off, so it shares the build either way.
  for (const auto &p : def.params)
    if (!def.applies(p, values)) values[p.name] = off_value(p);
  std::map<std::string, std::string> keys;
  for (const auto &st : def.stages) {
    StageRun sr;
    sr.id = st.id;
    sr.system = st.system;
    std::string key = st.system + "|" + std::to_string(module_time(st.system)) + "|";
    std::shared_ptr<const std::vector<uint8_t>> input;
    const std::string in = st.input.is_string() ? st.input.get<std::string>() : "";
    const bool pass = !in.empty() && in[0] == '@' && in.find('.') == std::string::npos &&
                      def.stage(in.substr(1)) && def.stage(in.substr(1))->output_channel == st.input_channel;
    if (pass) {
      const std::string up = in.substr(1);
      if (!keys.count(up)) {
        res.error = "stage " + st.id + " reads " + up + ", which hasn't run";
        return res;
      }
      key += "@" + keys[up];
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(keys[up]);
      if (it != cache_.end()) input = it->second.bytes;
    } else {
      nlohmann::json j;
      std::string error;
      if (!resolve(def, values, st.input, keys, j, error)) {
        res.error = "stage " + st.id + ": " + error;
        return res;
      }
      const auto *info = foundry::json::find_channel(st.input_channel);
      if (!info) {
        res.error = "stage " + st.id + ": unknown channel " + st.input_channel;
        return res;
      }
      auto bytes = std::make_shared<std::vector<uint8_t>>();
      try {
        info->from_json(j, *bytes);
      } catch (const std::exception &e) {
        res.error = "stage " + st.id + ": input doesn't match " + st.input_channel + ": " + e.what();
        return res;
      }
      key += hex(fnv64(*bytes));
      input = bytes;
    }
    bool hit;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hit = cache_.count(key) > 0;
      if (hit) sr.output_bytes = cache_[key].bytes->size();
    }
    if (hit && !verify_native) {
      sr.cached = true;
    } else {
      if (!input) {
        res.error = "stage " + st.id + ": its input fell out of the cache; run again";
        return res;
      }
      wasm::RunResult w = wasm_.run(forge::wasm_path(paths_, st.system), *input);
      sr.status = w.status;
      sr.error = w.error;
      sr.milliseconds = w.milliseconds;
      sr.fuel = w.fuel_used;
      sr.output_bytes = w.output.size();
      if (w.status != 0) {
        res.stages.push_back(sr);
        res.error = "stage " + st.id + " (" + st.system + ") failed: " + w.error;
        if (log) core::log().add("pipeline", def.name + ": " + res.error);
        return res;
      }
      if (verify_native) {
        sr.native_checked = true;
        std::string err;
        std::vector<uint8_t> native_out;
        int status = core::run_process({forge::native_path(paths_, st.system)}, err, input.get(), &native_out);
        sr.native_match = status == 0 && native_out == w.output;
        if (!sr.native_match) {
          core::log().add("native", def.name + ": " + st.system + " native output differs from WebAssembly");
          forge::dump_mismatch(st.system, *input, w.output, native_out);
        }
      }
      if (log && st.phase != "update") {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s: %s ran in %.1f ms (%zu KB out)", def.name.c_str(), st.system.c_str(),
                      w.milliseconds, w.output.size() / 1024);
        core::log().add("pipeline", buf);
      }
      if (!hit) remember(key, Entry{std::make_shared<const std::vector<uint8_t>>(std::move(w.output)), nullptr, nullptr});
    }
    keys[st.id] = key;
    if (hash_outputs) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(key);
      if (it != cache_.end()) res.output_hashes[st.id] = fnv64(*it->second.bytes);
    }
    res.stages.push_back(sr);
  }

  // The frame: the model, posed.
  const Stage *ms = def.stage(def.model_stage);
  Entry model_entry;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(keys[def.model_stage]);
    if (it == cache_.end()) {
      res.error = "the model fell out of the cache";
      return res;
    }
    model_entry = it->second;
    if (!model_versions_.count(keys[def.model_stage])) model_versions_[keys[def.model_stage]] = next_model_version_++;
    res.frame.model_version = model_versions_[keys[def.model_stage]];
  }
  std::shared_ptr<const Model> model = model_entry.model;
  if (!model) {
    std::string error;
    model = decode_model(def, ms->output_channel, *model_entry.bytes, error);
    if (!model) {
      res.error = error;
      return res;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(keys[def.model_stage]);
    if (it != cache_.end()) it->second.model = model;
  }
  std::vector<float> matrices;
  nlohmann::json pose_facts = nlohmann::json::object();
  if (const Stage *ps = def.stage(def.pose_stage)) {
    std::shared_ptr<const std::vector<uint8_t>> pose;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(keys[def.pose_stage]);
      if (it != cache_.end()) pose = it->second.bytes;
    }
    if (pose) decode_pose(ps->output_channel, *pose, matrices, res.frame.settled, pose_facts);
  }
  res.frame.mesh = skin(*model, matrices);
  res.frame.conformance_radius = model->radius;
  res.frame.conformance_height = model->conformance_height;
  res.frame.height = model->height;
  res.frame.bones = model->bones;
  res.frame.facts = model->facts;
  res.frame.collision = model->collision;
  res.frame.footprint_radius = model->footprint_radius;
  res.frame.footprint_cells = model->footprint_cells;
  res.frame.lot_cells = model->lot_cells;
  res.frame.wet_cells = model->wet_cells;
  res.frame.door_cells = model->door_cells;
  res.frame.places = model->places;
  res.frame.has_box = model->has_box;
  res.frame.box_min = model->box_min;
  res.frame.box_max = model->box_max;
  res.frame.guides = model->guides;
  res.frame.frame_m = def.frame_m;
  res.frame.terrain = !def.terrain.empty();
  res.frame.fires = model->fires;
  res.frame.extras = model->extras;
  for (const auto &p : def.params) {
    if (!values.contains(p.name) || !values[p.name].is_number()) continue;
    if (p.type == "time" && res.frame.time == 0) res.frame.time = values[p.name].get<float>();
    if (p.name == "wind_speed") res.frame.wind_speed = values[p.name].get<float>();
    if (p.name == "wind_heading") res.frame.wind_heading = values[p.name].get<float>();
  }
  res.frame.facts.update(pose_facts);
  res.ok = true;
  return res;
}

std::shared_ptr<const runtime::Mesh> skin(const Model &model, const std::vector<float> &m) {
  const size_t nb = m.size() / 16;
  if (nb == 0) return std::make_shared<runtime::Mesh>(model.rest);
  auto out = std::make_shared<runtime::Mesh>();
  out->colors = model.rest.colors;
  out->indices = model.rest.indices;
  const size_t n = model.rest.positions.size();
  out->positions.resize(n);
  out->normals.resize(model.rest.normals.size());
  auto xf = [&](uint32_t b, const foundry::vec3 &p, float w) {
    const float *a = &m[static_cast<size_t>(b) * 16];
    return foundry::vec3{(a[0] * p.x + a[4] * p.y + a[8] * p.z + w * a[12]),
                         (a[1] * p.x + a[5] * p.y + a[9] * p.z + w * a[13]),
                         (a[2] * p.x + a[6] * p.y + a[10] * p.z + w * a[14])};
  };
  for (size_t i = 0; i < n; ++i) {
    uint32_t ba = i < model.bone_a.size() && model.bone_a[i] < nb ? model.bone_a[i] : 0;
    uint32_t bb = i < model.bone_b.size() && model.bone_b[i] < nb ? model.bone_b[i] : ba;
    float w = i < model.bone_w.size() ? model.bone_w[i] : 0;
    const auto &p = model.rest.positions[i];
    foundry::vec3 pa = xf(ba, p, 1), pb = w > 0 ? xf(bb, p, 1) : pa;
    out->positions[i] = {pa.x + (pb.x - pa.x) * w, pa.y + (pb.y - pa.y) * w, pa.z + (pb.z - pa.z) * w};
    if (i < model.rest.normals.size()) {
      const auto &q = model.rest.normals[i];
      foundry::vec3 na = xf(ba, q, 0), nb2 = w > 0 ? xf(bb, q, 0) : na;
      out->normals[i] = {na.x + (nb2.x - na.x) * w, na.y + (nb2.y - na.y) * w, na.z + (nb2.z - na.z) * w};
    }
  }
  return out;
}

}  // namespace pipeline
