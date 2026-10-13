// The scale plan (design/quality.perf.md, "Scale"): a plotter's scenes at
// several sizes, simulated and drawn in full by the native runtime, as a game
// would run them.
//
// Each size is an N x N grid of regions (the glen's ground, each with its own
// world seed), grown to full and spawned with collision bodies, then measured
// from three cameras: the overview, which sees the whole scene from far above,
// a vista across it from a corner at game height, and a game camera standing in
// it. Every frame updates (wind, animation), prepares
// (culling, levels of detail, pose uploads), and draws; one frame in ten waits
// for the GPU on its own, to time the GPU alone.
//
// The largest listed size is then measured as a ladder: the runtime's cost
// decisions (design/submit.md, "Cost") turned on one at a time, from none (the
// game's detail everywhere, everything animated every frame) to all, so each
// one's gain is measured; and what they cost in looks, as the share of pixels
// that visibly change against the scene drawn in full at the same moment.
// With --find FPS, the grid then keeps growing until the overview drops below
// that frame rate: the most trees in view the machine holds at it.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "platform.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace stress {

namespace {

struct Placed {
  foundry_instance handle = FOUNDRY_NO_INSTANCE;
  int pipeline = -1;  // index into the bindings
  float x = 0, z = 0;
  bool tree = false;  // a tree standing (not a stump): what "trees visible" counts
};

struct Scene {
  std::vector<foundry_region> regions;
  std::vector<Placed> placed;
  float half_extent = 0;
};

int find_param(const stress_pipeline *p, const char *name) {
  for (uint32_t i = 0; i < p->param_count; ++i)
    if (std::strcmp(p->params[i].name, name) == 0) return static_cast<int>(i);
  return -1;
}

void wait_built(Ctx &ctx, double max_s) {
  for (int k = 0; k < static_cast<int>(max_s * 60); ++k) {
    foundry_diag_counters c;
    foundry_diag_counters_get(ctx.rt, &c);
    if (c.waiting == 0 && c.cache_pending == 0) break;
    SDL_Delay(1);
    ctx.frame(false);
  }
}

// What the runtime drew in the last frame, by binding: it says, for each instance, whether it was
// drawn and with how many triangles (at the level of detail it picked).
struct Seen {
  std::vector<uint64_t> instances, triangles;  // per binding
  uint64_t trees = 0;
};

Seen seen(Ctx &ctx, const Scene &scene, size_t bindings) {
  Seen out;
  out.instances.assign(bindings, 0);
  out.triangles.assign(bindings, 0);
  for (const Placed &p : scene.placed) {
    foundry_diag_instance info;
    if (foundry_diag_instance_get(ctx.rt, p.handle, &info) != FOUNDRY_OK || !info.ready || !info.drawn) continue;
    if (p.pipeline < 0) continue;
    ++out.instances[static_cast<size_t>(p.pipeline)];
    out.triangles[static_cast<size_t>(p.pipeline)] += info.drawn_triangles;
    if (p.tree) ++out.trees;
  }
  return out;
}

struct Measure {
  Stats total, update, prepare, draw, gpu_wait, gpu;
  Stats integrate, classify, animate, results;  // inside update
  foundry_diag_counters counters{};
  double fps = 0, fps_p95 = 0;
  json stages = json::object();  // update stages: CPU ms and calls per frame, summed over threads
};

std::map<std::string, std::pair<uint64_t, double>> stage_totals(Ctx &ctx) {
  std::vector<foundry_diag_stage> st(256);
  const uint32_t n = foundry_diag_stages(ctx.rt, st.data(), static_cast<uint32_t>(st.size()));
  std::map<std::string, std::pair<uint64_t, double>> out;
  for (uint32_t i = 0; i < n && i < st.size(); ++i)
    if (st[i].update) out[st[i].label] = {st[i].calls, st[i].total_ms};
  return out;
}

Measure measure(Ctx &ctx, int frames, bool render) {
  for (int k = 0; k < 30; ++k) ctx.frame(render);  // warm up: uploads, first poses
  std::vector<double> total, update, prepare, draw, wait, gpu, integrate, classify, animate, results;
  const auto before = stage_totals(ctx);
  for (int k = 0; k < frames; ++k) {
    const bool sync = render && k % 10 == 9;
    const FrameTime t = ctx.frame(render, sync);
    if (sync) {
      gpu.push_back(t.gpu_sync);
      continue;  // a synchronous frame's total includes the wait; keep it out of the frame rate
    }
    total.push_back(t.total);
    update.push_back(t.update);
    prepare.push_back(t.prepare);
    draw.push_back(t.draw);
    wait.push_back(t.gpu_wait);
    integrate.push_back(t.integrate);
    classify.push_back(t.classify);
    animate.push_back(t.animate);
    results.push_back(t.results);
  }
  Measure m;
  for (const auto &[label, after] : stage_totals(ctx)) {
    auto it = before.find(label);
    const uint64_t calls = after.first - (it == before.end() ? 0 : it->second.first);
    const double ms = after.second - (it == before.end() ? 0 : it->second.second);
    if (calls) m.stages[label] = {{"cpu_ms_per_frame", ms / frames}, {"calls_per_frame", static_cast<double>(calls) / frames},
                                  {"us_per_call", 1000.0 * ms / static_cast<double>(calls)}};
  }
  m.total = stats(total);
  m.update = stats(update);
  m.prepare = stats(prepare);
  m.draw = stats(draw);
  m.gpu_wait = stats(wait);
  m.gpu = stats(gpu);
  m.integrate = stats(integrate);
  m.classify = stats(classify);
  m.animate = stats(animate);
  m.results = stats(results);
  foundry_diag_counters_get(ctx.rt, &m.counters);
  m.fps = m.total.mean > 0 ? 1000.0 / m.total.mean : 0;
  m.fps_p95 = m.total.p95 > 0 ? 1000.0 / m.total.p95 : 0;
  return m;
}

json to_json(const Measure &m, const Seen &s, const stress_pipeline *const *pipelines, uint32_t count) {
  json by = json::object();
  for (uint32_t i = 0; i < count; ++i)
    by[pipelines[i]->name] = {{"visible", s.instances[i]}, {"triangles", s.triangles[i]}};
  return {{"fps", std::round(m.fps * 10) / 10},
          {"fps_at_p95", std::round(m.fps_p95 * 10) / 10},
          {"frame_ms", stress::to_json(m.total)},
          {"update_ms", stress::to_json(m.update)},
          {"prepare_ms", stress::to_json(m.prepare)},
          {"draw_ms", stress::to_json(m.draw)},
          {"update_parts_ms", {{"integrate", std::round(m.integrate.mean * 1000) / 1000},
                               {"classify", std::round(m.classify.mean * 1000) / 1000},
                               {"animate", std::round(m.animate.mean * 1000) / 1000},
                               {"results", std::round(m.results.mean * 1000) / 1000}}},
          {"gpu_wait_ms", stress::to_json(m.gpu_wait)},
          {"gpu_frame_ms", stress::to_json(m.gpu)},
          {"instances", m.counters.instances},
          {"awake", m.counters.awake},
          {"drawn", m.counters.drawn},
          {"culled", m.counters.culled},
          {"draws", m.counters.draws},
          {"triangles", m.counters.triangles},
          {"builds", m.counters.cache_entries},
          {"too_small", m.counters.too_small},
          {"by_level", {m.counters.by_level[0], m.counters.by_level[1], m.counters.by_level[2]}},
          {"animated", m.counters.animated},
          {"not_animated_unseen", m.counters.unseen},
          {"not_animated_rate", m.counters.rate_skipped},
          {"bones_uploaded", m.counters.bones_uploaded},
          {"trees_visible", s.trees},
          {"update_stages", m.stages},
          {"by_pipeline", by}};
}

std::string line(const std::string &what, const Measure &m, const Seen &s) {
  return fmt("%-44s %6.1f fps (p95 frame %5.2f ms)  update %5.2f  prepare %5.2f  draw %4.2f  gpu %5.2f ms  "
             "%6llu drawn (%llu trees)  %5.1f M triangles  %llu draws  %llu animated  [update: integrate %.2f classify %.2f "
             "animate %.2f results %.2f]",
             what.c_str(), m.fps, m.total.p95, m.update.mean, m.prepare.mean, m.draw.mean, m.gpu.mean,
             (unsigned long long)m.counters.drawn, (unsigned long long)s.trees, m.counters.triangles / 1e6,
             (unsigned long long)m.counters.draws, (unsigned long long)m.counters.animated, m.integrate.mean,
             m.classify.mean, m.animate.mean, m.results.mean);
}

}  // namespace

