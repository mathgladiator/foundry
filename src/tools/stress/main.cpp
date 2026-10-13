// The stress binary's front door (design/quality.perf.md): the command line,
// plans, the run, the report and its comparison with the timeline, sampling
// for reference hashes, crash capture, and replay.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "platform.h"

#ifndef FOUNDRY_WORKSPACE
#define FOUNDRY_WORKSPACE "demo"
#endif
#ifndef FOUNDRY_SOURCE_DIR
#define FOUNDRY_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace stress {

namespace {

Journal *g_journal = nullptr;

void on_crash(int sig) {
  // Best effort: the journal's tail is the reproduction.
  if (g_journal) g_journal->write({{"op", "crash"}, {"signal", sig}});
  if (g_journal) g_journal->flush();
  const char msg[] = "stress: crashed; the journal's last action is the reproduction (--replay)\n";
  (void)!write(2, msg, sizeof msg - 1);
  _exit(3);
}

std::string utc_stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string utc_iso() {
  std::time_t t = std::time(nullptr);
  std::tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string run(const std::string &cmd) {
  std::string out;
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) return out;
  char buf[256];
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

json read_json(const std::string &path) {
  std::ifstream f(path);
  if (!f) return nullptr;
  try {
    return json::parse(f);
  } catch (...) {
    return nullptr;
  }
}

bool load_plan(const std::string &workspace, const std::string &arg, Plan &plan, std::string &error) {
  std::string path = arg;
  if (!fs::exists(path)) path = workspace + "/perf/plans/" + arg + ".json";
  json j = read_json(path);
  if (!j.is_object()) {
    error = "can't read plan " + arg + " (looked for " + path + ")";
    return false;
  }
  plan.source = j;
  plan.name = j.value("name", fs::path(path).stem().string());
  plan.seed = j.value("seed", 7ull);
  plan.gpu = j.value("gpu", "auto");
  if (j.contains("resolution")) {
    plan.width = j["resolution"][0];
    plan.height = j["resolution"][1];
  }
  plan.dt = j.value("dt", 1.0 / 60.0);
  plan.budget_ms = j.value("budget_ms", 1000.0 / 60.0);
  plan.update_threads = j.value("update_threads", 0u);
  plan.build_threads = j.value("build_threads", 0u);
  plan.fixed = j.value("fixed", json::object());
  plan.ranges = j.value("ranges", json::object());
  plan.thresholds = j.value("thresholds", json::object());
  for (const auto &p : j.value("phases", json::array())) {
    if (!p.contains("kind")) {
      error = "a phase has no kind";
      return false;
    }
    const std::string k = p["kind"];
    if (k != "diversity" && k != "forest" && k != "events" && k != "soak" && k != "explore") {
      error = "unknown phase kind " + k;
      return false;
    }
    plan.phases.push_back(p);
  }
  return true;
}

json describe_pipeline(const stress_pipeline &pl) {
  static const char *kinds[] = {"enum", "u32", "f32", "clock", "event"};
  json params = json::array();
  for (uint32_t i = 0; i < pl.param_count; ++i) {
    const auto &p = pl.params[i];
    json j = {{"name", p.name}, {"kind", kinds[p.kind]}, {"phase", p.phase == STRESS_BUILD ? "build" : "update"}};
    if (p.kind == STRESS_ENUM) {
      j["values"] = json::array();
      for (uint32_t k = 0; k < p.count; ++k) j["values"].push_back(p.values[k]);
      j["default"] = p.values[p.fallback.u];
    } else if (p.kind == STRESS_U32 || p.kind == STRESS_F32) {
      j["range"] = {p.min, p.max};
      j["default"] = p.kind == STRESS_U32 ? json(p.fallback.u) : json(p.fallback.f);
    }
    params.push_back(j);
  }
  json systems = json::array();
  for (uint32_t i = 0; i < pl.system_count; ++i) systems.push_back(pl.systems[i]);
  return {{"pipeline", pl.name}, {"state", pl.state}, {"systems", systems}, {"params", params}};
}

// The latest earlier report for this pipeline and plan on this machine.
json previous_report(const std::string &dir, const std::string &pipeline, const std::string &plan,
                     const std::string &fingerprint, std::string &name) {
  std::vector<std::string> files;
  std::error_code ec;
  const std::string prefix = "report-" + pipeline + "-";
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    std::string fn = e.path().filename().string();
    if (fn.rfind(prefix, 0) == 0 && e.path().extension() == ".json") files.push_back(fn);
  }
  std::sort(files.rbegin(), files.rend());
  for (const auto &fn : files) {
    json r = read_json(dir + "/" + fn);
    if (!r.is_object()) continue;
    if (r.value("plan_name", "") != plan) continue;
    if (!r.contains("machine") || r["machine"].value("fingerprint", "") != fingerprint) continue;
    name = fn.substr(0, fn.size() - 5);
    return r;
  }
  return nullptr;
}

const json *phase_of(const json &report, const std::string &kind) {
  for (const auto &p : report["phases"])
    if (p["kind"] == kind) return &p;
  return nullptr;
}

std::string pct(double now, double before) {
  if (before <= 0) return "n/a";
  return fmt("%+.0f%%", (now - before) / before * 100);
}

std::string compare(const json &now, const json &before, const std::string &name) {
  std::vector<std::string> parts;
  const json *a = phase_of(now, "diversity"), *b = phase_of(before, "diversity");
  if (a && b) {
    parts.push_back("generation p50 " + pct((*a)["data"]["generation_ms"]["p50"], (*b)["data"]["generation_ms"]["p50"]));
    parts.push_back("diversity frame p95 " +
                    pct((*a)["data"]["frames"]["total_ms"]["p95"], (*b)["data"]["frames"]["total_ms"]["p95"]));
  }
  a = phase_of(now, "forest");
  b = phase_of(before, "forest");
  if (a && b) {
    uint32_t best = 0;
    double pa = 0, pb = 0;
    for (const auto &sa : (*a)["data"]["samples"])
      for (const auto &sb : (*b)["data"]["samples"])
        if (sa["count"] == sb["count"] && sa["count"].get<uint32_t>() >= best) {
          best = sa["count"];
          pa = sa["p95_ms"];
          pb = sb["p95_ms"];
        }
    if (best) parts.push_back(fmt("forest p95 at %u ", best) + pct(pa, pb));
    parts.push_back("capacity " + pct((*a)["data"]["capacity"].get<double>(), (*b)["data"]["capacity"].get<double>()));
  }
  a = phase_of(now, "events");
  b = phase_of(before, "events");
  // A pipeline without events records no results; compare only when both runs have some.
  auto results = [](const json *ph) {
    return ph && (*ph)["data"].is_object() && (*ph)["data"].contains("results") && (*ph)["data"]["results"].is_array() &&
           !(*ph)["data"]["results"].empty();
  };
  if (results(a) && results(b)) {
    const auto &ra = (*a)["data"]["results"][0], &rb = (*b)["data"]["results"][0];
    if (ra["count"] == rb["count"]) parts.push_back("events spike " + pct(ra["spike_max_ms"], rb["spike_max_ms"]));
  }
  a = phase_of(now, "soak");
  b = phase_of(before, "soak");
  if (a && b)
    parts.push_back(fmt("soak slope %+.1f -> %+.1f MB/h", (*b)["data"]["rss_slope_mb_per_hour"].get<double>(),
                        (*a)["data"]["rss_slope_mb_per_hour"].get<double>()));
  std::string out = "vs " + name + ":";
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : " ") + parts[i];
  return out;
}

