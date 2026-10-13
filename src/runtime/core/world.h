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
#include "render/field_renderer.h"
#include "render/skinned_renderer.h"
#include "render/sky_renderer.h"
#include "sky/sky.h"

namespace runtime {

namespace physics {
struct Shapes;
}

enum class Status { ok = 0, stale_handle, bad_parameter, wrong_pipeline, failed };

struct Config {
  SDL_GPUDevice *device = nullptr;  // null: everything but submission
  SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  unsigned update_threads = 0;  // 0: the machine's cores minus the generation threads
  unsigned build_threads = 0;   // 0: a quarter of the cores, at least one
  double release_seconds = 2.0;  // how long an unused build stays cached
  uint64_t cache_budget = uint64_t(1) << 30;  // bytes of CPU and GPU memory for unused builds
  // The game's Jolt world (design/api.md, "Physics"): a blind pointer, checked against our Jolt
  // version, and the object layer for each collision purpose (solid, soft, query, sensor).
  void *physics = nullptr;
  uint64_t physics_version = 0;
  uint16_t physics_layers[4] = {0, 0, 0, 0};
};

// What the runtime decides about cost (design/submit.md, "Cost"): each instance's level of detail
// and animation rate from its size on screen, and what is too small to draw. The quality setting
// picks the thresholds; diagnostics can turn each decision off to measure it.
enum CostFlags : uint32_t {
  kCostDetail = 1u << 0,     // pick each instance's level of detail from its size on screen
  kCostTooSmall = 1u << 1,   // don't draw what is smaller than a few pixels
  kCostUnseen = 1u << 2,     // don't animate what is out of view (events still play)
  kCostRate = 1u << 3,       // animate small things less often
  kCostRetain = 1u << 4,     // keep poses on the GPU, uploading only those that changed
  kCostAll = 0x1fu,
};

struct Counters {
  uint64_t instances = 0, awake = 0, waiting = 0, failed = 0;
  uint64_t cache_entries = 0, cache_pending = 0, cache_bytes = 0, cache_gpu_bytes = 0;
  uint64_t cache_hits = 0, cache_misses = 0, cache_evictions = 0;
  uint64_t generations = 0, generation_failures = 0;
  uint64_t gpu_buffers_created = 0, gpu_buffers_released = 0, gpu_bytes = 0;
  uint64_t draws = 0, drawn = 0, culled = 0, triangles = 0;
  uint64_t bodies = 0;  // collision bodies the runtime has in the game's physics world
  // The last frame's cost decisions.
  uint64_t too_small = 0;              // in view, but too small to draw
  uint64_t by_level[4] = {0, 0, 0, 0};  // drawn at each level of detail (finest first)
  uint64_t animated = 0, unseen = 0, rate_skipped = 0;  // awake instances posed, and not posed (out of view, by rate)
  uint64_t bones_uploaded = 0;          // bones whose pose changed and went to the GPU
  uint64_t lod_generations = 0;         // of the generations, the runtime's coarser levels of detail
  uint64_t fields = 0, field_tiles = 0;  // fields, and the tiles of theirs drawn in the last frame
  uint64_t field_tiles_by_level[4] = {0, 0, 0, 0};
};

struct FrameTimes {
  double update_ms = 0, prepare_ms = 0, draw_ms = 0;
  // Inside update: finished builds and switches, classification, choosing and posing, the rest
  // (results, releases).
  double integrate_ms = 0, classify_ms = 0, animate_ms = 0, results_ms = 0;
};

struct StageTotals {
  std::string label;
  bool update = false;
  uint64_t calls = 0;
  double total_ms = 0, max_ms = 0;
};

struct InstanceInfo {
  bool ready = false, sleeping = false, settled = false, failed = false;
  bool drawn = false;              // in the last prepared frame
  uint32_t level = 0;              // the level of detail it was drawn at
  uint32_t drawn_triangles = 0;    // at that level
  float pixels = 0;                // its projected size, in pixels
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
  // A field (design/plotter.md, "Fields"): one instance over many hex cells, each showing the tile
  // its cell chooses (the pipeline's cell stage, run here for every cell), drawn instanced by chunk.
  uint64_t spawn_field(const PipelineType *type, const void *params, const float transform[16], const FieldCell *cells,
                       size_t count);
  Status destroy(uint64_t handle);
  Status set_transform(uint64_t handle, const float transform[16]);
  Status set_param(uint64_t handle, const PipelineType *type, size_t offset, const void *value, size_t size,
                   bool build);
  Status trigger(uint64_t handle, const PipelineType *type, size_t event);
  // Diagnostics: sets a parameter by its name, from text (an enum's value name, a number, true or
  // false), clamped to its range. bad_parameter if the pipeline has no such parameter.
  Status set_named(uint64_t handle, const char *name, const char *value);
  Status sleeping(uint64_t handle, bool &out) const;
  // The instance's update result (design/api.md, "Instance results"), as current values: bit i
  // is event i's flag (it fired and its animation is over: the pose has settled since), and
  // kResultReady says the build it asked for is the one drawn.
  static constexpr uint32_t kResultReady = 1u << 31;
  Status result(uint64_t handle, uint32_t &bits) const;
  // The instances whose results changed in the last update.
  const std::vector<uint64_t> &changed() const { return changed_; }
  void set_camera(const float view[16], const float projection[16]);
  void set_viewport(uint32_t width, uint32_t height);
  void set_quality(int quality);  // 0 low, 1 medium, 2 high
  void set_cost(uint32_t flags) { cost_ = flags; }
  // Diagnostics: draw an instance at one level of detail (its pipeline's levels, then the far level),
  // whatever its size; -1 lets its size pick again.
  Status force_level(uint64_t handle, int level);
  uint32_t cost() const { return cost_; }

