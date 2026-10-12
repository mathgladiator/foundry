// perf_stress (design/quality.perf.md): builds a pipeline's stress binary,
// hashes the configs it will sample in WebAssembly as the reference, runs it
// as a child process (a crash never takes the foundry down), and returns its
// summary and report.
#include "perf/stress.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace perf {

namespace {

using nlohmann::json;

std::string hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

struct Run {
  int id = 0;
  std::string pipeline;
  std::mutex mutex;
  std::vector<std::string> lines;
  bool done = false;
  bool is_video = false;  // a QA video export, not a stress run
  int exit = -1;
  std::string report, error, video;
};

std::mutex g_runs_mutex;
std::map<int, std::shared_ptr<Run>> g_runs;
int g_next_run = 1;

json run_json(Run &r) {
  std::lock_guard<std::mutex> lock(r.mutex);
  json j = {{"job", r.id}, {"pipeline", r.pipeline}, {"done", r.done}, {"output", r.lines}};
  if (r.done) {
    static const char *meaning[] = {"passed", "a threshold failed", "the plan is invalid", "the run crashed"};
    j["exit"] = r.exit;
    j["result"] = r.exit >= 0 && r.exit <= 3 ? meaning[r.exit] : "the binary didn't run";
    if (!r.report.empty()) j["report"] = r.report;
    if (!r.video.empty()) j["video"] = r.video;
  }
  if (!r.error.empty()) j["error"] = r.error;
  return j;
}

// The reference: every config the diversity phase will draw, hashed in WebAssembly.
bool make_reference(const forge::Paths &paths, pipeline::Runner &runner, const std::string &binary,
                    const std::vector<std::string> &plan_args, const std::string &out, Run &r, std::string &error) {
  const std::string sample = out + ".sample.json";
  std::vector<std::string> argv = {binary, "--workspace", paths.workspace, "--sample", sample};
  argv.insert(argv.end(), plan_args.begin(), plan_args.end());
  std::string output;
  if (core::run_process(argv, output) != 0) {
    error = "sampling failed: " + output;
    return false;
  }
  json s;
  try {
    std::ifstream f(sample);
    s = json::parse(f);
  } catch (const std::exception &e) {
    error = std::string("can't read the sample: ") + e.what();
    return false;
  }
  pipeline::Definition def;
  if (!pipeline::load(paths, s["pipeline"], def, error)) return false;
  std::vector<std::string> events;
  for (const auto &p : def.params)
    if (p.type == "event") events.push_back(p.name);
  std::string time_param = "time";
  for (const auto &p : def.params)
    if (p.type == "time") time_param = p.name;
  core::Job job("Stress reference: " + def.name);
  size_t k = 0, n = s["configs"].size();
  for (auto &c : s["configs"]) {
    json poses = json::array();
    std::string model;
    for (const auto &clocks : s["clocks"]) {
      json values = c["values"];
      values[time_param] = clocks[0];
      for (size_t e = 0; e < events.size(); ++e) values[events[e]] = clocks[1 + e];
      pipeline::Runner::Result res = runner.run(def, values, false, false, true);
      if (!res.ok) {
        error = "WebAssembly run failed for " + c["values"].dump() + ": " + res.error;
        return false;
      }
      model = hex(res.output_hashes[def.model_stage]);
      poses.push_back(def.pose_stage.empty() ? hex(0) : hex(res.output_hashes[def.pose_stage]));
    }
    c["model"] = model;
    c["poses"] = poses;
    ++k;
    job.update(static_cast<float>(k) / n, std::to_string(k) + " of " + std::to_string(n) + " configs");
  }
  std::ofstream(out) << s.dump(1) << "\n";
  fs::remove(sample);
  std::lock_guard<std::mutex> lock(r.mutex);
  r.lines.push_back("reference: " + std::to_string(n) + " configs hashed in WebAssembly");
  return true;
}

void execute(forge::Paths paths, pipeline::Runner &runner, json args, std::shared_ptr<Run> r) {
  const std::string name = r->pipeline;
  core::Job job("Stress: " + name);
  auto say = [&](const std::string &line) {
    std::lock_guard<std::mutex> lock(r->mutex);
    r->lines.push_back(line);
  };
  auto finish = [&](int exit, const std::string &error) {
    std::lock_guard<std::mutex> lock(r->mutex);
    r->exit = exit;
    r->error = error;
    r->done = true;
  };
  // Regenerate the pipeline code and build the binary.
  std::string output;
  job.update(-1, "generating and building");
  if (core::run_process({"python3", paths.root + "/tools/gen_pipelines.py", "--workspace", paths.workspace}, output) != 0) {
    finish(-1, "gen_pipelines failed: " + output);
    return;
  }
  if (core::run_process({"cmake", "--build", paths.build, "--target", "stress_" + name}, output) != 0) {
    finish(-1, "build failed: " + output.substr(output.size() > 4000 ? output.size() - 4000 : 0));
    return;
  }
  const std::string binary = paths.build + "/stress/stress_" + name;
  std::vector<std::string> plan_args;
  const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
  fs::create_directories(paths.workspace + "/perf/runs");
  if (args.contains("plan") && args["plan"].is_object()) {
    const std::string path = paths.workspace + "/perf/runs/plan-" + stamp + ".json";
    std::ofstream(path) << args["plan"].dump(1) << "\n";
    plan_args = {"--plan", path};
  } else {
    plan_args = {"--plan", args.value("plan", "quick")};
  }
  if (args.contains("phase")) plan_args.insert(plan_args.end(), {"--phase", args["phase"].get<std::string>()});
  if (args.contains("seconds")) plan_args.insert(plan_args.end(), {"--seconds", std::to_string(args["seconds"].get<double>())});
  std::vector<std::string> argv = {binary, "--workspace", paths.workspace};
  argv.insert(argv.end(), plan_args.begin(), plan_args.end());
  if (args.contains("gpu")) argv.insert(argv.end(), {"--gpu", args["gpu"].get<std::string>()});
  if (args.value("reference", true)) {
    job.update(-1, "hashing the reference in WebAssembly");
    const std::string ref = paths.workspace + "/perf/runs/reference-" + name + "-" + stamp + ".json";
    std::string error;
    if (make_reference(paths, runner, binary, plan_args, ref, *r, error)) {
      argv.insert(argv.end(), {"--reference", ref});
    } else {
      say("reference skipped: " + error);
    }
  }
  if (args.value("no_report", false)) argv.push_back("--no-report");
  // Run it, streaming its lines.
  job.update(-1, "running");
  std::string cmd;
  for (const auto &a : argv) cmd += quote(a) + " ";
  cmd += "2>&1";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) {
    finish(-1, "can't start " + binary);
    return;
  }
  char buf[4096];
  while (fgets(buf, sizeof buf, p)) {
    std::string line = buf;
    while (!line.empty() && line.back() == '\n') line.pop_back();
    say(line);
    if (line.rfind("report: ", 0) == 0) {
      std::lock_guard<std::mutex> lock(r->mutex);
      r->report = line.substr(8);
    } else if (!line.empty() && line[0] != ' ') {
      core::log().add("stress", name + ": " + line);
      job.update(-1, line.substr(0, 80));
    }
  }
  int status = pclose(p);
  int exit = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
  finish(exit, "");
}

