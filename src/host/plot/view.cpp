#include "plot/view.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <cmath>
#include <set>

#include "core/log.h"
#include "core/util.h"
#include "json/registry.h"
#include "physics/toy.h"

namespace plot {

namespace {

constexpr float kSqrt3 = 1.7320508f;

float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
float cell_z(int r) { return 1.5f * static_cast<float>(r); }
int hex_distance(int dq, int dr) { return (std::abs(dq) + std::abs(dr) + std::abs(dq + dr)) / 2; }

uint32_t pack(float r, float g, float b) {
  auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return c(r) | (c(g) << 8) | (c(b) << 16) | (255u << 24);
}

// The `set` bits a region gives, from the attributes it names (when it doesn't say).
nlohmann::json normalize(const nlohmann::json &regions) {
  nlohmann::json out = nlohmann::json::array();
  for (auto r : regions) {
    if (!r.contains("set")) {
      uint32_t set = 0;
      if (r.contains("ground")) set |= 1;
      if (r.contains("available")) set |= 2;
      if (r.contains("keep_clear_m")) set |= 4;
      if (r.contains("height")) set |= 8;
      if (r.contains("moisture")) set |= 16;
      r["set"] = set;
    }
    out.push_back(r);
  }
  return out;
}

nlohmann::json region(const char *shape, int q, int r, int radius, int priority) {
  return {{"shape", shape}, {"q", q}, {"r", r}, {"radius", radius}, {"priority", priority}};
}

void place(const runtime::Mesh &m, foundry::vec3 at, float yaw, runtime::Mesh &out) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const uint32_t base = static_cast<uint32_t>(out.positions.size());
  for (const auto &p : m.positions) out.positions.push_back({at.x + p.x * cy + p.z * sy, at.y + p.y, at.z - p.x * sy + p.z * cy});
  for (const auto &n : m.normals) out.normals.push_back({n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy});
  if (m.normals.size() != m.positions.size())
    for (size_t i = m.normals.size(); i < m.positions.size(); ++i) out.normals.push_back({0, 1, 0});
  if (m.colors.size() == m.positions.size()) out.colors.insert(out.colors.end(), m.colors.begin(), m.colors.end());
  else out.colors.insert(out.colors.end(), m.positions.size(), pack(0.7f, 0.7f, 0.7f));
  for (uint32_t i : m.indices) out.indices.push_back(base + i);
}

}  // namespace

View::~View() {
  if (worker_.joinable()) worker_.join();
}

const std::vector<std::string> &View::presets() {
  static const std::vector<std::string> p = {"glen", "hex", "ring", "strip", "union", "pond", "path", "slope", "ragged", "single"};
  return p;
}

nlohmann::json View::preset(const std::string &name, int R, uint32_t seed) {
  R = std::max(R, 1);
  nlohmann::json g = nlohmann::json::array();
  auto soil = [&](nlohmann::json r) {
    r["ground"] = "soil";
    r["available"] = true;
    r["moisture"] = 0.55f;
    return r;
  };
  if (name == "single") {
    g.push_back(soil(region("hex", 0, 0, 0, 0)));
  } else if (name == "ring") {
    g.push_back(soil(region("hex", 0, 0, R, 0)));
    auto cut = region("hex", 0, 0, std::max(R - 4, 0), 1);
    cut["cut"] = true;
    g.push_back(cut);
  } else if (name == "strip") {
    for (int q = -R; q <= R; q += 2) g.push_back(soil(region("hex", q, 0, 2, 0)));
  } else if (name == "union") {
    g.push_back(soil(region("hex", 0, 0, R, 0)));
    g.push_back(soil(region("hex", R, -R / 2, std::max(1, R * 2 / 3), 0)));
  } else if (name == "ragged") {
    uint32_t h = seed * 2654435761u + 7;
    for (int i = 0; i < 6; ++i) {
      h = h * 1664525u + 1013904223u;
      const int q = static_cast<int>(h % (2 * R + 1)) - R;
      h = h * 1664525u + 1013904223u;
      const int r = static_cast<int>(h % (2 * R + 1)) - R;
      h = h * 1664525u + 1013904223u;
      g.push_back(soil(region("hex", q / 2, r / 2, 2 + static_cast<int>(h % std::max(2, R / 2)), 0)));
    }
  } else {
    g.push_back(soil(region("hex", 0, 0, R, 0)));
    if (name == "pond" || name == "glen") {
      auto pond = region("hex", R / 3, -R / 4, std::max(1, R / 4), 1);
      pond["ground"] = "water";
      pond["moisture"] = 1.0f;
      g.push_back(pond);
    }
    if (name == "path" || name == "glen") {
      auto path = region("line", -R, R / 2, 0, 0);
      path["q2"] = R;
      path["r2"] = -R / 2;
      path["priority"] = 2;
      path["ground"] = "path";
      path["available"] = false;
      path["keep_clear_m"] = 1.2f;
      g.push_back(path);
    }
    if (name == "slope" || name == "glen") {
      // Gentle ground: rings rising toward a knoll, each a few decimetres above the last.
      const int knoll_q = -R / 3, knoll_r = R / 3;
      for (int k = 3; k >= 1; --k) {
        auto rise = region("hex", knoll_q, knoll_r, std::max(1, R * k / 6), 0);
        rise["height"] = 0.25f * static_cast<float>(4 - k);
        g.push_back(rise);
      }
    }
  }
  return normalize(g);
}

