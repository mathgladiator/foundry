// fire.h: the fire system's C API over the runtime (design/submit.md, "Fire"). The configs are
// compiled in (fires.cpp, generated from config/fire_configs/).
#include <algorithm>

#include "fires.h"
#include "internal.h"

extern "C" {

void foundry_set_wind(foundry_runtime *rt, float speed, float heading_deg) { rt->world.set_wind(std::max(speed, 0.0f), heading_deg); }

foundry_status foundry_set_burning(foundry_runtime *rt, foundry_instance instance, float amount) {
  return foundry_status_of(rt->world.set_burning(instance, amount));
}

foundry_status foundry_get_burning(foundry_runtime *rt, foundry_instance instance, float *amount) {
  float a = 0;
  const runtime::Status s = rt->world.burning(instance, a);
  if (amount) *amount = a;
  return foundry_status_of(s);
}

uint32_t foundry_fire_config_count(void) { return static_cast<uint32_t>(fires::configs().size()); }

const char *foundry_fire_config_name(uint32_t index, uint32_t *id) {
  const auto &c = fires::configs();
  if (index >= c.size()) return nullptr;
  if (id) *id = c[index].id;
  return fires::names()[index];
}

}  // extern "C"
