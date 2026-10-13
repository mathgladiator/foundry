#include "core/world.h"

#include "physics/jolt.h"
#include "render/simplify.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace runtime {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t make_handle(uint32_t index, uint32_t generation) { return (uint64_t(generation) << 32) | (index + 1); }

// The cost thresholds by quality (design/submit.md, "Cost"), on an instance's projected size: the
// diameter of its bounding sphere on screen, in pixels.
struct Thresholds {
  float too_small;   // below this, it isn't drawn
  float rate[4];     // at or above rate[0] it animates every frame, rate[1] every 2nd, rate[2] every 4th, rate[3]
                     // every 8th, else every 16th
};
// Levels of detail switch only at the sizes each pipeline measured (PipelineType::detail_pixels).
const Thresholds kThresholds[3] = {
    {10, {240, 72, 24, 12}},  // low
    {7, {200, 60, 20, 10}},   // medium
    {4, {160, 48, 16, 8}},    // high
};
constexpr float kHysteresis = 1.15f;  // a level comes back only this much above where it left
constexpr int kFarCells = 12;         // the far level's grid: cells across the build's largest side
constexpr uint64_t kFarSalt = 0x9e3779b97f4a7c15ull;  // a far build's key: the coarsest build's, salted

// The detail parameter's value for a level: a pipeline level as itself, the far level as the coarsest.
uint32_t detail_value(uint32_t level, uint32_t levels) { return std::min(level, levels - 1); }

uint32_t rate_for(const Thresholds &t, float pixels) {
  return pixels >= t.rate[0] ? 1 : pixels >= t.rate[1] ? 2 : pixels >= t.rate[2] ? 4 : pixels >= t.rate[3] ? 8 : 16;
}

}  // namespace

World::World(const Config &config, std::string &error) : config_(config) {
  unsigned cores = std::max(1u, std::thread::hardware_concurrency());
  unsigned builders = config.build_threads ? config.build_threads : std::max(1u, cores / 4);
  unsigned updaters = config.update_threads ? config.update_threads : std::max(1u, cores - builders);
  pool_ = std::make_unique<Pool>(updaters - 1);  // the calling thread is one of them
  builders_ = std::make_unique<Queue>(builders);
  if (config.physics && !physics::check(config.physics, config.physics_version, error)) return;
  if (config.device) {
    if (!renderer_.init(config.device, config.color_format, config.depth_format, error)) return;
    if (!field_renderer_.init(config.device, config.color_format, config.depth_format, error)) return;
    have_renderer_ = true;
  }
  ok_ = true;
}

World::~World() {
  builders_.reset();  // finish generation jobs before anything they touch goes away
  for (Slot &s : slots_)
    if (s.alive) set_bodies(s, nullptr);
  for (auto &[key, e] : cache_) {
    if (e.uploaded && e.field) field_renderer_.release(e.fgpu);
    else if (e.uploaded) renderer_.release(e.gpu);
  }
  if (have_renderer_) {
    renderer_.shutdown();
    field_renderer_.shutdown();
  }
}

World::Slot *World::slot(uint64_t handle) {
  uint32_t index = static_cast<uint32_t>(handle & 0xffffffffu), generation = static_cast<uint32_t>(handle >> 32);
  if (index == 0 || index > slots_.size()) return nullptr;
  Slot &s = slots_[index - 1];
  return s.alive && s.generation == generation ? &s : nullptr;
}

const World::Slot *World::slot(uint64_t handle) const { return const_cast<World *>(this)->slot(handle); }

World::Entry *World::acquire(const PipelineType *type, const std::vector<uint8_t> &params, uint64_t key, bool far) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    ++it->second.refs;
    ++counters_.cache_hits;
    return &it->second;
  }
  ++counters_.cache_misses;
  Entry &e = cache_[key];
  e.type = type;
  e.refs = 1;
  e.order = next_order_++;
  if (!free_dense_.empty()) {
    e.dense = free_dense_.back();
    free_dense_.pop_back();
  } else {
    e.dense = next_dense_++;
  }
  auto copy = std::make_shared<std::vector<uint8_t>>(params);
  builders_->push([this, type, copy, key, far] {
    Finished f;
    f.key = key;
    f.build = type->build(copy->data(), f.error);
    if (far && f.build) {  // the far level: the coarsest build, clustered
      simplify(f.build->mesh, kFarCells);
      f.build->triangles = static_cast<uint32_t>(f.build->mesh.indices.size() / 3);
    }
    std::lock_guard<std::mutex> lock(finished_mutex_);
    finished_.push_back(std::move(f));
  });
  return &e;
}

void World::want(Slot &s) {
  const uint64_t key = s.type->build_key(s.params.data());
  if (key == s.wanted) return;
  if (s.wanted && s.wanted != s.current) unref(s.wanted);
  s.wanted = key;
  touch(s);
  if (s.has_current && key == s.current) return;
  acquire(s.type, s.params, key);
  waiting_.push_back(make_handle(static_cast<uint32_t>(&s - slots_.data()), s.generation));
}

void World::touch(Slot &s) {
  if (s.touched) return;
  s.touched = true;
  touched_.push_back(static_cast<uint32_t>(&s - slots_.data()));
}

std::vector<StageTotals *> &World::update_stages(const PipelineType *type) {
  for (auto &[t, v] : stage_slots_)
    if (t == type) return v;
  stage_slots_.push_back({type, {}});
  auto &v = stage_slots_.back().second;
  for (const StageInfo &st : type->stages()) {
    if (!st.update) continue;
    StageTotals &tot = stages_[st.label];
    tot.label = st.label;
    tot.update = true;
    v.push_back(&tot);
  }
  return v;
}

void World::drop_lods(Slot &s) {
  for (int l = 0; l < 4; ++l) {
    if (s.lods[l]) unref(s.lods[l]);
    s.lods[l] = 0;
    s.lod_entries[l] = nullptr;
    s.lod_ok[l] = -1;
  }
}

void World::bind_pose(Slot &s, const Entry &e) {
  s.pose = s.type->bind(*e.build);
  s.pose_dirty = true;
  s.posed_frame = 0;
  if (have_renderer_ && s.bone_count != e.build->bones) {
    renderer_.free_bones(s.bone_offset, s.bone_count);
    s.bone_count = e.build->bones;
    s.bone_offset = renderer_.alloc_bones(s.bone_count);
  }
}

void World::unref(uint64_t key) {
  auto it = cache_.find(key);
  if (it == cache_.end() || it->second.refs == 0) return;
  if (--it->second.refs == 0) it->second.idle_since = clock_;
}

