#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>

#include "fires.h"
#include "internal.h"
#include "pipelines_internal.h"
#include "runtime_diag.h"

namespace {
thread_local std::string g_last_error;
}

extern "C" {

foundry_runtime *foundry_runtime_create(const foundry_runtime_desc *desc) {
  runtime::Config c;
  if (desc) {
    c.device = desc->device;
    if (desc->color_format != SDL_GPU_TEXTUREFORMAT_INVALID) c.color_format = desc->color_format;
    if (desc->depth_format != SDL_GPU_TEXTUREFORMAT_INVALID) c.depth_format = desc->depth_format;
    c.update_threads = desc->update_threads;
    c.build_threads = desc->build_threads;
    if (desc->physics) {
      if (desc->physics->tag != FOUNDRY_PHYSICS_JOLT) {
        g_last_error = "the physics pointer isn't tagged as Jolt (FOUNDRY_PHYSICS_JOLT)";
        return nullptr;
      }
      c.physics = desc->physics->system;
      c.physics_version = desc->physics->jolt_version;
      for (int i = 0; i < FOUNDRY_PURPOSE_COUNT; ++i) c.physics_layers[i] = desc->physics->layers[i];
      if (!c.physics) {
        g_last_error = "the physics pointer is null";
        return nullptr;
      }
    }
  }
  std::string error;
  auto *rt = new foundry_runtime(c, error);
  if (!rt->world.ok()) {
    g_last_error = error;
    delete rt;
    return nullptr;
  }
  rt->world.set_fire_configs(fires::configs());
  if (desc) {
    rt->quality = desc->quality;
    rt->world.set_quality(static_cast<int>(desc->quality));
  }
  return rt;
}

void foundry_runtime_destroy(foundry_runtime *rt) { delete rt; }

const char *foundry_last_error(void) { return g_last_error.c_str(); }

void foundry_update(foundry_runtime *rt, float dt) { rt->world.update(dt); }
void foundry_prepare(foundry_runtime *rt, SDL_GPUCommandBuffer *cmd) { rt->world.prepare(cmd); }
void foundry_draw(foundry_runtime *rt, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
  rt->world.draw(cmd, pass);
}

void foundry_set_camera(foundry_runtime *rt, const float view[16], const float projection[16]) {
  rt->world.set_camera(view, projection);
}

void foundry_set_viewport(foundry_runtime *rt, uint32_t width, uint32_t height) { rt->world.set_viewport(width, height); }

void foundry_set_quality(foundry_runtime *rt, foundry_quality quality) {
  rt->quality = quality;
  rt->world.set_quality(static_cast<int>(quality));
}

void foundry_look_default(foundry_look *look) {
  if (!look) return;
  const runtime::Look d;
  look->vision = FOUNDRY_VISION_TYPICAL;
  look->contrast = d.contrast;
  look->saturation = d.saturation;
  look->edges = d.edges;
  look->effect = FOUNDRY_LOOK_EFFECT_NONE;
  for (int i = 0; i < 3; ++i) look->tint[i] = d.tint[i];
  look->strength = d.strength;
}

void foundry_set_look(foundry_runtime *rt, const foundry_look *look) {
  runtime::Look l;
  if (look) {
    l.vision = std::clamp(static_cast<int>(look->vision), 0, 3);
    l.contrast = std::clamp(look->contrast, 0.0f, 1.0f);
    l.saturation = std::clamp(look->saturation, 0.0f, 3.0f);
    l.edges = std::clamp(look->edges, 0.0f, 1.0f);
    l.effect = std::clamp(static_cast<int>(look->effect), 0, 2);
    for (int i = 0; i < 3; ++i) l.tint[i] = std::clamp(look->tint[i], 0.0f, 1.0f);
    l.strength = std::clamp(look->strength, 0.0f, 1.0f);
  }
  rt->world.set_look(l);
}

void foundry_get_look(foundry_runtime *rt, foundry_look *look) {
  if (!look) return;
  const runtime::Look &l = rt->world.look();
  look->vision = static_cast<foundry_vision>(l.vision);
  look->contrast = l.contrast;
  look->saturation = l.saturation;
  look->edges = l.edges;
  look->effect = static_cast<foundry_look_effect>(l.effect);
  for (int i = 0; i < 3; ++i) look->tint[i] = l.tint[i];
  look->strength = l.strength;
}

foundry_status foundry_set_transform(foundry_runtime *rt, foundry_instance instance, const float transform[16]) {
  return foundry_status_of(rt->world.set_transform(instance, transform));
}

foundry_status foundry_is_sleeping(foundry_runtime *rt, foundry_instance instance, bool *sleeping) {
  bool s = false;
  runtime::Status st = rt->world.sleeping(instance, s);
  if (sleeping) *sleeping = s;
  return foundry_status_of(st);
}

uint32_t foundry_changed(foundry_runtime *rt, const foundry_instance **instances) {
  const auto &c = rt->world.changed();
  if (instances) *instances = c.empty() ? nullptr : c.data();
  return static_cast<uint32_t>(c.size());
}

foundry_status foundry_destroy(foundry_runtime *rt, foundry_instance instance) {
  return foundry_status_of(rt->world.destroy(instance));
}

// Diagnostics (runtime_diag.h).

void foundry_diag_counters_get(foundry_runtime *rt, foundry_diag_counters *out) {
  runtime::Counters c = rt->world.counters();
  *out = {c.instances, c.awake, c.waiting, c.failed,
          c.cache_entries, c.cache_pending, c.cache_bytes, c.cache_gpu_bytes,
          c.cache_hits, c.cache_misses, c.cache_evictions,
          c.generations, c.generation_failures,
          c.gpu_buffers_created, c.gpu_buffers_released, c.gpu_bytes,
          c.draws, c.drawn, c.culled, c.triangles, c.bodies,
          c.too_small, {c.by_level[0], c.by_level[1], c.by_level[2], c.by_level[3]},
          c.animated, c.unseen, c.rate_skipped, c.bones_uploaded, c.lod_generations, c.fields, c.field_tiles,
          {c.field_tiles_by_level[0], c.field_tiles_by_level[1], c.field_tiles_by_level[2], c.field_tiles_by_level[3]},
          c.terrains, c.terrain_nodes, c.terrain_triangles};
}

void foundry_diag_frame_get(foundry_runtime *rt, foundry_diag_frame *out) {
  runtime::FrameTimes t = rt->world.frame_times();
  *out = {t.update_ms, t.prepare_ms, t.draw_ms, t.integrate_ms, t.classify_ms, t.animate_ms, t.results_ms};
}

uint32_t foundry_diag_stages(foundry_runtime *rt, foundry_diag_stage *out, uint32_t capacity) {
  auto stages = rt->world.stage_totals();
  for (uint32_t i = 0; i < capacity && i < stages.size(); ++i) {
    foundry_diag_stage &d = out[i];
    std::memset(&d, 0, sizeof d);
    std::strncpy(d.label, stages[i].label.c_str(), sizeof d.label - 1);
    d.update = stages[i].update;
    d.calls = stages[i].calls;
    d.total_ms = stages[i].total_ms;
    d.max_ms = stages[i].max_ms;
  }
  return static_cast<uint32_t>(stages.size());
}

foundry_status foundry_diag_instance_get(foundry_runtime *rt, foundry_instance instance, foundry_diag_instance *out) {
  runtime::InstanceInfo info;
  runtime::Status s = rt->world.info(instance, info);
  std::memset(out, 0, sizeof *out);
  if (s != runtime::Status::ok) return foundry_status_of(s);
  out->ready = info.ready;
  out->sleeping = info.sleeping;
  out->settled = info.settled;
  out->failed = info.failed;
  out->drawn = info.drawn;
  out->level = info.level;
  out->drawn_triangles = info.drawn_triangles;
  out->pixels = info.pixels;
  out->triangles = info.triangles;
  out->bones = info.bones;
  out->height = info.height;
  out->radius = info.radius;
  out->build_key = info.build_key;
  std::strncpy(out->error, info.error.c_str(), sizeof out->error - 1);
  return FOUNDRY_OK;
}

foundry_status foundry_diag_hash(foundry_runtime *rt, foundry_instance instance, const float *clocks,
                                 uint32_t clock_count, uint64_t *model, uint64_t *pose) {
  uint64_t m = 0, p = 0;
  runtime::Status s = rt->world.hash(instance, clocks, clock_count, m, p);
  if (model) *model = m;
  if (pose) *pose = p;
  return foundry_status_of(s);
}

uint64_t foundry_diag_bad_poses(foundry_runtime *rt) { return rt->world.bad_poses(); }

void foundry_diag_flush(foundry_runtime *rt) { rt->world.flush(); }

void foundry_diag_set_cost(foundry_runtime *rt, uint32_t flags) { rt->world.set_cost(flags & runtime::kCostAll); }

foundry_status foundry_diag_set_param(foundry_runtime *rt, foundry_instance instance, const char *name, const char *value) {
  return foundry_status_of(rt->world.set_named(instance, name, value));
}

foundry_instance foundry_diag_spawn(foundry_runtime *rt, const char *pipeline, const char *const *names, const char *const *values,
                                    uint32_t count, const float transform[16]) {
  const pipelines::Entry *e = pipelines::find(pipeline);
  if (!rt || !e) return FOUNDRY_NO_INSTANCE;
  const runtime::PipelineType *type = e->type();
  std::vector<uint8_t> params(type->params_size(), 0);
  e->defaults(params.data());
  size_t n = 0;
  const runtime::ParamInfo *table = type->param_table(n);
  for (uint32_t a = 0; a < count; ++a) {
    const runtime::ParamInfo *p = nullptr;
    for (size_t i = 0; i < n; ++i)
      if (std::strcmp(table[i].name, names[a]) == 0) p = &table[i];
    if (!p) return FOUNDRY_NO_INSTANCE;
    uint8_t *at = params.data() + p->offset;
    switch (p->kind) {
      case runtime::ParamInfo::kEnum: {
        uint32_t v = p->count;
        for (uint32_t k = 0; k < p->count; ++k)
          if (std::strcmp(p->values[k], values[a]) == 0) v = k;
        if (v == p->count) return FOUNDRY_NO_INSTANCE;
        std::memcpy(at, &v, sizeof v);
        break;
      }
      case runtime::ParamInfo::kU32: {
        const uint32_t v = static_cast<uint32_t>(std::clamp(std::strtod(values[a], nullptr), p->min, p->max));
        std::memcpy(at, &v, sizeof v);
        break;
      }
      case runtime::ParamInfo::kF32: {
        const float v = static_cast<float>(std::clamp(std::strtod(values[a], nullptr), p->min, p->max));
        std::memcpy(at, &v, sizeof v);
        break;
      }
      case runtime::ParamInfo::kBool: {
        const bool v = std::strcmp(values[a], "true") == 0 || std::strcmp(values[a], "1") == 0;
        std::memcpy(at, &v, sizeof v);
        break;
      }
    }
  }
  return rt->world.spawn(type, params.data(), transform);
}

foundry_status foundry_diag_force_level(foundry_runtime *rt, foundry_instance instance, int level) {
  return foundry_status_of(rt->world.force_level(instance, level));
}

}  // extern "C"
