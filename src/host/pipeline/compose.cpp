#include "pipeline/compose.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "core/log.h"
#include "physics/toy.h"

namespace pipeline {

namespace {

std::mutex g_grower_mutex;
NatureGrower g_grower;

void place(const runtime::Mesh &m, foundry::vec3 at, float yaw, runtime::Mesh &out) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const uint32_t base = static_cast<uint32_t>(out.positions.size());
  for (const auto &p : m.positions) out.positions.push_back({at.x + p.x * cy + p.z * sy, at.y + p.y, at.z - p.x * sy + p.z * cy});
  for (const auto &n : m.normals) out.normals.push_back({n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy});
  for (size_t i = m.normals.size(); i < m.positions.size(); ++i) out.normals.push_back({0, 1, 0});
  if (m.colors.size() == m.positions.size()) out.colors.insert(out.colors.end(), m.colors.begin(), m.colors.end());
  else out.colors.insert(out.colors.end(), m.positions.size(), 0xFFB3B3B3u);
  for (uint32_t i : m.indices) out.indices.push_back(base + i);
}

// The parent's values a child pipeline shares: those named (or every one it has a parameter for).
void share(const Definition &child, const nlohmann::json &parent, const nlohmann::json &names, nlohmann::json &v) {
  if (names.is_array()) {
    for (const auto &n : names)
      if (n.is_string() && parent.contains(n.get<std::string>()) && child.param(n.get<std::string>())) v[n.get<std::string>()] = parent[n.get<std::string>()];
    return;
  }
  for (auto it = parent.begin(); it != parent.end(); ++it)
    if (child.param(it.key()) && !v.contains(it.key())) v[it.key()] = it.value();
}

}  // namespace