uint64_t World::spawn(const PipelineType *type, const void *params, const float transform[16]) {
  uint32_t index;
  if (!free_.empty()) {
    index = free_.back();
    free_.pop_back();
  } else {
    index = static_cast<uint32_t>(slots_.size());
    slots_.emplace_back();
  }
  Slot &s = slots_[index];
  s.alive = true;
  s.type = type;
  s.params.assign(static_cast<const uint8_t *>(params), static_cast<const uint8_t *>(params) + type->params_size());
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 3; ++r) s.rows[r * 4 + c] = transform ? transform[c * 4 + r] : (c == r ? 1.0f : 0.0f);
  s.time = 0;
  s.fired.assign(type->event_count(), -1.0);
  s.current = s.wanted = 0;
  s.has_current = false;
  s.pose.reset();
  s.result = 0;
  s.posed_at = -1;
  s.awake = true;
  s.error.clear();
  s.entry = nullptr;
  s.level = s.drawn_level = 0;
  s.in_view = s.near_view = s.drawn = false;
  s.pixels = 0;
  s.posed_frame = 0;
  s.pose_dirty = false;
  s.touched = false;
  s.wants_lod = false;
  s.forced = -1;
  want(s);
  return make_handle(index, s.generation);
}

namespace {
constexpr uint64_t kFieldSalt = 0x7f4a7c159e3779b9ull;  // a field tile's key: its build key, salted (drawn its own way)
constexpr uint32_t kChunkCells = 8;                    // a field's chunks: 8 x 8 cells
const float kFieldPixels[3] = {96, 32, 10};             // levels' switch sizes for a field that measured none
constexpr float kFieldTileRadius = 1.5f;                // a tile's size, for its size on screen (as the stress tool measures)

uint32_t pack_tint(const float t[3]) {
  auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v * 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return byte(t[0]) | (byte(t[1]) << 8) | (byte(t[2]) << 16) | (255u << 24);
}

uint32_t cell_hash(int32_t q, int32_t r) {
  uint32_t h = static_cast<uint32_t>(q) * 0x9E3779B1u ^ (static_cast<uint32_t>(r) + 0x7f4a7c15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return h;
}
}  // namespace

uint64_t World::spawn_field(const PipelineType *type, const void *params, const float transform[16], const FieldCell *cells,
                            size_t count) {
  if (!type->is_field()) return 0;
  std::vector<FieldChoice> choices(count);
  std::string err;
  if (count && !type->field_cells(params, cells, count, choices.data(), err)) return 0;
  uint32_t index;
  if (!free_.empty()) {
    index = free_.back();
    free_.pop_back();
  } else {
    index = static_cast<uint32_t>(slots_.size());
    slots_.emplace_back();
  }
  Slot &s = slots_[index];
  s.alive = true;
  s.type = type;
  s.params.assign(static_cast<const uint8_t *>(params), static_cast<const uint8_t *>(params) + type->params_size());
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 3; ++r) s.rows[r * 4 + c] = transform ? transform[c * 4 + r] : (c == r ? 1.0f : 0.0f);
  s.time = 0;
  s.fired.clear();
  s.current = s.wanted = 0;
  s.has_current = false;
  s.pose.reset();
  s.result = 0;
  s.awake = false;  // nothing to pose: the wind moves it on the GPU
  s.error.clear();
  s.entry = nullptr;
  s.drawn = false;
  s.forced = -1;
  s.field = std::make_unique<Field>();
  Field &f = *s.field;
  // Each present cell's tile, placed in the world, grouped by chunk and then by variant.
  struct Placed {
    uint64_t chunk;
    uint32_t variant;
    FieldInstance inst;
  };
  std::vector<Placed> placed;
  placed.reserve(count);
  const float *m = s.rows;
  for (size_t i = 0; i < count; ++i) {
    const FieldChoice &c = choices[i];
    if (!c.present) continue;
    const float x = 1.7320508f * (static_cast<float>(cells[i].q) + 0.5f * static_cast<float>(cells[i].r));
    const float z = 1.5f * static_cast<float>(cells[i].r), y = cells[i].height;
    Placed p;
    p.inst.at[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    p.inst.at[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    p.inst.at[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    p.inst.scale = c.scale;
    p.inst.wind[0] = p.inst.wind[1] = 0;
    p.inst.tint = pack_tint(c.tint);
    p.inst.turn_phase = (c.turn % 6u) | (cell_hash(cells[i].q, cells[i].r) << 3);
    const int32_t cq = cells[i].q >= 0 ? cells[i].q / static_cast<int32_t>(kChunkCells) : -((-cells[i].q - 1) / static_cast<int32_t>(kChunkCells)) - 1;
    const int32_t cr = cells[i].r >= 0 ? cells[i].r / static_cast<int32_t>(kChunkCells) : -((-cells[i].r - 1) / static_cast<int32_t>(kChunkCells)) - 1;
    p.chunk = (uint64_t(static_cast<uint32_t>(cq)) << 32) | static_cast<uint32_t>(cr);
    p.variant = c.variant;
    placed.push_back(p);
  }
  std::sort(placed.begin(), placed.end(), [](const Placed &a, const Placed &b) {
    return a.chunk != b.chunk ? a.chunk < b.chunk : a.variant < b.variant;
  });
  f.tiles.reserve(placed.size());
  for (size_t i = 0; i < placed.size(); ++i) {
    if (i == 0 || placed[i].chunk != placed[i - 1].chunk) f.chunks.emplace_back();
    Field::Chunk &ch = f.chunks.back();
    if (ch.runs.empty() || placed[i].variant != ch.runs.back().variant || placed[i].chunk != placed[i - 1].chunk)
      ch.runs.push_back({placed[i].variant, static_cast<uint32_t>(f.tiles.size()), 0});
    ++ch.runs.back().count;
    f.tiles.push_back(placed[i].inst);
  }
  // Each chunk's bounding sphere: its tiles' centres, plus a tile's reach and height.
  for (Field::Chunk &ch : f.chunks) {
    double sum[3] = {0, 0, 0};
    uint32_t n = 0;
    for (const auto &run : ch.runs)
      for (uint32_t t = run.first; t < run.first + run.count; ++t, ++n)
        for (int k = 0; k < 3; ++k) sum[k] += f.tiles[t].at[k];
    for (int k = 0; k < 3; ++k) ch.c[k] = static_cast<float>(sum[k] / std::max(1u, n));
    float r2 = 0;
    for (const auto &run : ch.runs)
      for (uint32_t t = run.first; t < run.first + run.count; ++t) {
        const float dx = f.tiles[t].at[0] - ch.c[0], dy = f.tiles[t].at[1] - ch.c[1], dz = f.tiles[t].at[2] - ch.c[2];
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
      }
    ch.r = std::sqrt(r2) + 2.0f;
  }
  field_slots_.push_back(index);
  return make_handle(index, s.generation);
}

// A field's tile build for a variant at a level: asked for once, kept until the field goes.
World::Entry *World::field_tile(Slot &s, uint32_t variant, uint32_t level, bool request) {
  Field &f = *s.field;
  const uint32_t id = (variant << 4) | level;
  auto it = f.builds.find(id);
  if (it != f.builds.end()) return it->second.second;
  if (!request) return nullptr;
  std::vector<uint8_t> params(s.params.size());
  s.type->field_tile_params(s.params.data(), variant, params.data());
  if (s.type->detail_levels() > 1) std::memcpy(params.data() + s.type->detail_offset(), &level, sizeof level);
  const uint64_t key = s.type->build_key(params.data()) ^ kFieldSalt;
  Entry *e = acquire(s.type, params, key);
  e->field = true;
  f.builds[id] = {key, e};
  return e;
}

void World::drop_field_builds(Slot &s) {
  for (auto &[id, b] : s.field->builds) unref(b.first);
  s.field->builds.clear();
}

void World::set_bodies(Slot &s, Entry *entry) {
  if (!config_.physics) return;
  physics::remove(config_.physics, s.bodies);
  s.bodies.clear();
  if (!entry || !entry->build) return;
  if (!entry->shapes_made) {
    entry->shapes_made = true;
    if (!entry->build->collision.empty()) {
      std::string err;
      entry->shapes = physics::make_shapes(entry->build->collision, err);
      if (!entry->shapes) s.error = err;
    }
  }
  if (entry->shapes)
    s.bodies = physics::add(config_.physics, *entry->shapes, config_.physics_layers, s.rows,
                            (uint64_t(s.generation) << 32) | (uint32_t(&s - slots_.data()) + 1));
}

Status World::destroy(uint64_t handle) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  set_bodies(*s, nullptr);
  if (s->wanted && s->wanted != s->current) unref(s->wanted);
  if (s->has_current) unref(s->current);
  drop_lods(*s);
  if (s->field) {
    drop_field_builds(*s);
    s->field.reset();
    const uint32_t index = static_cast<uint32_t>(s - slots_.data());
    field_slots_.erase(std::remove(field_slots_.begin(), field_slots_.end(), index), field_slots_.end());
  }
  if (have_renderer_) renderer_.free_bones(s->bone_offset, s->bone_count);
  s->bone_offset = s->bone_count = 0;
  s->entry = nullptr;
  s->alive = false;
  ++s->generation;
  s->pose.reset();
  s->params.clear();
  s->params.shrink_to_fit();
  free_.push_back(static_cast<uint32_t>(s - slots_.data()));
  return Status::ok;
}

Status World::set_transform(uint64_t handle, const float transform[16]) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 3; ++r) s->rows[r * 4 + c] = transform[c * 4 + r];
  if (config_.physics) physics::move(config_.physics, s->bodies, s->rows);
  return Status::ok;
}