// Regenerates the pipeline code and builds stress_<name>; returns the binary, or empty with an error.
std::string build_binary(const forge::Paths &paths, const std::string &name, std::string &error) {
  std::string output;
  if (core::run_process({"python3", paths.root + "/tools/gen_pipelines.py", "--workspace", paths.workspace}, output) != 0) {
    error = "gen_pipelines failed: " + output;
    return "";
  }
  if (core::run_process({"cmake", "--build", paths.build, "--target", "stress_" + name}, output) != 0) {
    error = "build failed: " + output.substr(output.size() > 4000 ? output.size() - 4000 : 0);
    return "";
  }
  return paths.build + "/stress/stress_" + name;
}

void execute_video(forge::Paths paths, VideoRequest q, std::shared_ptr<Run> r) {
  core::Job job("QA video: " + q.pipeline);
  auto finish = [&](int exit, const std::string &error) {
    std::lock_guard<std::mutex> lock(r->mutex);
    r->exit = exit;
    r->error = error;
    r->done = true;
  };
  job.update(-1, "building");
  std::string error;
  const std::string binary = build_binary(paths, q.pipeline, error);
  if (binary.empty()) {
    finish(-1, error);
    return;
  }
  char stamp[32];
  std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", std::gmtime(&now));
  fs::create_directories(paths.workspace + "/qa/videos");
  const std::string out = paths.workspace + "/qa/videos/" + q.pipeline + "-" + stamp + ".mp4";
  std::vector<std::string> argv = {binary, "--workspace", paths.workspace, "--video", out,
                                   "--floats", std::to_string(q.floats), "--seconds-per", std::to_string(q.seconds),
                                   "--fps", std::to_string(q.fps), "--size", q.size};
  if (!q.fixed.empty()) argv.insert(argv.end(), {"--fixed", q.fixed.dump()});
  if (!q.ranges.empty()) argv.insert(argv.end(), {"--ranges", q.ranges.dump()});
  std::string cmd;
  for (const auto &a : argv) cmd += quote(a) + " ";
  cmd += "2>&1";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) {
    finish(-1, "can't start " + binary);
    return;
  }
  char buf[4096];
  size_t total = 0;
  while (fgets(buf, sizeof buf, p)) {
    std::string line = buf;
    while (!line.empty() && line.back() == '\n') line.pop_back();
    {
      std::lock_guard<std::mutex> lock(r->mutex);
      r->lines.push_back(line);
    }
    size_t a = 0, b = 0;
    if (std::sscanf(line.c_str(), "video: %zu variations", &total) == 1) {
      core::log().add("video", q.pipeline + ": " + line.substr(7));
    } else if (std::sscanf(line.c_str(), "video: %zu/%zu", &a, &b) == 2 && b > 0) {
      job.update(static_cast<float>(a) / static_cast<float>(b), std::to_string(a) + " of " + std::to_string(b) + " variations");
    }
  }
  int status = pclose(p);
  int exit = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
  if (exit == 0) {
    std::lock_guard<std::mutex> lock(r->mutex);
    r->video = out;
    core::log().add("video", q.pipeline + ": wrote " + out);
  }
  finish(exit, exit == 0 ? "" : "the export failed; see its output");
}

}  // namespace

