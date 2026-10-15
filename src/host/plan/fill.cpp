#include "plan/fill.h"

#include <cmath>
#include <map>

#include "json/registry.h"
#include "pipeline/config.h"
#include "plan/planner.h"

namespace plan {

namespace {

constexpr float kSqrt3 = 1.7320508f, kPi = 3.14159265f;
float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
float cell_z(int r) { return 1.5f * static_cast<float>(r); }
uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
foundry::vec3 vec(const nlohmann::json &j) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }

}  // namespace

bool settle(const forge::Paths &paths, pipeline::Runner &runner, wasm::Runner &wasm, const std::string &planner, uint32_t seed,
            const nlohmann::json &overrides, const nlohmann::json &shared, Settlement &out, std::string &error) {
  out = Settlement();
  Planner pl;
  if (!load(paths, runner, planner, pl, error)) return false;
  nlohmann::json cfg = pl.config;
  cfg.update(overrides);
  const auto *in = foundry::json::find_channel("plan_request");
  const auto *outc = foundry::json::find_channel("plan_plan");
  if (!in || !outc) {
    error = "the plan channels aren't in the registry";
    return false;
  }
  const nlohmann::json req = {{"config", cfg}, {"seed", seed}, {"plots", 0}, {"tier", "town"}};
  std::vector<uint8_t> bytes;
  try {
    in->from_json(req, bytes);
  } catch (const std::exception &e) {
    error = std::string("the request doesn't match plan_request: ") + e.what();
    return false;
  }
  wasm::RunResult w = wasm.run(forge::wasm_path(paths, pl.system), bytes);
  if (w.status != 0 || !outc->to_json(w.output.data(), w.output.size(), out.plan)) {
    error = pl.system + " failed: " + w.error;
    return false;
  }
  const nlohmann::json &plan = out.plan;
  out.radius_m = static_cast<float>(plan.value("radius", 0u)) * 1.65f;
  for (const auto &c : plan.value("cells", nlohmann::json::array())) {
    const auto &u = c["use"];
    if (u.is_string() ? u.get<std::string>() == "open" : u.get<uint32_t>() == 6u) continue;
    out.built.push_back(c["q"].get<int32_t>());
    out.built.push_back(c["r"].get<int32_t>());
  }
  std::map<std::string, std::string> surfaces;  // road kind -> surface
  auto surface_of = [&](const std::string &kind) {
    auto it = surfaces.find(kind);
    if (it != surfaces.end()) return it->second;
    const nlohmann::json e = pipeline::config::entry(paths, "road_kinds", kind);
    const std::string s = e.is_object() ? e["value"].value("surface", std::string("dirt")) : std::string("dirt");
    surfaces[kind] = s;
    return s;
  };
  // The fill's pipelines, for their parameters.
  std::map<std::string, pipeline::Definition> defs;
  auto def = [&](const std::string &name) -> const pipeline::Definition * {
    if (name.empty()) return nullptr;
    auto it = defs.find(name);
    if (it == defs.end()) {
      pipeline::Definition d;
      std::string e;
      if (!pipeline::load(paths, name, d, e)) return nullptr;
      it = defs.emplace(name, d).first;
    }
    return &it->second;
  };
  auto add = [&](const std::string &pipe, nlohmann::json values, foundry::vec3 at, float yaw, const char *what) {
    const pipeline::Definition *d = def(pipe);
    if (!d) return;
    for (auto it = shared.begin(); it != shared.end(); ++it)
      if (d->param(it.key())) values[it.key()] = it.value();
    out.fills.push_back({pipe, values, at, yaw, what});
  };
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
    return *fit[hash32(id * 2654435761u + seed) % fit.size()];
  };
  auto by_wealth = [](const nlohmann::json &table, float w, const std::string &fallback) {
    for (const auto &sw : table)
      if (w >= sw[0].get<float>()) return sw[1].get<std::string>();
    return fallback;
  };
  const nlohmann::json fr = pl.fill.value("roads", nlohmann::json::object()), fb = pl.fill.value("buildings", nlohmann::json::object()),
                       fw = pl.fill.value("waterfronts", nlohmann::json::object()), fc = pl.fill.value("civics", nlohmann::json::object()),
                       fk = pl.fill.value("castles", nlohmann::json::object()), fcr = pl.fill.value("crops", nlohmann::json::object());
  for (const auto &p : plan.value("plots", nlohmann::json::array())) {
    const int id = p["id"].get<int>();
    const std::string kind = p.value("kind", std::string("building"));
    const uint32_t lot = p.value("lot", 0u);
    const foundry::vec3 pos = vec(p["position"]);
    const float yaw = p.value("yaw_deg", 0.0f) * kPi / 180.0f;
    const std::string cls = p.value("size_class", std::string()), use = p.value("use", std::string());
    const float w = p.value("wealth", 0.0f);
    const auto &pc = p["cells"];
    if (kind == "road" && def(fr.value("pipeline", ""))) {
      // A road piece per cell, turned to the smallest of its six turns (so pieces share builds).
      const std::string road_kind = fr.value("kinds", nlohmann::json::object()).value(p.value("road", std::string("lane")), std::string("street"));
      const auto &hs = p["heights"];
      const auto &es = p["edges"];
      const auto &ro = p["roles"];
      const auto halves = p.value("halves", nlohmann::json::array()), fences = p.value("fences", nlohmann::json::array()),
                 doors = p.value("doors", nlohmann::json::array());
      const pipeline::Param *half_param = def(fr["pipeline"])->param("half");
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
        nlohmann::json v = {{fr.value("kind_param", std::string("kind")), road_kind}, {"mask", cm}, {"fences", cf}, {"doors", cd},
                            {"role", roles[role < 2 ? role : 0]},
                            {"seed", 1 + hash32(static_cast<uint32_t>(q * 92821 + r * 68917) ^ seed) % std::max(1u, fr.value("seeds", 3u))}};
        if (half_param && ch < half_param->values.size()) v["half"] = half_param->values[ch];
        add(fr["pipeline"], v, {cell_x(q, r), hs[k].get<float>(), cell_z(r)}, static_cast<float>(turn) * kPi / 3.0f, "road_cells");
        out.streets.push_back(q);
        out.streets.push_back(r);
        out.street_surfaces.push_back(surface_of(road_kind));
      }
      continue;
    }
    if (lot >= pl.lots.size() || pl.lots[lot].builds.empty()) continue;
    const Lot &L = pl.lots[lot];
    if (kind == "waterfront" && !fw.empty()) {
      nlohmann::json v = pick_build(L, fw, "harbour", static_cast<uint32_t>(id));
      v[fw.value("drop_param", std::string("drop"))] = p.value("drop", 0.8f);
      v["seed"] = 1 + static_cast<uint32_t>(id) % 4u;
      add(fw.value("pipeline", ""), v, pos, yaw, "waterfronts");
      continue;
    }
    const bool household = cls == "solo" || cls == "family" || cls == "extended";
    if (kind == "building" && household && !fb.empty()) {
      nlohmann::json values = pick_build(L, fb, use, static_cast<uint32_t>(id));
      values[fb.value("stage_param", std::string("stage"))] = by_wealth(fb.value("stage_by_wealth", nlohmann::json::array()), w, "basic");
      values[fb.value("seed_param", std::string("seed"))] = 1 + (static_cast<uint32_t>(id) % std::max(1u, fb.value("seeds", 4u)));
      add(fb.value("pipeline", ""), values, pos, yaw, "buildings");
    } else if (kind == "building" && (cls == "civic" || cls == "centre") && !fc.empty()) {
      nlohmann::json values = pick_build(L, fc, use, static_cast<uint32_t>(id));
      values[fc.value("grandeur_param", std::string("grandeur"))] = by_wealth(fc.value("grandeur_by_wealth", nlohmann::json::array()), w, "fine");
      values["seed"] = 1 + static_cast<uint32_t>(id) % 3u;
      add(fc.value("pipeline", ""), values, pos, yaw, cls == "centre" ? "centres" : "civic");
    } else if (kind == "building" && cls == "field" && !fcr.empty()) {
      nlohmann::json values = L.builds[0];
      const auto types = fcr.value("types", nlohmann::json::array());
      if (!types.empty()) values[fcr.value("type_param", std::string("type"))] = types[hash32(static_cast<uint32_t>(id) * 2654435761u + seed) % types.size()];
      if (fcr.contains("growth")) values[fcr.value("growth_param", std::string("growth"))] = fcr["growth"];
      values["seed"] = 1 + static_cast<uint32_t>(id) % 4u;
      add(fcr.value("pipeline", ""), values, pos, yaw, "fields");
    } else if (kind == "building" && cls == "castle" && !fk.empty()) {
      nlohmann::json values = pick_build(L, fk, use, static_cast<uint32_t>(id));
      values["seed"] = 1 + static_cast<uint32_t>(id) % 3u;
      add(fk.value("pipeline", ""), values, pos, yaw, use == "keep" ? "keep" : "castle_parts");
    }
  }
  // Lines: walls and fences, in chunks of about chunk_links links, each its own instance.
  const nlohmann::json fl = pl.fill.value("lines", nlohmann::json::object());
  for (const auto &ln : plan.value("lines", nlohmann::json::array())) {
    const std::string lk = ln.value("kind", std::string("wall"));
    const nlohmann::json spec = fl.value(lk, nlohmann::json::object());
    const std::string pipe = spec.value("pipeline", std::string()), kind_name = spec.value("kind", std::string());
    if (spec.empty() || !def(pipe)) continue;
    const nlohmann::json net = ln.value("network", nlohmann::json::object());
    const size_t links = net.value("links", nlohmann::json::array()).size() / 2;
    const uint32_t per = std::max(1u, fl.value("chunk_links", 40u));
    const uint32_t chunks = std::max<uint32_t>(1, static_cast<uint32_t>((links + per - 1) / per));
    for (uint32_t c = 0; c < chunks; ++c) {
      nlohmann::json v = {{spec.value("kind_param", std::string("kind")), kind_name},
                          {spec.value("network_param", std::string("network")), net},
                          {"chunks", chunks},
                          {"chunk", c}};
      add(pipe, v, {0, 0, 0}, 0, lk == "fence" ? "fences" : "walls");
    }
  }
  return true;
}

}  // namespace plan
