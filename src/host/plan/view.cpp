#include "plan/view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include "core/log.h"
#include "core/util.h"
#include "json/registry.h"
#include "physics/toy.h"
#include "plot/grow.h"
#include "plot/plotter.h"

namespace plan {

namespace {

constexpr float kSqrt3 = 1.7320508f, kPi = 3.14159265f;
using foundry::vec3;

float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
float cell_z(int r) { return 1.5f * static_cast<float>(r); }

uint32_t pack(float r, float g, float b) {
  auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return c(r) | (c(g) << 8) | (c(b) << 16) | (255u << 24);
}
struct Rgb {
  float r, g, b;
};
uint32_t pack(Rgb c, float shade = 1) { return pack(c.r * shade, c.g * shade, c.b * shade); }

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// ---- geometry helpers for the ground and stand-ins ----

struct Builder {
  runtime::Mesh &m;
  uint32_t vertex(vec3 p, vec3 n, uint32_t c) {
    m.positions.push_back(p);
    m.normals.push_back(n);
    m.colors.push_back(c);
    return static_cast<uint32_t>(m.positions.size() - 1);
  }
  void quad(vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, uint32_t col) {
    const uint32_t i0 = vertex(a, n, col), i1 = vertex(b, n, col), i2 = vertex(c, n, col), i3 = vertex(d, n, col);
    // Wind so the face looks along n.
    const vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    if (cr.x * n.x + cr.y * n.y + cr.z * n.z >= 0) {
      for (uint32_t i : {i0, i1, i2, i0, i2, i3}) m.indices.push_back(i);
    } else {
      for (uint32_t i : {i0, i2, i1, i0, i3, i2}) m.indices.push_back(i);
    }
  }
  void tri(vec3 a, vec3 b, vec3 c, vec3 n, uint32_t col) {
    const uint32_t i0 = vertex(a, n, col), i1 = vertex(b, n, col), i2 = vertex(c, n, col);
    const vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    if (cr.x * n.x + cr.y * n.y + cr.z * n.z >= 0) {
      for (uint32_t i : {i0, i1, i2}) m.indices.push_back(i);
    } else {
      for (uint32_t i : {i0, i2, i1}) m.indices.push_back(i);
    }
  }
  // A box standing on `base` (its bottom's middle), half sizes hx, hz, height h, turned by yaw
  // (radians, +z toward +x).
  void box(vec3 base, float hx, float h, float hz, float yaw, Rgb c) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    auto P = [&](float x, float y, float z) { return vec3{base.x + x * cy + z * sy, base.y + y, base.z - x * sy + z * cy}; };
    auto N = [&](float x, float y, float z) { return vec3{x * cy + z * sy, y, -x * sy + z * cy}; };
    const uint32_t top = pack(c), side = pack(c, 0.82f), dark = pack(c, 0.7f);
    quad(P(-hx, h, -hz), P(hx, h, -hz), P(hx, h, hz), P(-hx, h, hz), N(0, 1, 0), top);
    quad(P(-hx, 0, hz), P(hx, 0, hz), P(hx, h, hz), P(-hx, h, hz), N(0, 0, 1), side);
    quad(P(-hx, 0, -hz), P(hx, 0, -hz), P(hx, h, -hz), P(-hx, h, -hz), N(0, 0, -1), dark);
    quad(P(hx, 0, -hz), P(hx, 0, hz), P(hx, h, hz), P(hx, h, -hz), N(1, 0, 0), side);
    quad(P(-hx, 0, -hz), P(-hx, 0, hz), P(-hx, h, hz), P(-hx, h, -hz), N(-1, 0, 0), dark);
  }
  // A gable roof over a box's top (ridge along x), eaves at y0 and the ridge rise above them.
  void roof(vec3 base, float hx, float y0, float hz, float rise, float yaw, Rgb c) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    auto P = [&](float x, float y, float z) { return vec3{base.x + x * cy + z * sy, base.y + y, base.z - x * sy + z * cy}; };
    auto N = [&](float x, float y, float z) {
      const float l = std::sqrt(x * x + y * y + z * z);
      return vec3{(x * cy + z * sy) / l, y / l, (-x * sy + z * cy) / l};
    };
    const float o = 0.4f;
    quad(P(-hx - o, y0, hz + o), P(hx + o, y0, hz + o), P(hx + o, y0 + rise, 0), P(-hx - o, y0 + rise, 0), N(0, hz, rise), pack(c));
    quad(P(-hx - o, y0, -hz - o), P(hx + o, y0, -hz - o), P(hx + o, y0 + rise, 0), P(-hx - o, y0 + rise, 0), N(0, hz, -rise), pack(c, 0.8f));
    tri(P(hx, y0, hz), P(hx, y0, -hz), P(hx, y0 + rise, 0), N(1, 0, 0), pack(c, 0.75f));
    tri(P(-hx, y0, hz), P(-hx, y0, -hz), P(-hx, y0 + rise, 0), N(-1, 0, 0), pack(c, 0.75f));
  }
  // A hex prism (pointy-top, circumradius rad) from y0 to y1 at (x, z).
  void prism(float x, float z, float rad, float y0, float y1, Rgb c, bool sides = true) {
    const uint32_t top = pack(c);
    const uint32_t centre = vertex({x, y1, z}, {0, 1, 0}, top);
    uint32_t ring[6];
    for (int k = 0; k < 6; ++k) {
      const float a = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k);
      ring[k] = vertex({x + rad * std::cos(a), y1, z + rad * std::sin(a)}, {0, 1, 0}, top);
    }
    for (int k = 0; k < 6; ++k)
      for (uint32_t i : {centre, ring[(k + 1) % 6], ring[k]}) m.indices.push_back(i);
    if (!sides) return;
    for (int k = 0; k < 6; ++k) {
      const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k), a1 = a0 + kPi / 3.0f, am = (a0 + a1) * 0.5f;
      quad({x + rad * std::cos(a0), y0, z + rad * std::sin(a0)}, {x + rad * std::cos(a1), y0, z + rad * std::sin(a1)},
           {x + rad * std::cos(a1), y1, z + rad * std::sin(a1)}, {x + rad * std::cos(a0), y1, z + rad * std::sin(a0)},
           {std::cos(am), 0, std::sin(am)}, pack(c, 0.8f));
    }
  }
  // A box along an edge from a to b (on the ground at their heights' lower), thickness t, height h.
  void along(vec3 a, vec3 b, float t, float h, Rgb c, float below = 0) {
    const float dx = b.x - a.x, dz = b.z - a.z, len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-3f) return;
    const float yaw = std::atan2(dz, dx);  // the edge along local x after turning
    box({(a.x + b.x) * 0.5f, std::min(a.y, b.y) - below, (a.z + b.z) * 0.5f}, len * 0.5f, h + below, t * 0.5f, -yaw, c);
  }
};