// Each kind by its pipeline: the figure's species, age, and seed set on that pipeline's parameters,
// built once per distinct set of values and placed at every figure that asks for it.
uint32_t fill_figures(Runner &runner, const nlohmann::json &block, const nlohmann::json &figures, const nlohmann::json &parent,
                      runtime::Mesh &mesh, std::vector<PlacedBodies> &bodies, nlohmann::json &report) {
  std::map<std::string, Definition> defs;
  std::map<std::string, std::shared_ptr<const Runner::Result>> built;
  uint32_t placed = 0, unknown = 0, failed = 0;
  std::string error;
  const nlohmann::json kinds = block.value("kinds", nlohmann::json::object());
  for (const auto &f : figures) {
    const std::string kind = f.value("kind", nlohmann::json("tree")).is_string() ? f["kind"].get<std::string>() : "tree";
    if (!kinds.contains(kind)) {
      ++unknown;
      continue;
    }
    const nlohmann::json &spec = kinds[kind];
    const std::string pipe = spec.value("pipeline", "");
    if (!defs.count(pipe)) {
      Definition d;
      if (!load(runner.paths(), pipe, d, error)) {
        report["figures_error"] = pipe + ": " + error;
        return placed;
      }
      defs[pipe] = d;
    }
    const Definition &d = defs[pipe];
    nlohmann::json v = spec.value("params", nlohmann::json::object());
    share(d, parent, spec.value("shared", nlohmann::json()), v);
    const std::string species_param = spec.value("species", "species");
    const Param *sp = d.param(species_param);
    const std::string species = f.value("species", "");
    if (!sp || std::find(sp->values.begin(), sp->values.end(), species) == sp->values.end()) {
      ++unknown;
      continue;
    }
    v[species_param] = species;
    const Param *ap = d.param(spec.value("age", "age"));
    if (ap && !ap->values.empty()) v[ap->name] = ap->values[std::min<size_t>(f.value("age", 0u), ap->values.size() - 1)];
    const Param *seed = d.param(spec.value("seed", "seed"));
    const uint32_t distinct = spec.value("distinct_seeds", 4u);
    if (seed) v[seed->name] = 1 + f.value("seed", 0u) % std::max(1u, distinct);
    const std::string key = pipe + v.dump();
    if (!built.count(key)) built[key] = std::make_shared<Runner::Result>(runner.run(d, v, false, false));
    const Runner::Result &r = *built[key];
    if (!r.ok || !r.frame.mesh) {
      ++failed;
      continue;
    }
    const auto &p = f["position"];
    const foundry::vec3 at{p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
    const float yaw = f.value("yaw", 0.0f) * 3.14159265f / 180.0f;
    place(*r.frame.mesh, at, yaw, mesh);
    if (r.frame.collision) bodies.push_back({r.frame.collision->bodies, at, yaw});
    ++placed;
  }
  report["figures"] = placed;
  report["figure_builds"] = built.size();
  if (unknown) report["figures_unknown"] = unknown;
  if (failed) report["figures_failed"] = failed;
  return placed;
}

void set_nature_grower(NatureGrower grower) {
  std::lock_guard<std::mutex> lock(g_grower_mutex);
  g_grower = std::move(grower);
}

bool composes(const Definition &def) { return !def.nature.empty() || !def.surround.empty() || !def.figures.empty(); }

Composite compose(Runner &runner, const Definition &def, const Frame &frame, const nlohmann::json &values, const ComposeOptions &o) {
  Composite c;
  if (!frame.mesh) return c;
  const auto t0 = std::chrono::steady_clock::now();
  auto mesh = std::make_shared<runtime::Mesh>(*frame.mesh);
  const nlohmann::json extras = frame.extras ? *frame.extras : nlohmann::json::object();
  const nlohmann::json full = runner.complete(def, values);

  // The surround: its pipeline round the model, given the model's hull inline.
  if (o.surround && !def.surround.empty()) {
    const std::string pipe = def.surround.value("pipeline", ""), field = def.surround.value("hull", "");
    Definition sd;
    std::string error;
    if (!extras.contains(field)) {
      c.report["surround_error"] = "the model has no " + field;
    } else if (!load(runner.paths(), pipe, sd, error)) {
      c.report["surround_error"] = pipe + ": " + error;
    } else {
      nlohmann::json v = def.surround.value("params", nlohmann::json::object());
      share(sd, full, def.surround.value("shared", nlohmann::json()), v);
      v.update(o.surround_values);
      const std::string hp = sd.terrain.value("hull", "");
      if (sd.param(hp)) v[hp] = extras[field];
      auto r = runner.run(sd, v, false, false);
      if (r.ok && r.frame.mesh) {
        // Without its stand-in floor: the model is the ground inside.
        runtime::Mesh m = *r.frame.mesh;
        const size_t floor = r.frame.facts.value("terrain_floor_triangles", size_t(0));
        m.indices.resize(m.indices.size() - std::min(m.indices.size(), floor * 3));
        place(m, {0, 0, 0}, 0, *mesh);
        if (r.frame.collision) {
          physics::wireframe(*r.frame.collision, c.wire);
          c.bodies.push_back({r.frame.collision->bodies, {0, 0, 0}, 0});
        }
        c.guides.insert(c.guides.end(), r.frame.guides.begin(), r.frame.guides.end());
        c.fires = r.frame.fires;
        c.own_ground = r.frame.terrain;
        c.reach = r.frame.facts.value("radius", 0.0f);
        c.report["surround"] = pipe;
        c.report["surround_triangles"] = m.indices.size() / 3;
        if (o.figures && !sd.figures.empty() && r.frame.extras && r.frame.extras->contains(sd.figures.value("field", "")))
          fill_figures(runner, sd.figures, (*r.frame.extras)[sd.figures.value("field", "")], runner.complete(sd, v), *mesh, c.bodies, c.report);
      } else {
        c.report["surround_error"] = pipe + ": " + r.error;
      }
    }
  }
  // The model's own figures.
  if (o.figures && !def.figures.empty() && extras.contains(def.figures.value("field", "")))
    fill_figures(runner, def.figures, extras[def.figures.value("field", "")], full, *mesh, c.bodies, c.report);
  // The plotter over its open land.
  if (o.nature && !def.nature.empty() && extras.contains(def.nature.value("regions", ""))) {
    NatureGrower grow;
    {
      std::lock_guard<std::mutex> lock(g_grower_mutex);
      grow = g_grower;
    }
    const std::string plotter = o.plotter.empty() ? def.nature.value("plotter", "") : o.plotter;
    const nlohmann::json &regions = extras[def.nature.value("regions", "")];
    std::string error;
    if (!grow) {
      c.report["nature_error"] = "no plotters here";
    } else {
      const uint32_t n = grow(plotter, regions.value("regions", nlohmann::json::array()), full.value("seed", 1u), full, *mesh, error);
      c.report["nature"] = n;
      c.report["plotter"] = plotter;
      if (!error.empty()) c.report["nature_error"] = error;
    }
  }
  c.mesh = mesh;
  c.report["triangles"] = mesh->indices.size() / 3;
  c.report["milliseconds"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return c;
}

Composer::~Composer() {
  if (worker_.joinable()) worker_.join();
}

void Composer::want(const Definition &def, const Frame &frame, const nlohmann::json &values, const ComposeOptions &o) {
  const std::string key = def.name + "|" + std::to_string(frame.model_version) + "|" + values.dump() + "|" + std::to_string(o.nature) + o.plotter +
                          std::to_string(o.surround) + std::to_string(o.figures) + o.surround_values.dump();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == key_ || busy_) return;  // a newer want is taken up when this one finishes
    key_ = key;
  }
  if (worker_.joinable()) worker_.join();
  busy_ = true;
  const int gen = ++generation_;
  worker_ = std::thread([this, def, frame, values, o, gen] {
    Composite c = compose(runner_, def, frame, values, o);
    std::lock_guard<std::mutex> lock(mutex_);
    if (gen == generation_) {
      c.version = latest_.version + 1;
      latest_ = std::move(c);
      core::log().add("pipeline", def.name + ": composed " + latest_.report.dump());
    }
    busy_ = false;
  });
}

Composite Composer::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

}  // namespace pipeline
