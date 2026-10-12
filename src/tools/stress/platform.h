// The stress platform's internals (design/quality.perf.md): shared by its
// files, never seen by a binding.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "physics.h"
#include "runtime_diag.h"
#include "stress.h"

namespace stress {

using json = nlohmann::json;

// PCG32, as the systems use: every random choice in a run comes from the plan's seed.
struct Rng {
  uint64_t state = 0x853c49e6748fea9bull, inc = 0xda3e39cb94b95bdbull;
  explicit Rng(uint64_t seed = 1, uint64_t stream = 1) {
    inc = (stream << 1u) | 1u;
    next();
    state += seed;
    next();
  }
  uint32_t next() {
    uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
  }
  double unit() { return next() / 4294967296.0; }
  uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(unit() * n) : 0; }
};

// One sampled configuration: a value per parameter.
using Config = std::vector<stress_value>;

struct Plan {
  std::string name = "custom";
  uint64_t seed = 7;
  std::string gpu = "auto";  // auto, none, required
  bool window = false;       // --window: draw into a window you can fly through
  uint32_t width = 1920, height = 1080;
  double dt = 1.0 / 60.0;
  double budget_ms = 1000.0 / 60.0;
  uint32_t update_threads = 0, build_threads = 0;
  json fixed = json::object(), ranges = json::object(), thresholds = json::object();
  std::vector<json> phases;
  json source;  // the plan as given
  double threshold(const char *key, double fallback) const {
    return thresholds.contains(key) ? thresholds[key].get<double>() : fallback;
  }
};

// Timing of one frame, in milliseconds.
struct FrameTime {
  double total = 0, update = 0, prepare = 0, draw = 0, gpu_wait = 0;
  double gpu_sync = -1;  // sampled frames: submit to fence, waited on right away
};

struct Stats {
  double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
  size_t n = 0;
};
Stats stats(std::vector<double> v);
json to_json(const Stats &s);

struct Memory {
  double rss_mb = 0, rss_anon_mb = 0, hwm_mb = 0, private_dirty_mb = 0, heap_mb = 0;
};
Memory memory();  // the process, from the OS (and glibc's heap in use)
void trim();      // returns freed heap to the OS, before a drain sample
json to_json(const Memory &m);

// The machine, for the report and for comparing runs.
json machine(const std::string &gpu_name, const std::string &gpu_driver);

// The journal: every action, so a crash leaves its reproduction (--replay).
class Journal {
 public:
  bool open(const std::string &path);
  void close();
  uint64_t index() const { return index_; }
  void write(const json &line);
  void flush();
  void frames(uint32_t n, double dt);  // batches frames into one line

 private:
  void flush_frames();
  FILE *f_ = nullptr;
  uint64_t index_ = 0;
  uint32_t pending_frames_ = 0;
  double pending_dt_ = 0;
};

// Off-screen rendering, a window, or none.
struct Gpu {
  SDL_GPUDevice *device = nullptr;
  SDL_Window *window = nullptr;
  SDL_GPUTexture *color = nullptr, *depth = nullptr;
  uint32_t depth_w = 0, depth_h = 0;
  SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  std::string name, driver, note;
  std::vector<SDL_GPUFence *> in_flight;
  bool init(const Plan &plan, std::string &error);
  void shutdown();
};

// The window's fly camera: Tab toggles it, the right mouse button looks,
// WASD moves, Space and C rise and fall, Shift and the wheel go faster.
struct Fly {
  bool on = false;
  float eye[3] = {0, 2, 0}, yaw = 0, pitch = 0, speed = 12;
  bool look = false;
};

// A runtime with its pipeline, and the frame loop every phase uses.
struct Ctx {
  const stress_pipeline *pipeline = nullptr;
  Plan plan;
  Gpu gpu;
  bool use_gpu = false;
  foundry_runtime *rt = nullptr;
  ToyPhysics *physics = nullptr;  // the toy Jolt world the runtime puts collision bodies in
  Journal journal;
  Rng rng{7, 1};
  double sim_time = 0;  // seconds simulated so far
  uint64_t frames = 0;
  float view[16], projection[16];
  // The window: the fly camera, the phases' own camera (restored when flying stops), and this frame's keys.
  Fly fly;
  float cam_eye[3] = {0, 10, 30}, cam_target[3] = {0, 0, 0}, cam_far = 1000;
  std::vector<SDL_Keycode> pressed;
  std::string hud;  // shown in the window's title
  std::string title_name = "region";  // the window title's name without a pipeline (a region run)
  double last_wall = 0, hud_at = 0;
  void pump();       // pumps the window's events: quit, keys, the fly camera
  void apply_camera();