bool View::show(const std::string &name, std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_ && plotter_.name == name) return true;
  }
  Plotter p;
  if (!load(paths_, runner_, name, p, error)) return false;
  std::map<std::string, pipeline::Definition> defs;
  for (const auto &k : p.kinds)
    if (!defs.count(k.pipeline)) {
      pipeline::Definition d;
      if (!pipeline::load(paths_, k.pipeline, d, error)) return false;
      defs[k.pipeline] = d;
    }
  const nlohmann::json view = p.file.value("view", nlohmann::json::object());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    plotter_ = p;
    defs_ = defs;
    loaded_ = true;
    seed_ = view.value("seed", 1u);
    shared_ = p.shared;
    if (view.contains("shared")) shared_.update(view["shared"]);
    regions_ = preset(view.value("preset", std::string("glen")), view.value("radius", 12), seed_);
    occupants_.clear();
    last_grow_ = nlohmann::json::object();
    ++version_;
  }
  return resolve_cells(error);
}

std::string View::name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? plotter_.name : "";
}

Plotter View::plotter() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return plotter_;
}

nlohmann::json View::regions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return regions_;
}

void View::set_seed(uint32_t seed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (seed == seed_) return;
  seed_ = seed;
  occupants_.clear();  // another world: what stood belonged to the old seed's plot
  ++version_;
}

uint32_t View::seed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seed_;
}

void View::set_shared(const nlohmann::json &patch) {
  std::lock_guard<std::mutex> lock(mutex_);
  shared_.update(patch);
  ++version_;
}

nlohmann::json View::shared() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return shared_;
}

nlohmann::json View::request(bool regrow, uint32_t max, const nlohmann::json &exclusions, bool use_focus,
                             const float focus[3], bool want_cells) const {
  nlohmann::json occ = nlohmann::json::array();
  for (const auto &o : occupants_) {
    const auto &p = o.placement;
    occ.push_back({{"q", p["q"]}, {"r", p["r"]}, {"layer", p["layer"]}, {"index", p["index"]}, {"kind", p["kind"]},
                   {"position", p["position"]}, {"footprint_m", p["footprint_m"]}, {"footprint_cells", p["footprint_cells"]}});
  }
  return {{"config", plotter_.config},
          {"ground", {{"regions", regions_}}},
          {"occupants", occ},
          {"exclusions", exclusions.is_array() ? exclusions : nlohmann::json::array()},
          {"seed", seed_},
          {"mode", regrow ? "regrow" : "seed"},
          {"max", max},
          {"focus", {focus ? focus[0] : 0.0f, focus ? focus[1] : 0.0f, focus ? focus[2] : 0.0f}},
          {"use_focus", use_focus},
          {"want_cells", want_cells}};
}

bool View::run_plot(const nlohmann::json &req, nlohmann::json &result, bool verify_native, bool &native_match,
                    double &ms, std::string &error) {
  const auto *in = foundry::json::find_channel("plot_v1_request");
  const auto *out = foundry::json::find_channel("plot_v1_result");
  if (!in || !out) {
    error = "the plot channels aren't in the registry";
    return false;
  }
  std::vector<uint8_t> bytes;
  try {
    in->from_json(req, bytes);
  } catch (const std::exception &e) {
    error = std::string("the request doesn't match plot_v1_request: ") + e.what();
    return false;
  }
  std::string system;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    system = plotter_.system;
  }
  wasm::RunResult w = wasm_.run(forge::wasm_path(paths_, system), bytes);
  ms = w.milliseconds;
  if (w.status != 0) {
    error = system + " failed: " + w.error;
    return false;
  }
  native_match = false;
  if (verify_native) {
    std::string err;
    std::vector<uint8_t> native_out;
    const int status = core::run_process({forge::native_path(paths_, system)}, err, &bytes, &native_out);
    native_match = status == 0 && native_out == w.output;
    if (!native_match) core::log().add("native", system + ": native output differs from WebAssembly");
  }
  if (!out->to_json(w.output.data(), w.output.size(), result)) {
    error = "the output doesn't unpack as plot_v1_result";
    return false;
  }
  return true;
}