Status World::set_param(uint64_t handle, const PipelineType *type, size_t offset, const void *value, size_t size,
                        bool build) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  if (s->type != type) return Status::wrong_pipeline;
  if (offset + size > s->params.size()) return Status::bad_parameter;
  std::memcpy(s->params.data() + offset, value, size);
  if (s->field) {
    if (build) drop_field_builds(*s);  // its tiles are asked for again, with the new parameters
    return Status::ok;
  }
  if (build) {
    s->error.clear();
    want(*s);
  }
  s->awake = true;  // setting a parameter wakes the instance (api.md)
  touch(*s);
  return Status::ok;
}

Status World::set_named(uint64_t handle, const char *name, const char *value) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  if (!name || !value) return Status::bad_parameter;
  size_t count = 0;
  const ParamInfo *table = s->type->param_table(count);
  for (size_t i = 0; i < count; ++i) {
    const ParamInfo &p = table[i];
    if (std::strcmp(p.name, name) != 0) continue;
    switch (p.kind) {
      case ParamInfo::kEnum: {
        uint32_t v = p.count;
        for (uint32_t k = 0; k < p.count; ++k)
          if (std::strcmp(p.values[k], value) == 0) v = k;
        if (v == p.count) {
          char *end = nullptr;
          const unsigned long n = std::strtoul(value, &end, 10);
          if (end == value || n >= p.count) return Status::bad_parameter;
          v = static_cast<uint32_t>(n);
        }
        return set_param(handle, s->type, p.offset, &v, sizeof v, p.build);
      }
      case ParamInfo::kU32: {
        const double d = std::clamp(std::strtod(value, nullptr), p.min, p.max);
        const uint32_t v = static_cast<uint32_t>(d);
        return set_param(handle, s->type, p.offset, &v, sizeof v, p.build);
      }
      case ParamInfo::kF32: {
        const float v = static_cast<float>(std::clamp(std::strtod(value, nullptr), p.min, p.max));
        return set_param(handle, s->type, p.offset, &v, sizeof v, p.build);
      }
      case ParamInfo::kBool: {
        const bool v = std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
        return set_param(handle, s->type, p.offset, &v, sizeof v, p.build);
      }
    }
  }
  return Status::bad_parameter;
}

Status World::trigger(uint64_t handle, const PipelineType *type, size_t event) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  if (s->type != type) return Status::wrong_pipeline;
  if (event >= s->fired.size()) return Status::bad_parameter;
  s->fired[event] = s->time;
  s->awake = true;
  touch(*s);
  return Status::ok;
}

uint32_t World::result_of(const Slot &s) const {
  uint32_t bits = 0;
  const bool settled = s.pose && s.pose->settled;
  // Over: it fired, and a pose computed since then has settled (a pose from before is stale).
  for (size_t i = 0; i < s.fired.size() && i < 31; ++i)
    if (s.fired[i] >= 0 && settled && s.posed_at > s.fired[i]) bits |= 1u << i;
  if (s.has_current && s.current == s.wanted) bits |= kResultReady;
  return bits;
}

Status World::result(uint64_t handle, uint32_t &bits) const {
  const Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  bits = result_of(*s);
  return Status::ok;
}

Status World::sleeping(uint64_t handle, bool &out) const {
  const Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  out = !s->awake;
  return Status::ok;
}

void World::set_camera(const float view[16], const float projection[16]) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += projection[k * 4 + r] * view[c * 4 + k];
      view_proj_[c * 4 + r] = sum;
    }
  // The eye, for distances: the view's translation brought back through its rotation.
  for (int i = 0; i < 3; ++i) eye_[i] = -(view[i * 4 + 0] * view[12] + view[i * 4 + 1] * view[13] + view[i * 4 + 2] * view[14]);
  p11_ = std::fabs(projection[5]);
  ortho_ = projection[11] == 0.0f;
  have_camera_ = true;
  ++camera_serial_;
}

Status World::force_level(uint64_t handle, int level) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  s->forced = static_cast<int8_t>(std::clamp(level, -1, 3));
  ++camera_serial_;
  return Status::ok;
}

void World::set_viewport(uint32_t width, uint32_t height) {
  (void)width;
  viewport_h_ = std::max(1u, height);
  ++camera_serial_;
}

void World::set_quality(int quality) {
  quality_ = std::clamp(quality, 0, 2);
  ++camera_serial_;
}

void World::clocks(const Slot &s, std::vector<float> &out) const {
  out.resize(1 + s.fired.size());
  out[0] = static_cast<float>(s.time);
  for (size_t i = 0; i < s.fired.size(); ++i)
    out[1 + i] = s.fired[i] >= 0 ? static_cast<float>(s.time - s.fired[i]) : -1.0f;
}

