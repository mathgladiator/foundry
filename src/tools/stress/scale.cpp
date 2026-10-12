// The scale plan (design/quality.perf.md, "Scale"): a plotter's scenes at
// several sizes, simulated and drawn in full by the native runtime, as a game
// would run them.
//
// Each size is an N x N grid of regions (the glen's ground, each with its own
// world seed), grown to full and spawned with collision bodies, then measured
// from two cameras: the overview, which sees the whole scene, and a game
// camera standing in it. Every frame updates (wind, animation), prepares
// (culling, bone streaming), and draws; one frame in ten waits for the GPU on
// its own, to time the GPU alone. The largest size is then measured again with
// the scene changed, to say where the time goes: every instance at medium and
// at low detail, no wind (so everything sleeps), both, and with no drawing.
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

// What the camera sees, counted the way the runtime culls: each instance's bounding sphere against
// the frustum. The runtime doesn't say which pipeline it drew, so the tool counts for itself.
struct Seen {
  std::vector<uint64_t> instances, triangles;  // per binding
  uint64_t trees = 0;
};

Seen seen(Ctx &ctx, const Scene &scene, size_t bindings) {
  float vp[16];
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += ctx.projection[k * 4 + r] * ctx.view[c * 4 + k];
      vp[c * 4 + r] = s;
    }
  float planes[6][4];
  auto row = [&](int r, int i) { return vp[i * 4 + r]; };
  for (int i = 0; i < 4; ++i) {
    planes[0][i] = row(3, i) + row(0, i);
    planes[1][i] = row(3, i) - row(0, i);
    planes[2][i] = row(3, i) + row(1, i);
    planes[3][i] = row(3, i) - row(1, i);
    planes[4][i] = row(2, i);
    planes[5][i] = row(3, i) - row(2, i);
  }
  for (auto &p : planes) {
    const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (float &v : p) v /= len;
  }
  Seen out;
  out.instances.assign(bindings, 0);
  out.triangles.assign(bindings, 0);
  for (const Placed &p : scene.placed) {
    foundry_diag_instance info;
    if (foundry_diag_instance_get(ctx.rt, p.handle, &info) != FOUNDRY_OK || !info.ready) continue;
    const float c[3] = {p.x, info.height * 0.5f, p.z};
    bool in = true;
    for (const auto &pl : planes)
      if (pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3] < -info.radius) {
        in = false;
        break;
      }
    if (!in || p.pipeline < 0) continue;
    ++out.instances[static_cast<size_t>(p.pipeline)];
    out.triangles[static_cast<size_t>(p.pipeline)] += info.triangles;
    if (p.tree) ++out.trees;
  }
  return out;
}

struct Measure {
  Stats total, update, prepare, draw, gpu_wait, gpu;
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
  std::vector<double> total, update, prepare, draw, wait, gpu;
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
          {"gpu_wait_ms", stress::to_json(m.gpu_wait)},
          {"gpu_frame_ms", stress::to_json(m.gpu)},
          {"instances", m.counters.instances},
          {"awake", m.counters.awake},
          {"drawn", m.counters.drawn},
          {"culled", m.counters.culled},
          {"draws", m.counters.draws},
          {"triangles", m.counters.triangles},
          {"builds", m.counters.cache_entries},
          {"trees_visible", s.trees},
          {"update_stages", m.stages},
          {"by_pipeline", by}};
}

std::string line(const std::string &what, const Measure &m, const Seen &s) {
  return fmt("%-22s %6.1f fps (p95 frame %5.2f ms)  update %5.2f  prepare %5.2f  draw %4.2f  gpu %5.2f ms  "
             "%6llu drawn (%llu trees)  %5.1f M triangles  %llu draws",
             what.c_str(), m.fps, m.total.p95, m.update.mean, m.prepare.mean, m.draw.mean, m.gpu.mean,
             (unsigned long long)m.counters.drawn, (unsigned long long)s.trees, m.counters.triangles / 1e6,
             (unsigned long long)m.counters.draws);
}

}  // namespace

