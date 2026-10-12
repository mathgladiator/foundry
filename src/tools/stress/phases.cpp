// The four phases (design/quality.perf.md): diversity, forest, events, and
// soak. None of them knows what pipeline it runs: each works from the
// binding's parameter table alone.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "platform.h"

namespace stress {

namespace {

foundry_diag_counters counters(Ctx &ctx) {
  foundry_diag_counters c;
  foundry_diag_counters_get(ctx.rt, &c);
  return c;
}

std::string hex(uint64_t v) { return fmt("%016llx", static_cast<unsigned long long>(v)); }

double get(const json &phase, const char *key, double fallback) {
  return phase.contains(key) && phase[key].is_number() ? phase[key].get<double>() : fallback;
}

std::vector<uint32_t> counts_of(const json &phase, std::vector<uint32_t> fallback) {
  if (!phase.contains("counts")) return fallback;
  std::vector<uint32_t> out;
  for (const auto &c : phase["counts"]) out.push_back(c.get<uint32_t>());
  return out;
}

// Runs frames until every instance has its build (or the limit passes); returns sim seconds.
double wait_ready(Ctx &ctx, double limit_seconds) {
  const double start = ctx.sim_time;
  while (ctx.sim_time - start < limit_seconds) {
    foundry_diag_counters c = counters(ctx);
    if (c.waiting == 0 && c.cache_pending == 0) break;
    ctx.frame();
  }
  return ctx.sim_time - start;
}

// Destroys everything, waits out the release timer, and releases what's left.
void clear(Ctx &ctx, std::vector<foundry_instance> &handles) {
  for (foundry_instance h : handles) ctx.destroy(h);
  handles.clear();
  for (int i = 0; i < 300; ++i) {  // let pending builds finish
    if (counters(ctx).cache_pending == 0) break;
    ctx.frame(false);
  }
  for (int i = 0; i < 10; ++i) ctx.frame();
  foundry_diag_flush(ctx.rt);
  ctx.journal.write({{"op", "flush"}});
}

// Update parameters near a set's own, so instances of one set move on their own.
void jitter(Ctx &ctx, Config &c, Rng &rng) {
  for (uint32_t p : ctx.update_params()) {
    const auto &d = ctx.pipeline->params[p];
    if (d.kind != STRESS_F32) continue;
    double span = d.max - d.min;
    c[p].f = static_cast<float>(std::clamp(c[p].f + (rng.unit() * 2 - 1) * 0.05 * span, d.min, d.max));
  }
}

json frame_json(const std::vector<FrameTime> &frames) {
  std::vector<double> total, update, prepare, draw, wait, sync;
  for (const auto &f : frames) {
    if (f.gpu_sync >= 0) {
      sync.push_back(f.gpu_sync);
      continue;  // a synchronous frame isn't a normal one
    }
    total.push_back(f.total);
    update.push_back(f.update);
    prepare.push_back(f.prepare);
    draw.push_back(f.draw);
    wait.push_back(f.gpu_wait);
  }
  json j = {{"total_ms", to_json(stats(total))}, {"update_ms", to_json(stats(update))},
            {"prepare_ms", to_json(stats(prepare))}, {"draw_ms", to_json(stats(draw))},
            {"gpu_wait_ms", to_json(stats(wait))}};
  if (!sync.empty()) j["gpu_frame_ms"] = to_json(stats(sync));
  return j;
}

std::vector<double> totals(const std::vector<FrameTime> &frames) {
  std::vector<double> v;
  for (const auto &f : frames)
    if (f.gpu_sync < 0) v.push_back(f.total);
  return v;
}

void snap(Ctx &ctx, const std::string &name) {
  if (!ctx.snapshot_dir.empty()) ctx.snapshot(ctx.snapshot_dir + "/" + name + ".png");
}

std::vector<float> clock_set(const Ctx &ctx, double time, double event) {
  std::vector<float> c(ctx.clock_count(), static_cast<float>(event));
  c[0] = static_cast<float>(time);
  return c;
}

void fail(std::vector<Failure> &failures, Ctx &ctx, const std::string &phase, const std::string &check,
          const std::string &detail, const json &config = nullptr) {
  ctx.journal.flush();
  failures.push_back({phase, check, detail, config, ctx.journal.index()});
}

}  // namespace

std::vector<std::vector<float>> default_clocks(const Ctx &ctx) {
  return {clock_set(ctx, 1.0, -1), clock_set(ctx, 2.5, -1), clock_set(ctx, 3.0, 1.5)};
}

// --- diversity ------------------------------------------------------------------

PhaseResult run_diversity(Ctx &ctx, const json &phase, std::vector<Failure> &failures, const json *reference) {
  PhaseResult res;
  res.kind = "diversity";
  Rng rng(ctx.plan.seed, 11);
  std::vector<Config> configs;
  std::vector<std::vector<float>> clock_sets = default_clocks(ctx);
  if (reference) {
    for (const auto &c : (*reference)["configs"]) {
      Config cfg;
      if (!ctx.parse(c["values"], cfg)) {
        fail(failures, ctx, "diversity", "reference", "a reference config doesn't parse", c["values"]);
        continue;
      }
      configs.push_back(cfg);
    }
    clock_sets.clear();
    for (const auto &cs : (*reference)["clocks"]) clock_sets.push_back(cs.get<std::vector<float>>());
  } else {
    configs = sample(ctx, static_cast<size_t>(get(phase, "configs", 100)), rng);
  }
  const size_t n = configs.size();
  const double seconds = get(phase, "seconds", 30), wave_seconds = get(phase, "wave_seconds", 0.5);
  const size_t wave = static_cast<size_t>(get(phase, "wave", 10));
  const double retune = get(phase, "retune_seconds", 3), event_fraction = get(phase, "event_fraction", 0.2);
  const float spacing = static_cast<float>(get(phase, "spacing", 10));
  auto layout = hex_layout(n, spacing, rng, 0.2f);
  float extent = 0;
  for (auto &p : layout) extent = std::max({extent, std::fabs(p.first), std::fabs(p.second)});
  ctx.overview(extent + spacing, 16);

  std::vector<foundry_instance> handles(n, FOUNDRY_NO_INSTANCE);
  std::vector<double> spawned_at(n, 0), latency(n, -1);
  std::vector<double> fired_at(n, -1);
  std::vector<FrameTime> frames;
  size_t next = 0;
  const double start = ctx.sim_time;
  double next_wave = start, next_retune = start + retune, next_check = start + 1;
  bool fired = false, bad_reported = false;
  const auto events = ctx.events();
  while (ctx.sim_time - start < seconds || next < n) {
    if (next < n && ctx.sim_time >= next_wave) {
      for (size_t k = 0; k < wave && next < n; ++k, ++next) {
        float m[16];
        translation(layout[next].first, layout[next].second, static_cast<float>(rng.unit() * 6.283), m);
        handles[next] = ctx.spawn(configs[next], m);
        spawned_at[next] = now_ms();
        if (!handles[next]) fail(failures, ctx, "diversity", "spawn", "spawn refused the config", ctx.describe(configs[next]));
      }
      next_wave += wave_seconds;
    }
    frames.push_back(ctx.frame());
    for (size_t i = 0; i < next; ++i) {
      if (latency[i] >= 0 || !handles[i]) continue;
      foundry_diag_instance info;
      if (foundry_diag_instance_get(ctx.rt, handles[i], &info) == FOUNDRY_OK && info.ready) latency[i] = now_ms() - spawned_at[i];
    }
    if (ctx.sim_time >= next_retune) {  // the wind changes
      for (size_t i = 0; i < next; ++i)
        for (uint32_t p : ctx.update_params()) ctx.set(handles[i], p, ctx.random_value(p, rng));
      next_retune += retune;
    }
    if (!fired && !events.empty() && ctx.sim_time - start >= seconds / 3 && next >= n) {
      for (size_t i = 0; i < n; ++i)
        if (rng.unit() < event_fraction) {
          for (uint32_t e : events) ctx.trigger(handles[i], e);
          fired_at[i] = ctx.sim_time;
        }
      fired = true;
    }
    if (ctx.sim_time >= next_check) {
      uint64_t bad = foundry_diag_bad_poses(ctx.rt);
      if (bad && !bad_reported) {
        fail(failures, ctx, "diversity", "motion", fmt("%llu instances have a non-finite or runaway pose",
                                                       static_cast<unsigned long long>(bad)));
        bad_reported = true;
      }
      next_check += 1;
    }
    if (ctx.sim_time - start > seconds + 120) break;  // builds never finished
  }
  wait_ready(ctx, 60);
  ctx.frame();
  snap(ctx, "diversity");

  // Errors, geometry, and events that never settled.
  uint64_t failed = 0, unsettled = 0;
  std::vector<double> tris;
  for (size_t i = 0; i < n; ++i) {
    if (!handles[i]) continue;
    foundry_diag_instance info;
    foundry_diag_instance_get(ctx.rt, handles[i], &info);
    if (!info.ready || info.failed) {
      ++failed;
      fail(failures, ctx, "diversity", "errors", info.ready ? info.error : std::string("never built: ") + info.error,
           ctx.describe(configs[i]));
    }
    tris.push_back(info.triangles);
    if (fired_at[i] >= 0 && ctx.sim_time - fired_at[i] > 12 && !info.settled) {
      ++unsettled;
      fail(failures, ctx, "diversity", "settle", "an event never settled", ctx.describe(configs[i]));
    }
  }

  // Hashes at fixed clocks, with each config's own update parameters back in place.
  std::vector<uint64_t> models(n, 0);
  std::vector<std::vector<uint64_t>> poses(n);
  for (size_t i = 0; i < n; ++i) {
    if (!handles[i]) continue;
    for (uint32_t p : ctx.update_params()) ctx.set(handles[i], p, configs[i][p]);
    for (const auto &cs : clock_sets) {
      uint64_t m = 0, p = 0;
      foundry_diag_hash(ctx.rt, handles[i], cs.data(), static_cast<uint32_t>(cs.size()), &m, &p);
      models[i] = m;
      poses[i].push_back(p);
    }
  }
  json matched = nullptr;
  if (reference) {
    size_t ok = 0, k = 0;
    for (const auto &c : (*reference)["configs"]) {
      if (k >= n) break;
      bool same = c.contains("model") && c["model"] == hex(models[k]);
      for (size_t j = 0; same && j < poses[k].size(); ++j)
        same = c["poses"].size() > j && c["poses"][j] == hex(poses[k][j]);
      if (same) ++ok;
      else fail(failures, ctx, "diversity", "native = wasm", "native output differs from WebAssembly", c["values"]);
      ++k;
    }
    matched = {{"matched", ok}, {"of", n}};
  }

  // Determinism: regrow a sample from scratch and compare.
  const size_t twins = std::min(n, static_cast<size_t>(get(phase, "twins", 20)));
  std::vector<foundry_instance> live(handles.begin(), handles.end());
  live.erase(std::remove(live.begin(), live.end(), FOUNDRY_NO_INSTANCE), live.end());
  clear(ctx, live);
  std::vector<foundry_instance> twin_handles;
  for (size_t i = 0; i < twins; ++i) {
    float m[16];
    translation(layout[i].first, layout[i].second, 0, m);
    twin_handles.push_back(ctx.spawn(configs[i], m));
  }
  wait_ready(ctx, 60);
  size_t deterministic = 0;
  for (size_t i = 0; i < twins; ++i) {
    bool same = true;
    for (size_t j = 0; j < clock_sets.size(); ++j) {
      uint64_t m = 0, p = 0;
      foundry_diag_hash(ctx.rt, twin_handles[i], clock_sets[j].data(), static_cast<uint32_t>(clock_sets[j].size()), &m, &p);
      same = same && m == models[i] && p == poses[i][j];
    }
    if (same) ++deterministic;
    else fail(failures, ctx, "diversity", "deterministic", "regrowing the same config gave a different result",
              ctx.describe(configs[i]));
  }

  // Threads: the same frames with one update thread and with many give the same poses.
  const size_t thread_check = std::min(twins, static_cast<size_t>(get(phase, "thread_check", 10)));
  size_t thread_same = 0;
  {
    foundry_runtime_desc desc = {};
    desc.update_threads = 1;
    desc.build_threads = 1;
    foundry_runtime *solo = foundry_runtime_create(&desc);
    std::vector<foundry_instance> solo_handles;
    for (size_t i = 0; i < thread_check; ++i) {
      float m[16];
      translation(layout[i].first, layout[i].second, 0, m);
      solo_handles.push_back(ctx.pipeline->spawn(solo, configs[i].data(), m));
    }
    // Give both the same clocks: run the solo runtime until its builds are ready, then step both together.
    for (int k = 0; k < 6000; ++k) {
      foundry_diag_counters c;
      foundry_diag_counters_get(solo, &c);
      if (c.waiting == 0 && c.cache_pending == 0) break;
      foundry_update(solo, 0);
      SDL_Delay(1);
    }
    // Fresh instances on the main runtime (their builds are cached by the twins), spawned with the solo ones'
    // clocks at zero, so both step through the same times.
    std::vector<foundry_instance> mains;
    for (size_t i = 0; i < thread_check; ++i) {
      float m[16];
      translation(layout[i].first, layout[i].second, 0, m);
      mains.push_back(ctx.spawn(configs[i], m));
    }
    foundry_update(ctx.rt, 0);
    for (size_t i = 0; i < thread_check; ++i)
      for (uint32_t e : ctx.events()) {
        ctx.trigger(mains[i], e);
        ctx.pipeline->trigger(solo, solo_handles[i], e);
      }
    for (int f = 0; f < 120; ++f) {
      ctx.frame();
      foundry_update(solo, static_cast<float>(ctx.plan.dt));
    }
    for (size_t i = 0; i < thread_check; ++i) {
      uint64_t a = 0, b = 0, m = 0;
      foundry_diag_hash(ctx.rt, mains[i], nullptr, 0, &m, &a);
      foundry_diag_hash(solo, solo_handles[i], nullptr, 0, &m, &b);
      if (a == b) ++thread_same;
      else fail(failures, ctx, "diversity", "threads", "one update thread and many gave different poses",
                ctx.describe(configs[i]));
    }
    foundry_runtime_destroy(solo);
    twin_handles.insert(twin_handles.end(), mains.begin(), mains.end());
  }
  clear(ctx, twin_handles);

  // Statistics.
  std::vector<double> lat;
  size_t worst = 0;
  for (size_t i = 0; i < n; ++i)
    if (latency[i] >= 0) {
      lat.push_back(latency[i]);
      if (latency[i] > latency[worst]) worst = i;
    }
  Stats ls = stats(lat), fs = stats(totals(frames));
  json stages = json::array();
  foundry_diag_stage st[32];
  uint32_t ns = foundry_diag_stages(ctx.rt, st, 32);
  for (uint32_t i = 0; i < ns && i < 32; ++i)
    stages.push_back({{"stage", st[i].label}, {"update", st[i].update}, {"calls", st[i].calls},
                      {"mean_ms", st[i].calls ? st[i].total_ms / st[i].calls : 0}, {"max_ms", st[i].max_ms}});
  res.data = {{"configs", n},
              {"seconds", seconds},
              {"generation_ms", to_json(ls)},
              {"generation_worst", n ? ctx.describe(configs[worst]) : json(nullptr)},
              {"frames", frame_json(frames)},
              {"stages", stages},
              {"triangles", to_json(stats(tris))},
              {"failed", failed},
              {"unsettled", unsettled},
              {"deterministic", {{"same", deterministic}, {"of", twins}}},
              {"threads", {{"same", thread_same}, {"of", thread_check}}},
              {"reference", matched}};
  for (const auto &f : failures)
    if (f.phase == "diversity") res.pass = false;
  std::string ref = matched.is_null() ? "native = wasm: no reference"
                                      : fmt("native = wasm %zu/%zu", matched["matched"].get<size_t>(), n);
  std::string worst_name;
  if (n) {
    json d = ctx.describe(configs[worst]);
    for (auto it = d.begin(); it != d.end(); ++it)
      if (it.value().is_string()) worst_name += (worst_name.empty() ? "" : " ") + it.value().get<std::string>();
  }
  res.summary = fmt("%zu configs  %.0f s  %s  %s  gen p50 %.1f ms p99 %.1f ms (%s)  frame p95 %.2f ms", n, seconds,
                    res.pass ? "PASS" : "FAIL", ref.c_str(), ls.p50, ls.p99, worst_name.c_str(), fs.p95);
  return res;
}

// --- forest -----------------------------------------------------------------------

PhaseResult run_forest(Ctx &ctx, const json &phase, std::vector<Failure> &failures) {
  PhaseResult res;
  res.kind = "forest";
  Rng rng(ctx.plan.seed, 23);
  const size_t sets_n = static_cast<size_t>(get(phase, "sets", 10));
  auto counts = counts_of(phase, {100, 200, 400, 800, 1600, 3200});
  const double seconds = get(phase, "seconds", 10), budget = get(phase, "budget_ms", ctx.plan.budget_ms);
  const float spacing = static_cast<float>(get(phase, "spacing", 9));
  const std::string cam = phase.value("camera", "overview");
  const int refine = static_cast<int>(get(phase, "refine", 3));
  std::vector<Config> sets = sample(ctx, sets_n, rng);
  // Distinct builds expected: one per distinct set of build parameters.
  std::set<std::vector<uint32_t>> distinct;
  for (const auto &s : sets) {
    std::vector<uint32_t> key;
    for (uint32_t i = 0; i < ctx.pipeline->param_count; ++i)
      if (ctx.pipeline->params[i].phase == STRESS_BUILD && ctx.pipeline->params[i].kind != STRESS_CLOCK &&
          ctx.pipeline->params[i].kind != STRESS_EVENT)
        key.push_back(s[i].u);
    distinct.insert(key);
  }
  const uint64_t expected = distinct.size();
  const uint32_t max_count = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
  auto layout = hex_layout(max_count, spacing, rng, 0.3f);
  std::vector<foundry_instance> handles;
  const uint64_t gen0 = counters(ctx).generations, hits0 = counters(ctx).cache_hits, miss0 = counters(ctx).cache_misses;

  auto resize = [&](uint32_t count) {
    while (handles.size() > count) {
      ctx.destroy(handles.back());
      handles.pop_back();
    }
    while (handles.size() < count) {
      size_t i = handles.size();
      Config c = sets[i % sets.size()];
      jitter(ctx, c, rng);
      float m[16];
      translation(layout[i].first, layout[i].second, static_cast<float>(rng.unit() * 6.283), m);
      handles.push_back(ctx.spawn(c, m));
    }
    float extent = 0;
    for (size_t i = 0; i < count; ++i) extent = std::max({extent, std::fabs(layout[i].first), std::fabs(layout[i].second)});
    ctx.overview(extent + spacing, 14);
    return extent;
  };
  auto measure = [&](uint32_t count, double secs) {
    const float extent = resize(count);
    wait_ready(ctx, 120);
    for (int i = 0; i < 30; ++i) ctx.frame();  // warm
    const Memory before = memory();
    std::vector<FrameTime> frames;
    const double start = ctx.sim_time;
    while (ctx.sim_time - start < secs) {
      if (cam == "walk") {  // a ground-level path through the forest
        const float t = static_cast<float>(ctx.sim_time - start) * 1.5f - extent;
        const float eye[3] = {t, 1.7f, 0.3f * spacing}, target[3] = {t + 10, 1.5f, 0.3f * spacing};
        ctx.camera(eye, target, 400);
      }
      frames.push_back(ctx.frame(true, frames.size() % 10 == 9));
      if (frames.size() > 20 && stats(totals(frames)).p50 > budget * 8 && ctx.sim_time - start > secs / 4) break;
    }
    snap(ctx, fmt("forest-%u", count));
    foundry_diag_counters c = counters(ctx);
    Stats s = stats(totals(frames));
    json j = {{"count", count},
              {"frames", frame_json(frames)},
              {"p95_ms", s.p95},
              {"draws", c.draws},
              {"drawn", c.drawn},
              {"culled", c.culled},
              {"triangles", c.triangles},
              {"cache_entries", c.cache_entries}, {"bodies", c.bodies},
              {"cache_gpu_mb", c.cache_gpu_bytes / 1048576.0},
              {"generations", c.generations - gen0},
              {"memory", to_json(before)}};
    return std::make_pair(s.p95, j);
  };

  json samples = json::array();
  uint32_t good = 0, bad = 0;
  double good_p95 = 0;
  for (uint32_t count : counts) {
    auto [p95, j] = measure(count, seconds);
    samples.push_back(j);
    if (p95 <= budget) {
      good = count;
      good_p95 = p95;
    } else {
      bad = count;
      break;
    }
  }
  for (int i = 0; i < refine && bad && bad - good > std::max<uint32_t>(10, good / 20); ++i) {
    uint32_t mid = good + (bad - good) / 2;
    auto [p95, j] = measure(mid, seconds / 2);
    j["refine"] = true;
    samples.push_back(j);
    if (p95 <= budget) {
      good = mid;
      good_p95 = p95;
    } else {
      bad = mid;
    }
  }
  foundry_diag_counters c = counters(ctx);
  const uint64_t generations = c.generations - gen0;
  if (generations > expected)
    fail(failures, ctx, "forest", "caching", fmt("%llu generations for %llu distinct sets: instances are not sharing builds",
                                                 static_cast<unsigned long long>(generations),
                                                 static_cast<unsigned long long>(expected)));
  // Memory per instance and scaling, from the plain (not refined) samples.
  std::vector<json> plain;
  for (const auto &s : samples)
    if (!s.value("refine", false)) plain.push_back(s);
  double kb_per_instance = 0, slope = 0;
  bool superlinear = false;
  if (plain.size() >= 2) {
    const json &a = plain.front(), &b = plain.back();
    double dn = b["count"].get<double>() - a["count"].get<double>();
    kb_per_instance = (b["memory"]["rss_mb"].get<double>() - a["memory"]["rss_mb"].get<double>()) * 1024 / dn;
    // Least squares of p50 frame time against count.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto &s : plain) {
      double x = s["count"].get<double>(), y = s["frames"]["total_ms"]["p50"].get<double>();
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }
    double k = static_cast<double>(plain.size());
    slope = (k * sxy - sx * sy) / std::max(1e-9, k * sxx - sx * sx);
    double first = a["frames"]["total_ms"]["p50"].get<double>() / a["count"].get<double>();
    double last = b["frames"]["total_ms"]["p50"].get<double>() / b["count"].get<double>();
    superlinear = last > first * 1.5 && b["count"].get<double>() >= 4 * a["count"].get<double>();
  }
  clear(ctx, handles);
  const double hit_rate = (c.cache_hits - hits0) / std::max(1.0, double(c.cache_hits - hits0 + c.cache_misses - miss0));
  res.data = {{"sets", sets_n},
              {"camera", cam},
              {"budget_ms", budget},
              {"samples", samples},
              {"capacity", good},
              {"capacity_p95_ms", good_p95},
              {"capacity_is_lower_bound", bad == 0},
              {"generations", generations},
              {"expected_generations", expected},
              {"hit_rate", hit_rate},
              {"kb_per_instance", kb_per_instance},
              {"ms_per_instance", slope},
              {"superlinear", superlinear}};
  for (const auto &f : failures)
    if (f.phase == "forest") res.pass = false;
  std::string range = counts.empty() ? "" : fmt("%u..%u", counts.front(), bad ? bad : counts.back());
  res.summary = fmt("%zu sets  %s  %s  budget %.1f ms holds to %s%u (p95 %.1f ms)  generations %llu/%llu  hits %.1f%%%s",
                    sets_n, range.c_str(), res.pass ? "PASS" : "FAIL", budget, bad == 0 ? "at least " : "", good,
                    good_p95, static_cast<unsigned long long>(generations), static_cast<unsigned long long>(expected),
                    hit_rate * 100, superlinear ? "  SUPERLINEAR" : "");
  return res;
}

