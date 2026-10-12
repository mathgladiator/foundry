// The region run (design/quality.perf.md, "Regions"; design/plotter.md): a
// plotter's regions measured in the native runtime, the way a game uses them.
//
// 1. Grow: a grid of regions (the glen's ground, each with its own world seed)
//    grown in batches over frames, every placement spawned into a toy Jolt world.
// 2. Steady: the full scene, with its wind, for a few seconds.
// 3. Bomb and regrow: per cycle, everything within a blast radius of a random
//    point in every region is destroyed and vacated, then regrown over frames.
// 4. Drain: every instance and region destroyed; instances, builds, and bodies
//    must come back to zero.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "platform.h"

namespace fs = std::filesystem;

namespace stress {

std::string stamp_now(bool iso) {
  std::time_t t = std::time(nullptr);
  std::tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, iso ? "%Y-%m-%dT%H:%M:%SZ" : "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

// The foundry's "glen" ground (src/host/plot/view.cpp) at radius R, or a plain hex.
std::vector<foundry_hex_region> plot_ground(const std::string &preset, int R) {
  std::vector<foundry_hex_region> g;
  foundry_hex_region soil = {};
  soil.shape = FOUNDRY_HEX_HEX;
  soil.radius = static_cast<uint32_t>(R);
  soil.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE | FOUNDRY_SET_MOISTURE;
  soil.ground = FOUNDRY_GROUND_SOIL;
  soil.available = true;
  soil.moisture = 0.55f;
  g.push_back(soil);
  if (preset != "glen") return g;
  foundry_hex_region pond = {};
  pond.shape = FOUNDRY_HEX_HEX;
  pond.q = R / 3;
  pond.r = -R / 4;
  pond.radius = static_cast<uint32_t>(std::max(1, R / 4));
  pond.priority = 1;
  pond.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_MOISTURE;
  pond.ground = FOUNDRY_GROUND_WATER;
  pond.moisture = 1.0f;
  g.push_back(pond);
  foundry_hex_region path = {};
  path.shape = FOUNDRY_HEX_LINE;
  path.q = -R;
  path.r = R / 2;
  path.q2 = R;
  path.r2 = -R / 2;
  path.priority = 2;
  path.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE | FOUNDRY_SET_KEEP_CLEAR;
  path.ground = FOUNDRY_GROUND_PATH;
  path.available = false;
  path.keep_clear_m = 1.2f;
  g.push_back(path);
  for (int k = 3; k >= 1; --k) {
    foundry_hex_region rise = {};
    rise.shape = FOUNDRY_HEX_HEX;
    rise.q = -R / 3;
    rise.r = R / 3;
    rise.radius = static_cast<uint32_t>(std::max(1, R * k / 6));
    rise.set = FOUNDRY_SET_HEIGHT;
    rise.height = 0.25f * static_cast<float>(4 - k);
    g.push_back(rise);
  }
  return g;
}

namespace {

struct Live {
  foundry_region region = FOUNDRY_NO_REGION;
  float offset[2] = {0, 0};
  std::map<std::tuple<int32_t, int32_t, uint32_t, uint32_t>, foundry_instance> handles;  // slot -> instance
  uint32_t volume = 1;
};

std::tuple<int32_t, int32_t, uint32_t, uint32_t> key(const foundry_slot &s) { return {s.q, s.r, s.layer, s.index}; }

}  // namespace

}  // namespace stress

extern "C" int stress_region_main(int argc, char **argv, foundry_plotter plotter, const char *name,
                                  const stress_pipeline *const *pipelines, uint32_t count) {
  using namespace stress;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plan" && std::string(argv[i + 1]) == "scale")
      return scale_main(argc, argv, plotter, name, pipelines, count);
  std::string workspace = FOUNDRY_WORKSPACE, gpu_mode = "auto", preset = "glen";
  bool write_report = true, window = false;
  int side = 4, R = 12, batch = 64, cycles = 3;
  double steady_s = 4, blast = 8;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--workspace") workspace = next();
    else if (a == "--no-report") write_report = false;
    else if (a == "--gpu") gpu_mode = next();
    else if (a == "--window") window = true;
    else if (a == "--regions") side = std::max(1, std::atoi(next().c_str()));
    else if (a == "--radius") R = std::max(1, std::atoi(next().c_str()));
    else if (a == "--batch") batch = std::max(1, std::atoi(next().c_str()));
    else if (a == "--cycles") cycles = std::max(0, std::atoi(next().c_str()));
    else if (a == "--blast") blast = std::atof(next().c_str());
    else if (a == "--steady") steady_s = std::atof(next().c_str());
    else if (a == "--preset") preset = next();
    else if (a == "--plan") next();  // region (this run); scale is handled above
    else {
      std::cerr << "usage: stress_plotter_<name> [--plan region|scale] [--regions N (an N x N grid)] [--radius cells] [--preset glen|hex]\n"
                   "                            [--batch placements per frame] [--cycles N] [--blast metres]\n"
                   "                            [--steady seconds] [--gpu auto|none|required] [--window] [--no-report]\n";
      return 2;
    }
  }
  Ctx ctx;
  ctx.title_name = name;
  ctx.plan.gpu = gpu_mode;
  if (window) {
    ctx.plan.window = true;
    ctx.plan.gpu = "required";
    ctx.plan.width = 1600;
    ctx.plan.height = 900;
    write_report = false;
  }
  SDL_Init(SDL_INIT_VIDEO);
  if (ctx.plan.gpu != "none") {
    std::string gerr;
    ctx.use_gpu = ctx.gpu.init(ctx.plan, gerr);
    if (!ctx.use_gpu && ctx.plan.gpu == "required") {
      std::cerr << "stress: " << gerr << "\n";
      return 2;
    }
  }
  std::string error;
  if (!ctx.create(error)) {
    std::cerr << "stress: " << error << "\n";
    return 2;
  }
  json failures = json::array();
  auto fail = [&](const std::string &check, const std::string &detail) {
    failures.push_back({{"phase", "region"}, {"check", check}, {"detail", detail}});
  };
  const auto g = plot_ground(preset, R);
  // Regions side by side: each region's space moved so their hexes don't overlap.
  const float pitch = (2.0f * static_cast<float>(R) + 2.0f) * 1.7320508f;
  std::vector<Live> regions(static_cast<size_t>(side * side));
  for (int i = 0; i < side * side; ++i) {
    Live &L = regions[i];
    L.offset[0] = (static_cast<float>(i % side) - 0.5f * static_cast<float>(side - 1)) * pitch;
    L.offset[1] = (static_cast<float>(i / side) - 0.5f * static_cast<float>(side - 1)) * pitch;
    foundry_region_desc d = {};
    d.plotter = plotter;
    d.world_seed = 7 + static_cast<uint32_t>(i);
    d.regions = g.data();
    d.region_count = static_cast<uint32_t>(g.size());
    const float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, L.offset[0], 0, L.offset[1], 1};
    std::memcpy(d.transform, t, sizeof t);
    L.region = foundry_region_create(ctx.rt, &d);
    if (L.region == FOUNDRY_NO_REGION) {
      std::cerr << "stress: can't create a region for plotter " << name << "\n";
      return 2;
    }
  }
  ctx.overview(0.5f * pitch * static_cast<float>(side), 10.0f);

  // Grows every region round-robin, batch placements per frame, spawning each; returns frames taken.
  std::vector<double> grow_ms, frame_ms;
  uint64_t placed = 0;
  auto grow_all = [&](foundry_grow_mode mode) {
    for (auto &L : regions) L.volume = 1;
    int frames = 0;
    for (bool any = true; any && frames < 100000; ++frames) {
      any = false;
      int budget = batch;
      for (auto &L : regions) {
        if (L.volume == 0 || budget <= 0) continue;
        std::vector<foundry_placement> out(static_cast<size_t>(budget));
        const double t0 = now_ms();
        const uint32_t n = foundry_region_grow(ctx.rt, L.region, mode, nullptr, 0, nullptr, out.data(),
                                               static_cast<uint32_t>(budget), &L.volume);
        grow_ms.push_back(now_ms() - t0);
        for (uint32_t k = 0; k < n; ++k) L.handles[key(out[k].slot)] = foundry_placement_spawn(ctx.rt, L.region, &out[k]);
        placed += n;
        budget -= static_cast<int>(n);
      }
      for (const auto &L : regions) any = any || L.volume > 0;  // regions this frame's budget didn't reach too
      frame_ms.push_back(ctx.frame().total);
    }
    return frames;
  };
  auto settle = [&](double max_s) {
    const double t0 = now_ms();
    for (int k = 0; k < static_cast<int>(max_s * 60); ++k) {
      foundry_diag_counters c;
      foundry_diag_counters_get(ctx.rt, &c);
      if (c.waiting == 0) break;
      frame_ms.push_back(ctx.frame().total);
    }
    return (now_ms() - t0) / 1000.0;
  };

  // 1. Grow.
  const double grow_t0 = now_ms();
  const int grow_frames = grow_all(FOUNDRY_GROW_SEED);
  const double built_s = settle(120);
  const double grow_wall_s = (now_ms() - grow_t0) / 1000.0;
  foundry_diag_counters full;
  foundry_diag_counters_get(ctx.rt, &full);
  uint64_t occupants = 0;
  for (auto &L : regions) occupants += foundry_region_count(ctx.rt, L.region);
  const Stats grow_stats = stats(grow_ms);
  const Stats grow_frames_stats = stats(frame_ms);
  if (full.failed) fail("build", std::to_string(full.failed) + " instances failed to build");
  if (full.bodies != full.instances)
    fail("bodies", fmt("%llu bodies for %llu instances", (unsigned long long)full.bodies, (unsigned long long)full.instances));

  // 2. Steady.
  frame_ms.clear();
  for (int k = 0; k < static_cast<int>(steady_s * 60); ++k) frame_ms.push_back(ctx.frame(true, k % 10 == 0).total);
  const Stats steady = stats(frame_ms);
  if (!ctx.snapshot_dir.empty()) ctx.snapshot(ctx.snapshot_dir + "/region.png");

  // 3. Bomb and regrow.
  json cycle_reports = json::array();
  Rng rng(7, 31);
  for (int cyc = 0; cyc < cycles; ++cyc) {
    uint64_t cleared = 0;
    frame_ms.clear();
    for (auto &L : regions) {
      const float r = static_cast<float>(R) * 1.2f;
      const float c[3] = {static_cast<float>(rng.unit() * 2 - 1) * r, 0, static_cast<float>(rng.unit() * 2 - 1) * r};
      std::vector<foundry_slot> slots(4096);
      const uint32_t n = foundry_region_find(ctx.rt, L.region, c, static_cast<float>(blast), slots.data(), 4096);
      slots.resize(std::min<uint32_t>(n, 4096));
      for (const auto &s : slots) {
        auto it = L.handles.find(key(s));
        if (it != L.handles.end()) {
          foundry_destroy(ctx.rt, it->second);
          L.handles.erase(it);
        }
      }
      foundry_region_vacate(ctx.rt, L.region, slots.data(), static_cast<uint32_t>(slots.size()));
      cleared += slots.size();
    }
    const FrameTime spike = ctx.frame(true, true);
    grow_ms.clear();
    const uint64_t before = placed;
    const int frames = grow_all(FOUNDRY_GROW_REGROW);
    const double rebuilt_s = settle(60);
    uint64_t now_occ = 0;
    for (auto &L : regions) now_occ += foundry_region_count(ctx.rt, L.region);
    cycle_reports.push_back({{"cleared", cleared},
                             {"regrown", placed - before},
                             {"occupants_after", now_occ},
                             {"frames_to_regrow", frames},
                             {"seconds_to_rebuild", rebuilt_s},
                             {"bomb_frame_ms", spike.total},
                             {"regrow_frame_ms", to_json(stats(frame_ms))},
                             {"grow_call_ms", to_json(stats(grow_ms))}});
  }

  // 4. Drain.
  for (auto &L : regions) {
    for (auto &[k, h] : L.handles) foundry_destroy(ctx.rt, h);
    L.handles.clear();
    foundry_region_destroy(ctx.rt, L.region);
  }
  for (int i = 0; i < static_cast<int>(2.5 / ctx.plan.dt); ++i) ctx.frame();
  foundry_diag_flush(ctx.rt);
  if (ctx.physics) toy_physics_settle(ctx.physics);
  foundry_diag_counters drained;
  foundry_diag_counters_get(ctx.rt, &drained);
  const uint32_t world_bodies = ctx.physics ? toy_physics_bodies(ctx.physics) : 0;
  if (drained.instances || drained.cache_entries || drained.bodies || world_bodies)
    fail("drain", fmt("after the drain: %llu instances, %llu builds, %llu bodies (%u in the world)",
                      (unsigned long long)drained.instances, (unsigned long long)drained.cache_entries,
                      (unsigned long long)drained.bodies, world_bodies));

  const double hectares = static_cast<double>(regions.size()) * (3.0 * R * (R + 1) + 1) * 2.5980762 / 10000.0;
  json summary = json::array();
  summary.push_back(fmt("%s  %zu regions of radius %d (%s, %.2f ha)  %s", name, regions.size(), R, preset.c_str(), hectares,
                        failures.empty() ? "PASS" : "FAIL"));
  summary.push_back(fmt("grow     %llu placements in %d frames of %d  grow call p50 %.2f ms max %.2f ms  built in %.1f s  frame p95 %.2f ms",
                        (unsigned long long)occupants, grow_frames, batch, grow_stats.p50, grow_stats.max, built_s,
                        grow_frames_stats.p95));
  summary.push_back(fmt("steady   %llu instances, %llu distinct builds, %llu bodies  frame p50 %.2f ms p95 %.2f ms",
                        (unsigned long long)full.instances, (unsigned long long)full.cache_entries,
                        (unsigned long long)full.bodies, steady.p50, steady.p95));
  for (size_t i = 0; i < cycle_reports.size(); ++i) {
    const auto &c = cycle_reports[i];
    summary.push_back(fmt("bomb %zu   cleared %llu within %.0f m, regrew %llu in %d frames, rebuilt in %.1f s, frame at the blast %.2f ms",
                          i + 1, (unsigned long long)c["cleared"].get<uint64_t>(), blast,
                          (unsigned long long)c["regrown"].get<uint64_t>(), c["frames_to_regrow"].get<int>(),
                          c["seconds_to_rebuild"].get<double>(), c["bomb_frame_ms"].get<double>()));
  }
  summary.push_back(fmt("drain    %llu instances, %llu builds, %llu bodies left", (unsigned long long)drained.instances,
                        (unsigned long long)drained.cache_entries, (unsigned long long)drained.bodies));
  for (const auto &l : summary) std::cout << l.get<std::string>() << "\n";
  for (const auto &f : failures) std::cout << "  FAIL " << f["check"].get<std::string>() << ": " << f["detail"].get<std::string>() << "\n";

  json report = {{"plotter", name},
                 {"kind", "region"},
                 {"started", stamp_now(true)},
                 {"machine", machine(ctx.use_gpu ? ctx.gpu.name : "", ctx.use_gpu ? ctx.gpu.driver : "")},
                 {"settings", {{"regions", regions.size()}, {"radius", R}, {"preset", preset}, {"batch", batch},
                               {"cycles", cycles}, {"blast_m", blast}, {"steady_s", steady_s}}},
                 {"hectares", hectares},
                 {"grow", {{"placements", occupants}, {"frames", grow_frames}, {"grow_call_ms", to_json(grow_stats)},
                           {"frame_ms", to_json(grow_frames_stats)}, {"seconds_to_build", built_s}, {"wall_s", grow_wall_s}}},
                 {"steady", {{"instances", full.instances}, {"builds", full.cache_entries}, {"bodies", full.bodies},
                             {"triangles", full.triangles}, {"frame_ms", to_json(steady)}}},
                 {"cycles", cycle_reports},
                 {"drain", {{"instances", drained.instances}, {"builds", drained.cache_entries}, {"bodies", drained.bodies},
                            {"world_bodies", world_bodies}}},
                 {"memory", to_json(memory())},
                 {"failures", failures},
                 {"pass", failures.empty()},
                 {"summary", summary}};
  if (write_report) {
    const std::string dir = workspace + "/perf";
    fs::create_directories(dir);
    const std::string path = dir + "/report-plotter_" + std::string(name) + "-" + stamp_now(false) + ".json";
    std::ofstream(path) << report.dump(1) << "\n";
    std::cout << "report: " << path << "\n";
  }
  ctx.destroy();
  ctx.gpu.shutdown();
  SDL_Quit();
  return failures.empty() ? 0 : 1;
}