int replay(const stress_pipeline &pl, const std::string &path, uint64_t until) {
  std::ifstream f(path);
  if (!f) {
    std::cerr << "can't read " << path << "\n";
    return 2;
  }
  Ctx ctx;
  ctx.pipeline = &pl;
  std::string error;
  if (!ctx.create(error)) {
    std::cerr << error << "\n";
    return 2;
  }
  std::map<uint64_t, foundry_instance> handles;
  std::string line;
  uint64_t done = 0;
  while (std::getline(f, line)) {
    json j = json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    if (j.value("i", 0ull) > until) break;
    const std::string op = j.value("op", "");
    auto h = [&] { return handles.count(j["handle"]) ? handles[j["handle"]] : FOUNDRY_NO_INSTANCE; };
    if (op == "spawn") {
      Config c;
      ctx.parse(j["values"], c);
      float m[16];
      for (int k = 0; k < 16; ++k) m[k] = j["transform"][k];
      handles[j["handle"]] = pl.spawn(ctx.rt, c.data(), m);
    } else if (op == "set") {
      int p = ctx.param(j["param"]);
      Config c;
      ctx.parse({{j["param"].get<std::string>(), j["value"]}}, c);
      if (p >= 0) pl.set(ctx.rt, h(), p, c[p]);
    } else if (op == "trigger") {
      int p = ctx.param(j["param"]);
      if (p >= 0) pl.trigger(ctx.rt, h(), p);
    } else if (op == "destroy") {
      foundry_destroy(ctx.rt, h());
    } else if (op == "frames") {
      for (uint32_t k = 0; k < j["n"].get<uint32_t>(); ++k) {
        foundry_update(ctx.rt, j["dt"].get<float>());
        foundry_prepare(ctx.rt, nullptr);
      }
    } else if (op == "flush") {
      foundry_diag_flush(ctx.rt);
    }
    ++done;
  }
  foundry_diag_counters c;
  foundry_diag_counters_get(ctx.rt, &c);
  std::cout << fmt("replayed %llu actions: %llu instances, %llu cache entries, %llu generations\n",
                   static_cast<unsigned long long>(done), static_cast<unsigned long long>(c.instances),
                   static_cast<unsigned long long>(c.cache_entries), static_cast<unsigned long long>(c.generations));
  ctx.destroy();
  return 0;
}