void World::integrate() {
  std::vector<Finished> done;
  {
    std::lock_guard<std::mutex> lock(finished_mutex_);
    done.swap(finished_);
  }
  for (Finished &f : done) {
    auto it = cache_.find(f.key);
    if (it == cache_.end()) continue;
    Entry &e = it->second;
    e.pending = false;
    ++counters_.generations;
    if (e.lod) ++counters_.lod_generations;
    if (!f.build) {
      e.failed = true;
      e.error = f.error.empty() ? "build failed" : f.error;
      ++counters_.generation_failures;
    } else {
      e.build = std::move(f.build);
      const auto &stages = e.type->stages();
      size_t b = 0;
      for (const StageInfo &st : stages) {
        if (st.update) continue;
        if (b < e.build->stage_ms.size()) {
          StageTotals &t = stages_[st.label];
          t.label = st.label;
          ++t.calls;
          t.total_ms += e.build->stage_ms[b];
          t.max_ms = std::max(t.max_ms, e.build->stage_ms[b]);
        }
        ++b;
      }
      if (!have_renderer_) e.build->mesh.clear();
    }
    if (e.refs == 0) e.idle_since = clock_;
  }
  // Instances whose wanted build is ready switch to it; the old one stays on screen until then.
  size_t keep = 0;
  for (size_t w = 0; w < waiting_.size(); ++w) {
    Slot *sp = slot(waiting_[w]);
    if (!sp || !sp->wanted || (sp->has_current && sp->wanted == sp->current)) continue;  // gone, or done
    Slot &s = *sp;
    auto it = cache_.find(s.wanted);
    if (it == cache_.end() || it->second.pending) {
      waiting_[keep++] = waiting_[w];
      continue;
    }
    if (it->second.failed) {
      s.error = it->second.error;
      continue;
    }
    if (s.has_current) unref(s.current);
    drop_lods(s);  // they were coarser versions of the old build
    s.current = s.wanted;
    s.has_current = true;
    s.entry = &it->second;
    s.level = s.drawn_level = 0;
    bind_pose(s, it->second);
    s.awake = true;
    set_bodies(s, &it->second);  // the new build's collision replaces the old (a stump for a trunk)
    touch(s);
  }
  waiting_.resize(keep);
}

void World::evict(std::map<uint64_t, Entry>::iterator it) {
  if (it->second.uploaded && it->second.field) field_renderer_.release(it->second.fgpu);
  else if (it->second.uploaded) renderer_.release(it->second.gpu);
  free_dense_.push_back(it->second.dense);
  ++counters_.cache_evictions;
  cache_.erase(it);
}

void World::release_idle(bool all) {
  uint64_t idle_bytes = 0;
  for (auto it = cache_.begin(); it != cache_.end();) {
    Entry &e = it->second;
    if (e.refs == 0 && !e.pending && (all || clock_ - e.idle_since >= config_.release_seconds)) {
      evict(it++);
      continue;
    }
    if (e.refs == 0 && !e.pending && e.build) idle_bytes += e.build->bytes + e.gpu.bytes;
    ++it;
  }
  // Over budget: evict the oldest unused builds first.
  while (idle_bytes > config_.cache_budget) {
    auto oldest = cache_.end();
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
      if (it->second.refs == 0 && !it->second.pending && (oldest == cache_.end() || it->second.order < oldest->second.order))
        oldest = it;
    if (oldest == cache_.end()) break;
    if (oldest->second.build) idle_bytes -= std::min(idle_bytes, oldest->second.build->bytes + oldest->second.gpu.bytes);
    evict(oldest);
  }
}

void World::flush() { release_idle(true); }

// Where each instance stands on screen: in view, its projected size, and the level of detail that
// size asks for (with hysteresis). With request, the coarser builds it asks for are requested.
void World::classify(bool request) {
  classified_serial_ = camera_serial_;
  if (!have_camera_) return;
  float planes[6][4];
  const float *m = view_proj_;
  auto row = [&](int r, int i) { return m[i * 4 + r]; };
  for (int i = 0; i < 4; ++i) {
    planes[0][i] = row(3, i) + row(0, i);
    planes[1][i] = row(3, i) - row(0, i);
    planes[2][i] = row(3, i) + row(1, i);
    planes[3][i] = row(3, i) - row(1, i);
    planes[4][i] = row(2, i);
    planes[5][i] = row(3, i) - row(2, i);
  }
  for (auto &p : planes) {
    float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (float &v : p) v /= len;
  }
  const Thresholds &t = kThresholds[quality_];
  const float scale_px = static_cast<float>(viewport_h_) * p11_;
  const bool detail = cost_ & kCostDetail;
  constexpr size_t kGrain = 512;
  std::vector<std::vector<uint32_t>> asks((slots_.size() + kGrain - 1) / kGrain);
  pool_->parallel_for(slots_.size(), kGrain, [&](size_t b, size_t e) {
    std::vector<uint32_t> &ask = asks[b / kGrain];
    for (size_t i = b; i < e; ++i) {
      Slot &s = slots_[i];
      if (!s.alive || !s.has_current || !s.entry || !s.entry->build) continue;
      const Build &build = *s.entry->build;
      const float *r = s.rows;
      const float cy = build.height * 0.5f;
      const float c[3] = {r[1] * cy + r[3], r[5] * cy + r[7], r[9] * cy + r[11]};
      const float scale = std::sqrt(std::max({r[0] * r[0] + r[4] * r[4] + r[8] * r[8], r[1] * r[1] + r[5] * r[5] + r[9] * r[9],
                                              r[2] * r[2] + r[6] * r[6] + r[10] * r[10]}));
      const float radius = build.radius * scale;
      float worst = 1e30f;
      for (const auto &p : planes) worst = std::min(worst, p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3]);
      s.in_view = worst >= -radius;
      s.near_view = worst >= -(radius * 1.5f + 2.0f);  // a margin, so what turns into view is already posed
      const float dx = c[0] - eye_[0], dy = c[1] - eye_[1], dz = c[2] - eye_[2];
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      s.pixels = ortho_ ? scale_px * radius : (d > radius ? scale_px * radius / d : 1e9f);
      // The level its size asks for: coarser while below the threshold, finer only clearly above it.
      const uint32_t levels = s.type->detail_levels();
      if (!detail || levels < 2) {
        s.level = 0;
        continue;
      }
      uint32_t base = 0;
      std::memcpy(&base, s.params.data() + s.type->detail_offset(), sizeof base);
      static const float kNever[3] = {0, 0, 0};
      const float *switch_px = s.type->detail_pixels(quality_);
      if (!switch_px) switch_px = kNever;  // not measured: the game's level, always
      // The pipeline's levels, then the far level (when the pipeline measured where it switches in).
      const uint32_t far = levels <= 3 ? levels : 0;
      const uint32_t last = far && switch_px[levels - 1] > 0 ? far : std::min<uint32_t>(levels, 3) - 1;
      uint32_t level = std::max<uint32_t>(s.level, base);
      if (s.forced >= 0) {
        level = std::clamp<uint32_t>(static_cast<uint32_t>(s.forced), base, far ? far : levels - 1);
      } else {
        level = std::min(level, last);
        while (level < last && s.pixels < switch_px[level]) ++level;
        while (level > base && s.pixels > switch_px[level - 1] * kHysteresis) --level;
      }
      s.level = static_cast<uint8_t>(level);
      if (level > base && !s.lods[level]) ask.push_back(static_cast<uint32_t>(i));
    }
  });
  if (!request || !detail) return;
  std::vector<uint8_t> params;
  for (const auto &ask : asks)
  for (uint32_t i : ask) {
    Slot &s = slots_[i];
    params = s.params;
    const uint32_t levels = s.type->detail_levels();
    const uint32_t value = detail_value(s.level, levels);
    const bool far = s.level >= levels;
    std::memcpy(params.data() + s.type->detail_offset(), &value, sizeof value);
    const uint64_t key = s.type->build_key(params.data()) ^ (far ? kFarSalt : 0);
    s.lods[s.level] = key;
    s.lod_entries[s.level] = acquire(s.type, params, key, far);
    if (s.lod_entries[s.level]->pending && s.lod_entries[s.level]->refs == 1) s.lod_entries[s.level]->lod = true;
  }
}

