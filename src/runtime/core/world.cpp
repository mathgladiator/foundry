#include "core/world.h"

#include "physics/jolt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace runtime {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t make_handle(uint32_t index, uint32_t generation) { return (uint64_t(generation) << 32) | (index + 1); }

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
    have_renderer_ = true;
  }
  ok_ = true;
}

World::~World() {
  builders_.reset();  // finish generation jobs before anything they touch goes away
  for (Slot &s : slots_)
    if (s.alive) set_bodies(s, nullptr);
  for (auto &[key, e] : cache_)
    if (e.uploaded) renderer_.release(e.gpu);
  if (have_renderer_) renderer_.shutdown();
}

World::Slot *World::slot(uint64_t handle) {
  uint32_t index = static_cast<uint32_t>(handle & 0xffffffffu), generation = static_cast<uint32_t>(handle >> 32);
  if (index == 0 || index > slots_.size()) return nullptr;
  Slot &s = slots_[index - 1];
  return s.alive && s.generation == generation ? &s : nullptr;
}

const World::Slot *World::slot(uint64_t handle) const { return const_cast<World *>(this)->slot(handle); }

void World::want(Slot &s) {
  const uint64_t key = s.type->build_key(s.params.data());
  if (key == s.wanted) return;
  if (s.wanted && s.wanted != s.current) unref(s.wanted);
  s.wanted = key;
  if (s.has_current && key == s.current) return;
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    ++it->second.refs;
    ++counters_.cache_hits;
    return;
  }
  ++counters_.cache_misses;
  Entry &e = cache_[key];
  e.type = s.type;
  e.refs = 1;
  e.order = next_order_++;
  auto params = std::make_shared<std::vector<uint8_t>>(s.params);
  const PipelineType *type = s.type;
  builders_->push([this, type, params, key] {
    Finished f;
    f.key = key;
    f.build = type->build(params->data(), f.error);
    std::lock_guard<std::mutex> lock(finished_mutex_);
    finished_.push_back(std::move(f));
  });
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
  want(s);
  return make_handle(index, s.generation);
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
  if (build) {
    s->error.clear();
    want(*s);
  }
  s->awake = true;  // setting a parameter wakes the instance (api.md)
  return Status::ok;
}

Status World::trigger(uint64_t handle, const PipelineType *type, size_t event) {
  Slot *s = slot(handle);
  if (!s) return Status::stale_handle;
  if (s->type != type) return Status::wrong_pipeline;
  if (event >= s->fired.size()) return Status::bad_parameter;
  s->fired[event] = s->time;
  s->awake = true;
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
  have_camera_ = true;
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
  for (Slot &s : slots_) {
    if (!s.alive || !s.wanted || (s.has_current && s.wanted == s.current)) continue;
    auto it = cache_.find(s.wanted);
    if (it == cache_.end() || it->second.pending) continue;
    if (it->second.failed) {
      s.error = it->second.error;
      continue;
    }
    if (s.has_current) unref(s.current);
    s.current = s.wanted;
    s.has_current = true;
    s.pose = s.type->bind(*it->second.build);
    s.awake = true;
    set_bodies(s, &it->second);  // the new build's collision replaces the old (a stump for a trunk)
  }
}