  // The sky (design/observatory.md): an observatory (null: none, the classic fixed light), the state
  // a game sets on it, and where its shadow map looks (radius 0: ahead of the camera, 60 m).
  void set_sky(std::shared_ptr<const sky::Observatory> observatory);
  void set_sky_state(const sky::State &state);
  void set_shadow_focus(const float centre[3], float radius);
  // The sky's last frame (where the sun and moons are, the light); false without a sky.
  bool sky_frame(sky::Frame &out) const;

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
    std::shared_ptr<physics::Shapes> shapes;  // made on first use, if the build collides
    bool shapes_made = false;
    uint64_t order = 0;  // when it was created, for eviction
    uint32_t dense = 0;  // a small index, for bucketing draws
    bool lod = false;    // made for a coarser level of detail (the runtime's choice, not the game's)
    bool field = false;  // a field's tile: drawn by the field renderer
    FieldMesh fgpu;
    // As a coarser level of detail: whether it poses like the finer build of that key (its bones
    // and their motion are the same, so it can share that instance's pose).
    std::vector<std::pair<uint64_t, bool>> poses_like;
  };
  struct Field;
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
    std::vector<uint32_t> bodies;  // in the game's physics world
    uint32_t result = 0;           // the last result bits reported
    double posed_at = -1;          // the instance time of the last pose
    bool awake = true;
    std::string error;
    Entry *entry = nullptr;        // current's cache entry (held by a reference, so it stays)
    // Coarser levels of detail of the current build (by level; 0: not asked for) and their entries.
    uint64_t lods[4] = {0, 0, 0, 0};
    Entry *lod_entries[4] = {nullptr, nullptr, nullptr, nullptr};
    uint8_t level = 0;             // the level its size asks for (with hysteresis)
    uint8_t drawn_level = 0;       // the level it was drawn at last
    bool in_view = false;          // inside the frustum (last classification)
    bool near_view = false;        // inside it with a margin: what gets animated
    bool drawn = false;
    float pixels = 0;              // projected size of its bounding sphere, pixels
    uint64_t posed_frame = 0;      // the world frame of the last pose
    uint32_t bone_offset = 0, bone_count = 0;  // its slot in the renderer's bone buffer
    bool pose_dirty = false;       // posed since its bones were last uploaded
    bool touched = false;          // its result may have changed this frame (listed in touched_)
    bool wants_lod = false;        // classification asked for a coarser build it doesn't have
    int8_t lod_ok[4] = {-1, -1, -1, -1};  // whether each coarser build poses like its own (-1: not checked yet)
    int8_t forced = -1;            // diagnostics: the level it is drawn at, whatever its size (-1: its size picks)
    std::unique_ptr<Field> field;  // a field's cells and chunks (null: an ordinary instance)
  };
  // A field: its tiles by chunk (8 x 8 cells), each chunk's tiles in runs of one variant, and the
  // tile builds it uses, by variant and level.
  struct Field {
    std::vector<FieldInstance> tiles;
    struct Run {
      uint32_t variant, first, count;
    };
    struct Chunk {
      float c[3] = {0, 0, 0};
      float r = 0;
      std::vector<Run> runs;
      uint8_t level = 0;
    };
    std::vector<Chunk> chunks;
    std::map<uint32_t, std::pair<uint64_t, Entry *>> builds;  // (variant << 4 | level) -> key, entry
    uint64_t drawn_tiles = 0, drawn_triangles = 0;
    uint32_t level_drawn = 0;  // a level its tiles were drawn at in the last frame (diagnostics)
  };
  Entry *field_tile(Slot &s, uint32_t variant, uint32_t level, bool request);
  void drop_field_builds(Slot &s);
  void prepare_fields(SDL_GPUCopyPass *copy);
  void prepare_sky(SDL_GPUCommandBuffer *cmd);
  Slot *slot(uint64_t handle);
  const Slot *slot(uint64_t handle) const;
  void want(Slot &s);
  Entry *acquire(const PipelineType *type, const std::vector<uint8_t> &params, uint64_t key, bool far = false);
  void unref(uint64_t key);
  void drop_lods(Slot &s);
  void bind_pose(Slot &s, const Entry &e);
  void classify(bool request);
  Entry *pick_lod(Slot &s, bool &unchecked);
  bool poses_alike(Slot &s, Entry &coarse);
  void integrate();
  void release_idle(bool all);
  void evict(std::map<uint64_t, Entry>::iterator it);
  void clocks(const Slot &s, std::vector<float> &out) const;
  void set_bodies(Slot &s, Entry *entry);  // swap the slot's bodies for the entry's (null: none)
  uint32_t result_of(const Slot &s) const;

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
  float view_[16] = {}, projection_[16] = {};
  float eye_[3] = {0, 0, 0};
  float p11_ = 1;            // the projection's vertical scale (1 / tan(fov / 2))
  bool ortho_ = false;
  bool have_camera_ = false;
  uint64_t camera_serial_ = 0, classified_serial_ = ~0ull;
  uint32_t viewport_h_ = 1080;
  int quality_ = 2;
  uint32_t cost_ = kCostAll;
  uint64_t frame_ = 0;
  std::vector<DrawRange> frame_ranges_;
  FieldRenderer field_renderer_;
  std::vector<FieldRange> field_ranges_;
  SkyRenderer sky_;
  std::shared_ptr<const sky::Observatory> observatory_;
  sky::State sky_state_;
  sky::Frame sky_frame_;
  bool have_sky_frame_ = false;
  float shadow_centre_[3] = {0, 0, 0}, shadow_radius_ = 0;  // radius 0: ahead of the camera
  ShadowView shadow_;
  bool shadow_drawn_ = false;  // this frame
  std::vector<uint32_t> field_slots_;  // the slots that are fields
  uint64_t field_triangles_ = 0;       // drawn by fields in the last frame
  std::vector<uint32_t> free_dense_;
  uint32_t next_dense_ = 0;

  std::vector<uint64_t> changed_;  // handles whose results changed in the last update
  // Work lists, so a frame touches only what can change: slots waiting for a build, and slots
  // whose result may have changed.
  std::vector<uint64_t> waiting_;  // handles
  std::vector<uint32_t> touched_;
  void touch(Slot &s);
  // Update-stage totals per pipeline type, resolved once (labels are looked up per type, not per call).
  std::vector<std::pair<const PipelineType *, std::vector<StageTotals *>>> stage_slots_;
  std::vector<StageTotals *> &update_stages(const PipelineType *type);
  Counters counters_;
  FrameTimes times_;
  std::map<std::string, StageTotals> stages_;
};

}  // namespace runtime