// Whether a coarser build poses like the instance's own: the same bones, moving the same way, so
// the instance's pose can drive it. Checked once per pair, at two moments (swaying, and with every
// event fired), with this instance's parameters.
bool World::poses_alike(Slot &s, Entry &coarse) {
  for (const auto &[key, alike] : coarse.poses_like)
    if (key == s.current) return alike;
  const Build &fine = *s.entry->build, &other = *coarse.build;
  bool alike = fine.bones == other.bones;
  if (alike && s.type->has_update()) {
    std::vector<uint8_t> params = s.params;
    uint32_t level = 0;
    for (int l = 0; l < 4; ++l)
      if (s.lod_entries[l] == &coarse) level = static_cast<uint32_t>(l);
    level = detail_value(level, s.type->detail_levels());
    std::memcpy(params.data() + s.type->detail_offset(), &level, sizeof level);
    const size_t events = s.type->event_count();
    for (int moment = 0; moment < 2 && alike; ++moment) {
      std::vector<float> clocks(1 + events, -1.0f);
      clocks[0] = moment == 0 ? 0.37f : 2.5f;
      if (moment == 1)
        for (size_t i = 0; i < events; ++i) clocks[1 + i] = 0.8f;
      auto a = s.type->bind(fine), b = s.type->bind(other);
      std::string err;
      alike = s.type->update(fine, *a, s.params.data(), clocks.data(), err) &&
              s.type->update(other, *b, params.data(), clocks.data(), err) &&
              s.type->pose_hash(*a) == s.type->pose_hash(*b);
    }
  }
  coarse.poses_like.push_back({s.current, alike});
  return alike;
}

// The entry an instance is drawn with: the coarsest ready level at or finer than the one its size
// asks for that poses like its own build, or its own build. Safe to run in parallel: a level not yet
// checked is passed over (unchecked is set, and the caller checks it once, serially).
World::Entry *World::pick_lod(Slot &s, bool &unchecked) {
  for (int l = s.level; l > 0; --l) {
    Entry *e = s.lod_entries[l];
    if (!e || !e->build || e->failed || (have_renderer_ && !e->uploaded)) continue;
    if (s.lod_ok[l] < 0) {
      unchecked = true;
      continue;
    }
    if (s.lod_ok[l] == 1) {
      s.drawn_level = static_cast<uint8_t>(l);
      return e;
    }
  }
  uint32_t base = 0;
  if (s.type->detail_levels() > 1) std::memcpy(&base, s.params.data() + s.type->detail_offset(), sizeof base);
  s.drawn_level = static_cast<uint8_t>(base);
  return s.entry;
}

void World::update(float dt) {
  const auto t0 = std::chrono::steady_clock::now();
  clock_ += dt;
  ++frame_;
  integrate();
  const auto t1 = std::chrono::steady_clock::now();
  classify(true);
  const auto t2 = std::chrono::steady_clock::now();
  times_.integrate_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  times_.classify_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  const Thresholds &t = kThresholds[quality_];
  const bool skip_unseen = have_camera_ && (cost_ & kCostUnseen), skip_rate = have_camera_ && (cost_ & kCostRate);
  const bool skip_small = cost_ & kCostTooSmall;
  // Who is posed this frame: decided in parallel, gathered in order.
  struct Part {
    std::vector<Slot *> active;
    uint64_t unseen = 0, rate_skipped = 0;
  };
  constexpr size_t kGrain = 512;
  std::vector<Part> parts((slots_.size() + kGrain - 1) / kGrain);
  pool_->parallel_for(slots_.size(), kGrain, [&](size_t b, size_t e) {
    Part &part = parts[b / kGrain];
    for (size_t i = b; i < e; ++i) {
      Slot &s = slots_[i];
      if (!s.alive || !s.awake) continue;
      s.time += dt;
      if (s.has_current && s.type->has_update() && s.pose) {
        // An event plays out wherever it is, so its result arrives; anything else is posed only
        // when it can be seen, and less often the smaller it is. Poses are pure functions of time,
        // so a skipped frame costs nothing in accuracy: the next pose is exact.
        bool event_live = false;
        for (size_t k = 0; k < s.fired.size() && k < 31; ++k)
          if (s.fired[k] >= 0 && !(s.result & (1u << k))) event_live = true;
        // Out of view, or too small to be drawn: not seen.
        if (!event_live && skip_unseen && (!s.near_view || (skip_small && s.pixels < t.too_small))) {
          ++part.unseen;
          continue;
        }
        if (!event_live && skip_rate && s.posed_frame) {
          const uint32_t rate = rate_for(t, s.pixels);
          if (rate > 1 && frame_ - s.posed_frame < 2 * rate && (frame_ + i) % rate != 0) {
            ++part.rate_skipped;
            continue;
          }
        }
        part.active.push_back(&s);
      } else if (s.has_current && !s.type->has_update()) {
        s.awake = false;  // nothing moves: sleep
      }
    }
  });
  std::vector<Slot *> active;
  uint64_t unseen = 0, rate_skipped = 0;
  for (Part &part : parts) {
    active.insert(active.end(), part.active.begin(), part.active.end());
    unseen += part.unseen;
    rate_skipped += part.rate_skipped;
  }
  pool_->parallel_for(active.size(), 16, [&](size_t b, size_t e) {
    std::vector<float> c;
    std::vector<uint8_t> local;
    for (size_t i = b; i < e; ++i) {
      Slot &s = *active[i];
      const Entry &entry = *s.entry;
      clocks(s, c);
      // The wind's heading is the world's: in the instance's own frame, it is turned back by the
      // instance's rotation, so a turned tree bends the same way as its neighbours.
      const uint8_t *params = s.params.data();
      const size_t wh = s.type->wind_heading_offset();
      if (wh != SIZE_MAX) {
        local = s.params;
        float heading = 0;
        std::memcpy(&heading, local.data() + wh, sizeof heading);
        const float rad = heading * 0.017453293f, dx = std::sin(rad), dz = std::cos(rad);
        const float *r = s.rows;  // world = rows * local: local = rows^T * world
        const float lx = r[0] * dx + r[8] * dz, lz = r[2] * dx + r[10] * dz;
        float h = std::atan2(lx, lz) * 57.29578f;
        if (h < 0) h += 360.0f;
        std::memcpy(local.data() + wh, &h, sizeof h);
        params = local.data();
      }
      std::string err;
      if (!s.type->update(*entry.build, *s.pose, params, c.data(), err)) {
        s.error = err;
        s.awake = false;
        continue;
      }
      s.posed_at = s.time;
      s.posed_frame = frame_;
      s.pose_dirty = true;
      if (s.pose->settled) s.awake = false;  // every update stage voted to sleep (design/submit.md)
    }
  });
  const auto t3 = std::chrono::steady_clock::now();
  times_.animate_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
  counters_.animated = active.size();
  counters_.unseen = unseen;
  counters_.rate_skipped = rate_skipped;
  for (Slot *s : active) {
    std::vector<StageTotals *> &totals = update_stages(s->type);
    for (size_t u = 0; u < totals.size() && u < s->pose->stage_ms.size(); ++u) {
      StageTotals &tot = *totals[u];
      ++tot.calls;
      tot.total_ms += s->pose->stage_ms[u];
      tot.max_ms = std::max(tot.max_ms, s->pose->stage_ms[u]);
    }
    touch(*s);
  }
  // Results: list the instances whose current values changed (the game diffs them for edges). Only
  // a pose, a switch of build, or the game's own call can change one.
  changed_.clear();
  for (uint32_t i : touched_) {
    Slot &s = slots_[i];
    s.touched = false;
    if (!s.alive) continue;
    const uint32_t bits = result_of(s);
    if (bits != s.result) {
      s.result = bits;
      changed_.push_back(make_handle(i, s.generation));
    }
  }
  touched_.clear();
  release_idle(false);
  times_.results_ms = ms_since(t3);
  times_.update_ms = ms_since(t0);
}