int scale_main(int argc, char **argv, foundry_plotter plotter, const char *name, const stress_pipeline *const *pipelines,
               uint32_t count) {
  std::string workspace = FOUNDRY_WORKSPACE, gpu_mode = "auto", preset = "glen", sizes_arg = "1,2,4,8,12,16";
  bool write_report = true, experiments = true;
  int R = 12, frames = 240, max_side = 64, find_step = 4;
  double find_fps = 0;
  std::string snapshot_dir, quality = "high";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--workspace") workspace = next();
    else if (a == "--plan") next();
    else if (a == "--no-report") write_report = false;
    else if (a == "--gpu") gpu_mode = next();
    else if (a == "--sizes") sizes_arg = next();
    else if (a == "--radius") R = std::max(1, std::atoi(next().c_str()));
    else if (a == "--frames") frames = std::max(20, std::atoi(next().c_str()));
    else if (a == "--preset") preset = next();
    else if (a == "--no-experiments") experiments = false;
    else if (a == "--snapshot") snapshot_dir = next();
    else if (a == "--find") find_fps = std::atof(next().c_str());
    else if (a == "--find-step") find_step = std::max(1, std::atoi(next().c_str()));
    else if (a == "--max-side") max_side = std::max(1, std::atoi(next().c_str()));
    else if (a == "--quality") quality = next();
    else {
      std::cerr << "usage: stress_plotter_<name> --plan scale [--sizes 1,2,4,8 (grid sides)] [--radius cells]\n"
                   "       [--frames N] [--preset glen|hex] [--no-experiments] [--snapshot dir] [--gpu auto|none|required]\n"
                   "       [--find FPS [--find-step 4] [--max-side 64]] [--quality low|medium|high] [--no-report]\n";
      return 2;
    }
  }
  std::vector<int> sides;
  {
    std::stringstream ss(sizes_arg);
    std::string tok;
    while (std::getline(ss, tok, ','))
      if (std::atoi(tok.c_str()) > 0) sides.push_back(std::atoi(tok.c_str()));
  }
  if (sides.empty()) return 2;

  Ctx ctx;
  ctx.title_name = name;
  ctx.plan.gpu = gpu_mode;
  ctx.max_bodies = 1u << 21;  // the largest scenes hold a few hundred thousand bodies
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
  if (!snapshot_dir.empty()) fs::create_directories(snapshot_dir);
  foundry_set_quality(ctx.rt, quality == "low" ? FOUNDRY_QUALITY_LOW : quality == "medium" ? FOUNDRY_QUALITY_MEDIUM : FOUNDRY_QUALITY_HIGH);
  const auto ground = plot_ground(preset, R);
  const float pitch = (2.0f * static_cast<float>(R) + 2.0f) * 1.7320508f;
  const double region_ha = (3.0 * R * (R + 1) + 1) * 2.5980762 / 10000.0;
  // Each kind's binding, by the pipeline its label starts with.
  const uint32_t kinds = foundry_plotter_kind_count(plotter);
  std::vector<int> kind_binding(kinds, -1);
  std::vector<bool> kind_tree(kinds, false);
  for (uint32_t k = 0; k < kinds; ++k) {
    const std::string label = foundry_plotter_kind_name(plotter, k);
    const std::string pipe = label.substr(0, label.find(' '));
    for (uint32_t b = 0; b < count; ++b)
      if (pipe == pipelines[b]->name) kind_binding[k] = static_cast<int>(b);
    kind_tree[k] = pipe.rfind("tree_", 0) == 0;
  }

  std::cout << name << "  scale plan  " << ctx.gpu.name << " (" << ctx.gpu.driver << ")  " << ctx.plan.width << "x"
            << ctx.plan.height << "  quality " << quality << "\n";
  json sizes_out = json::array(), summary = json::array(), failures = json::array();
  json ladder_out = json::array(), quality_out = json::object(), found = nullptr;
  json headline = nullptr;
  auto say = [&](const std::string &l) {
    summary.push_back(l);
    std::cout << l << "\n" << std::flush;
  };
  auto snap = [&](const std::string &file) {
    if (!snapshot_dir.empty()) ctx.snapshot(snapshot_dir + "/" + file);
  };
  auto game_camera = [&]() {
    const float eye[3] = {0, 12, 25}, target[3] = {0, 0, -5};
    ctx.camera(eye, target, 500);
  };
  auto vista_camera = [&](float half) {
    const float eye[3] = {-half, 25, -half}, target[3] = {half * 0.5f, 0, half * 0.5f};
    ctx.camera(eye, target, 4 * half + 100);
  };

  // One grid size: grown, measured from both cameras, and (when asked) laddered and judged.
  // Returns the overview's frame rate.
  auto run_size = [&](int side, bool ladder) -> double {
    Scene scene;
    scene.half_extent = 0.5f * pitch * static_cast<float>(side);
    const double t0 = now_ms();
    double grow_ms = 0;
    for (int i = 0; i < side * side; ++i) {
      const float ox = (static_cast<float>(i % side) - 0.5f * static_cast<float>(side - 1)) * pitch;
      const float oz = (static_cast<float>(i / side) - 0.5f * static_cast<float>(side - 1)) * pitch;
      foundry_region_desc d = {};
      d.plotter = plotter;
      d.world_seed = 7 + static_cast<uint32_t>(i);
      d.regions = ground.data();
      d.region_count = static_cast<uint32_t>(ground.size());
      const float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, ox, 0, oz, 1};
      std::memcpy(d.transform, t, sizeof t);
      const foundry_region region = foundry_region_create(ctx.rt, &d);
      scene.regions.push_back(region);
      std::vector<foundry_placement> out(4096);
      uint32_t volume = 1;
      while (volume > 0) {
        const double g0 = now_ms();
        const uint32_t n = foundry_region_grow(ctx.rt, region, FOUNDRY_GROW_SEED, nullptr, 0, nullptr, out.data(),
                                               static_cast<uint32_t>(out.size()), &volume);
        grow_ms += now_ms() - g0;
        if (n == 0) break;
        for (uint32_t k = 0; k < n; ++k) {
          Placed p;
          p.handle = foundry_placement_spawn(ctx.rt, region, &out[k]);
          p.pipeline = out[k].kind < kinds ? kind_binding[out[k].kind] : -1;
          p.tree = out[k].kind < kinds && kind_tree[out[k].kind];
          p.x = ox + out[k].position[0];
          p.z = oz + out[k].position[2];
          scene.placed.push_back(p);
        }
      }
    }
    wait_built(ctx, 300);
    const double ready_s = (now_ms() - t0) / 1000.0;
    Memory mem = memory();

    // The overview sees everything from far above; the vista stands at a corner of the world, 25 m
    // up, and looks across all of it (the near trees full size, the far ones a few pixels); the game
    // camera stands at the scene's centre, 12 m up and 25 m back, looking ahead and down, with a
    // 500 m far plane. Coarser levels of detail are built on first sight, so each camera settles
    // before it is measured.
    json cams = json::object();
    double overview_fps = 0, vista_fps = 0;
    for (const char *cam : {"overview", "vista", "game"}) {
      const bool over = std::strcmp(cam, "overview") == 0, vista = std::strcmp(cam, "vista") == 0;
      if (over) ctx.overview(scene.half_extent, 10.0f);
      else if (vista) vista_camera(scene.half_extent);
      else game_camera();
      for (int k = 0; k < 4; ++k) ctx.frame(true);
      wait_built(ctx, 300);
      const Measure m = measure(ctx, frames, true);
      const Seen s = seen(ctx, scene, count);
      cams[cam] = to_json(m, s, pipelines, count);
      say(fmt("%2dx%-2d %-8s", side, side, cam) + " " + line("", m, s).substr(45));
      if (vista) vista_fps = m.fps;
      if (over) {
        overview_fps = m.fps;
        if (headline.is_null() && s.trees >= 1000)
          headline = {{"grid", side}, {"trees_visible", s.trees}, {"instances", m.counters.drawn},
                      {"fps", cams[cam]["fps"]}, {"frame_ms_p95", m.total.p95}};
      }
      snap(fmt("scale-%dx%d-%s.png", side, side, cam));
    }
    foundry_diag_counters c;
    foundry_diag_counters_get(ctx.rt, &c);
    if (c.failed) failures.push_back({{"check", "build"}, {"detail", fmt("%llu instances failed to build", (unsigned long long)c.failed)}});
    sizes_out.push_back({{"grid", side},
                         {"regions", side * side},
                         {"hectares", region_ha * side * side},
                         {"placements", scene.placed.size()},
                         {"grow_ms", grow_ms},
                         {"seconds_to_ready", ready_s},
                         {"bodies", c.bodies},
                         {"builds", c.cache_entries},
                         {"memory", to_json(mem)},
                         {"cameras", cams}});

    if (ladder) {
      // The ladder: each cost decision turned on in turn, in the order of the most gain for the
      // least change in looks, from both cameras.
      struct Step {
        const char *what;
        uint32_t cost;
      };
      const Step steps[] = {
          {"none (the game's detail, all animated)", 0},
          {"+ poses kept on the GPU", FOUNDRY_DIAG_COST_RETAIN},
          {"+ out of view not animated", FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_UNSEEN},
          {"+ small things animated less often", FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_UNSEEN | FOUNDRY_DIAG_COST_RATE},
          {"+ detail by size on screen",
           FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_UNSEEN | FOUNDRY_DIAG_COST_RATE | FOUNDRY_DIAG_COST_DETAIL},
          {"+ too small not drawn (all)", FOUNDRY_DIAG_COST_ALL},
      };
      for (const char *cam : {"overview", "game"}) {
        if (std::strcmp(cam, "overview") == 0) ctx.overview(scene.half_extent, 10.0f);
        else game_camera();
        for (const Step &st : steps) {
          foundry_diag_set_cost(ctx.rt, st.cost);
          for (int k = 0; k < 4; ++k) ctx.frame(true);
          wait_built(ctx, 300);
          const Measure m = measure(ctx, frames, true);
          const Seen s = seen(ctx, scene, count);
          json j = to_json(m, s, pipelines, count);
          j["camera"] = cam;
          j["step"] = st.what;
          j["cost"] = st.cost;
          ladder_out.push_back(j);
          say(fmt("%2dx%-2d %-8s %s", side, side, cam, st.what) + " " + line("", m, s).substr(45));
        }
        // No drawing at all: the CPU's floor with every decision on.
        foundry_diag_set_cost(ctx.rt, FOUNDRY_DIAG_COST_ALL);
        {
          const Measure m = measure(ctx, frames, false);
          const Seen s = seen(ctx, scene, count);
          json j = to_json(m, s, pipelines, count);
          j["camera"] = cam;
          j["step"] = "all, no drawing (the CPU alone)";
          j["cost"] = FOUNDRY_DIAG_COST_ALL;
          ladder_out.push_back(j);
          say(fmt("%2dx%-2d %-8s %s", side, side, cam, "all, no drawing (the CPU alone)") + " " + line("", m, s).substr(45));
        }

        // What the decisions cost in looks, against the scene drawn in full at the same moment.
        // Time stands still (dt 0) for the comparisons, so every pose is exact and only the
        // decision differs; the rate's cost is its stale poses, against the same moment posed
        // fresh.
        std::vector<uint8_t> all_on, exact, detail_only, small_only, full;
        foundry_diag_set_cost(ctx.rt, FOUNDRY_DIAG_COST_ALL);
        for (int k = 0; k < 30; ++k) ctx.frame(true);
        ctx.read_frame(all_on);
        const double dt = ctx.plan.dt;
        ctx.plan.dt = 0;
        auto still = [&](uint32_t cost, std::vector<uint8_t> &px) {
          foundry_diag_set_cost(ctx.rt, cost);
          ctx.frame(true);
          ctx.read_frame(px);
        };
        still(FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_DETAIL | FOUNDRY_DIAG_COST_TOO_SMALL, exact);
        still(FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_DETAIL, detail_only);
        still(FOUNDRY_DIAG_COST_RETAIN | FOUNDRY_DIAG_COST_TOO_SMALL, small_only);
        still(FOUNDRY_DIAG_COST_RETAIN, full);
        ctx.plan.dt = dt;
        foundry_diag_set_cost(ctx.rt, FOUNDRY_DIAG_COST_ALL);
        json q = json::object();
        auto judge = [&](const char *what, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
          std::vector<uint8_t> mask;
          const Diff d = image_diff(a, b, snapshot_dir.empty() ? nullptr : &mask);
          q[what] = {{"image_pct", std::round(d.image * 1000) / 1000}, {"object_pct", std::round(d.object * 1000) / 1000}};
          say(fmt("%2dx%-2d %-8s looks: %-28s %6.3f%% of the image, %6.3f%% of what shows, visibly changed", side, side,
                  cam, what, d.image, d.object));
          if (!mask.empty())
            stbi_write_png((snapshot_dir + fmt("/quality-%s-%s.png", cam, what)).c_str(), static_cast<int>(ctx.plan.width),
                           static_cast<int>(ctx.plan.height), 4, mask.data(), static_cast<int>(ctx.plan.width * 4));
        };
        judge("rate", all_on, exact);
        judge("detail", detail_only, full);
        judge("too_small", small_only, full);
        judge("all", all_on, full);
        quality_out[cam] = q;
        if (!snapshot_dir.empty()) {
          auto write = [&](const char *what, const std::vector<uint8_t> &px) {
            stbi_write_png((snapshot_dir + fmt("/quality-%s-%s.png", cam, what)).c_str(), static_cast<int>(ctx.plan.width),
                           static_cast<int>(ctx.plan.height), 4, px.data(), static_cast<int>(ctx.plan.width * 4));
          };
          write("full", full);
          write("all-on", all_on);
        }
      }
      foundry_diag_set_cost(ctx.rt, FOUNDRY_DIAG_COST_ALL);
    }

    for (const Placed &p : scene.placed) foundry_destroy(ctx.rt, p.handle);
    for (foundry_region r : scene.regions) foundry_region_destroy(ctx.rt, r);
    for (int i = 0; i < 160; ++i) ctx.frame(false);
    foundry_diag_flush(ctx.rt);
    return std::min(overview_fps, vista_fps);
  };

  for (size_t si = 0; si < sides.size(); ++si) run_size(sides[si], experiments && si + 1 == sides.size());
  // The most trees in view at the target frame rate: grow the grid until the overview drops below it.
  if (find_fps > 0) {
    // Both wide views must hold it: the overview and the vista.
    auto held = [&](const json &sz) {
      return std::min(sz["cameras"]["overview"]["fps"].get<double>(), sz["cameras"]["vista"]["fps"].get<double>());
    };
    json best = nullptr;
    for (const json &sz : sizes_out)
      if (held(sz) >= find_fps) best = sz;
    int side = sides.back();
    double fps = held(sizes_out.back());
    while (fps >= find_fps && side + find_step <= max_side) {
      side += find_step;
      fps = run_size(side, false);
      if (fps >= find_fps) best = sizes_out.back();
    }
    if (!best.is_null()) {
      const json &o = best["cameras"]["overview"], &v = best["cameras"]["vista"];
      found = {{"target_fps", find_fps},
               {"grid", best["grid"]},
               {"trees_visible", o["trees_visible"]},
               {"instances", o["drawn"]},
               {"placements", best["placements"]},
               {"fps", o["fps"]},
               {"frame_ms_p95", o["frame_ms"]["p95"]},
               {"vista", {{"trees_visible", v["trees_visible"]}, {"instances", v["drawn"]}, {"fps", v["fps"]},
                          {"frame_ms_p95", v["frame_ms"]["p95"]}}},
               {"next_grid_fps", std::round(fps * 10) / 10},
               {"stopped_by", fps >= find_fps ? "max side" : "frame rate"}};
    }
  }

  json report = {{"plotter", name},
                 {"kind", "scale"},
                 {"started", stamp_now(true)},
                 {"machine", machine(ctx.use_gpu ? ctx.gpu.name : "", ctx.use_gpu ? ctx.gpu.driver : "")},
                 {"settings", {{"sizes", sides}, {"radius", R}, {"preset", preset}, {"frames", frames},
                               {"width", ctx.plan.width}, {"height", ctx.plan.height}, {"gpu", ctx.use_gpu},
                               {"quality", quality}, {"find_fps", find_fps}}},
                 {"headline", headline},
                 {"most_trees", found},
                 {"sizes", sizes_out},
                 {"ladder", ladder_out},
                 {"looks", quality_out},
                 {"failures", failures},
                 {"pass", failures.empty()},
                 {"summary", summary}};
  if (!headline.is_null())
    std::cout << fmt("headline: %d trees visible (%llu things drawn) at %.1f fps\n", headline["trees_visible"].get<int>(),
                     (unsigned long long)headline["instances"].get<uint64_t>(), headline["fps"].get<double>());
  if (!found.is_null())
    std::cout << fmt("most trees at %.0f fps: grid %d, overview %llu trees visible (%llu things drawn) at %.1f fps, vista %llu "
                     "trees visible (%llu things drawn) at %.1f fps\n",
                     find_fps, found["grid"].get<int>(), (unsigned long long)found["trees_visible"].get<uint64_t>(),
                     (unsigned long long)found["instances"].get<uint64_t>(), found["fps"].get<double>(),
                     (unsigned long long)found["vista"]["trees_visible"].get<uint64_t>(),
                     (unsigned long long)found["vista"]["instances"].get<uint64_t>(), found["vista"]["fps"].get<double>());
  if (write_report) {
    const std::string dir = workspace + "/perf";
    fs::create_directories(dir);
    const std::string path = dir + "/report-plotter_" + std::string(name) + "-scale-" + stamp_now(false) + ".json";
    std::ofstream(path) << report.dump(1) << "\n";
    std::cout << "report: " << path << "\n";
  }
  ctx.destroy();
  ctx.gpu.shutdown();
  SDL_Quit();
  return failures.empty() ? 0 : 1;
}

}  // namespace stress
