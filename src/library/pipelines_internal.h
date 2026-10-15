// Every pipeline compiled into the runtime, by name (tools/gen_pipelines.py writes
// src/library/pipelines.cpp): its type and its default parameters, so a pipeline can be spawned by
// name with parameters given as text (runtime_diag.h, foundry_diag_spawn). Never included by runtime.h.
#pragma once

#include "core/pipeline.h"

namespace pipelines {

struct Entry {
  const char *name;
  const runtime::PipelineType *(*type)();
  void (*defaults)(void *params);
};

// The pipeline of this name, or null.
const Entry *find(const char *name);

}  // namespace pipelines