bool View::resolve_cells(std::string &error) {
  nlohmann::json req, res;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    req = request(false, 0, nlohmann::json::array(), false, nullptr, true);
  }
  bool match = false;
  double ms = 0;
  if (!run_plot(req, res, false, match, ms, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  cells_ = res.value("cells", nlohmann::json::array());
  ++version_;
  return true;
}

bool View::set_regions(const nlohmann::json &regions, nlohmann::json &out, std::string &error) {
  nlohmann::json removed = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
      error = "no plotter shown";
      return false;
    }
    regions_ = normalize(regions);
  }
  if (!resolve_cells(error)) return false;
  // Occupants whose footprint (or own cell) left the region go: what remains never moves.
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<std::pair<int, int>> cells;
  for (const auto &c : cells_) cells.insert({c["q"].get<int>(), c["r"].get<int>()});
  std::vector<Occupant> kept;
  for (auto &o : occupants_) {
    const auto &p = o.placement;
    // Its footprint is centred on the cell that made its slot.
    const int q = p["q"].get<int>(), r = p["r"].get<int>();
    const int k = std::max(0, p.value("footprint_cells", 0u) >= 37 ? 3 : p.value("footprint_cells", 0u) >= 19 ? 2
                                                              : p.value("footprint_cells", 0u) >= 7 ? 1 : 0);
    bool inside = true;
    for (int dq = -k; dq <= k && inside; ++dq)
      for (int dr = -k; dr <= k && inside; ++dr)
        if (hex_distance(dq, dr) <= k && !cells.count({q + dq, r + dr})) inside = false;
    if (inside) kept.push_back(o);
    else removed.push_back(p);
  }
  occupants_.swap(kept);
  ++version_;
  out = {{"cells", cells_.size()}, {"removed", removed.size()}, {"occupants", occupants_.size()}};
  return true;
}

bool View::grow(bool regrow, uint32_t max, const nlohmann::json &exclusions, bool use_focus, const float focus[3],
                bool verify_native, nlohmann::json &out, std::string &error) {
  nlohmann::json req, res;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
      error = "no plotter shown";
      return false;
    }
    req = request(regrow, max, exclusions, use_focus, focus, false);
  }
  bool match = false;
  double ms = 0;
  if (!run_plot(req, res, verify_native, match, ms, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, int> by_kind;
  for (const auto &p : res.value("placements", nlohmann::json::array())) {
    Occupant o;
    o.placement = p;
    o.kind = p["kind"].get<uint32_t>();
    if (o.kind < plotter_.kinds.size()) ++by_kind[plotter_.kinds[o.kind].label()];
    occupants_.push_back(o);
  }
  last_grow_ = {{"mode", regrow ? "regrow" : "seed"},
                {"placed", res.value("placements", nlohmann::json::array()).size()},
                {"volume", res.value("volume", 0u)},
                {"by_kind", by_kind},
                {"plot_ms", std::round(ms * 100) / 100}};
  if (verify_native) last_grow_["native_matches_wasm"] = match;
  ++version_;
  out = last_grow_;
  out["occupants"] = occupants_.size();
  return true;
}

nlohmann::json View::clear(const nlohmann::json &how) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool fell = how.value("fell", false);
  const std::string layer = how.value("layer", std::string());
  const float fraction = how.value("fraction", 1.0f);
  const bool all = how.value("all", false);
  const bool stumps = how.value("stumps", false);  // clear the stumps felled trees left
  const bool circle = how.contains("center");
  const float cx = circle ? how["center"][0].get<float>() : 0, cz = circle ? how["center"][1].get<float>() : 0;
  const float radius = how.value("radius", 4.0f);
  uint32_t removed = 0, felled = 0;
  std::vector<Occupant> kept;
  uint32_t h = how.value("seed", 7u) * 2654435761u + 1;
  for (auto &o : occupants_) {
    const auto &p = o.placement;
    const Kind *k = o.kind < plotter_.kinds.size() ? &plotter_.kinds[o.kind] : nullptr;
    bool hit = all || (stumps && o.extra.value("stump", false));
    if (circle) {
      const float dx = p["position"][0].get<float>() - cx, dz = p["position"][2].get<float>() - cz;
      hit = dx * dx + dz * dz <= radius * radius;
    }
    if (!all && !stumps && !circle && !layer.empty()) hit = k && k->layer == layer;
    if (hit && !layer.empty() && k && k->layer != layer) hit = false;
    if (hit && fraction < 1) {
      h = h * 1664525u + 1013904223u;
      hit = static_cast<float>(h >> 8) / 16777216.0f < fraction;
    }
    if (!hit) {
      kept.push_back(o);
      continue;
    }
    // Felling turns a tree into its stump: the occupant stays, holding its footprint.
    const bool has_stump = k && defs_.count(k->pipeline) && defs_.at(k->pipeline).param("stump");
    if (fell && has_stump && !o.extra.value("stump", false)) {
      o.extra["stump"] = true;
      ++felled;
      kept.push_back(o);
    } else {
      ++removed;
    }
  }
  occupants_.swap(kept);
  ++version_;
  return {{"removed", removed}, {"felled", felled}, {"occupants", occupants_.size()}};
}

