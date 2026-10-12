// What the library's implementation shares: the runtime behind the C handle.
// Never included by runtime.h.
#pragma once

#include <memory>

#include "core/world.h"
#include "runtime.h"

namespace regions {
struct Store;  // the runtime's regions (region.cpp)
void destroy(Store *s);
}

struct foundry_runtime {
  explicit foundry_runtime(const runtime::Config &c, std::string &error) : world(c, error) {}
  ~foundry_runtime() { regions::destroy(regions); }
  runtime::World world;
  foundry_quality quality = FOUNDRY_QUALITY_HIGH;
  regions::Store *regions = nullptr;  // made on first use
};

inline foundry_status foundry_status_of(runtime::Status s) { return static_cast<foundry_status>(s); }