void World::prepare(SDL_GPUCommandBuffer *cmd) {
  const auto t0 = std::chrono::steady_clock::now();
  SDL_GPUCopyPass *copy = have_renderer_ && cmd ? SDL_BeginGPUCopyPass(cmd) : nullptr;
  if (copy) {
    for (auto &[key, e] : cache_) {
      if (e.pending || e.failed || e.uploaded || !e.build) continue;
      if (e.field) e.fgpu = field_renderer_.upload(copy, e.build->mesh);
      else e.gpu = renderer_.upload(copy, e.build->mesh);
      e.uploaded = true;
      e.build->mesh.clear();
    }
  }
  if (classified_serial_ != camera_serial_) classify(false);  // the camera moved since the update

  // What is drawn, and with which build: in view, big enough, at its level of detail.
  const Thresholds &t = kThresholds[quality_];
  const bool too_small = cost_ & kCostTooSmall, detail = cost_ & kCostDetail;
  struct Visible {
    uint32_t slot;
    Entry *entry;
  };
  struct Part {
    std::vector<Visible> visible;
    std::vector<uint32_t> unchecked;
    uint64_t culled = 0, small = 0, by_level[4] = {0, 0, 0, 0};
  };
  constexpr size_t kGrain = 512;
  std::vector<Part> parts((slots_.size() + kGrain - 1) / kGrain);
  pool_->parallel_for(slots_.size(), kGrain, [&](size_t b, size_t e) {
    Part &part = parts[b / kGrain];
    for (size_t i = b; i < e; ++i) {
      Slot &s = slots_[i];
      s.drawn = false;
      if (!s.alive || !s.has_current || !s.entry || !s.entry->build) continue;
      if (have_camera_ && !s.in_view) {
        ++part.culled;
        continue;
      }
      if (have_camera_ && too_small && s.pixels < t.too_small) {
        ++part.small;
        continue;
      }
      Entry *entry = s.entry;
      if (detail) {
        bool unchecked = false;
        entry = pick_lod(s, unchecked);
        if (unchecked) part.unchecked.push_back(static_cast<uint32_t>(i));
      } else {
        uint32_t base = 0;
        if (s.type->detail_levels() > 1) std::memcpy(&base, s.params.data() + s.type->detail_offset(), sizeof base);
        s.drawn_level = static_cast<uint8_t>(base);
      }
      s.drawn = true;
      ++part.by_level[std::min<uint32_t>(s.drawn_level, 3)];
      part.visible.push_back({static_cast<uint32_t>(i), entry});
    }
  });
  std::vector<Visible> visible;
  uint64_t culled = 0, small = 0;
  uint64_t by_level[4] = {0, 0, 0, 0};
  for (Part &part : parts) {
    visible.insert(visible.end(), part.visible.begin(), part.visible.end());
    culled += part.culled;
    small += part.small;
    for (int l = 0; l < 4; ++l) by_level[l] += part.by_level[l];
    // Coarser levels that became ready: check, once, that they pose like the instance's own build
    // (drawn from the next frame).
    for (uint32_t i : part.unchecked) {
      Slot &s = slots_[i];
      for (int l = 1; l < 4; ++l) {
        Entry *e = s.lod_entries[l];
        if (s.lod_ok[l] >= 0 || !e || !e->build || e->failed) continue;
        s.lod_ok[l] = poses_alike(s, *e) ? 1 : 0;
      }
    }
  }
  // Bucket by build (a counting sort on the entries' small indices): each bucket is one draw.
  std::vector<uint32_t> start(next_dense_ + 1, 0);
  for (const Visible &v : visible) ++start[v.entry->dense + 1];
  for (uint32_t d = 0; d < next_dense_; ++d) start[d + 1] += start[d];
  std::vector<Visible> sorted(visible.size());
  {
    std::vector<uint32_t> at(start.begin(), start.end() - 1);
    for (const Visible &v : visible) sorted[at[v.entry->dense]++] = v;
  }
  // The bones that changed and can be seen go up this frame; the rest wait in their slots.
  std::vector<uint32_t> dirty, dirty_at;
  uint32_t dirty_bones = 0;
  if (have_renderer_) {
    const bool retain = cost_ & kCostRetain;  // off: every pose drawn goes up, every frame
    for (const Visible &v : sorted) {
      Slot &s = slots_[v.slot];
      if ((retain && !s.pose_dirty) || s.bone_count == 0) continue;
      dirty.push_back(v.slot);
      dirty_at.push_back(dirty_bones);
      dirty_bones += s.bone_count;
    }
  }
  uint64_t triangles = 0;
  for (const Visible &v : sorted) triangles += v.entry->build->triangles;
  if (copy) {
    SkinnedRenderer::Frame f = renderer_.begin_frame(static_cast<uint32_t>(sorted.size()), dirty_bones);
    if (f.instances) {
      pool_->parallel_for(sorted.size(), 256, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
          const Slot &s = slots_[sorted[i].slot];
          SkinnedInstance &inst = f.instances[i];
          std::memcpy(inst.row0, s.rows, 16);
          std::memcpy(inst.row1, s.rows + 4, 16);
          std::memcpy(inst.row2, s.rows + 8, 16);
          inst.bone_offset = s.bone_offset;
          inst.pad[0] = inst.pad[1] = inst.pad[2] = 0;
        }
      });
      pool_->parallel_for(dirty.size(), 32, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
          Slot &s = slots_[dirty[i]];
          float *out = f.rows + size_t(dirty_at[i]) * 12;
          uint32_t *where = f.slots + dirty_at[i];
          const std::vector<float> *m = s.pose ? &s.pose->matrices : nullptr;
          for (uint32_t bone = 0; bone < s.bone_count; ++bone) {
            float *rows = out + bone * 12;
            where[bone] = s.bone_offset + bone;
            if (m && m->size() >= size_t(bone + 1) * 16) {
              const float *mm = m->data() + size_t(bone) * 16;
              for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) rows[r * 4 + c] = mm[c * 4 + r];
            } else {
              const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
              std::memcpy(rows, id, sizeof id);
            }
          }
          s.pose_dirty = false;
        }
      });
    }
    renderer_.end_frame(copy);
  }
  prepare_fields(copy);
  if (copy) {
    SDL_EndGPUCopyPass(copy);
    renderer_.scatter(cmd);
  }
  frame_ranges_.clear();
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i == 0 || sorted[i].entry != sorted[i - 1].entry)
      frame_ranges_.push_back({&sorted[i].entry->gpu, static_cast<uint32_t>(i), 0});
    ++frame_ranges_.back().count;
  }
  counters_.draws = frame_ranges_.size();
  counters_.drawn = sorted.size();
  counters_.culled = culled;
  counters_.too_small = small;
  for (int l = 0; l < 4; ++l) counters_.by_level[l] = by_level[l];
  counters_.triangles = triangles + field_triangles_;
  counters_.bones_uploaded = copy ? dirty_bones : 0;
  times_.prepare_ms = ms_since(t0);
}