int scale_main(int argc, char **argv, foundry_plotter plotter, const char *name, const stress_pipeline *const *pipelines,
               uint32_t count) {
  std::string workspace = FOUNDRY_WORKSPACE, gpu_mode = "auto", preset = "glen", sizes_arg = "1,2,4,8,12,16";
  bool write_report = true, experiments = true;
  int R = 12, frames = 240;
  std::string snapshot_dir;
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
    else {
      std::cerr << "usage: stress_plotter_<name> --plan scale [--sizes 1,2,4,8 (grid sides)] [--radius cells]\n"
                   "       [--frames N] [--preset glen|hex] [--no-experiments] [--snapshot dir] [--gpu auto|none|required]\n"
                   "       [--no-report]\n";
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
            << ctx.plan.height << "\n";
  json sizes_out = json::array(), summary = json::array(), failures = json::array();
  json experiments_out = json::array();
  json headline = nullptr;
  for (size_t si = 0; si < sides.size(); ++si) {
    const int side = sides[si];
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

    json cams = json::object();
    // The overview sees everything; the game camera stands at the scene's centre, 12 m up and
    // 25 m back, looking ahead and down, with a 500 m far plane.
    for (const char *cam : {"overview", "game"}) {
      if (std::strcmp(cam, "overview") == 0) {
        ctx.overview(scene.half_extent, 10.0f);
      } else {
        const float eye[3] = {0, 12, 25}, target[3] = {0, 0, -5};
        ctx.camera(eye, target, 500);
      }
      const Measure m = measure(ctx, frames, true);
      const Seen s = seen(ctx, scene, count);
      cams[cam] = to_json(m, s, pipelines, count);
      const std::string what = fmt("%2dx%-2d %-8s", side, side, cam);
      summary.push_back(line(what, m, s));
      std::cout << summary.back().get<std::string>() << "\n" << std::flush;
      if (std::strcmp(cam, "overview") == 0 && headline.is_null() && s.trees >= 1000)
        headline = {{"grid", side}, {"trees_visible", s.trees}, {"instances", m.counters.drawn}, {"fps", cams[cam]["fps"]},
                    {"frame_ms_p95", m.total.p95}};
      if (!snapshot_dir.empty()) ctx.snapshot(fmt("%s/scale-%dx%d-%s.png", snapshot_dir.c_str(), side, side, cam));
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

    // The largest scene, changed one way at a time (overview camera).
    if (experiments && si + 1 == sides.size()) {
      ctx.overview(scene.half_extent, 10.0f);
      auto set_all = [&](const char *param, auto value_of) {
        for (const Placed &p : scene.placed) {
          if (p.pipeline < 0) continue;
          const stress_pipeline *b = pipelines[p.pipeline];
          const int idx = find_param(b, param);
          if (idx < 0) continue;
          b->set(ctx.rt, p.handle, static_cast<uint32_t>(idx), value_of(b, idx));
        }
        wait_built(ctx, 300);
      };
      auto detail = [&](const char *level) {
        set_all("detail", [&](const stress_pipeline *b, int idx) {
          stress_value v{};
          const stress_param &pp = b->params[idx];
          for (uint32_t e = 0; e < pp.count; ++e)
            if (std::strcmp(pp.values[e], level) == 0) v.u = e;
          return v;
        });
      };
      auto wind = [&](float speed) {
        set_all("wind_speed", [&](const stress_pipeline *, int) {
          stress_value v{};
          v.f = speed;
          return v;
        });
      };
      auto run = [&](const std::string &what, bool render) {
        const Measure m = measure(ctx, frames, render);
        const Seen s = seen(ctx, scene, count);
        json j = to_json(m, s, pipelines, count);
        j["experiment"] = what;
        experiments_out.push_back(j);
        summary.push_back(line(fmt("%2dx%-2d %s", side, side, what.c_str()), m, s));
        std::cout << summary.back().get<std::string>() << "\n" << std::flush;
        if (!snapshot_dir.empty() && render) {
          std::string file = what;
          std::replace(file.begin(), file.end(), ' ', '-');
          ctx.snapshot(fmt("%s/scale-%dx%d-%s.png", snapshot_dir.c_str(), side, side, file.c_str()));
        }
      };
      run("as placed", true);
      run("no drawing", false);
      detail("medium");
      run("medium detail", true);
      detail("low");
      run("low detail", true);
      wind(0);
      run("low detail, no wind", true);
      detail("high");
      run("high detail, no wind", true);
    }

    for (const Placed &p : scene.placed) foundry_destroy(ctx.rt, p.handle);
    for (foundry_region r : scene.regions) foundry_region_destroy(ctx.rt, r);
    for (int i = 0; i < 160; ++i) ctx.frame(false);
    foundry_diag_flush(ctx.rt);
  }

  json report = {{"plotter", name},
                 {"kind", "scale"},
                 {"started", stamp_now(true)},
                 {"machine", machine(ctx.use_gpu ? ctx.gpu.name : "", ctx.use_gpu ? ctx.gpu.driver : "")},
                 {"settings", {{"sizes", sides}, {"radius", R}, {"preset", preset}, {"frames", frames},
                               {"width", ctx.plan.width}, {"height", ctx.plan.height}, {"gpu", ctx.use_gpu}}},
                 {"headline", headline},
                 {"sizes", sizes_out},
                 {"experiments", experiments_out},
                 {"failures", failures},
                 {"pass", failures.empty()},
                 {"summary", summary}};
  if (!headline.is_null())
    std::cout << fmt("headline: %d trees visible (%llu things drawn) at %.1f fps\n", headline["trees_visible"].get<int>(),
                     (unsigned long long)headline["instances"].get<uint64_t>(), headline["fps"].get<double>());
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