namespace {
// The running video export (0 if none); call with g_runs_mutex held.
int running_video_locked() {
  for (const auto &[id, r] : g_runs) {
    if (!r->is_video) continue;
    std::lock_guard<std::mutex> lock(r->mutex);
    if (!r->done) return id;
  }
  return 0;
}
}  // namespace

int running_video() {
  std::lock_guard<std::mutex> lock(g_runs_mutex);
  return running_video_locked();
}

int latest_video(const std::string &pipeline) {
  std::lock_guard<std::mutex> lock(g_runs_mutex);
  int latest = 0;
  for (const auto &[id, r] : g_runs)
    if (r->is_video && r->pipeline == pipeline) latest = id;  // ids grow, and the map is ordered
  return latest;
}

int start_video(const forge::Paths &paths, const VideoRequest &request, int *blocked_by) {
  auto r = std::make_shared<Run>();
  r->pipeline = request.pipeline;
  r->is_video = true;
  {
    std::lock_guard<std::mutex> lock(g_runs_mutex);
    if (const int busy = running_video_locked()) {
      if (blocked_by) *blocked_by = busy;
      return 0;
    }
    r->id = g_next_run++;
    g_runs[r->id] = r;
  }
  std::thread(execute_video, paths, request, r).detach();
  return r->id;
}

json job_status(int job) {
  std::shared_ptr<Run> r;
  {
    std::lock_guard<std::mutex> lock(g_runs_mutex);
    auto it = g_runs.find(job);
    if (it == g_runs.end()) return nullptr;
    r = it->second;
  }
  return run_json(*r);
}