// Fields: each chunk in view picks its level by the size of a tile at its nearest point (with
// hysteresis); its runs of tiles are drawn with that level's build of their variant (or the nearest
// level that is ready), streamed as instances with the field's wind, one draw per tile build. The
// choosing and the copying run across the update threads; builds not yet asked for are asked for
// once, between them, and drawn from a later frame.
void World::prepare_fields(SDL_GPUCopyPass *copy) {
  field_ranges_.clear();
  field_triangles_ = 0;
  counters_.field_tiles = 0;
  for (auto &n : counters_.field_tiles_by_level) n = 0;
  if (field_slots_.empty()) return;
  float planes[6][4];
  const float *m = view_proj_;
  auto row = [&](int r, int i) { return m[i * 4 + r]; };
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
  const float scale_px = static_cast<float>(viewport_h_) * p11_;
  struct Pick {
    Entry *e;
    uint32_t slot, first, count;
    float wind[2];
  };
  struct Want {
    uint32_t slot, variant, level;
  };
  struct Part {
    std::vector<Pick> picks;
    std::vector<Want> wants;
    uint64_t tiles = 0, triangles = 0, by_level[4] = {0, 0, 0, 0};
  };
  constexpr size_t kGrain = 4;  // fields per task
  std::vector<Part> parts((field_slots_.size() + kGrain - 1) / kGrain);
  pool_->parallel_for(field_slots_.size(), kGrain, [&](size_t b, size_t e) {
    Part &part = parts[b / kGrain];
    for (size_t fi = b; fi < e; ++fi) {
      const uint32_t index = field_slots_[fi];
      Slot &s = slots_[index];
      Field &f = *s.field;
      f.drawn_tiles = f.drawn_triangles = 0;
      float speed = 0, heading = 0;
      s.type->field_wind(s.params.data(), speed, heading);
      const float rad = heading * 3.14159265f / 180.0f;
      const float wind[2] = {std::sin(rad) * speed / 10.0f, std::cos(rad) * speed / 10.0f};
      const uint32_t levels = std::max<uint32_t>(1, std::min<uint32_t>(s.type->detail_levels(), 4));
      const float *switch_px = s.type->detail_pixels(quality_);
      if (!switch_px) switch_px = kFieldPixels;
      // Read-only here: what isn't built yet is wanted, and asked for after.
      auto built = [&](uint32_t variant, uint32_t level) -> Entry * {
        auto it = f.builds.find((variant << 4) | level);
        return it == f.builds.end() ? nullptr : it->second.second;
      };
      auto ready = [&](Entry *x) { return x && x->build && !x->failed && (!have_renderer_ || x->uploaded); };
      for (Field::Chunk &ch : f.chunks) {
        if (have_camera_) {
          bool in = true;
          for (const auto &p : planes)
            if (p[0] * ch.c[0] + p[1] * ch.c[1] + p[2] * ch.c[2] + p[3] < -ch.r) {
              in = false;
              break;
            }
          if (!in) continue;
        }
        // A tile's size at the chunk's nearest point.
        const float dx = ch.c[0] - eye_[0], dy = ch.c[1] - eye_[1], dz = ch.c[2] - eye_[2];
        const float d = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - ch.r, 0.5f);
        const float pixels = have_camera_ ? (ortho_ ? scale_px * kFieldTileRadius : scale_px * kFieldTileRadius / d) : 1e9f;
        uint32_t level = std::min<uint32_t>(ch.level, levels - 1);
        if (s.forced >= 0) {
          level = std::min<uint32_t>(static_cast<uint32_t>(s.forced), levels - 1);
        } else if (cost_ & kCostDetail) {
          while (level + 1 < levels && level < 3 && pixels < switch_px[level]) ++level;
          while (level > 0 && pixels > switch_px[level - 1] * kHysteresis) --level;
        } else {
          level = 0;
        }
        ch.level = static_cast<uint8_t>(level);
        for (const Field::Run &run : ch.runs) {
          Entry *x = built(run.variant, level);
          if (!x) part.wants.push_back({index, run.variant, level});
          uint32_t used = level;
          if (!ready(x)) {  // the nearest level that is ready, coarser first
            x = nullptr;
            for (uint32_t k = 1; k < levels && !x; ++k) {
              if (level + k < levels && ready(built(run.variant, level + k))) x = built(run.variant, level + k), used = level + k;
              if (!x && level >= k && ready(built(run.variant, level - k))) x = built(run.variant, level - k), used = level - k;
            }
          }
          if (!x) continue;
          part.picks.push_back({x, index, run.first, run.count, {wind[0], wind[1]}});
          f.drawn_tiles += run.count;
          f.drawn_triangles += uint64_t(run.count) * x->build->triangles;
          part.by_level[std::min<uint32_t>(used, 3)] += run.count;
          f.level_drawn = used;
        }
      }
      part.tiles += f.drawn_tiles;
      part.triangles += f.drawn_triangles;
    }
  });
  // Ask for what was wanted (each once), then gather the counts.
  for (Part &part : parts) {
    for (const Want &w : part.wants) field_tile(slots_[w.slot], w.variant, w.level, true);
    counters_.field_tiles += part.tiles;
    field_triangles_ += part.triangles;
    for (int l = 0; l < 4; ++l) counters_.field_tiles_by_level[l] += part.by_level[l];
  }
  // One draw per tile build: count, place, and copy each pick's tiles with its wind.
  std::vector<uint32_t> start(next_dense_ + 1, 0);
  uint32_t total = 0;
  for (const Part &part : parts)
    for (const Pick &p : part.picks) {
      start[p.e->dense + 1] += p.count;
      total += p.count;
    }
  for (uint32_t d = 0; d < next_dense_; ++d) start[d + 1] += start[d];
  if (!copy || total == 0) return;
  std::vector<uint32_t> at(start.begin(), start.end() - 1);
  std::vector<Entry *> by_dense(next_dense_, nullptr);
  std::vector<std::pair<const Pick *, uint32_t>> placed;  // each pick and where its tiles go
  for (const Part &part : parts)
    for (const Pick &p : part.picks) {
      placed.push_back({&p, at[p.e->dense]});
      at[p.e->dense] += p.count;
      by_dense[p.e->dense] = p.e;
    }
  FieldInstance *out = field_renderer_.begin_frame(total);
  pool_->parallel_for(placed.size(), 64, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      const Pick &p = *placed[i].first;
      FieldInstance *dst = out + placed[i].second;
      std::memcpy(dst, slots_[p.slot].field->tiles.data() + p.first, size_t(p.count) * sizeof(FieldInstance));
      for (uint32_t k = 0; k < p.count; ++k) {
        dst[k].wind[0] = p.wind[0];
        dst[k].wind[1] = p.wind[1];
      }
    }
  });
  field_renderer_.end_frame(copy);
  for (uint32_t d = 0; d < next_dense_; ++d)
    if (by_dense[d] && start[d + 1] > start[d]) field_ranges_.push_back({&by_dense[d]->fgpu, start[d], start[d + 1] - start[d]});
}