void place(const runtime::Mesh &m, vec3 at, float yaw, runtime::Mesh &out) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const uint32_t base = static_cast<uint32_t>(out.positions.size());
  for (const auto &p : m.positions) out.positions.push_back({at.x + p.x * cy + p.z * sy, at.y + p.y, at.z - p.x * sy + p.z * cy});
  for (const auto &n : m.normals) out.normals.push_back({n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy});
  for (size_t i = m.normals.size(); i < m.positions.size(); ++i) out.normals.push_back({0, 1, 0});
  if (m.colors.size() == m.positions.size()) out.colors.insert(out.colors.end(), m.colors.begin(), m.colors.end());
  else out.colors.insert(out.colors.end(), m.positions.size(), pack(0.7f, 0.7f, 0.7f));
  for (uint32_t i : m.indices) out.indices.push_back(base + i);
}

vec3 vec(const nlohmann::json &j) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }

int tier_index(const nlohmann::json &v) {
  if (v.is_number()) return std::clamp(v.get<int>(), 0, 2);
  if (v.is_string())
    for (size_t i = 0; i < tiers().size(); ++i)
      if (tiers()[i] == v.get<std::string>()) return static_cast<int>(i);
  return 2;
}

std::string key_of(const Settings &st, const nlohmann::json &config) {
  return config.dump() + "|" + std::to_string(st.seed) + "|" + std::to_string(st.plots) + "|" + std::to_string(st.tier);
}

// A plan's summary: counts against the config's ranges, the navigation checks, and its timing.
nlohmann::json summarize(const nlohmann::json &plan, const nlohmann::json &config) {
  nlohmann::json r = nlohmann::json::object();
  std::map<std::string, int> by_class, by_shape, by_use, by_tier, by_kind, roads, halves, lines;
  int fenced = 0;
  const auto &plots = plan.value("plots", nlohmann::json::array());
  int homes = 0, buildings = 0;
  for (const auto &p : plots) {
    const std::string kind = p.value("kind", std::string("?"));
    ++by_kind[kind];
    ++by_tier[p.value("tier", std::string("?"))];
    if (kind == "road") {
      roads[p.value("road", std::string("?"))] += static_cast<int>(p.value("cells", nlohmann::json::array()).size() / 2);
      for (const auto &h : p.value("halves", nlohmann::json::array()))
        if (h.get<uint32_t>() < 6) ++halves[p.value("road", std::string("?"))];
      for (const auto &f : p.value("fences", nlohmann::json::array()))
        for (uint32_t bits = f.get<uint32_t>(); bits; bits &= bits - 1) ++fenced;
      continue;
    }
    if (kind == "water") continue;
    ++buildings;
    ++by_class[p.value("size_class", std::string("?"))];
    if (kind == "building") ++by_shape[p.value("shape", std::string("?"))];
    ++by_use[p.value("use", std::string("?"))];
    const std::string c = p.value("size_class", std::string());
    if (kind == "building" && (c == "solo" || c == "family" || c == "extended")) ++homes;
  }
  std::map<std::string, int> uses;
  for (const auto &c : plan.value("cells", nlohmann::json::array())) ++uses[c.value("use", std::string("?"))];
  r["plots"] = plots.size();
  r["buildings"] = buildings;
  r["household_plots"] = homes;
  r["by_kind"] = by_kind;
  r["road_cells"] = roads;
  r["halves"] = halves;
  r["fenced_edges"] = fenced;
  for (const auto &l : plan.value("lines", nlohmann::json::array())) ++lines[l.value("kind", std::string("?"))];
  r["lines"] = lines;
  r["open_cells"] = plan.value("open_cells", 0);
  r["plots_range"] = {config.value("plots_min", 0), config.value("plots_max", 0)};
  r["by_class"] = by_class;
  r["by_shape"] = by_shape;
  r["by_use"] = by_use;
  r["by_tier"] = by_tier;
  r["cells_by_use"] = uses;
  r["cells"] = plan.value("cells", nlohmann::json::array()).size();
  r["radius_cells"] = plan.value("radius", 0);
  r["hectares"] = std::round(static_cast<double>(plan.value("cells", nlohmann::json::array()).size()) * 2.5980762 / 100.0) / 100.0;
  r["entrances"] = plan.value("entrances", nlohmann::json::array()).size() / 2;
  for (const char *k : {"plots_unreached", "cart_unreached", "repairs"}) r[k] = plan.value(k, 0);
  for (const char *k : {"longest_walk_m", "steepest_street", "highest_step_m", "retaining_m"})
    r[k] = std::round(plan.value(k, 0.0) * 100) / 100;
  r["nav_polys"] = plan.value("nav_flags", nlohmann::json::array()).size();
  int half_polys = 0;
  for (const auto &c : plan.value("nav_counts", nlohmann::json::array())) half_polys += c.get<int>() == 5 ? 1 : 0;
  r["nav_half_polys"] = half_polys;
  r["nav_links"] = plan.value("nav_links", nlohmann::json::array()).size() / 2;
  if (!plan.value("note", std::string()).empty()) r["note"] = plan["note"];
  r["ok"] = plan.value("plots_unreached", 1) == 0 && plan.value("cart_unreached", 1) == 0;
  return r;
}

}  // namespace