nlohmann::json View::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json kinds = nlohmann::json::array();
  for (const auto &k : plotter_.kinds)
    kinds.push_back({{"id", k.id}, {"layer", k.layer}, {"kind", k.label()}, {"footprint_m", k.footprint_m},
                     {"footprint_cells", k.footprint_cells}, {"regrow_only", k.regrow_only}});
  return {{"plotter", plotter_.name},
          {"system", plotter_.system},
          {"seed", seed_},
          {"shared", shared_},
          {"regions", regions_},
          {"cells", cells_.size()},
          {"kinds", kinds},
          {"last_grow", last_grow_},
          {"metrics", metrics_locked()}};
}

nlohmann::json View::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_locked();
}

nlohmann::json View::metrics_locked() const {
  nlohmann::json m = nlohmann::json::object();
  const double hectares = static_cast<double>(cells_.size()) * 2.5980762 / 10000.0;
  m["cells"] = cells_.size();
  m["hectares"] = std::round(hectares * 10000) / 10000;
  m["occupants"] = occupants_.size();
  std::map<std::string, std::vector<const Occupant *>> by_layer;
  std::map<std::string, int> by_kind, stumps;
  std::set<std::string> builds;
  for (const auto &o : occupants_) {
    const Kind *k = o.kind < plotter_.kinds.size() ? &plotter_.kinds[o.kind] : nullptr;
    by_layer[k ? k->layer : "?"].push_back(&o);
    if (k) {
      ++by_kind[k->label()];
      nlohmann::json v = values_for(plotter_, *k, o.placement["seed"].get<uint32_t>(), o.placement.value("regrown", false), shared_);
      v.update(o.extra);
      builds.insert(k->pipeline + v.dump());
      if (o.extra.value("stump", false)) ++stumps[k->label()];
    }
  }
  m["by_kind"] = by_kind;
  if (!stumps.empty()) m["stumps"] = stumps;
  m["distinct_builds"] = builds.size();
  nlohmann::json layers = nlohmann::json::object();
  const auto &cfg_layers = plotter_.config.value("layers", nlohmann::json::array());
  for (size_t li = 0; li < cfg_layers.size(); ++li) {
    const std::string name = cfg_layers[li]["name"];
    const auto &list = by_layer[name];
    float nearest = 1e9f;
    for (size_t i = 0; i < list.size(); ++i)
      for (size_t j = i + 1; j < list.size(); ++j) {
        const auto &a = list[i]->placement["position"], &b = list[j]->placement["position"];
        nearest = std::min(nearest, std::hypot(a[0].get<float>() - b[0].get<float>(), a[2].get<float>() - b[2].get<float>()));
      }
    layers[name] = {{"count", list.size()},
                    {"per_hectare", hectares > 0 ? std::round(list.size() / hectares) : 0},
                    {"spacing_m", cfg_layers[li]["spacing_m"]},
                    {"nearest_m", list.size() > 1 ? std::round(nearest * 100) / 100 : 0.0f}};
  }
  m["layers"] = layers;
  // Footprint overlaps: two occupants whose footprints share a cell. There must be none.
  uint32_t overlaps = 0;
  for (size_t i = 0; i < occupants_.size(); ++i)
    for (size_t j = i + 1; j < occupants_.size(); ++j) {
      const auto &a = occupants_[i].placement, &b = occupants_[j].placement;
      const uint32_t ca = a.value("footprint_cells", 0u), cb = b.value("footprint_cells", 0u);
      if (!ca || !cb) continue;
      auto cell = [](const nlohmann::json &p, int &q, int &r) {
        q = p["q"].get<int>();  // footprints centre on the cell that made the slot
        r = p["r"].get<int>();
      };
      int qa, ra, qb, rb;
      cell(a, qa, ra);
      cell(b, qb, rb);
      auto rings = [](uint32_t c) { return c >= 37 ? 3 : c >= 19 ? 2 : c >= 7 ? 1 : 0; };
      if (hex_distance(qa - qb, ra - rb) <= rings(ca) + rings(cb)) ++overlaps;
    }
  m["footprint_overlaps"] = overlaps;
  return m;
}