// --- events -----------------------------------------------------------------------

PhaseResult run_events(Ctx &ctx, const json &phase, std::vector<Failure> &failures) {
  PhaseResult res;
  res.kind = "events";
  Rng rng(ctx.plan.seed, 37);
  std::vector<uint32_t> events;
  for (uint32_t e : ctx.events())
    if (!phase.contains("event") || phase["event"] == ctx.pipeline->params[e].name) events.push_back(e);
  if (events.empty()) {
    res.summary = "no events";
    return res;
  }
  const size_t sets_n = static_cast<size_t>(get(phase, "sets", 10));
  auto counts = counts_of(phase, {100});
  const double stagger = get(phase, "stagger_seconds", 0), limit = get(phase, "seconds", 15);
  const double steady = get(phase, "steady_seconds", 1), budget = get(phase, "budget_ms", ctx.plan.budget_ms);
  const float spacing = static_cast<float>(get(phase, "spacing", 9));
  const size_t alone_n = static_cast<size_t>(get(phase, "same_as_alone", 3));
  std::vector<Config> sets = sample(ctx, sets_n, rng);
  json results = json::array();
  uint32_t headline = 0;
  std::string headline_line;
  for (uint32_t e : events) {
    const char *ename = ctx.pipeline->params[e].name;
    for (uint32_t count : counts) {
      trim();
      const Memory m0 = memory();
      auto layout = hex_layout(count, spacing, rng, 0.3f);
      std::vector<foundry_instance> handles;
      std::vector<Config> cfgs;
      std::vector<uint64_t> spawn_frame;
      for (uint32_t i = 0; i < count; ++i) {
        Config c = sets[i % sets.size()];
        jitter(ctx, c, rng);
        float m[16];
        translation(layout[i].first, layout[i].second, static_cast<float>(rng.unit() * 6.283), m);
        handles.push_back(ctx.spawn(c, m));
        cfgs.push_back(c);
        spawn_frame.push_back(ctx.frames);
      }
      float extent = 0;
      for (auto &p : layout) extent = std::max({extent, std::fabs(p.first), std::fabs(p.second)});
      ctx.overview(extent + spacing, 14);
      wait_ready(ctx, 120);
      for (int i = 0; i < 30; ++i) ctx.frame();
      std::vector<FrameTime> base;
      double t0 = ctx.sim_time;
      while (ctx.sim_time - t0 < steady) base.push_back(ctx.frame());
      // Fire.
      std::vector<uint64_t> fire_frame(count, 0);
      std::vector<FrameTime> during;
      size_t fired = 0;
      const double fire_start = ctx.sim_time;
      double settled_at = -1, peak_rss = memory().rss_mb, next_mem = fire_start + 0.5;
      double first_frame = 0;
      while (ctx.sim_time - fire_start < limit + stagger) {
        const size_t due = stagger <= 0 ? count
                                        : std::min<size_t>(count, static_cast<size_t>(std::ceil(
                                                                      (ctx.sim_time - fire_start + ctx.plan.dt) / stagger * count)));
        for (; fired < due; ++fired) {
          ctx.trigger(handles[fired], e);
          fire_frame[fired] = ctx.frames;
        }
        FrameTime f = ctx.frame(true, !during.empty() && during.size() % 10 == 9);
        if (during.empty()) first_frame = f.total;
        during.push_back(f);
        if (during.size() == static_cast<size_t>(1.5 / ctx.plan.dt)) snap(ctx, fmt("events-%s-%u", ename, count));
        if (ctx.sim_time >= next_mem) {
          peak_rss = std::max(peak_rss, memory().rss_mb);
          next_mem += 0.5;
        }
        if (fired == count) {
          bool all = true;
          for (foundry_instance h : handles) {
            foundry_diag_instance info;
            foundry_diag_instance_get(ctx.rt, h, &info);
            if (!info.settled) {
              all = false;
              break;
            }
          }
          if (all) {
            settled_at = ctx.sim_time - fire_start;
            break;
          }
        }
      }
      uint64_t unsettled = 0;
      for (size_t i = 0; i < handles.size(); ++i) {
        foundry_diag_instance info;
        foundry_diag_instance_get(ctx.rt, handles[i], &info);
        if (!info.settled) {
          if (unsettled++ == 0)
            fail(failures, ctx, "events", "settle", fmt("%s: instances never settled", ename), ctx.describe(cfgs[i]));
        }
      }
      // Same as alone: replay a few instances by themselves, frame for frame.
      size_t same = 0, checked = 0;
      const uint64_t end_frame = ctx.frames;
      for (size_t k = 0; k < std::min(alone_n, handles.size()); ++k) {
        const size_t i = (k * handles.size()) / std::max<size_t>(1, alone_n);
        foundry_runtime_desc desc = {};
        desc.update_threads = 1;
        desc.build_threads = 1;
        foundry_runtime *solo = foundry_runtime_create(&desc);
        float m[16];
        translation(0, 0, 0, m);
        foundry_instance h = ctx.pipeline->spawn(solo, cfgs[i].data(), m);
        // Wait for the build without moving time, so the clocks line up.
        for (int w = 0; w < 6000; ++w) {
          foundry_diag_counters c;
          foundry_diag_counters_get(solo, &c);
          if (c.waiting == 0 && c.cache_pending == 0) break;
          foundry_update(solo, 0);
          SDL_Delay(1);
        }
        for (uint64_t f = spawn_frame[i]; f < fire_frame[i]; ++f) foundry_update(solo, static_cast<float>(ctx.plan.dt));
        ctx.pipeline->trigger(solo, h, e);
        for (uint64_t f = fire_frame[i]; f < end_frame; ++f) foundry_update(solo, static_cast<float>(ctx.plan.dt));
        uint64_t a = 0, b = 0, mm = 0;
        foundry_diag_hash(ctx.rt, handles[i], nullptr, 0, &mm, &a);
        foundry_diag_hash(solo, h, nullptr, 0, &mm, &b);
        ++checked;
        if (a == b) ++same;
        else fail(failures, ctx, "events", "same as alone", fmt("%s: firing in bulk changed an instance's result", ename),
                  ctx.describe(cfgs[i]));
        foundry_runtime_destroy(solo);
      }
      clear(ctx, handles);
      trim();
      const Memory m1 = memory();
      Stats bs = stats(totals(base)), ds = stats(totals(during));
      std::vector<double> bu, du;
      for (const auto &f : base) bu.push_back(f.update);
      for (const auto &f : during) du.push_back(f.update);
      const double idle_us = stats(bu).mean * 1000 / count, active_us = stats(du).mean * 1000 / count;
      json r = {{"event", ename},
                {"count", count},
                {"stagger_seconds", stagger},
                {"steady", frame_json(base)},
                {"during", frame_json(during)},
                {"spike_first_ms", first_frame},
                {"spike_max_ms", ds.max},
                {"update_us_per_instance_idle", idle_us},
                {"update_us_per_instance_active", active_us},
                {"settle_seconds", settled_at},
                {"unsettled", unsettled},
                {"same_as_alone", {{"same", same}, {"of", checked}}},
                {"peak_rss_over_mb", peak_rss - m0.rss_mb},
                {"heap_remainder_mb", m1.heap_mb - m0.heap_mb},
                {"memory_before", to_json(m0)},
                {"memory_after", to_json(m1)}};
      results.push_back(r);
      if (ds.max <= budget) headline = std::max(headline, count);
      if (std::fabs(m1.heap_mb - m0.heap_mb) > ctx.plan.threshold("events_heap_tolerance_mb", 4))
        fail(failures, ctx, "events", "memory", fmt("%s: %.1f MB of heap still held after the instances were destroyed",
                                                    ename, m1.heap_mb - m0.heap_mb));
      if (headline_line.empty() || count == counts.front())
        headline_line = fmt("%s  %u  spike %.1f ms over %.1f ms  %s  memory back within %.1f MB", ename, count, ds.max,
                            bs.p50,
                            settled_at >= 0 ? fmt("all settled in %.1f s", settled_at).c_str()
                                            : fmt("%llu unsettled", static_cast<unsigned long long>(unsettled)).c_str(),
                            std::fabs(m1.heap_mb - m0.heap_mb));
    }
  }
  res.data = {{"results", results}, {"budget_ms", budget}, {"capacity_in_budget", headline}};
  for (const auto &f : failures)
    if (f.phase == "events") res.pass = false;
  std::string largest;
  if (counts.size() > 1) largest = fmt("  in budget up to %u", headline);
  res.summary = fmt("%s  %s%s", headline_line.c_str(), res.pass ? "PASS" : "FAIL", largest.c_str());
  return res;
}

