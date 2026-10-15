#include "plot/grow.h"

#include <cmath>
#include <map>
#include <memory>

#include "json/registry.h"
#include "plot/plotter.h"

namespace plot {

namespace {

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

}  // namespace

uint32_t grow(const forge::Paths &paths, pipeline::Runner &runner, wasm::Runner &wasm, const std::string &plotter,
              const nlohmann::json &regions, uint32_t seed, const nlohmann::json &shared, runtime::Mesh &mesh, std::string &error) {
  Plotter pl;
  if (!load(paths, runner, plotter, pl, error)) return 0;
  const auto *in = foundry::json::find_channel("plot_request");
  const auto *out = foundry::json::find_channel("plot_result");
  if (!in || !out) {
    error = "the plot channels aren't in the registry";
    return 0;
  }
  const nlohmann::json req = {{"config", pl.config},       {"ground", {{"regions", regions}}}, {"occupants", nlohmann::json::array()},
                              {"exclusions", nlohmann::json::array()}, {"seed", seed},          {"mode", "seed"},
                              {"max", 100000},             {"focus", {0, 0, 0}},             {"use_focus", false},
                              {"want_cells", false}};
  std::vector<uint8_t> bytes;
  try {
    in->from_json(req, bytes);
  } catch (const std::exception &e) {
    error = std::string("the request doesn't match plot_request: ") + e.what();
    return 0;
  }
  wasm::RunResult w = wasm.run(forge::wasm_path(paths, pl.system), bytes);
  nlohmann::json res;
  if (w.status != 0 || !out->to_json(w.output.data(), w.output.size(), res)) {
    error = pl.system + " failed: " + w.error;
    return 0;
  }
  std::map<std::string, pipeline::Definition> defs;
  std::map<std::string, std::shared_ptr<const runtime::Mesh>> built;  // by kind and values
  nlohmann::json sh = pl.shared;
  for (auto it = shared.begin(); it != shared.end(); ++it)
    if (sh.contains(it.key())) sh[it.key()] = it.value();
  uint32_t n = 0;
  for (const auto &p : res.value("placements", nlohmann::json::array())) {
    const uint32_t k = p.value("kind", 0u);
    if (k >= pl.kinds.size()) continue;
    const Kind &kind = pl.kinds[k];
    if (!defs.count(kind.pipeline)) {
      pipeline::Definition d;
      if (!pipeline::load(paths, kind.pipeline, d, error)) return n;
      defs[kind.pipeline] = d;
    }
    const nlohmann::json v = values_for(pl, kind, p.value("seed", 1u), false, sh);
    const std::string key = kind.pipeline + v.dump();
    if (!built.count(key)) {
      auto r = runner.run(defs[kind.pipeline], v, false, false);
      built[key] = r.ok ? r.frame.mesh : nullptr;
    }
    if (!built[key]) continue;
    const auto &at = p["position"];
    place(*built[key], {at[0].get<float>(), at[1].get<float>(), at[2].get<float>()}, p.value("yaw", 0.0f), mesh);
    ++n;
  }
  return n;
}

}  // namespace plot