void World::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
  const auto t0 = std::chrono::steady_clock::now();
  if (have_renderer_ && cmd && pass) {
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    renderer_.draw(cmd, pass, frame_ranges_, have_camera_ ? view_proj_ : identity);
    field_renderer_.draw(cmd, pass, field_ranges_, have_camera_ ? view_proj_ : identity, static_cast<float>(clock_));
  }
  times_.draw_ms = ms_since(t0);
}

Counters World::counters() const {
  Counters c = counters_;
  for (const Slot &s : slots_) {
    if (!s.alive) continue;
    c.bodies += s.bodies.size();
    ++c.instances;
    if (s.awake) ++c.awake;
    if (s.field) ++c.fields;
    else if (!s.has_current) ++c.waiting;
    if (!s.error.empty()) ++c.failed;
  }
  for (const auto &[key, e] : cache_) {
    ++c.cache_entries;
    if (e.pending) ++c.cache_pending;
    if (e.build) c.cache_bytes += e.build->bytes;
    c.cache_gpu_bytes += e.gpu.bytes;
  }
  if (have_renderer_) {
    c.gpu_buffers_created = renderer_.buffers_created();
    c.gpu_buffers_released = renderer_.buffers_released();
    c.gpu_bytes = renderer_.gpu_bytes();
  }
  return c;
}

std::vector<StageTotals> World::stage_totals() const {
  std::vector<StageTotals> out;
  for (const auto &[label, t] : stages_) out.push_back(t);
  return out;
}

Status World::info(uint64_t handle, InstanceInfo &out) const {
  const Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  out = {};
  out.sleeping = !s->awake;
  out.failed = !s->error.empty();
  out.error = s->error;
  if (s->field) {
    out.ready = true;
    out.drawn = s->field->drawn_tiles > 0;
    out.level = s->field->level_drawn;
    out.drawn_triangles = static_cast<uint32_t>(std::min<uint64_t>(s->field->drawn_triangles, 0xffffffffu));
    return Status::ok;
  }
  out.build_key = s->current;
  out.drawn = s->drawn;
  out.level = s->drawn_level;
  out.pixels = s->pixels;
  if (s->drawn) {
    Entry *e = s->drawn_level < 4 && s->lod_entries[s->drawn_level] ? s->lod_entries[s->drawn_level] : s->entry;
    if (e && e->build) out.drawn_triangles = e->build->triangles;
  }
  if (s->has_current) {
    const Entry &e = cache_.find(s->current)->second;
    out.ready = e.build != nullptr;
    if (e.build) {
      out.triangles = e.build->triangles;
      out.bones = e.build->bones;
      out.height = e.build->height;
      out.radius = e.build->radius;
      if (!e.build->problem.empty() && out.error.empty()) {
        out.failed = true;
        out.error = e.build->problem;
      }
    }
  }
  out.settled = s->pose && s->pose->settled;
  return Status::ok;
}

Status World::hash(uint64_t handle, const float *clocks, size_t count, uint64_t &model, uint64_t &pose) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  if (!s->has_current) return Status::failed;
  const Entry &e = cache_.find(s->current)->second;
  if (!e.build) return Status::failed;
  model = e.build->model_hash;
  pose = 0;
  if (!s->type->has_update()) return Status::ok;
  if (!clocks) {
    pose = s->pose ? s->type->pose_hash(*s->pose) : 0;
    return Status::ok;
  }
  if (count != 1 + s->type->event_count()) return Status::bad_parameter;
  auto scratch = s->type->bind(*e.build);
  std::string err;
  if (!s->type->update(*e.build, *scratch, s->params.data(), clocks, err)) return Status::failed;
  pose = s->type->pose_hash(*scratch);
  return Status::ok;
}

uint64_t World::bad_poses() const {
  uint64_t bad = 0;
  for (const Slot &s : slots_) {
    if (!s.alive || !s.has_current || !s.pose) continue;
    const Entry &e = cache_.find(s.current)->second;
    if (!e.build) continue;
    const float limit = 3 * (e.build->height + e.build->radius) + 5;
    const auto &m = s.pose->matrices;
    bool ok = true;
    for (size_t i = 0; i < m.size() && ok; ++i) ok = std::isfinite(m[i]);
    for (size_t b = 0; ok && (b + 1) * 16 <= m.size(); ++b) {
      const float *t = m.data() + b * 16 + 12;
      ok = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) < limit;
    }
    if (!ok) ++bad;
  }
  return bad;
}

}  // namespace runtime