void World::evict(std::map<uint64_t, Entry>::iterator it) {
  if (it->second.uploaded) renderer_.release(it->second.gpu);
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

void World::update(float dt) {
  const auto t0 = std::chrono::steady_clock::now();
  clock_ += dt;
  integrate();
  std::vector<Slot *> active;
  active.reserve(slots_.size());
  for (Slot &s : slots_) {
    if (!s.alive || !s.awake) continue;
    s.time += dt;
    if (s.has_current && s.type->has_update() && s.pose) active.push_back(&s);
    else if (s.has_current && !s.type->has_update()) s.awake = false;  // nothing moves: sleep
  }
  pool_->parallel_for(active.size(), 16, [&](size_t b, size_t e) {
    std::vector<float> c;
    for (size_t i = b; i < e; ++i) {
      Slot &s = *active[i];
      const Entry &entry = cache_.find(s.current)->second;
      clocks(s, c);
      std::string err;
      if (!s.type->update(*entry.build, *s.pose, s.params.data(), c.data(), err)) {
        s.error = err;
        s.awake = false;
      } else if ((s.posed_at = s.time, s.pose->settled)) {
        s.awake = false;  // every update stage voted to sleep (design/submit.md)
      }
    }
  });
  for (Slot *s : active) {
    const auto &stages = s->type->stages();
    size_t u = 0;
    for (const StageInfo &st : stages) {
      if (!st.update) continue;
      if (u < s->pose->stage_ms.size()) {
        StageTotals &t = stages_[st.label];
        t.label = st.label;
        t.update = true;
        ++t.calls;
        t.total_ms += s->pose->stage_ms[u];
        t.max_ms = std::max(t.max_ms, s->pose->stage_ms[u]);
      }
      ++u;
    }
  }
  // Results: list the instances whose current values changed (the game diffs them for edges).
  changed_.clear();
  for (size_t i = 0; i < slots_.size(); ++i) {
    Slot &s = slots_[i];
    if (!s.alive) continue;
    const uint32_t bits = result_of(s);
    if (bits != s.result) {
      s.result = bits;
      changed_.push_back(make_handle(static_cast<uint32_t>(i), s.generation));
    }
  }
  release_idle(false);
  times_.update_ms = ms_since(t0);
}

void World::prepare(SDL_GPUCommandBuffer *cmd) {
  const auto t0 = std::chrono::steady_clock::now();
  SDL_GPUCopyPass *copy = have_renderer_ && cmd ? SDL_BeginGPUCopyPass(cmd) : nullptr;
  if (copy) {
    for (auto &[key, e] : cache_) {
      if (e.pending || e.failed || e.uploaded || !e.build) continue;
      e.gpu = renderer_.upload(copy, e.build->mesh);
      e.uploaded = true;
      e.build->mesh.clear();
    }
  }

  // Cull against the camera's frustum (Gribb and Hartmann's planes).
  float planes[6][4];
  if (have_camera_) {
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
  }
  struct Visible {
    uint64_t key;
    Slot *slot;
    const Entry *entry;
  };
  std::vector<Visible> visible;
  visible.reserve(slots_.size());
  uint64_t culled = 0;
  for (Slot &s : slots_) {
    if (!s.alive || !s.has_current) continue;
    const Entry &e = cache_.find(s.current)->second;
    if (!e.build) continue;
    if (have_camera_) {
      const float *r = s.rows;
      const float cy = e.build->height * 0.5f;
      const float c[3] = {r[1] * cy + r[3], r[5] * cy + r[7], r[9] * cy + r[11]};
      float scale = std::sqrt(std::max({r[0] * r[0] + r[4] * r[4] + r[8] * r[8], r[1] * r[1] + r[5] * r[5] + r[9] * r[9],
                                        r[2] * r[2] + r[6] * r[6] + r[10] * r[10]}));
      const float radius = e.build->radius * scale;
      bool in = true;
      for (const auto &p : planes)
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -radius) {
          in = false;
          break;
        }
      if (!in) {
        ++culled;
        continue;
      }
    }
    visible.push_back({s.current, &s, &e});
  }
  std::stable_sort(visible.begin(), visible.end(), [](const Visible &a, const Visible &b) { return a.key < b.key; });

  frame_instances_.resize(visible.size());
  std::vector<uint32_t> offsets(visible.size());
  uint32_t total_bones = 0;
  for (size_t i = 0; i < visible.size(); ++i) {
    offsets[i] = total_bones;
    total_bones += visible[i].entry->build->bones;
  }
  frame_bones_.resize(size_t(total_bones) * 12);
  uint64_t triangles = 0;
  pool_->parallel_for(visible.size(), 64, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      const Visible &v = visible[i];
      SkinnedInstance &inst = frame_instances_[i];
      std::memcpy(inst.row0, v.slot->rows, 16);
      std::memcpy(inst.row1, v.slot->rows + 4, 16);
      std::memcpy(inst.row2, v.slot->rows + 8, 16);
      inst.bone_offset = offsets[i];
      float *out = frame_bones_.data() + size_t(offsets[i]) * 12;
      const uint32_t n = v.entry->build->bones;
      const std::vector<float> *m = v.slot->pose ? &v.slot->pose->matrices : nullptr;
      for (uint32_t bone = 0; bone < n; ++bone) {
        float *rows = out + bone * 12;
        if (m && m->size() >= size_t(bone + 1) * 16) {
          const float *mm = m->data() + size_t(bone) * 16;
          for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) rows[r * 4 + c] = mm[c * 4 + r];
        } else {
          const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
          std::memcpy(rows, id, sizeof id);
        }
      }
    }
  });
  frame_ranges_.clear();
  for (size_t i = 0; i < visible.size(); ++i) {
    triangles += visible[i].entry->build->triangles;
    if (i == 0 || visible[i].key != visible[i - 1].key)
      frame_ranges_.push_back({&visible[i].entry->gpu, static_cast<uint32_t>(i), 0});
    ++frame_ranges_.back().count;
  }
  if (copy) {
    renderer_.stream(copy, frame_instances_, frame_bones_);
    SDL_EndGPUCopyPass(copy);
  }
  counters_.draws = frame_ranges_.size();
  counters_.drawn = visible.size();
  counters_.culled = culled;
  counters_.triangles = triangles;
  times_.prepare_ms = ms_since(t0);
}

void World::draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
  const auto t0 = std::chrono::steady_clock::now();
  if (have_renderer_ && cmd && pass) {
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    renderer_.draw(cmd, pass, frame_ranges_, have_camera_ ? view_proj_ : identity);
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
    if (!s.has_current) ++c.waiting;
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
  out.build_key = s->current;
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