void usage() {
  std::cerr << "usage: stress_<pipeline> [--describe] [--plan NAME|PATH] [--phase KIND] [--seconds N]\n"
               "                         [--gpu auto|none|required] [--workspace DIR] [--reference FILE]\n"
               "                         [--sample OUT] [--no-report] [--snapshot DIR|run] [--window]\n"
               "                         [--video OUT.mp4 [--floats K] [--seconds-per S] [--fps N] [--size WxH]]\n"
               "                         [--lod REPORT.json [--lod-configs N] [--snapshot DIR]]\n"
               "                         [--fixed JSON] [--ranges JSON]\n"
               "                         [--replay JOURNAL [--until N]]\n";
}

}  // namespace

}  // namespace stress

extern "C" int stress_main(int argc, char **argv, const stress_pipeline *const *pipelines, uint32_t count) {
  using namespace stress;
  std::string plan_arg = "quick", only_phase, gpu_override, workspace = FOUNDRY_WORKSPACE, reference_path, sample_out,
              replay_path, which, snapshot_dir;
  double seconds_override = -1;
  bool describe = false, write_report = true, window = false;
  VideoOptions video;
  LodOptions lod;
  bool lod_mode = false;
  std::string size_arg, fixed_arg, ranges_arg;
  bool plan_given = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plan") plan_given = true;
  uint64_t until = ~0ull;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--describe") describe = true;
    else if (a == "--plan") plan_arg = next();
    else if (a == "--phase") only_phase = next();
    else if (a == "--seconds") seconds_override = std::atof(next().c_str());
    else if (a == "--gpu") gpu_override = next();
    else if (a == "--workspace") workspace = next();
    else if (a == "--reference") reference_path = next();
    else if (a == "--sample") sample_out = next();
    else if (a == "--no-report") write_report = false;
    else if (a == "--replay") replay_path = next();
    else if (a == "--until") until = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--pipeline") which = next();
    else if (a == "--snapshot") snapshot_dir = next();
    else if (a == "--window") window = true;
    else if (a == "--video") video.path = next();
    else if (a == "--floats") video.floats = std::max(1, std::atoi(next().c_str()));
    else if (a == "--seconds-per") video.seconds = std::max(0.1, std::atof(next().c_str()));
    else if (a == "--title-seconds") video.title_seconds = std::max(0.0, std::atof(next().c_str()));
    else if (a == "--fps") video.fps = std::clamp(std::atoi(next().c_str()), 1, 120);
    else if (a == "--size") size_arg = next();
    else if (a == "--lod") lod.report_path = next();
    else if (a == "--lod-configs") lod.configs = std::max(1, std::atoi(next().c_str()));
    else if (a == "--fixed") fixed_arg = next();
    else if (a == "--ranges") ranges_arg = next();
    else {
      usage();
      return 2;
    }
  }
  const stress_pipeline *pl = pipelines[0];
  for (uint32_t i = 0; i < count; ++i)
    if (which == pipelines[i]->name) pl = pipelines[i];
  if (describe) {
    std::cout << describe_pipeline(*pl).dump(2) << "\n";
    return 0;
  }
  if (!replay_path.empty()) return replay(*pl, replay_path, until);

  Ctx ctx;
  ctx.pipeline = pl;
  std::string error;
  if ((plan_given || video.path.empty()) && !load_plan(workspace, plan_arg, ctx.plan, error)) {
    std::cerr << "stress: " << error << "\n";
    return 2;
  }
  try {
    const json fixed = fixed_arg.empty() ? json::object() : json::parse(fixed_arg);
    const json ranges = ranges_arg.empty() ? json::object() : json::parse(ranges_arg);
    for (auto it = fixed.begin(); it != fixed.end(); ++it) ctx.plan.fixed[it.key()] = it.value();
    for (auto it = ranges.begin(); it != ranges.end(); ++it) ctx.plan.ranges[it.key()] = it.value();
  } catch (const std::exception &e) {
    std::cerr << "stress: --fixed and --ranges take JSON objects: " << e.what() << "\n";
    return 2;
  }
  if (!video.path.empty()) {  // the QA video: rendered off screen, nothing else runs
    ctx.plan.gpu = "required";
    ctx.plan.width = 1280;
    ctx.plan.height = 720;
    if (!size_arg.empty() && std::sscanf(size_arg.c_str(), "%ux%u", &ctx.plan.width, &ctx.plan.height) != 2) {
      std::cerr << "stress: --size takes WIDTHxHEIGHT\n";
      return 2;
    }
    ctx.plan.dt = 1.0 / video.fps;
    SDL_Init(SDL_INIT_VIDEO);
    std::string gerr;
    ctx.use_gpu = ctx.gpu.init(ctx.plan, gerr);
    if (!ctx.use_gpu) {
      std::cerr << "stress: video needs a GPU: " << gerr << "\n";
      return 2;
    }
    if (!ctx.create(error)) {
      std::cerr << "stress: " << error << "\n";
      return 2;
    }
    fs::create_directories(fs::path(video.path).parent_path().empty() ? "." : fs::path(video.path).parent_path());
    const int rc = run_video(ctx, video);
    ctx.destroy();
    ctx.gpu.shutdown();
    SDL_Quit();
    return rc;
  }
  if (!gpu_override.empty()) ctx.plan.gpu = gpu_override;
  lod_mode = !lod.report_path.empty();
  if (lod_mode) {  // detail sizes: rendered off screen, nothing else runs
    lod.snapshot_dir = snapshot_dir;
    ctx.plan.gpu = "required";
    ctx.plan.width = 1920;
    ctx.plan.height = 1080;
    SDL_Init(SDL_INIT_VIDEO);
    std::string gerr;
    ctx.use_gpu = ctx.gpu.init(ctx.plan, gerr);
    if (!ctx.use_gpu) {
      std::cerr << "stress: detail sizes need a GPU: " << gerr << "\n";
      return 2;
    }
    if (!ctx.create(error)) {
      std::cerr << "stress: " << error << "\n";
      return 2;
    }
    const int rc = run_lod(ctx, lod);
    ctx.destroy();
    ctx.gpu.shutdown();
    SDL_Quit();
    return rc;
  }
  if (window) {
    ctx.plan.window = true;
    ctx.plan.gpu = "required";
    if (ctx.plan.width == 1920) {  // a window that fits on screen
      ctx.plan.width = 1600;
      ctx.plan.height = 900;
    }
    write_report = false;  // a window waits on the display: its numbers stay out of the timeline
  }
  if (!only_phase.empty()) {
    std::vector<json> kept;
    for (auto &p : ctx.plan.phases)
      if (p["kind"] == only_phase) kept.push_back(p);
    if (kept.empty()) kept.push_back({{"kind", only_phase}});
    ctx.plan.phases = kept;
  }
  if (seconds_override >= 0)
    for (auto &p : ctx.plan.phases) {
      if (p["kind"] == "soak") p["minutes"] = seconds_override / 60;
      else p["seconds"] = seconds_override;
    }
  ctx.rng = Rng(ctx.plan.seed, 1);

  if (!sample_out.empty()) {  // the configs diversity draws, for the foundry to hash in WebAssembly
    json phase = json::object();
    for (auto &p : ctx.plan.phases)
      if (p["kind"] == "diversity") phase = p;
    Rng rng(ctx.plan.seed, 11);
    auto configs = sample(ctx, static_cast<size_t>(phase.value("configs", 100.0)), rng);
    json out = {{"pipeline", pl->name}, {"plan", ctx.plan.name}, {"seed", ctx.plan.seed}};
    out["clocks"] = default_clocks(ctx);
    out["configs"] = json::array();
    for (const auto &c : configs) out["configs"].push_back({{"values", ctx.describe(c)}});
    std::ofstream(sample_out) << out.dump(1) << "\n";
    std::cout << "wrote " << configs.size() << " configs to " << sample_out << "\n";
    return 0;
  }
  json reference = nullptr;
  if (!reference_path.empty()) {
    reference = read_json(reference_path);
    if (!reference.is_object() || !reference.contains("configs")) {
      std::cerr << "stress: can't read reference " << reference_path << "\n";
      return 2;
    }
  }

  SDL_Init(SDL_INIT_VIDEO);  // may fail without a display; off-screen rendering doesn't need one
  if (ctx.plan.gpu != "none") {
    std::string gerr;
    ctx.use_gpu = ctx.gpu.init(ctx.plan, gerr);
    if (!ctx.use_gpu) {
      if (ctx.plan.gpu == "required") {
        std::cerr << "stress: " << gerr << "\n";
        return 2;
      }
      ctx.gpu.note = "no GPU (" + gerr + "); ran without submission";
    }
  }
  if (!ctx.create(error)) {
    std::cerr << "stress: " << error << "\n";
    return 2;
  }

  const std::string stamp = utc_stamp();
  const std::string perf_dir = workspace + "/perf";
  const std::string run_dir = perf_dir + "/runs/" + std::string(pl->name) + "-" + stamp;
  fs::create_directories(run_dir);
  ctx.journal.open(run_dir + "/journal.jsonl");
  ctx.snapshot_dir = snapshot_dir == "run" ? run_dir : snapshot_dir;
  if (!ctx.snapshot_dir.empty()) fs::create_directories(ctx.snapshot_dir);
  ctx.journal.write({{"op", "start"}, {"pipeline", pl->name}, {"plan", ctx.plan.source}, {"seed", ctx.plan.seed}});
  g_journal = &ctx.journal;
  std::signal(SIGSEGV, on_crash);
  std::signal(SIGABRT, on_crash);
  std::signal(SIGFPE, on_crash);
  std::signal(SIGBUS, on_crash);

  json report = {{"pipeline", pl->name},
                 {"state", pl->state},
                 {"plan_name", ctx.plan.name},
                 {"plan", ctx.plan.source},
                 {"seed", ctx.plan.seed},
                 {"started", utc_iso()}};
  report["systems"] = describe_pipeline(*pl)["systems"];
  report["machine"] = machine(ctx.use_gpu ? ctx.gpu.name : "", ctx.use_gpu ? ctx.gpu.driver : "");
  std::string commit = run(std::string("git -C '") + FOUNDRY_SOURCE_DIR + "' rev-parse --short HEAD 2>/dev/null");
  if (!run(std::string("git -C '") + FOUNDRY_SOURCE_DIR + "' status --porcelain 2>/dev/null | head -1").empty())
    commit += "+dirty";
  report["build"] = {{"commit", commit}, {"gpu", ctx.use_gpu ? "rendered off screen" : ctx.gpu.note.empty() ? "none" : ctx.gpu.note},
                     {"resolution", {ctx.plan.width, ctx.plan.height}}, {"dt", ctx.plan.dt}};

  const json &m = report["machine"];
  std::string header = fmt("stress_%s (%s)  plan %s  seed %llu  %s  %s  %s%s  build %s", pl->name, pl->state,
                           ctx.plan.name.c_str(), static_cast<unsigned long long>(ctx.plan.seed),
                           m["os"].get<std::string>().c_str(), m["cpu"].get<std::string>().c_str(),
                           m["gpu"].get<std::string>().c_str(),
                           ctx.use_gpu ? (" (" + ctx.gpu.driver + ")").c_str() : "", commit.c_str());
  std::cout << header << "\n" << std::flush;

  std::vector<Failure> failures;
  json phases = json::array();
  std::vector<std::string> lines = {header};
  bool pass = true;
  for (const auto &phase : ctx.plan.phases) {
    const std::string kind = phase["kind"];
    ctx.journal.write({{"op", "phase"}, {"kind", kind}});
    const double t0 = now_ms();
    PhaseResult r;
    if (kind == "diversity") r = run_diversity(ctx, phase, failures, reference.is_object() ? &reference : nullptr);
    else if (kind == "forest") r = run_forest(ctx, phase, failures);
    else if (kind == "events") r = run_events(ctx, phase, failures);
    else if (kind == "soak") r = run_soak(ctx, phase, failures);
    else r = run_explore(ctx, phase, failures);
    ctx.journal.flush();
    r.data["wall_seconds"] = (now_ms() - t0) / 1000;
    phases.push_back({{"kind", r.kind}, {"pass", r.pass}, {"summary", r.summary}, {"data", r.data}});
    pass = pass && r.pass;
    std::string line = fmt("%-10s %s", kind.c_str(), r.summary.c_str());
    lines.push_back(line);
    std::cout << line << "\n" << std::flush;
  }
  report["phases"] = phases;
  json fl = json::array();
  for (const auto &f : failures)
    fl.push_back({{"phase", f.phase}, {"check", f.check}, {"detail", f.detail}, {"config", f.config}, {"journal", f.journal}});
  report["failures"] = fl;
  report["pass"] = pass;
  report["finished"] = utc_iso();
  report["run_dir"] = fs::relative(run_dir, workspace).string();

  std::string prev_name;
  json prev = previous_report(perf_dir, pl->name, ctx.plan.name, m["fingerprint"], prev_name);
  std::string cmp = prev.is_object() ? compare(report, prev, prev_name)
                                     : "first report for this pipeline and plan on this machine";
  report["compared_with"] = prev.is_object() ? json(prev_name) : json(nullptr);
  lines.push_back(cmp);
  std::cout << cmp << "\n";
  for (size_t i = 0; i < failures.size() && i < 10; ++i)
    std::cout << fmt("  FAIL %s/%s: %s %s\n", failures[i].phase.c_str(), failures[i].check.c_str(),
                     failures[i].detail.c_str(), failures[i].config.is_null() ? "" : failures[i].config.dump().c_str());
  report["summary"] = lines;

  ctx.journal.close();
  g_journal = nullptr;
  std::string report_path = run_dir + "/report.json";
  if (write_report) report_path = perf_dir + "/report-" + std::string(pl->name) + "-" + stamp + ".json";
  std::ofstream(report_path) << report.dump(1) << "\n";
  std::cout << "report: " << report_path << "\n";

  ctx.destroy();
  ctx.gpu.shutdown();
  SDL_Quit();
  return pass ? 0 : 1;
}