View::~View() {
  if (worker_.joinable()) worker_.join();
}

bool View::show(const std::string &name, std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_ && planner_.name == name) return true;
  }
  Planner p;
  if (!load(paths_, runner_, name, p, error)) return false;
  // Every pipeline the fill names (buildings, roads, waterfronts, civic buildings, castles, walls,
  // fences, gates), by name.
  std::map<std::string, pipeline::Definition> defs;
  std::vector<nlohmann::json> stack = {p.fill};
  while (!stack.empty()) {
    const nlohmann::json j = stack.back();
    stack.pop_back();
    if (!j.is_object()) continue;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "pipeline" && it.value().is_string() && !defs.count(it.value().get<std::string>())) {
        pipeline::Definition d;
        if (!pipeline::load(paths_, it.value().get<std::string>(), d, error)) return false;
        defs[it.value().get<std::string>()] = d;
      } else if (it.value().is_object()) {
        stack.push_back(it.value());
      }
    }
  }
  const nlohmann::json view = p.file.value("view", nlohmann::json::object());
  std::lock_guard<std::mutex> lock(mutex_);
  planner_ = p;
  fill_defs_ = defs;
  loaded_ = true;
  settings_ = Settings();
  settings_.seed = view.value("seed", 1u);
  settings_.plots = view.value("plots", 0u);
  settings_.tier = static_cast<uint32_t>(tier_index(view.value("tier", nlohmann::json("town"))));
  settings_.shared = view.value("shared", nlohmann::json::object());
  settings_.nature = view.value("nature", std::string());
  plan_ = nlohmann::json();
  report_ = nlohmann::json::object();
  plan_key_.clear();
  ++version_;
  return true;
}

std::string View::name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? planner_.name : "";
}

Planner View::planner() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return planner_;
}

Settings View::settings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

bool View::set(const nlohmann::json &patch, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) {
    error = "no planner shown";
    return false;
  }
  Settings &s = settings_;
  if (patch.contains("seed")) s.seed = std::max(1u, patch["seed"].get<uint32_t>());
  if (patch.contains("plots")) s.plots = patch["plots"].get<uint32_t>();
  if (patch.contains("tier")) s.tier = static_cast<uint32_t>(tier_index(patch["tier"]));
  for (const char *k : {"fill", "lines", "outlines", "nav", "collision"})
    if (patch.contains(k)) {
      const std::string n = k;
      bool &b = n == "fill" ? s.fill : n == "lines" ? s.lines : n == "outlines" ? s.outlines : n == "nav" ? s.nav : s.collision;
      b = patch[k].get<bool>();
    }
  if (patch.contains("nature")) s.nature = patch["nature"].is_string() ? patch["nature"].get<std::string>() : "";
  if (patch.contains("colour")) s.colour = patch["colour"].get<std::string>();
  if (patch.contains("shared")) s.shared.update(patch["shared"]);
  if (patch.contains("config"))
    for (auto it = patch["config"].begin(); it != patch["config"].end(); ++it) {
      if (it.value().is_null()) s.overrides.erase(it.key());
      else s.overrides[it.key()] = it.value();
    }
  ++version_;
  return true;
}

nlohmann::json View::config() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json c = planner_.config;
  c.update(settings_.overrides);
  return c;
}

bool View::save(std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) {
    error = "no planner shown";
    return false;
  }
  nlohmann::json f = planner_.file;
  f["config"].update(settings_.overrides);
  f["view"]["seed"] = settings_.seed;
  f["view"]["plots"] = settings_.plots;
  f["view"]["tier"] = tiers()[settings_.tier];
  if (!core::write_json(paths_.workspace + "/planners/" + planner_.name + ".json", f)) {
    error = "couldn't write planners/" + planner_.name + ".json";
    return false;
  }
  planner_.file = f;
  planner_.config.update(settings_.overrides);
  settings_.overrides = nlohmann::json::object();
  return true;
}

bool View::run_plan(const Settings &st, const nlohmann::json &config, bool verify_native, nlohmann::json &plan,
                    nlohmann::json &report, std::string &error) {
  const auto *in = foundry::json::find_channel("plan_v1_request");
  const auto *out = foundry::json::find_channel("plan_v1_plan");
  if (!in || !out) {
    error = "the plan channels aren't in the registry";
    return false;
  }
  const nlohmann::json req = {{"config", config}, {"seed", st.seed}, {"plots", st.plots}, {"tier", tiers()[st.tier]}};
  std::vector<uint8_t> bytes;
  try {
    in->from_json(req, bytes);
  } catch (const std::exception &e) {
    error = std::string("the request doesn't match plan_v1_request: ") + e.what();
    return false;
  }
  std::string system;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    system = planner_.system;
  }
  wasm::RunResult w = wasm_.run(forge::wasm_path(paths_, system), bytes);
  if (w.status != 0) {
    error = system + " failed: " + w.error;
    return false;
  }
  bool match = false;
  if (verify_native) {
    std::string err;
    std::vector<uint8_t> native_out;
    const int status = core::run_process({forge::native_path(paths_, system)}, err, &bytes, &native_out);
    match = status == 0 && native_out == w.output;
    if (!match) core::log().add("native", system + ": native output differs from WebAssembly");
  }
  if (!out->to_json(w.output.data(), w.output.size(), plan)) {
    error = "the output doesn't unpack as plan_v1_plan";
    return false;
  }
  report = summarize(plan, config);
  report["plan_ms"] = std::round(w.milliseconds * 10) / 10;
  report["plan_bytes"] = w.output.size();
  if (verify_native) report["native_matches_wasm"] = match;
  return true;
}