void register_stress_tools(mcp::Registry &registry, const forge::Paths &paths, pipeline::Runner &runner) {
  registry.add({"perf_stress",
                "Stress-test a pipeline in the flattened native runtime (design/quality.perf.md): regenerates and "
                "builds stress_<pipeline>, hashes the configs it samples in WebAssembly as the reference, runs the "
                "plan's phases (diversity, forest, events, soak), and writes perf/report-<pipeline>-<timestamp>.json "
                "(committed: the performance timeline). plan is a preset (quick, standard, soak) or an inline plan "
                "object; phase and seconds narrow it. wait (default true, false for soak) blocks until done; "
                "otherwise poll perf_stress_status with the job.",
                mcp::schema({{"pipeline", {{"type", "string"}, {"description", "e.g. tree_v1"}}},
                             {"plan", {{"description", "quick, standard, soak, or an inline plan object"}}},
                             {"phase", {{"type", "string"}, {"enum", {"diversity", "forest", "events", "soak"}}}},
                             {"seconds", {{"type", "number"}}},
                             {"gpu", {{"type", "string"}, {"enum", {"auto", "none", "required"}}}},
                             {"reference", {{"type", "boolean"}, {"description", "hash a WebAssembly reference (default true)"}}},
                             {"no_report", {{"type", "boolean"}, {"description", "keep the report out of the timeline"}}},
                             {"wait", {{"type", "boolean"}}}},
                            {"pipeline"}),
                [paths, &runner](const json &args) {
                  const std::string name = args.at("pipeline");
                  if (!core::valid_name(name)) return mcp::Result::error("bad pipeline name");
                  if (!fs::exists(paths.workspace + "/pipelines/" + name + ".json"))
                    return mcp::Result::error("no pipelines/" + name + ".json");
                  auto r = std::make_shared<Run>();
                  r->pipeline = name;
                  {
                    std::lock_guard<std::mutex> lock(g_runs_mutex);
                    r->id = g_next_run++;
                    g_runs[r->id] = r;
                  }
                  const bool wait = args.value("wait", !(args.value("plan", json("quick")) == "soak"));
                  std::thread t(execute, paths, std::ref(runner), args, r);
                  if (!wait) {
                    t.detach();
                    return mcp::Result::json({{"job", r->id}, {"status", "running; poll perf_stress_status"}});
                  }
                  t.join();
                  return mcp::Result::json(run_json(*r));
                }});

  registry.add({"pipeline_video",
                "Export a QA video of a pipeline (design/quality.perf.md, \"The QA video\"): a 5-second title slide, then "
                "about a second per variation, each titled with its parameters, rendered by the native runtime and "
                "encoded as H.264 MP4 into qa/videos/. Every combination of the discrete parameters (enums, config entries) is "
                "enumerated, times floats samples of the continuous ones; fixed pins parameters and ranges narrows enums "
                "to lists, to keep it short. Returns a job; poll perf_stress_status for progress and the file.",
                mcp::schema({{"pipeline", {{"type", "string"}}},
                             {"floats", {{"type", "integer"}, {"description", "continuous samples per combination (default 1)"}}},
                             {"seconds", {{"type", "number"}, {"description", "per variation (default 1)"}}},
                             {"fps", {{"type", "integer"}}},
                             {"size", {{"type", "string"}, {"description", "WIDTHxHEIGHT, default 1280x720"}}},
                             {"fixed", {{"type", "object"}}},
                             {"ranges", {{"type", "object"}}}},
                            {"pipeline"}),
                [paths](const json &args) {
                  VideoRequest q;
                  q.pipeline = args.at("pipeline");
                  if (!core::valid_name(q.pipeline)) return mcp::Result::error("bad pipeline name");
                  q.floats = args.value("floats", 1);
                  q.seconds = args.value("seconds", 1.0);
                  q.fps = args.value("fps", 30);
                  q.size = args.value("size", std::string("1280x720"));
                  q.fixed = args.value("fixed", json::object());
                  q.ranges = args.value("ranges", json::object());
                  int busy = 0;
                  const int job = start_video(paths, q, &busy);
                  if (!job) {
                    const json b = job_status(busy);
                    return mcp::Result::error("blocked: a QA video of " + b.value("pipeline", std::string("another pipeline")) +
                                              " is exporting (job " + std::to_string(busy) +
                                              "); one video exports at a time, so start this one when it finishes");
                  }
                  return mcp::Result::json({{"job", job}, {"status", "exporting; poll perf_stress_status"}});
                }});

  registry.add({"perf_stress_status", "The output so far of a perf_stress job, and its result once done.",
                mcp::schema({{"job", {{"type", "integer"}}}}, {"job"}), [](const json &args) {
                  std::shared_ptr<Run> r;
                  {
                    std::lock_guard<std::mutex> lock(g_runs_mutex);
                    auto it = g_runs.find(args.at("job").get<int>());
                    if (it == g_runs.end()) return mcp::Result::error("no such job");
                    r = it->second;
                  }
                  return mcp::Result::json(run_json(*r));
                }});
}

}  // namespace perf