// --- soak -------------------------------------------------------------------------

PhaseResult run_soak(Ctx &ctx, const json &phase, std::vector<Failure> &failures) {
  PhaseResult res;
  res.kind = "soak";
  Rng rng(ctx.plan.seed, 51);
  const double minutes = get(phase, "minutes", 5);
  const size_t population = static_cast<size_t>(get(phase, "population", 300));
  const double churn = get(phase, "churn_per_second", 20), sets_rate = get(phase, "set_per_second", 30);
  const double build_rate = get(phase, "build_change_per_second", 2), event_rate = get(phase, "event_per_second", 5);
  const double stale_rate = get(phase, "stale_per_second", 2), sample_seconds = get(phase, "sample_seconds", 5);
  const double drain_minutes = get(phase, "drain_minutes", std::max(0.5, minutes / 5));
  const int render_every = static_cast<int>(get(phase, "render_every", 4));
  const double slope_limit = ctx.plan.threshold("soak_rss_slope_mb_per_hour", 4);
  const double drain_tol = ctx.plan.threshold("drain_tolerance_mb", 8);
  const double plateau_minutes = get(phase, "plateau_min_minutes", 20);
  std::vector<Config> pool = sample(ctx, static_cast<size_t>(get(phase, "distinct", 40)), rng);
  const float half = std::sqrt(static_cast<float>(population)) * 5;
  ctx.overview(half, 14);
  std::vector<foundry_instance> handles, dead;
  auto spawn_one = [&] {
    Config c = pool[rng.below(static_cast<uint32_t>(pool.size()))];
    for (uint32_t p : ctx.update_params()) c[p] = ctx.random_value(p, rng);
    float m[16];
    translation(static_cast<float>((rng.unit() * 2 - 1) * half), static_cast<float>((rng.unit() * 2 - 1) * half),
                static_cast<float>(rng.unit() * 6.283), m);
    handles.push_back(ctx.spawn(c, m));
  };
  for (size_t i = 0; i < population; ++i) spawn_one();

  json samples = json::array(), drains = json::array();
  const double wall0 = now_ms(), sim0 = ctx.sim_time;
  double next_sample = wall0, next_drain = wall0 + drain_minutes * 60000;
  double acc_churn = 0, acc_set = 0, acc_build = 0, acc_event = 0, acc_stale = 0;
  uint64_t stale_ok = 0, stale_bad = 0;
  std::vector<double> window;
  const auto updates = ctx.update_params(), builds = ctx.build_params(), events = ctx.events();

  auto drain = [&] {
    clear(ctx, handles);
    for (int i = 0; i < static_cast<int>(2.5 / ctx.plan.dt); ++i) ctx.frame();  // the release timer
    foundry_diag_flush(ctx.rt);
    if (ctx.physics) toy_physics_settle(ctx.physics);
    trim();
    foundry_diag_counters c = counters(ctx);
    Memory m = memory();
    json d = {{"wall_minutes", (now_ms() - wall0) / 60000},
              {"memory", to_json(m)},
              {"instances", c.instances},
              {"cache_entries", c.cache_entries},
              {"gpu_buffers_live", static_cast<int64_t>(c.gpu_buffers_created - c.gpu_buffers_released)},
              {"gpu_mb", c.gpu_bytes / 1048576.0},
              {"bodies", c.bodies},
              {"physics_bodies", ctx.physics ? toy_physics_bodies(ctx.physics) : 0u}};
    if (c.instances != 0 || c.cache_entries != 0)
      fail(failures, ctx, "soak", "drain", fmt("after a drain, %llu instances and %llu cache entries remain",
                                               static_cast<unsigned long long>(c.instances),
                                               static_cast<unsigned long long>(c.cache_entries)));
    // Bodies balance: every instance's collision bodies left the physics world with it.
    if (c.bodies != 0 || (ctx.physics && toy_physics_bodies(ctx.physics) != 0))
      fail(failures, ctx, "soak", "drain", fmt("after a drain, %llu collision bodies remain in the runtime and %u in the world",
                                               static_cast<unsigned long long>(c.bodies),
                                               ctx.physics ? toy_physics_bodies(ctx.physics) : 0u));
    drains.push_back(d);
    for (size_t i = 0; i < population; ++i) spawn_one();
  };

  while (now_ms() - wall0 < minutes * 60000) {
    const double dt = ctx.plan.dt;
    acc_churn += churn * dt;
    acc_set += sets_rate * dt;
    acc_build += build_rate * dt;
    acc_event += event_rate * dt;
    acc_stale += stale_rate * dt;
    for (; acc_churn >= 1; acc_churn -= 1) {
      if (handles.empty()) break;
      size_t i = rng.below(static_cast<uint32_t>(handles.size()));
      ctx.destroy(handles[i]);
      dead.push_back(handles[i]);
      if (dead.size() > 64) dead.erase(dead.begin());
      handles[i] = handles.back();
      handles.pop_back();
      spawn_one();
    }
    for (; acc_set >= 1; acc_set -= 1)
      if (!handles.empty() && !updates.empty()) {
        uint32_t p = updates[rng.below(static_cast<uint32_t>(updates.size()))];
        ctx.set(handles[rng.below(static_cast<uint32_t>(handles.size()))], p, ctx.random_value(p, rng));
      }
    for (; acc_build >= 1; acc_build -= 1)
      if (!handles.empty() && !builds.empty()) {
        uint32_t p = builds[rng.below(static_cast<uint32_t>(builds.size()))];
        ctx.set(handles[rng.below(static_cast<uint32_t>(handles.size()))], p, ctx.random_value(p, rng));
      }
    for (; acc_event >= 1; acc_event -= 1)
      if (!handles.empty() && !events.empty())
        ctx.trigger(handles[rng.below(static_cast<uint32_t>(handles.size()))], events[rng.below(static_cast<uint32_t>(events.size()))]);
    for (; acc_stale >= 1; acc_stale -= 1)
      if (!dead.empty() && !updates.empty()) {
        foundry_instance h = dead[rng.below(static_cast<uint32_t>(dead.size()))];
        foundry_status s = ctx.set(h, updates[0], ctx.random_value(updates[0], rng));
        if (s == FOUNDRY_STATUS_STALE_HANDLE) ++stale_ok;
        else if (stale_bad++ == 0) fail(failures, ctx, "soak", "stale handle", "a destroyed handle was accepted");
      }
    window.push_back(ctx.frame(render_every <= 1 || ctx.frames % render_every == 0).total);
    const double now = now_ms();
    if (now >= next_sample) {
      foundry_diag_counters c = counters(ctx);
      samples.push_back({{"wall_minutes", (now - wall0) / 60000},
                         {"sim_minutes", (ctx.sim_time - sim0) / 60},
                         {"memory", to_json(memory())},
                         {"instances", c.instances},
                         {"awake", c.awake},
                         {"cache_entries", c.cache_entries}, {"bodies", c.bodies},
                         {"cache_mb", c.cache_bytes / 1048576.0},
                         {"gpu_mb", c.gpu_bytes / 1048576.0},
                         {"generations", c.generations},
                         {"evictions", c.cache_evictions},
                         {"frame_ms", to_json(stats(window))}});
      window.clear();
      next_sample = now + sample_seconds * 1000;
      ctx.journal.flush();
    }
    if (now >= next_drain) {
      drain();
      next_drain = now_ms() + drain_minutes * 60000;
    }
  }
  drain();
  clear(ctx, handles);

  // The plateau: a line through RSS after the warmup.
  const double wall_minutes = (now_ms() - wall0) / 60000, speed = (ctx.sim_time - sim0) / (wall_minutes * 60);
  double slope = 0;
  {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, k = 0;
    for (const auto &s : samples) {
      double x = s["wall_minutes"].get<double>() / 60, y = s["memory"]["rss_mb"].get<double>();
      if (x * 60 < wall_minutes * 0.1) continue;
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
      k += 1;
    }
    if (k >= 3) slope = (k * sxy - sx * sy) / std::max(1e-12, k * sxx - sx * sx);
  }
  const bool judged = wall_minutes >= plateau_minutes;
  if (judged && slope > slope_limit)
    fail(failures, ctx, "soak", "plateau", fmt("RSS grows %.1f MB/h after the warmup (limit %.1f)", slope, slope_limit));
  // Drains: each comes back to the first.
  size_t within = 0;
  double worst = 0;
  for (size_t i = 0; i < drains.size(); ++i) {
    double d_heap = drains[i]["memory"]["heap_mb"].get<double>() - drains[0]["memory"]["heap_mb"].get<double>();
    double d_rss = drains[i]["memory"]["rss_mb"].get<double>() - drains[0]["memory"]["rss_mb"].get<double>();
    bool gpu_same = drains[i]["gpu_buffers_live"] == drains[0]["gpu_buffers_live"];
    worst = std::max({worst, d_heap, d_rss});  // only growth is a leak: memory given back is fine
    if (d_heap <= drain_tol && d_rss <= drain_tol && gpu_same) ++within;
    else fail(failures, ctx, "soak", "drain", fmt("drain %zu: heap %+.1f MB, RSS %+.1f MB from the first%s", i, d_heap, d_rss,
                                                  gpu_same ? "" : ", GPU buffers differ"));
  }
  const double rss_first = samples.empty() ? 0 : samples.front()["memory"]["rss_mb"].get<double>();
  const double rss_last = samples.empty() ? 0 : samples.back()["memory"]["rss_mb"].get<double>();
  res.data = {{"minutes", wall_minutes},
              {"sim_minutes", (ctx.sim_time - sim0) / 60},
              {"speed", speed},
              {"population", population},
              {"rss_slope_mb_per_hour", slope},
              {"plateau_judged", judged},
              {"drains", drains},
              {"drains_within", within},
              {"stale_handles_rejected", stale_ok},
              {"samples", samples}};
  for (const auto &f : failures)
    if (f.phase == "soak") res.pass = false;
  res.summary = fmt("%.0f min  x%.1f  %s  RSS %.0f -> %.0f MB  slope %+.1f MB/h (%s)  drains %zu/%zu within %.1f MB",
                    wall_minutes, speed, res.pass ? "PASS" : "FAIL", rss_first, rss_last, slope,
                    judged ? fmt("limit %.0f", slope_limit).c_str() : fmt("not judged under %.0f min", plateau_minutes).c_str(),
                    within, drains.size(), worst);
  return res;
}

}  // namespace stress