bool View::plan(bool verify_native, nlohmann::json &report, std::string &error) {
  Settings st;
  nlohmann::json cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
      error = "no planner shown";
      return false;
    }
    st = settings_;
    cfg = planner_.config;
    cfg.update(settings_.overrides);
  }
  nlohmann::json plan;
  if (!run_plan(st, cfg, verify_native, plan, report, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  plan_ = plan;
  report_ = report;
  plan_key_ = key_of(st, cfg);
  ++version_;
  return true;
}

nlohmann::json View::report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}

nlohmann::json View::plan_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return plan_;
}

std::vector<float> View::walk_triangles() {
  Picture pic = picture();
  std::vector<float> out;
  if (!pic.mesh) return out;
  out.reserve(pic.mesh->indices.size() * 3);
  for (uint32_t i : pic.mesh->indices) {
    const auto &p = pic.mesh->positions[i];
    out.insert(out.end(), {p.x, p.y, p.z});
  }
  return out;
}

Picture View::picture() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (loaded_ && picture_.version != version_ && composing_ == 0) {
    if (worker_.joinable()) worker_.join();
    composing_ = version_;
    picture_.building = true;
    const int v = version_;
    worker_ = std::thread([this, v] { compose(v); });
  }
  return picture_;
}

void View::compose(int version) {
  Settings st;
  Planner pl;
  nlohmann::json cfg, plan;
  std::map<std::string, pipeline::Definition> defs;
  bool stale;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    st = settings_;
    pl = planner_;
    cfg = planner_.config;
    cfg.update(settings_.overrides);
    plan = plan_;
    defs = fill_defs_;
    stale = plan_key_ != key_of(st, cfg);
  }
  std::string error;
  nlohmann::json report;
  if (stale) {
    if (run_plan(st, cfg, false, plan, report, error)) {
      std::lock_guard<std::mutex> lock(mutex_);
      plan_ = plan;
      report_ = report;
      plan_key_ = key_of(st, cfg);
    }
  }
  auto mesh = std::make_shared<runtime::Mesh>();
  std::vector<viewport::Line> lines, collision;
  Builder b{*mesh};
  const auto &cells = plan.value("cells", nlohmann::json::array());
  const auto &plots = plan.value("plots", nlohmann::json::array());
  std::map<std::pair<int, int>, float> height;
  for (const auto &c : cells) height[{c["q"].get<int>(), c["r"].get<int>()}] = c.value("height", 0.0f);
  // With nature on, the ground is the nature ground (the shape and its apron); its cells beyond the
  // shape are drawn as open land.
  struct G {
    int q, r;
    float h;
    std::string use, road;
    int plot;
    std::string tier;
  };
  std::vector<G> ground;
  for (const auto &c : cells)
    ground.push_back({c["q"].get<int>(), c["r"].get<int>(), c.value("height", 0.0f), c.value("use", std::string("open")),
                      c.value("road", std::string("none")), c.value("plot", -1), c.value("tier", std::string("hamlet"))});
  const nlohmann::json nature_regions = plan.value("nature", nlohmann::json::object()).value("regions", nlohmann::json::array());
  if (!st.nature.empty())
    for (const auto &rg : nature_regions) {
      const auto &cs = rg["cells"];
      for (size_t k = 0; k + 1 < cs.size(); k += 2) {
        const int q = cs[k], r = cs[k + 1];
        if (height.count({q, r})) continue;
        height[{q, r}] = rg.value("height", 0.0f);
        ground.push_back({q, r, rg.value("height", 0.0f), rg.value("ground", std::string()) == "water" ? "water" : "open", "none", -1, "hamlet"});
      }
    }
  std::map<int, float> wealth;
  for (const auto &p : plots) wealth[p["id"].get<int>()] = p.value("wealth", 0.0f);
  float hmin = 1e9f, hmax = -1e9f;
  for (const auto &g : ground) {
    hmin = std::min(hmin, g.h);
    hmax = std::max(hmax, g.h);
  }
  const nlohmann::json fr = pl.fill.value("roads", nlohmann::json::object());
  const bool roads_filled = st.fill && defs.count(fr.value("pipeline", ""));
  // A hex outline at (q, r), lifted a little: wall and gate tiles are marked this way, not built.
  auto outline = [&](int q, int r, float y, float rad, float cr, float cg, float cb, float a) {
    const float x = cell_x(q, r), z = cell_z(r);
    for (int k = 0; k < 6; ++k) {
      const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k), a1 = a0 + kPi / 3.0f;
      lines.push_back({{x + rad * std::cos(a0), y, z + rad * std::sin(a0)}, {x + rad * std::cos(a1), y, z + rad * std::sin(a1)}, cr, cg, cb, a});
    }
  };
  // ---- the ground: every cell flat at its height; walls and gates are tiles ----
  for (const auto &g : ground) {
    const float x = cell_x(g.q, g.r), z = cell_z(g.r), y = g.h;
    Rgb col{0.42f, 0.62f, 0.32f};
    const bool built = g.use == "building" || g.use == "waterfront";
    const bool wall = g.use == "wall" || g.use == "keep_wall";
    if (st.colour == "height") {
      const float t = hmax > hmin ? (y - hmin) / (hmax - hmin) : 0;
      col = {0.25f + 0.6f * t, 0.45f + 0.35f * t, 0.6f - 0.3f * t};
    } else if (st.colour == "tier") {
      static const Rgb tc[3] = {{0.95f, 0.75f, 0.3f}, {0.45f, 0.75f, 0.95f}, {0.75f, 0.5f, 0.9f}};
      col = g.use == "road" || built ? tc[tier_index(g.tier)] : Rgb{0.5f, 0.55f, 0.5f};
    } else if (st.colour == "wealth" && built) {
      const float w = wealth[g.plot];
      col = {0.35f + 0.6f * w, 0.4f + 0.25f * w, 0.75f - 0.5f * w};
    } else if (g.use == "road" || g.use == "gate") {
      col = g.road == "square" || g.road == "market" ? Rgb{0.72f, 0.68f, 0.6f} : Rgb{0.66f, 0.57f, 0.43f};
    } else if (built) {
      col = {0.48f, 0.58f, 0.32f};
    } else if (g.use == "water") {
      col = {0.3f, 0.52f, 0.76f};
    } else if (wall) {
      col = g.use == "keep_wall" ? Rgb{0.5f, 0.48f, 0.52f} : Rgb{0.62f, 0.6f, 0.56f};
    }
    const float shade = 0.96f + 0.04f * static_cast<float>(hash32(static_cast<uint32_t>(g.q * 7919 + g.r * 104729)) % 3);
    col = {col.r * shade, col.g * shade, col.b * shade};
    // Road cells under a filled road are drawn by their pieces; their ground sits just below.
    const float top = y - (roads_filled && (g.use == "road" || g.use == "gate") ? 0.02f : 0.0f);
    b.prism(x, z, 1.0f, y, top, col, false);
    if (wall) outline(g.q, g.r, y + 0.05f, 0.82f, g.use == "keep_wall" ? 0.85f : 0.95f, g.use == "keep_wall" ? 0.7f : 0.92f, g.use == "keep_wall" ? 1.0f : 0.88f, 0.75f);
    if (g.use == "gate") outline(g.q, g.r, y + 0.07f, 0.82f, 0.3f, 0.95f, 1.0f, 0.95f);
    // Banks down to each lower neighbour, or a skirt where the drawn ground ends.
    for (int k = 0; k < 6; ++k) {
      const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k), a1 = a0 + kPi / 3.0f, am = (a0 + a1) * 0.5f;
      const float mx = x + std::cos(am) * kSqrt3, mz = z + std::sin(am) * kSqrt3;
      const int nq = static_cast<int>(std::lround(kSqrt3 / 3.0f * mx - mz / 3.0f)), nr = static_cast<int>(std::lround(2.0f / 3.0f * mz));
      auto it = height.find({nq, nr});
      const float below = it == height.end() ? y - 0.6f : it->second;
      if (below >= y - 1e-3f) continue;
      const bool retaining = y - below > 0.3f && g.use != "water" && it != height.end();
      const Rgb sc = retaining ? Rgb{0.55f, 0.53f, 0.5f} : Rgb{col.r * 0.62f, col.g * 0.55f, col.b * 0.45f};
      b.quad({x + std::cos(a0), y, z + std::sin(a0)}, {x + std::cos(a1), y, z + std::sin(a1)}, {x + std::cos(a1), below, z + std::sin(a1)},
             {x + std::cos(a0), below, z + std::sin(a0)}, {std::cos(am), 0, std::sin(am)}, pack(sc));
    }
  }
  // ---- plots ----
  double run_ms = 0;
  uint32_t failed = 0;
  std::map<std::string, uint32_t> drawn;
  std::string first_error;
  // Built pieces by their values: shared builds are built once.
  std::map<std::string, std::pair<std::shared_ptr<const runtime::Mesh>, std::shared_ptr<const physics::Collision>>> built_cache;
  std::map<std::string, std::vector<runtime::FirePoint>> fire_cache;
  std::vector<runtime::FirePoint> fires;
  auto run_fill = [&](const std::string &pipe, nlohmann::json values, vec3 at, float yaw, const char *what) {
    if (!defs.count(pipe)) return false;
    // The shared values (season, wind, a theme) on every pipeline that has them.
    for (auto it = st.shared.begin(); it != st.shared.end(); ++it)
      if (defs[pipe].param(it.key())) values[it.key()] = it.value();
    const std::string key = pipe + values.dump();
    auto hit = built_cache.find(key);
    if (hit == built_cache.end()) {
      const auto r0 = std::chrono::steady_clock::now();
      auto res = runner_.run(defs[pipe], values, false, false);
      run_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
      if (!res.ok || !res.frame.mesh) {
        ++failed;
        if (first_error.empty()) first_error = pipe + " " + values.dump() + ": " + res.error;
        built_cache[key] = {nullptr, nullptr};
        return false;
      }
      hit = built_cache.emplace(key, std::make_pair(res.frame.mesh, res.frame.collision)).first;
      fire_cache[key] = res.frame.fires;
    }
    if (!hit->second.first) return false;
    place(*hit->second.first, at, yaw, *mesh);
    {
      const float cy = std::cos(yaw), sy = std::sin(yaw);
      for (runtime::FirePoint f : fire_cache[key]) {
        const float x = f.pos[0], z = f.pos[2];
        f.pos[0] = at.x + x * cy + z * sy;
        f.pos[1] += at.y;
        f.pos[2] = at.z - x * sy + z * cy;
        fires.push_back(f);
      }
    }
    if (hit->second.second) physics::wireframe(*hit->second.second, collision, at, yaw);
    ++drawn[what];
    return true;
  };
  // The values that build on a lot for a plot's use: the lot's builds whose type the fill allows
  // for the use (any, if none), one picked by the plot's id.
  auto pick_build = [&](const Lot &lot, const nlohmann::json &fill, const std::string &use, uint32_t id) -> nlohmann::json {
    std::vector<const nlohmann::json *> fit;
    const nlohmann::json allowed = fill.value("uses", nlohmann::json::object()).value(use, nlohmann::json::array());
    const std::string type_param = fill.value("type_param", std::string("type"));
    for (const auto &bv : lot.builds)
      for (const auto &t : allowed)
        if (bv.value(type_param, std::string()) == t.get<std::string>()) fit.push_back(&bv);
    if (fit.empty())
      for (const auto &bv : lot.builds) fit.push_back(&bv);
    if (fit.empty()) return nlohmann::json();
    return *fit[hash32(id * 2654435761u + st.seed) % fit.size()];
  };
  auto by_wealth = [](const nlohmann::json &table, float w, const std::string &fallback) {
    for (const auto &sw : table)
      if (w >= sw[0].get<float>()) return sw[1].get<std::string>();
    return fallback;
  };
  const nlohmann::json fb = pl.fill.value("buildings", nlohmann::json::object()), fw = pl.fill.value("waterfronts", nlohmann::json::object()),
                       fc = pl.fill.value("civics", nlohmann::json::object()), fk = pl.fill.value("castles", nlohmann::json::object()),
                       fcr = pl.fill.value("crops", nlohmann::json::object());
  for (const auto &p : plots) {
    const int id = p["id"].get<int>();
    const std::string kind = p.value("kind", std::string("building"));
    const uint32_t lot = p.value("lot", 0u);
    const vec3 pos = vec(p["position"]);
    const float yaw = p.value("yaw_deg", 0.0f) * kPi / 180.0f;
    const std::string cls = p.value("size_class", std::string()), use = p.value("use", std::string());
    const float w = p.value("wealth", 0.0f);
    const auto &pc = p["cells"];
    if (st.outlines && kind != "road" && kind != "water") {
      std::set<std::pair<int, int>> mine;
      for (size_t k = 0; k + 1 < pc.size(); k += 2) mine.insert({pc[k].get<int>(), pc[k + 1].get<int>()});
      const bool ok = p.value("reached", true);
      for (const auto &[q, r] : mine) {
        const float x = cell_x(q, r), z = cell_z(r);
        for (int k = 0; k < 6; ++k) {
          const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(k), a1 = a0 + kPi / 3.0f, am = (a0 + a1) * 0.5f;
          const float mx = x + std::cos(am) * kSqrt3, mz = z + std::sin(am) * kSqrt3;
          const int nq = static_cast<int>(std::lround(kSqrt3 / 3.0f * mx - mz / 3.0f)), nr = static_cast<int>(std::lround(2.0f / 3.0f * mz));
          if (mine.count({nq, nr})) continue;
          const float y = pos.y + 0.04f;
          lines.push_back({{x + std::cos(a0), y, z + std::sin(a0)}, {x + std::cos(a1), y, z + std::sin(a1)}, kind == "waterfront" ? 0.4f : 1.0f,
                           ok ? (kind == "waterfront" ? 0.85f : 0.8f) : 0.25f, ok ? (kind == "waterfront" ? 1.0f : 0.3f) : 0.2f, ok ? 0.75f : 1.0f});
        }
      }
    }
    if (!st.fill) continue;
    if (kind == "road" && roads_filled) {
      // A road_v1 piece per cell: its kind by the road's class, its mask, half, and fences turned by
      // sixths to the smallest of their six turns (so pieces share builds), and the piece turned back.
      const std::string road_kind = fr.value("kinds", nlohmann::json::object()).value(p.value("road", std::string("lane")), std::string("street"));
      const auto &hs = p["heights"];
      const auto &es = p["edges"];
      const auto &ro = p["roles"];
      const auto halves = p.value("halves", nlohmann::json::array()), fences = p.value("fences", nlohmann::json::array()),
                 doors = p.value("doors", nlohmann::json::array());
      const pipeline::Param *half_param = defs[fr["pipeline"]].param("half");
      for (size_t k = 0; k * 2 + 1 < pc.size(); ++k) {
        const int q = pc[k * 2], r = pc[k * 2 + 1];
        const uint32_t world = es[k].get<uint32_t>();
        const uint32_t wh = k < halves.size() ? halves[k].get<uint32_t>() : 6u, wf = k < fences.size() ? fences[k].get<uint32_t>() : 0u,
                       wd = k < doors.size() ? doors[k].get<uint32_t>() : 0u;
        uint64_t best = ~0ull;
        int turn = 0;
        uint32_t cm = 0, ch = 6, cf = 0, cd = 0;
        for (int t = 0; t < 6; ++t) {
          uint32_t m = 0, f = 0, dd = 0;
          for (int d = 0; d < 6; ++d) {
            m |= ((world >> (3 * ((d + t) % 6))) & 7u) << (3 * d);
            f |= ((wf >> ((d + t) % 6)) & 1u) << d;
            dd |= ((wd >> ((d + t) % 6)) & 1u) << d;
          }
          const uint32_t h = wh < 6 ? static_cast<uint32_t>((static_cast<int>(wh) - t + 6) % 6) : 6u;
          const uint64_t key = static_cast<uint64_t>(m) | (static_cast<uint64_t>(f) << 18) | (static_cast<uint64_t>(h) << 24) | (static_cast<uint64_t>(dd) << 28);
          if (key < best) {
            best = key;
            turn = t;
            cm = m;
            ch = h;
            cf = f;
            cd = dd;
          }
        }
        static const char *roles[] = {"path", "bridge"};
        const uint32_t role = ro[k].get<uint32_t>();
        nlohmann::json v = {{fr.value("kind_param", std::string("kind")), road_kind},
                            {"mask", cm},
                            {"fences", cf},
                            {"doors", cd},
                            {"role", roles[role < 2 ? role : 0]},
                            {"seed", 1 + hash32(static_cast<uint32_t>(q * 92821 + r * 68917) ^ st.seed) % std::max(1u, fr.value("seeds", 3u))}};
        if (half_param && ch < half_param->values.size()) v["half"] = half_param->values[ch];
        run_fill(fr["pipeline"], v, {cell_x(q, r), hs[k].get<float>(), cell_z(r)}, static_cast<float>(turn) * kPi / 3.0f, "road_cells");
      }
      continue;
    }
    if (lot >= pl.lots.size() || pl.lots[lot].builds.empty()) continue;  // a plot without a fill: its outline only
    const Lot &L = pl.lots[lot];
    if (kind == "waterfront" && !fw.empty()) {
      nlohmann::json v = pick_build(L, fw, "harbour", static_cast<uint32_t>(id));
      v[fw.value("drop_param", std::string("drop"))] = p.value("drop", 0.8f);
      v["seed"] = 1 + static_cast<uint32_t>(id) % 4u;
      run_fill(fw.value("pipeline", ""), v, pos, yaw, "waterfronts");
      continue;
    }
    const bool household = cls == "solo" || cls == "family" || cls == "extended";
    if (kind == "building" && household && !fb.empty()) {
      nlohmann::json values = pick_build(L, fb, use, static_cast<uint32_t>(id));
      values[fb.value("stage_param", std::string("stage"))] = by_wealth(fb.value("stage_by_wealth", nlohmann::json::array()), w, "basic");
      values[fb.value("seed_param", std::string("seed"))] = 1 + (static_cast<uint32_t>(id) % std::max(1u, fb.value("seeds", 4u)));
      run_fill(fb.value("pipeline", ""), values, pos, yaw, "buildings");
    } else if (kind == "building" && (cls == "civic" || cls == "centre") && !fc.empty()) {
      nlohmann::json values = pick_build(L, fc, use, static_cast<uint32_t>(id));
      values[fc.value("grandeur_param", std::string("grandeur"))] = by_wealth(fc.value("grandeur_by_wealth", nlohmann::json::array()), w, "fine");
      values["seed"] = 1 + static_cast<uint32_t>(id) % 3u;
      run_fill(fc.value("pipeline", ""), values, pos, yaw, cls == "centre" ? "centres" : "civic");
    } else if (kind == "building" && cls == "field" && !fcr.empty()) {
      // A crop field: its lot's size, a crop picked from the fill's by the plot's id, its growth.
      nlohmann::json values = L.builds[0];
      const auto types = fcr.value("types", nlohmann::json::array());
      if (!types.empty()) values[fcr.value("type_param", std::string("type"))] = types[hash32(static_cast<uint32_t>(id) * 2654435761u + st.seed) % types.size()];
      if (fcr.contains("growth")) values[fcr.value("growth_param", std::string("growth"))] = fcr["growth"];
      values["seed"] = 1 + static_cast<uint32_t>(id) % 4u;
      run_fill(fcr.value("pipeline", ""), values, pos, yaw, "fields");
    } else if (kind == "building" && cls == "castle" && !fk.empty()) {
      nlohmann::json values = pick_build(L, fk, use, static_cast<uint32_t>(id));
      values["seed"] = 1 + static_cast<uint32_t>(id) % 3u;
      run_fill(fk.value("pipeline", ""), values, pos, yaw, use == "keep" ? "keep" : "castle_parts");
    }
  }
  // ---- lines: walls, the keep's wall, and fences, built as a game would, with gates where a wall
  // asks for them ----
  nlohmann::json line_report = nlohmann::json::array();
  const nlohmann::json fl = pl.fill.value("lines", nlohmann::json::object());
  if (st.lines && !fl.empty()) {
    for (const auto &ln : plan.value("lines", nlohmann::json::array())) {
      const std::string lk = ln.value("kind", std::string("wall"));
      const nlohmann::json spec = fl.value(lk, nlohmann::json::object());
      const std::string pipe = spec.value("pipeline", std::string()), kind_name = spec.value("kind", std::string());
      if (spec.empty() || !defs.count(pipe)) continue;
      const nlohmann::json net = ln.value("network", nlohmann::json::object());
      const size_t links = net.value("links", nlohmann::json::array()).size() / 2;
      // Long lines in chunks of about chunk_links links, each its own instance.
      const uint32_t chunks = std::max<uint32_t>(1, static_cast<uint32_t>((links + fl.value("chunk_links", 40u) - 1) / std::max(1u, fl.value("chunk_links", 40u))));
      const nlohmann::json gate = fl.value("gates", nlohmann::json::object()).value(lk, nlohmann::json::object());
      uint32_t drawn_chunks = 0, joins = 0, unit_links = 0, gates = 0;
      for (uint32_t c = 0; c < chunks; ++c) {
        nlohmann::json v = {{spec.value("kind_param", std::string("kind")), kind_name},
                            {spec.value("network_param", std::string("network")), net},
                            {"chunks", chunks},
                            {"chunk", c}};
        v.update(st.shared);
        const auto r0 = std::chrono::steady_clock::now();
        auto res = runner_.run(defs[pipe], v, false, false);
        run_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
        if (!res.ok || !res.frame.mesh) {
          ++failed;
          if (first_error.empty()) first_error = pipe + ": " + res.error;
          continue;
        }
        place(*res.frame.mesh, {0, 0, 0}, 0, *mesh);
        if (res.frame.collision) physics::wireframe(*res.frame.collision, collision);
        ++drawn_chunks;
        ++drawn[lk == "fence" ? "fence_chunks" : "wall_chunks"];
        joins += res.frame.facts.value("joins", 0u);
        unit_links += res.frame.facts.value("links", 0u);
        for (const auto &g : res.frame.places) {
          if (gate.empty()) break;
          const nlohmann::json gv = {{gate.value("type_param", std::string("type")), gate.value("type", std::string())},
                                     {gate.value("wall_param", std::string("wall")), kind_name},
                                     {gate.value("width_param", std::string("width")), g.size}};
          if (run_fill(gate.value("pipeline", ""), gv, g.at, g.yaw_deg * kPi / 180.0f, "gates")) ++gates;
        }
      }
      line_report.push_back({{"kind", lk},
                             {"nodes", net.value("nodes", nlohmann::json::array()).size() / 2},
                             {"links", links},
                             {"joins", joins},
                             {"gates", gates},
                             {"chunks", drawn_chunks}});
      (void)unit_links;
    }
  }
  // ---- nature on the open land ----
  uint32_t nature_n = 0;
  std::string nature_error;
  if (!st.nature.empty() && !nature_regions.empty())
    nature_n = plot::grow(paths_, runner_, wasm_, st.nature, nature_regions, st.seed, st.shared, *mesh, nature_error);
  // ---- lines: entrances (a gate tile's ring), and the navigation mesh ----
  const auto &ent = plan.value("entrances", nlohmann::json::array());
  for (size_t k = 0; k + 1 < ent.size(); k += 2) {
    const int q = ent[k], r = ent[k + 1];
    auto it = height.find({q, r});
    const float y = it == height.end() ? 0 : it->second;
    for (int sgm = 0; sgm < 24; ++sgm) {
      const float a0 = 2 * kPi * sgm / 24, a1 = 2 * kPi * (sgm + 1) / 24;
      lines.push_back({{cell_x(q, r) + 1.2f * std::cos(a0), y + 0.1f, cell_z(r) + 1.2f * std::sin(a0)},
                       {cell_x(q, r) + 1.2f * std::cos(a1), y + 0.1f, cell_z(r) + 1.2f * std::sin(a1)}, 0.3f, 0.95f, 1.0f, 1.0f});
    }
  }
  if (st.nav) {
    const auto &v = plan.value("nav_vertices", nlohmann::json::array());
    const auto &polys = plan.value("nav_polys", nlohmann::json::array());
    const auto &counts = plan.value("nav_counts", nlohmann::json::array());
    const auto &flags = plan.value("nav_flags", nlohmann::json::array());
    std::vector<size_t> first;
    size_t at = 0;
    for (const auto &c : counts) {
      first.push_back(at);
      at += c.get<size_t>();
    }
    for (size_t pi = 0; pi < counts.size(); ++pi) {
      const bool cart = pi < flags.size() && (flags[pi].get<uint32_t>() & 2u);
      const size_t n = counts[pi].get<size_t>();
      for (size_t k = 0; k < n; ++k) {
        const vec3 a = vec(v[polys[first[pi] + k].get<size_t>()]), c = vec(v[polys[first[pi] + (k + 1) % n].get<size_t>()]);
        lines.push_back({{a.x, a.y + 0.06f, a.z}, {c.x, c.y + 0.06f, c.z}, cart ? 0.35f : 0.3f, cart ? 0.95f : 0.75f, cart ? 0.95f : 0.5f, 0.45f});
      }
    }
    const auto &links = plan.value("nav_links", nlohmann::json::array());
    auto centre = [&](size_t pi) {
      vec3 sum{0, 0, 0};
      const size_t n = counts[pi].get<size_t>();
      for (size_t k = 0; k < n; ++k) {
        const vec3 a = vec(v[polys[first[pi] + k].get<size_t>()]);
        sum = {sum.x + a.x / static_cast<float>(n), sum.y + a.y / static_cast<float>(n), sum.z + a.z / static_cast<float>(n)};
      }
      return sum;
    };
    for (size_t k = 0; k + 1 < links.size(); k += 2) {
      const vec3 a = centre(links[k].get<size_t>()), c = centre(links[k + 1].get<size_t>());
      lines.push_back({{a.x, a.y + 0.1f, a.z}, {c.x, c.y + 0.1f, c.z}, 0.3f, 0.6f, 1.0f, 0.5f});
    }
  }
  std::string summary;
  for (const auto &[what, n] : drawn) summary += (summary.empty() ? "" : ", ") + std::to_string(n) + " " + what;
  core::log().add("plan", pl.name + ": composed " + (summary.empty() ? std::string("nothing") : summary) +
                              (st.nature.empty() ? "" : ", " + std::to_string(nature_n) + " plants from " + st.nature) + "; " +
                              std::to_string(mesh->indices.size() / 3) + " triangles (" + std::to_string(built_cache.size()) + " builds, " +
                              std::to_string(static_cast<int>(run_ms)) + " ms running the fill)");
  std::lock_guard<std::mutex> lock(mutex_);
  picture_.mesh = mesh;
  picture_.lines = std::move(lines);
  picture_.collision = std::move(collision);
  picture_.fires = std::move(fires);
  picture_.report = report_;
  nlohmann::json d = drawn;
  d["nature"] = nature_n;
  d["builds"] = built_cache.size();
  picture_.report["drawn"] = d;
  picture_.report["lines_laid"] = line_report;
  picture_.version = version;
  picture_.building = false;
  picture_.error = !error.empty() ? error
                                  : (failed ? std::to_string(failed) + " pieces failed to build; first: " + first_error
                                            : (!nature_error.empty() ? "nature: " + nature_error : ""));
  composing_ = 0;
}


}  // namespace plan
