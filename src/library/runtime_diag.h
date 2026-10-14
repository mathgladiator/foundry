/* runtime_diag.h: diagnostics for the stress tool and development builds
 * (design/quality.perf.md). Never part of runtime.h: these name stages, which
 * a game must never see. Compiled in with FOUNDRY_DIAGNOSTICS. */
#pragma once

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct foundry_diag_counters {
  uint64_t instances, awake, waiting, failed;
  uint64_t cache_entries, cache_pending, cache_bytes, cache_gpu_bytes;
  uint64_t cache_hits, cache_misses, cache_evictions;
  uint64_t generations, generation_failures;
  uint64_t gpu_buffers_created, gpu_buffers_released, gpu_bytes;
  uint64_t draws, drawn, culled, triangles; /* the last prepared frame */
  uint64_t bodies;                          /* collision bodies in the game's physics world */
  /* The last frame's cost decisions (design/submit.md, "Cost"). */
  uint64_t too_small;                       /* in view, too small to draw */
  uint64_t by_level[4];                     /* drawn at each level of detail, finest first */
  uint64_t animated, unseen, rate_skipped;  /* awake instances posed; not posed: out of view, by rate */
  uint64_t bones_uploaded;                  /* bones whose changed pose went to the GPU */
  uint64_t lod_generations;                 /* of the generations, the runtime's coarser levels of detail */
  uint64_t fields, field_tiles;             /* fields, and their tiles drawn in the last frame */
  uint64_t field_tiles_by_level[4];         /* those tiles, by level of detail */
  uint64_t terrains, terrain_nodes, terrain_triangles;  /* terrains drawn, their nodes and triangles (design/boundary.md) */
} foundry_diag_counters;

typedef struct foundry_diag_frame {
  double update_ms, prepare_ms, draw_ms; /* the last frame, on the CPU */
  /* inside update: finished builds, classification (view, size, detail), posing, results */
  double integrate_ms, classify_ms, animate_ms, results_ms;
} foundry_diag_frame;

typedef struct foundry_diag_stage {
  char label[64]; /* pipeline.stage, e.g. tree_v1.rig */
  bool update;
  uint64_t calls;
  double total_ms, max_ms;
} foundry_diag_stage;

typedef struct foundry_diag_instance {
  bool ready, sleeping, settled, failed;
  bool drawn;               /* in the last prepared frame */
  uint32_t level;           /* the level of detail it was drawn at (finest is 0) */
  uint32_t drawn_triangles; /* at that level */
  float pixels;             /* its projected size on screen */
  uint32_t triangles, bones;
  float height, radius;
  uint64_t build_key;
  char error[256];
} foundry_diag_instance;

void foundry_diag_counters_get(foundry_runtime *rt, foundry_diag_counters *out);
void foundry_diag_frame_get(foundry_runtime *rt, foundry_diag_frame *out);
/* Stage totals since the runtime was created; returns how many there are. */
uint32_t foundry_diag_stages(foundry_runtime *rt, foundry_diag_stage *out, uint32_t capacity);
foundry_status foundry_diag_instance_get(foundry_runtime *rt, foundry_instance instance, foundry_diag_instance *out);
/* Hashes of the instance's packed draw output, and of its packed pose at the
 * given clocks ([0] = time, then each event's seconds since it fired, or -1;
 * NULL hashes the live pose), comparable with the foundry's WebAssembly outputs. */
foundry_status foundry_diag_hash(foundry_runtime *rt, foundry_instance instance, const float *clocks,
                                 uint32_t clock_count, uint64_t *model, uint64_t *pose);
/* Instances whose pose has a non-finite matrix or a bone far outside its bounds. */
uint64_t foundry_diag_bad_poses(foundry_runtime *rt);
/* Turns the runtime's cost decisions on and off, to measure each one (bits, all on by default):
 * pick detail by size on screen, skip what is too small, don't animate what is out of view,
 * animate small things less often, upload only the poses that changed. */
#define FOUNDRY_DIAG_COST_DETAIL 1u
#define FOUNDRY_DIAG_COST_TOO_SMALL 2u
#define FOUNDRY_DIAG_COST_UNSEEN 4u
#define FOUNDRY_DIAG_COST_RATE 8u
#define FOUNDRY_DIAG_COST_RETAIN 16u /* keep poses on the GPU; off uploads every drawn pose each frame */
#define FOUNDRY_DIAG_COST_ALL 31u
void foundry_diag_set_cost(foundry_runtime *rt, uint32_t flags);
/* Draws an instance at one level of detail whatever its size: its pipeline's levels (finest is 0),
 * then the runtime's far level (one past the coarsest); -1 lets its size pick again. Needs
 * FOUNDRY_DIAG_COST_DETAIL on. */
foundry_status foundry_diag_force_level(foundry_runtime *rt, foundry_instance instance, int level);
/* Sets a parameter by its name, from text: an enum's value name (or index), a number, or true or
 * false, clamped to the parameter's range. FOUNDRY_STATUS_BAD_PARAMETER if the instance's pipeline
 * has no such parameter. The foundry uses it to share values (season, wind) across a region. */
foundry_status foundry_diag_set_param(foundry_runtime *rt, foundry_instance instance, const char *name, const char *value);
/* Releases every unused build now, without waiting for the release timer. */
void foundry_diag_flush(foundry_runtime *rt);

#ifdef __cplusplus
}
#endif
