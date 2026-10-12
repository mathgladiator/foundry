#include "plot/plotter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace plot {

const std::vector<std::string> &grounds() {
  static const std::vector<std::string> g = {"soil", "water", "rock", "sand", "path"};
  return g;
}

std::string Kind::label() const {
  std::string s = pipeline;
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.value().is_string()) s += " " + it.value().get<std::string>();
  return s;
}

uint32_t footprint_cells_for(float r) {
  if (r <= 0) return 0;
  return r <= 0.766f ? 1u : (r <= 1.9f ? 7u : (r <= 3.51f ? 19u : 37u));
}

std::vector<std::string> names(const forge::Paths &paths) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(paths.workspace + "/plotters", ec))
    if (e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

uint32_t ground_mask(const nlohmann::json &list, std::string &error) {
  uint32_t mask = 0;
  if (!list.is_array()) return 0;
  for (const auto &g : list) {
    auto it = std::find(grounds().begin(), grounds().end(), g.get<std::string>());
    if (it == grounds().end()) {
      error = "unknown ground " + g.get<std::string>();
      return 0;
    }
    mask |= 1u << (it - grounds().begin());
  }
  return mask;
}

// The kind's footprint, measured: its pipeline at the largest age (the last value of an `age`
// enum), from the pipeline's footprint fields, or else from what its solid and soft collision
// reaches at the ground. Query-only things (flowers) have none.
bool measure(pipeline::Runner &runner, const pipeline::Definition &def, const Kind &k, float &radius, std::string &error) {
  nlohmann::json values = k.params;
  const pipeline::Param *age = def.param("age");
  if (age && age->type == "enum" && !age->values.empty()) values["age"] = age->values.back();
  values["seed"] = 1;
  auto res = runner.run(def, values, false, false, false);
  if (!res.ok) {
    error = res.error;
    return false;
  }
  const pipeline::Frame &f = res.frame;
  if (f.footprint_radius > 0) {
    radius = f.footprint_radius;
    return true;
  }
  radius = 0;
  if (f.collision)
    for (const auto &s : f.collision->surfaces) {
      if (s.purpose > 1) continue;  // query and sensor bodies stand on nothing
      for (size_t i = 0; i + 2 < s.triangles.size(); i += 3)
        if (s.triangles[i + 1] < 0.5f) radius = std::max(radius, std::hypot(s.triangles[i], s.triangles[i + 2]));
    }
  return true;
}

}  // namespace