  bool create(std::string &error);
  void destroy();
  // One frame: update by the plan's dt, then prepare and (with a GPU) draw.
  FrameTime frame(bool render = true, bool sync = false);
  // Writes the last rendered frame as a PNG (no-op without a GPU).
  bool snapshot(const std::string &path);
  bool read_frame(std::vector<uint8_t> &rgba);  // the last rendered frame, RGBA rows top to bottom
  std::string snapshot_dir;  // --snapshot: phases write a PNG per step here
  void camera(const float eye[3], const float target[3], float far_z);
  // Overview camera over a square of this half-extent, for things this tall.
  void overview(float half_extent, float height);

  foundry_instance spawn(const Config &c, const float transform[16]);
  foundry_status set(foundry_instance i, uint32_t param, stress_value v);
  foundry_status trigger(foundry_instance i, uint32_t param);
  foundry_status destroy(foundry_instance i);

  // Parameters by kind.
  std::vector<uint32_t> update_params() const;  // u32/f32/enum with STRESS_UPDATE
  std::vector<uint32_t> build_params() const;
  std::vector<uint32_t> events() const;
  uint32_t clock_count() const { return 1 + static_cast<uint32_t>(events().size()); }
  int param(const std::string &name) const;
  json describe(const Config &c) const;  // a config as {name: value}
  bool parse(const json &j, Config &c) const;  // and back
  // A random value for one parameter, honouring the plan's fixed and ranges.
  stress_value random_value(uint32_t p, Rng &rng) const;
  Config fallback() const;
};

// Stratified sampling: every enum value appears about equally, and each
// number's two ends are always included.
std::vector<Config> sample(const Ctx &ctx, size_t n, Rng &rng);

// Hex positions spiralling out from the centre.
std::vector<std::pair<float, float>> hex_layout(size_t n, float spacing, Rng &rng, float jitter);
void translation(float x, float z, float yaw, float out[16]);

struct Failure {
  std::string phase, check, detail;
  json config;
  uint64_t journal = 0;
};

struct PhaseResult {
  std::string kind;
  bool pass = true;
  json data = json::object();
  std::string summary;  // one line
};

// Plotters (region.cpp, scale.cpp): a ground preset as the foundry's plotter view makes it, and the
// scale plan, which measures a plotter's scenes at several sizes (design/quality.perf.md, "Scale").
std::vector<foundry_hex_region> plot_ground(const std::string &preset, int radius);
std::string stamp_now(bool iso);
int scale_main(int argc, char **argv, foundry_plotter plotter, const char *name, const stress_pipeline *const *pipelines,
               uint32_t count);

// The clocks diversity hashes poses at: still, swaying, and mid-event.
std::vector<std::vector<float>> default_clocks(const Ctx &ctx);

PhaseResult run_diversity(Ctx &ctx, const json &phase, std::vector<Failure> &failures, const json *reference);
PhaseResult run_forest(Ctx &ctx, const json &phase, std::vector<Failure> &failures);
PhaseResult run_events(Ctx &ctx, const json &phase, std::vector<Failure> &failures);
PhaseResult run_soak(Ctx &ctx, const json &phase, std::vector<Failure> &failures);
PhaseResult run_explore(Ctx &ctx, const json &phase, std::vector<Failure> &failures);

// The QA video (design/quality.perf.md, "The QA video"): a title slide, then
// one clip per variation of the pipeline's discrete parameters, times
// `floats` samples of its continuous ones, each titled with its parameters.
struct VideoOptions {
  std::string path;
  int floats = 1;
  double seconds = 1.0, title_seconds = 5.0;
  int fps = 30;
};
int run_video(Ctx &ctx, const VideoOptions &o);

std::string fmt(const char *f, ...);
double now_ms();

}  // namespace stress