namespace stress {

// --- explore ----------------------------------------------------------------------

// A scene to wander through with --window, until the window closes: a forest of
// `count` instances from `sets` sets. Keys: B fires every event on the
// instances near where you're looking, R regrows the scene, N draws new sets,
// - and = turn the first update parameter (the wind) down and up.
PhaseResult run_explore(Ctx &ctx, const json &phase, std::vector<Failure> &failures) {
  (void)failures;
  PhaseResult res;
  res.kind = "explore";
  Rng rng(ctx.plan.seed, 61);
  const uint32_t count = static_cast<uint32_t>(get(phase, "count", 2000));
  const size_t sets_n = static_cast<size_t>(get(phase, "sets", 10));
  const float spacing = static_cast<float>(get(phase, "spacing", 9));
  const double seconds = get(phase, "seconds", ctx.gpu.window ? 1e12 : 30);
  const float reach = static_cast<float>(get(phase, "break_radius", 15));
  auto layout = hex_layout(count, spacing, rng, 0.3f);
  std::vector<Config> sets = sample(ctx, sets_n, rng);
  std::vector<foundry_instance> handles;
  std::vector<Config> cfgs;
  const auto updates = ctx.update_params();
  int dial = -1;  // the first f32 update parameter, e.g. the wind
  for (uint32_t p : updates)
    if (ctx.pipeline->params[p].kind == STRESS_F32) {
      dial = static_cast<int>(p);
      break;
    }
  float dial_value = dial >= 0 ? sets[0][dial].f : 0;
  auto grow = [&] {
    for (foundry_instance h : handles) ctx.destroy(h);
    handles.clear();
    cfgs.clear();
    for (uint32_t i = 0; i < count; ++i) {
      Config c = sets[i % sets.size()];
      jitter(ctx, c, rng);
      if (dial >= 0) c[dial].f = dial_value;
      float m[16];
      translation(layout[i].first, layout[i].second, static_cast<float>(rng.unit() * 6.283), m);
      handles.push_back(ctx.spawn(c, m));
      cfgs.push_back(c);
    }
  };
  grow();
  float extent = 0;
  for (auto &p : layout) extent = std::max({extent, std::fabs(p.first), std::fabs(p.second)});
  ctx.overview(extent + spacing, 14);
  std::vector<double> frame_ms;
  const double start = ctx.sim_time;
  while (ctx.sim_time - start < seconds) {
    frame_ms.push_back(ctx.frame().total);
    for (SDL_Keycode k : ctx.pressed) {
      if (k == SDLK_B) {  // fire the events near the point on the ground you're looking at
        float eye[3], dir[3];
        if (ctx.fly.on) {
          std::memcpy(eye, ctx.fly.eye, sizeof eye);
          dir[0] = std::sin(ctx.fly.yaw) * std::cos(ctx.fly.pitch);
          dir[1] = std::sin(ctx.fly.pitch);
          dir[2] = -std::cos(ctx.fly.yaw) * std::cos(ctx.fly.pitch);
        } else {
          std::memcpy(eye, ctx.cam_eye, sizeof eye);
          for (int i = 0; i < 3; ++i) dir[i] = ctx.cam_target[i] - ctx.cam_eye[i];
        }
        float t = dir[1] < -1e-3f ? -eye[1] / dir[1] : 30.0f;
        const float gx = eye[0] + dir[0] * t, gz = eye[2] + dir[2] * t;
        size_t fired = 0;
        for (size_t i = 0; i < handles.size(); ++i) {
          const float dx = layout[i].first - gx, dz = layout[i].second - gz;
          if (dx * dx + dz * dz > reach * reach) continue;
          for (uint32_t e : ctx.events()) ctx.trigger(handles[i], e);
          ++fired;
        }
        ctx.hud = fmt("fired on %zu", fired);
      } else if (k == SDLK_R) {
        grow();
        ctx.hud = "regrown";
      } else if (k == SDLK_N) {
        sets = sample(ctx, sets_n, rng);
        grow();
        ctx.hud = "new sets";
      } else if ((k == SDLK_MINUS || k == SDLK_EQUALS) && dial >= 0) {
        const auto &d = ctx.pipeline->params[dial];
        dial_value = static_cast<float>(std::clamp(dial_value + (k == SDLK_EQUALS ? 0.1 : -0.1) * (d.max - d.min), d.min, d.max));
        stress_value v;
        v.f = dial_value;
        for (foundry_instance h : handles) ctx.set(h, static_cast<uint32_t>(dial), v);
        ctx.hud = fmt("%s %.1f", d.name, dial_value);
      }
    }
    if (ctx.pressed.empty() && ctx.hud.empty())
      ctx.hud = dial >= 0 ? fmt("B: fire events  R: regrow  N: new sets  -/=: %s", ctx.pipeline->params[dial].name)
                          : "B: fire events  R: regrow  N: new sets";
  }
  for (foundry_instance h : handles) ctx.destroy(h);
  Stats s = stats(frame_ms);
  res.data = {{"count", count}, {"frame_ms", to_json(s)}};
  res.summary = fmt("%u instances  frame p50 %.2f ms p95 %.2f ms", count, s.p50, s.p95);
  return res;
}

}  // namespace stress