Picture View::picture() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (loaded_ && picture_.version != version_ && composing_ != version_ && composing_ == 0) {
    if (worker_.joinable()) worker_.join();
    composing_ = version_;
    picture_.building = true;
    const int v = version_;
    worker_ = std::thread([this, v] { compose(v); });
  }
  return picture_;
}

void View::compose(int version) {
  // A snapshot of the state, so the region can keep changing while this composes.
  Plotter p;
  std::map<std::string, pipeline::Definition> defs;
  nlohmann::json cells, shared;
  std::vector<Occupant> occupants;
  uint32_t seed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    p = plotter_;
    defs = defs_;
    cells = cells_;
    shared = shared_;
    occupants = occupants_;
    seed = seed_;
  }
  (void)seed;
  auto mesh = std::make_shared<runtime::Mesh>();
  std::vector<viewport::Line> lines, collision;
  // The ground: a flat hex tile per cell at its height, coloured by its ground.
  std::map<std::pair<int, int>, float> heights;
  for (const auto &c : cells) heights[{c["q"].get<int>(), c["r"].get<int>()}] = c.value("height", 0.0f);
  for (const auto &c : cells) {
    const int q = c["q"], r = c["r"];
    const float x = cell_x(q, r), z = cell_z(r), y = c.value("height", 0.0f);
    const std::string g = c.value("ground", std::string("soil"));
    const float grove = c.value("grove", 0.5f), wet = c.value("moisture", 0.5f);
    float cr = 0.45f, cg = 0.66f, cb = 0.32f;  // a sunny meadow
    if (g == "water") cr = 0.32f, cg = 0.55f, cb = 0.78f;
    else if (g == "path") cr = 0.74f, cg = 0.63f, cb = 0.45f;
    else if (g == "rock") cr = 0.6f, cg = 0.6f, cb = 0.58f;
    else if (g == "sand") cr = 0.86f, cg = 0.8f, cb = 0.58f;
    else {
      cr -= 0.08f * grove;  // groves shade the grass a little darker and wetter ground a little greener
      cg += 0.04f * (wet - 0.5f) - 0.04f * grove;
    }
    const float shade = 0.97f + 0.06f * static_cast<float>((static_cast<uint32_t>(q * 7 + r * 13)) % 3) / 2.0f;
    const uint32_t col = pack(cr * shade, cg * shade, cb * shade);
    const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
    mesh->positions.push_back({x, y, z});
    mesh->normals.push_back({0, 1, 0});
    mesh->colors.push_back(col);
    for (int k = 0; k < 6; ++k) {
      const float a = 3.14159265f / 6.0f + 3.14159265f / 3.0f * static_cast<float>(k);
      mesh->positions.push_back({x + std::cos(a), y, z + std::sin(a)});
      mesh->normals.push_back({0, 1, 0});
      mesh->colors.push_back(col);
    }
    for (int k = 0; k < 6; ++k) {
      mesh->indices.push_back(base);
      mesh->indices.push_back(base + 1 + static_cast<uint32_t>((k + 1) % 6));
      mesh->indices.push_back(base + 1 + static_cast<uint32_t>(k));
    }
    // Skirts: a bank down to each lower neighbour (or to the ground plane at the border), so rises read as ground.
    for (int k = 0; k < 6; ++k) {
      const float a0 = 3.14159265f / 6.0f + 3.14159265f / 3.0f * static_cast<float>(k), a1 = a0 + 3.14159265f / 3.0f;
      // The neighbour across this edge: its centre lies at the edge's midpoint, doubled.
      const float mx = x + std::cos((a0 + a1) * 0.5f) * kSqrt3, mz = z + std::sin((a0 + a1) * 0.5f) * kSqrt3;
      const float fq = kSqrt3 / 3.0f * mx - mz / 3.0f, fr = 2.0f / 3.0f * mz;
      const int nq = static_cast<int>(std::lround(fq)), nr = static_cast<int>(std::lround(fr));
      auto it = heights.find({nq, nr});
      const float below = it == heights.end() ? std::min(0.0f, y) - 0.05f : it->second;
      if (below >= y - 1e-3f) continue;
      const foundry::vec3 out{std::cos((a0 + a1) * 0.5f), 0, std::sin((a0 + a1) * 0.5f)};
      const uint32_t side = pack(cr * 0.62f, cg * 0.55f, cb * 0.45f);
      const uint32_t s0 = static_cast<uint32_t>(mesh->positions.size());
      mesh->positions.push_back({x + std::cos(a0), y, z + std::sin(a0)});
      mesh->positions.push_back({x + std::cos(a1), y, z + std::sin(a1)});
      mesh->positions.push_back({x + std::cos(a1), below, z + std::sin(a1)});
      mesh->positions.push_back({x + std::cos(a0), below, z + std::sin(a0)});
      for (int i = 0; i < 4; ++i) {
        mesh->normals.push_back(out);
        mesh->colors.push_back(side);
      }
      for (uint32_t i : {0u, 2u, 1u, 0u, 3u, 2u}) mesh->indices.push_back(s0 + i);
    }
    float lr = 0.85f, lg = 0.95f, lb = 0.8f;
    if (!c.value("available", true)) lr = 1.0f, lg = 0.45f, lb = 0.4f;
    for (int k = 0; k < 6; ++k) {
      const float a0 = 3.14159265f / 6.0f + 3.14159265f / 3.0f * static_cast<float>(k);
      const float a1 = a0 + 3.14159265f / 3.0f;
      lines.push_back({{x + std::cos(a0), y + 0.01f, z + std::sin(a0)}, {x + std::cos(a1), y + 0.01f, z + std::sin(a1)}, lr, lg, lb, 0.18f});
    }
  }
  // Every occupant: its kind's pipeline at its values, turned and placed.
  uint32_t failed = 0;
  std::string first_error;
  for (const auto &o : occupants) {
    if (o.kind >= p.kinds.size()) continue;
    const Kind &k = p.kinds[o.kind];
    auto def = defs.find(k.pipeline);
    if (def == defs.end()) continue;
    nlohmann::json v = values_for(p, k, o.placement["seed"].get<uint32_t>(), o.placement.value("regrown", false), shared);
    v.update(o.extra);
    auto res = runner_.run(def->second, v, false, false, false);
    if (!res.ok || !res.frame.mesh) {
      ++failed;
      if (first_error.empty()) first_error = k.label() + ": " + res.error;
      continue;
    }
    const auto &pos = o.placement["position"];
    const foundry::vec3 at{pos[0].get<float>(), pos[1].get<float>(), pos[2].get<float>()};
    const float yaw = o.placement.value("yaw", 0.0f);
    place(*res.frame.mesh, at, yaw, *mesh);
    if (res.frame.collision) physics::wireframe(*res.frame.collision, collision, at, yaw);
    // The footprint it reserves: a ring at its radius.
    const float fr = o.placement.value("footprint_m", 0.0f);
    if (fr > 0)
      for (int s = 0; s < 24; ++s) {
        const float a0 = 6.2831853f * static_cast<float>(s) / 24.0f, a1 = 6.2831853f * static_cast<float>(s + 1) / 24.0f;
        collision.push_back({{at.x + fr * std::cos(a0), at.y + 0.03f, at.z + fr * std::sin(a0)},
                             {at.x + fr * std::cos(a1), at.y + 0.03f, at.z + fr * std::sin(a1)}, 1.0f, 0.65f, 0.2f, 0.6f});
      }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  picture_.mesh = mesh;
  picture_.lines = std::move(lines);
  picture_.collision = std::move(collision);
  picture_.metrics = metrics_locked();
  picture_.version = version;
  picture_.building = false;
  picture_.error = failed ? std::to_string(failed) + " occupants failed to build; first: " + first_error : "";
  composing_ = 0;
}

}  // namespace plot
