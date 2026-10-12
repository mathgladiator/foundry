// The runtime's world (design/submit.md): pipeline instances with their own
// timelines, the shared cache of builds, generation in the background, the
// update broadcast across threads, sleep, culling, and submission.
//
// The C API (src/library) is a thin layer over this class; the game never
// sees it.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/pipeline.h"
#include "core/threads.h"
#include "render/skinned_renderer.h"

namespace runtime {

enum class Status { ok = 0, stale_handle, bad_parameter, wrong_pipeline, failed };

struct Config {
  SDL_GPUDevice *device = nullptr;  // null: everything but submission
  SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  unsigned update_threads = 0;  // 0: the machine's cores minus the generation threads
  unsigned build_threads = 0;   // 0: a quarter of the cores, at least one
  double release_seconds = 2.0;  // how long an unused build stays cached
  uint64_t cache_budget = uint64_t(1) << 30;  // bytes of CPU and GPU memory for unused builds
};

struct Counters {
  uint64_t instances = 0, awake = 0, waiting = 0, failed = 0;
  uint64_t cache_entries = 0, cache_pending = 0, cache_bytes = 0, cache_gpu_bytes = 0;
  uint64_t cache_hits = 0, cache_misses = 0, cache_evictions = 0;
  uint64_t generations = 0, generation_failures = 0;
  uint64_t gpu_buffers_created = 0, gpu_buffers_released = 0, gpu_bytes = 0;
  uint64_t draws = 0, drawn = 0, culled = 0, triangles = 0;
};

struct FrameTimes {
  double update_ms = 0, prepare_ms = 0, draw_ms = 0;
};

struct StageTotals {
  std::string label;
  bool update = false;
  uint64_t calls = 0;
  double total_ms = 0, max_ms = 0;
};

struct InstanceInfo {
  bool ready = false, sleeping = false, settled = false, failed = false;
  uint32_t triangles = 0, bones = 0;
  float height = 0, radius = 0;
  uint64_t build_key = 0;
  std::string error;
};

class World {
 public:
  explicit World(const Config &config, std::string &error);
  ~World();
  bool ok() const { return ok_; }

  uint64_t spawn(const PipelineType *type, const void *params, const float transform[16]);
  Status destroy(uint64_t handle);
  Status set_transform(uint64_t handle, const float transform[16]);
  Status set_param(uint64_t handle, const PipelineType *type, size_t offset, const void *value, size_t size,
                   bool build);
  Status trigger(uint64_t handle, const PipelineType *type, size_t event);
  Status sleeping(uint64_t handle, bool &out) const;
  void set_camera(const float view[16], const float projection[16]);

  void update(float dt);
  void prepare(SDL_GPUCommandBuffer *cmd);
  void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass);

  // Diagnostics.
  Counters counters() const;
  FrameTimes frame_times() const { return times_; }
  std::vector<StageTotals> stage_totals() const;
  Status info(uint64_t handle, InstanceInfo &out) const;
  // Hashes of the instance's draw output, and of its pose at these clocks (null: its live pose).
  Status hash(uint64_t handle, const float *clocks, size_t count, uint64_t &model, uint64_t &pose);
  // Instances whose pose has a non-finite matrix or a bone far outside its bounds.
  uint64_t bad_poses() const;
  // Destroys nothing, but releases every unused build now (the drain check).
  void flush();

 private:
  struct Entry {
    const PipelineType *type = nullptr;
    std::shared_ptr<Build> build;  // null while pending
    std::string error;
    bool pending = true, failed = false;
    uint32_t refs = 0;
    double idle_since = 0;
    SkinnedMesh gpu;
    bool uploaded = false;
    uint64_t order = 0;  // when it was created, for eviction
  };
  struct Slot {
    uint32_t generation = 1;
    bool alive = false;
    const PipelineType *type = nullptr;
    std::vector<uint8_t> params;
    float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    double time = 0;
    std::vector<double> fired;  // per event: the instance time it fired, or negative
    uint64_t current = 0, wanted = 0;  // build keys; 0 = none
    bool has_current = false;
    std::unique_ptr<PoseState> pose;
    bool awake = true;
    std::string error;
  };
  Slot *slot(uint64_t handle);
  const Slot *slot(uint64_t handle) const;
  void want(Slot &s);
  void unref(uint64_t key);
  void integrate();
  void release_idle(bool all);
  void evict(std::map<uint64_t, Entry>::iterator it);
  void clocks(const Slot &s, std::vector<float> &out) const;

  Config config_;
  bool ok_ = false;
  SkinnedRenderer renderer_;
  bool have_renderer_ = false;
  std::unique_ptr<Pool> pool_;
  std::unique_ptr<Queue> builders_;

  std::vector<Slot> slots_;
  std::vector<uint32_t> free_;
  std::map<uint64_t, Entry> cache_;
  uint64_t next_order_ = 0;
  double clock_ = 0;  // the world's time, for release timers

  struct Finished {
    uint64_t key;
    std::shared_ptr<Build> build;
    std::string error;
  };
  std::mutex finished_mutex_;
  std::vector<Finished> finished_;

  float view_proj_[16] = {};
  bool have_camera_ = false;
  std::vector<SkinnedInstance> frame_instances_;
  std::vector<float> frame_bones_;
  std::vector<DrawRange> frame_ranges_;

  Counters counters_;
  FrameTimes times_;
  std::map<std::string, StageTotals> stages_;
};

}  // namespace runtime
