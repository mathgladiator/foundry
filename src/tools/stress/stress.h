/* stress.h: what a generated stress binding hands the stress platform
 * (design/quality.perf.md). A binding is a table of its pipeline's parameters
 * and three thunks onto the pipeline's typed API; everything else (plans,
 * phases, measurement, reports) lives in the platform. */
#pragma once

#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum stress_kind {
  STRESS_ENUM,  /* an enum or a config entry: values[0..count) */
  STRESS_U32,
  STRESS_F32,
  STRESS_CLOCK, /* the instance's time: the runtime runs it */
  STRESS_EVENT  /* an event clock: started by trigger() */
} stress_kind;

typedef enum stress_phase {
  STRESS_BUILD, /* changing it builds something new (cached and shared) */
  STRESS_UPDATE /* only update stages read it: it moves the pose */
} stress_phase;

typedef union stress_value {
  uint32_t u;
  float f;
} stress_value;

typedef struct stress_param {
  const char *name;
  stress_kind kind;
  stress_phase phase;
  const char *const *values; /* STRESS_ENUM */
  uint32_t count;
  double min, max; /* STRESS_U32, STRESS_F32 */
  stress_value fallback;
  /* A gate: the parameter applies only where parameter `when` (a config entry) has
   * when_on[value] set; elsewhere it reads as off (value 0). -1 when ungated. */
  int32_t when;
  const unsigned char *when_on;
} stress_param;

typedef struct stress_pipeline {
  const char *name;  /* e.g. tree_v1 */
  const char *state; /* draft or final */
  const stress_param *params;
  uint32_t param_count;
  const char *const *systems; /* each stage's system, in order */
  uint32_t system_count;
  /* values has one entry per parameter (clocks and events are ignored). */
  foundry_instance (*spawn)(foundry_runtime *rt, const stress_value *values, const float transform[16]);
  foundry_status (*set)(foundry_runtime *rt, foundry_instance instance, uint32_t param, stress_value value);
  foundry_status (*trigger)(foundry_runtime *rt, foundry_instance instance, uint32_t param);
} stress_pipeline;

int stress_main(int argc, char **argv, const stress_pipeline *const *pipelines, uint32_t count);

#ifdef __cplusplus
}
#endif
