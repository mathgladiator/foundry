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
} foundry_diag_counters;

typedef struct foundry_diag_frame {
  double update_ms, prepare_ms, draw_ms; /* the last frame, on the CPU */
} foundry_diag_frame;

typedef struct foundry_diag_stage {
  char label[64]; /* pipeline.stage, e.g. tree_v1.rig */
  bool update;
  uint64_t calls;
  double total_ms, max_ms;
} foundry_diag_stage;

typedef struct foundry_diag_instance {
  bool ready, sleeping, settled, failed;
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
/* Releases every unused build now, without waiting for the release timer. */
void foundry_diag_flush(foundry_runtime *rt);

#ifdef __cplusplus
}
#endif
