// What the library's implementation shares: the runtime behind the C handle.
// Never included by runtime.h.
#pragma once

#include "core/world.h"
#include "runtime.h"

struct foundry_runtime {
  explicit foundry_runtime(const runtime::Config &c, std::string &error) : world(c, error) {}
  runtime::World world;
  foundry_quality quality = FOUNDRY_QUALITY_HIGH;
};

inline foundry_status foundry_status_of(runtime::Status s) { return static_cast<foundry_status>(s); }