bool load(const forge::Paths &paths, pipeline::Runner &runner, const std::string &name, Plotter &out, std::string &error) {
  const std::string path = paths.workspace + "/plotters/" + name + ".json";
  nlohmann::json j = core::read_json(path);
  if (!j.is_object()) {
    error = "no plotter " + name + " (plotters/" + name + ".json)";
    return false;
  }
  out = Plotter{};
  out.name = name;
  out.file = j;
  out.system = j.value("system", "");
  out.doc = j.value("doc", "");
  out.state = j.value("state", "draft");
  out.distinct_seeds = std::max(1u, j.value("distinct_seeds", 6u));
  if (out.system.empty()) {
    error = "plotter " + name + " names no plot system";
    return false;
  }
  const nlohmann::json cfg = j.value("config", nlohmann::json::object());
  nlohmann::json layers_out = nlohmann::json::array();
  std::map<std::string, pipeline::Definition> defs;
  bool wrote = false;
  try {
    const auto &layers = cfg.at("layers");
    std::vector<std::string> layer_names;
    for (const auto &L : layers) layer_names.push_back(L.at("name").get<std::string>());
    out.layers = layer_names;
    auto layer_index = [&](const std::string &n) -> uint32_t {
      auto it = std::find(layer_names.begin(), layer_names.end(), n);
      if (it == layer_names.end()) throw std::runtime_error("no layer " + n);
      return static_cast<uint32_t>(it - layer_names.begin());
    };
    for (size_t li = 0; li < layers.size(); ++li) {
      const auto &L = layers[li];
      nlohmann::json lo;
      lo["name"] = layer_names[li];
      lo["spacing_m"] = L.at("spacing_m").get<float>();
      lo["density"] = L.at("density").get<float>();
      const auto grove = L.value("grove", nlohmann::json::array({1.0, 1.0}));
      lo["grove_low"] = grove[0].get<float>();
      lo["grove_high"] = grove[1].get<float>();
      const auto edge = L.value("edge", nlohmann::json::object());
      lo["edge_low"] = edge.value("low", 1.0f);
      lo["edge_cells"] = edge.value("cells", 0u);
      lo["ground"] = ground_mask(L.value("ground", nlohmann::json::array()), error);
      if (!error.empty()) return false;
      lo["max_height_delta_m"] = L.value("max_height_delta_m", 0.5f);
      nlohmann::json keep = nlohmann::json::array(), prefer = nlohmann::json::array();
      const nlohmann::json keep_in = L.value("keep_clear", nlohmann::json::object());
      const nlohmann::json prefer_in = L.value("prefer_near", nlohmann::json::object());
      for (auto it = keep_in.begin(); it != keep_in.end(); ++it)
        keep.push_back({{"layer", layer_index(it.key())}, {"metres", it.value().get<float>()}});
      for (auto it = prefer_in.begin(); it != prefer_in.end(); ++it)
        prefer.push_back({{"layer", layer_index(it.key())},
                          {"within_m", it.value().value("within_m", 3.0f)},
                          {"boost", it.value().value("boost", 2.0f)}});
      lo["keep_clear"] = keep;
      lo["prefer_near"] = prefer;
      lo["regrows"] = L.value("regrows", true);
      for (const char *list : {"kinds", "regrow_kinds"}) {
        nlohmann::json kinds_out = nlohmann::json::array();
        nlohmann::json src = L.value(list, nlohmann::json::array());
        for (size_t ki = 0; ki < src.size(); ++ki) {
          nlohmann::json &kj = j["config"]["layers"][li][list][ki];
          Kind k;
          k.id = static_cast<uint32_t>(out.kinds.size());
          k.layer = layer_names[li];
          k.regrow_only = std::string(list) == "regrow_kinds";
          k.pipeline = kj.at("pipeline").get<std::string>();
          k.params = kj.value("params", nlohmann::json::object());
          k.vary = kj.value("vary", nlohmann::json::object());
          k.on_regrow = kj.value("on_regrow", nlohmann::json::object());
          k.weight = kj.value("weight", 1.0f);
          k.max_share = kj.value("max_share", 1.0f);
          if (!defs.count(k.pipeline)) {
            pipeline::Definition d;
            if (!pipeline::load(paths, k.pipeline, d, error)) return false;
            defs[k.pipeline] = d;
          }
          const pipeline::Definition &def = defs[k.pipeline];
          // Every parameter the kind names must be its pipeline's, with a value it takes.
          for (const nlohmann::json *group : {&k.params, &k.on_regrow})
            for (auto it = group->begin(); it != group->end(); ++it) {
              const pipeline::Param *p = def.param(it.key());
              if (!p) throw std::runtime_error(k.pipeline + " has no parameter " + it.key());
              if (!p->values.empty() && it.value().is_string() &&
                  std::find(p->values.begin(), p->values.end(), it.value().get<std::string>()) == p->values.end())
                throw std::runtime_error(k.pipeline + "." + it.key() + " has no value " + it.value().get<std::string>());
            }
          for (auto it = k.vary.begin(); it != k.vary.end(); ++it) {
            const pipeline::Param *p = def.param(it.key());
            if (!p) throw std::runtime_error(k.pipeline + " has no parameter " + it.key());
            for (auto v = it.value().begin(); v != it.value().end(); ++v)
              if (std::find(p->values.begin(), p->values.end(), v.key()) == p->values.end())
                throw std::runtime_error(k.pipeline + "." + it.key() + " has no value " + v.key());
          }
          if (!kj.contains("footprint_m")) {
            float r = 0;
            std::string err;
            if (!measure(runner, def, k, r, err)) {
              error = "measuring " + k.label() + ": " + err;
              return false;
            }
            kj["footprint_m"] = std::round(r * 1000) / 1000;
            kj["footprint_cells"] = footprint_cells_for(r);
            wrote = true;
            core::log().add("plot", name + ": measured " + k.label() + "'s footprint: " +
                                        std::to_string(kj["footprint_cells"].get<uint32_t>()) + " cells");
          }
          k.footprint_m = kj.value("footprint_m", 0.0f);
          k.footprint_cells = kj.value("footprint_cells", footprint_cells_for(k.footprint_m));
          std::string gerr;
          const uint32_t gm = ground_mask(kj.value("ground", nlohmann::json::array()), gerr);
          if (!gerr.empty()) throw std::runtime_error(gerr);
          kinds_out.push_back({{"id", k.id},
                               {"weight", k.weight},
                               {"footprint_m", k.footprint_m},
                               {"footprint_cells", k.footprint_cells},
                               {"max_share", k.max_share},
                               {"ground", gm}});
          // Shared parameters (gathered after every kind is read, below).
          for (const auto &p : def.params) {
            if (p.name == "seed" || p.type == "config" || p.type == "event" || p.type == "time") continue;
            if (p.name == "detail" || p.name == "stump" || p.name == "fruit" || p.name.find("break") != std::string::npos) continue;
            if (!out.shared.contains(p.name)) out.shared[p.name] = p.fallback;
          }
          out.kinds.push_back(k);
        }
        lo[list] = kinds_out;
      }
      layers_out.push_back(lo);
    }
  } catch (const std::exception &e) {
    error = std::string("bad plotter ") + name + ": " + e.what();
    return false;
  }
  // A parameter any kind fixes, varies, or sets on regrowth is the plotter's, never shared.
  for (const auto &k : out.kinds)
    for (const nlohmann::json *g : {&k.params, &k.vary, &k.on_regrow})
      for (auto it = g->begin(); it != g->end(); ++it) out.shared.erase(it.key());
  out.config = {{"grove_scale_cells", cfg.value("grove_scale_cells", 8.0f)},
                {"grove_octaves", cfg.value("grove_octaves", 2u)},
                {"layers", layers_out}};
  if (wrote) {
    out.file = j;
    core::write_json(path, j);
  }
  return true;
}

nlohmann::json values_for(const Plotter &p, const Kind &k, uint32_t seed, bool regrown, const nlohmann::json &shared) {
  nlohmann::json v = k.params;
  for (auto it = shared.begin(); it != shared.end(); ++it)
    if (!v.contains(it.key())) v[it.key()] = it.value();
  uint32_t h = hash32(seed ^ 0x51ed270bu);
  for (auto it = k.vary.begin(); it != k.vary.end(); ++it) {
    float total = 0;
    for (auto w = it.value().begin(); w != it.value().end(); ++w) total += w.value().get<float>();
    h = hash32(h + 0x9e3779b9u);
    float pick = static_cast<float>(h >> 8) / 16777216.0f * total;
    std::string chosen;
    for (auto w = it.value().begin(); w != it.value().end(); ++w) {
      chosen = w.key();
      if (pick < w.value().get<float>()) break;
      pick -= w.value().get<float>();
    }
    v[it.key()] = chosen;
  }
  if (regrown)
    for (auto it = k.on_regrow.begin(); it != k.on_regrow.end(); ++it) v[it.key()] = it.value();
  v["seed"] = 1 + (seed % p.distinct_seeds);
  return v;
}

}  // namespace plot
