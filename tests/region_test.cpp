// Regions as a game uses them (design/plotter.md, design/api.md "Regions"): the
// happy glen's ground, grown to full in batches, every placement spawned into a
// Jolt world, part of it cleared and regrown, a save restored (and an invalid one
// refused), and the ground shrunk. Prints the counts the plotter's book quotes.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "runtime.h"
#include "runtime_diag.h"

namespace {

int failures = 0;
#define CHECK(c, msg)                          \
  do {                                         \
    if (!(c)) {                                \
      std::printf("FAIL: %s (%s)\n", msg, #c); \
      ++failures;                              \
    }                                          \
  } while (0)

const JPH::BroadPhaseLayer kOnly(0);
class BroadPhase final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return kOnly; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "only"; }
#endif
};
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return false; }
};
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return false; }
};

// The foundry's "glen" ground preset at radius 12 (src/host/plot/view.cpp).
std::vector<foundry_hex_region> glen_ground() {
  std::vector<foundry_hex_region> g;
  foundry_hex_region soil = {};
  soil.shape = FOUNDRY_HEX_HEX;
  soil.radius = 12;
  soil.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE | FOUNDRY_SET_MOISTURE;
  soil.ground = FOUNDRY_GROUND_SOIL;
  soil.available = true;
  soil.moisture = 0.55f;
  g.push_back(soil);
  foundry_hex_region pond = {};
  pond.shape = FOUNDRY_HEX_HEX;
  pond.q = 4;
  pond.r = -3;
  pond.radius = 3;
  pond.priority = 1;
  pond.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_MOISTURE;
  pond.ground = FOUNDRY_GROUND_WATER;
  pond.moisture = 1.0f;
  g.push_back(pond);
  foundry_hex_region path = {};
  path.shape = FOUNDRY_HEX_LINE;
  path.q = -12;
  path.r = 6;
  path.q2 = 12;
  path.r2 = -6;
  path.priority = 2;
  path.set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE | FOUNDRY_SET_KEEP_CLEAR;
  path.ground = FOUNDRY_GROUND_PATH;
  path.available = false;
  path.keep_clear_m = 1.2f;
  g.push_back(path);
  for (int k = 3; k >= 1; --k) {
    foundry_hex_region rise = {};
    rise.shape = FOUNDRY_HEX_HEX;
    rise.q = -4;
    rise.r = 4;
    rise.radius = static_cast<uint32_t>(12 * k / 6);
    rise.set = FOUNDRY_SET_HEIGHT;
    rise.height = 0.25f * static_cast<float>(4 - k);
    g.push_back(rise);
  }
  return g;
}

}  // namespace

int main() {
  JPH::RegisterDefaultAllocator();
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
  BroadPhase broad;
  ObjectVsBroadPhase obp;
  ObjectPairs pairs;
  JPH::PhysicsSystem ps;
  ps.Init(65536, 0, 1024, 1024, broad, obp, pairs);
  foundry_physics phys = {};
  phys.tag = FOUNDRY_PHYSICS_JOLT;
  {
    using namespace JPH;
    phys.jolt_version = JPH_VERSION_ID;
  }
  phys.system = &ps;
  foundry_runtime_desc desc = {};
  desc.physics = &phys;
  foundry_runtime *rt = foundry_runtime_create(&desc);
  if (!rt) {
    std::printf("FAIL: runtime: %s\n", foundry_last_error());
    return 1;
  }

  const auto ground = glen_ground();
  foundry_region_desc rd = {};
  rd.plotter = FOUNDRY_PLOTTER_HAPPY_GLEN;
  rd.world_seed = 7;
  rd.regions = ground.data();
  rd.region_count = static_cast<uint32_t>(ground.size());
  foundry_region glen = foundry_region_create(rt, &rd);
  CHECK(glen != FOUNDRY_NO_REGION, "the glen region is created");

  // Grow to full in batches of 32, as a game would over frames.
  std::vector<foundry_placement> placed;
  std::vector<foundry_instance> handles;
  std::map<std::string, int> by_kind;
  uint32_t volume = 1, calls = 0;
  double grow_ms = 0, worst_ms = 0;
  while (volume > 0) {
    foundry_placement batch[32];
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t n = foundry_region_grow(rt, glen, FOUNDRY_GROW_SEED, nullptr, 0, nullptr, batch, 32, &volume);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    grow_ms += ms;
    worst_ms = ms > worst_ms ? ms : worst_ms;
    ++calls;
    if (n == 0) break;
    for (uint32_t i = 0; i < n; ++i) {
      placed.push_back(batch[i]);
      handles.push_back(foundry_placement_spawn(rt, glen, &batch[i]));
      ++by_kind[foundry_plotter_kind_name(FOUNDRY_PLOTTER_HAPPY_GLEN, batch[i].kind)];
    }
  }
  std::printf("grew %zu placements in %u calls (%.2f ms total, %.2f ms worst)\n", placed.size(), calls, grow_ms, worst_ms);
  for (const auto &[k, n] : by_kind) std::printf("  %s: %d\n", k.c_str(), n);
  CHECK(placed.size() == 134, "the same 134 placements the foundry grows for this ground and seed");
  CHECK(foundry_region_count(rt, glen) == placed.size(), "the region counts what it placed");
  for (foundry_instance h : handles) CHECK(h != FOUNDRY_NO_INSTANCE, "every placement spawns");

  // Builds finish; every instance with collision has its body.
  for (int i = 0; i < 3000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    foundry_update(rt, 1.0f / 60.0f);
    foundry_diag_counters c;
    foundry_diag_counters_get(rt, &c);
    if (c.waiting == 0) break;
  }
  foundry_diag_counters c;
  foundry_diag_counters_get(rt, &c);
  std::printf("%llu instances, %llu builds cached, %llu bodies (the world has %u)\n", (unsigned long long)c.instances,
              (unsigned long long)c.cache_entries, (unsigned long long)c.bodies, ps.GetNumBodies());
  CHECK(c.waiting == 0 && c.failed == 0, "every instance built");
  CHECK(c.bodies == placed.size() && ps.GetNumBodies() == placed.size(), "one collision body per placement");

  // A clearing: everything within 6 m of the middle goes (destroyed by the game, vacated in the region).
  const float centre[3] = {0, 0, 0};
  foundry_slot gone[256];
  const uint32_t found = foundry_region_find(rt, glen, centre, 6.0f, gone, 256);
  for (uint32_t i = 0; i < found; ++i)
    for (size_t k = 0; k < placed.size(); ++k)
      if (placed[k].slot.q == gone[i].q && placed[k].slot.r == gone[i].r && placed[k].slot.layer == gone[i].layer &&
          placed[k].slot.index == gone[i].index && handles[k]) {
        foundry_destroy(rt, handles[k]);
        handles[k] = FOUNDRY_NO_INSTANCE;
      }
  foundry_region_vacate(rt, glen, gone, found);
  CHECK(foundry_region_count(rt, glen) == placed.size() - found, "vacated slots are free");
  // Regrowth fills the clearing back (young), and the region is full again.
  foundry_placement regrow[256];
  uint32_t left = 0;
  const uint32_t back = foundry_region_grow(rt, glen, FOUNDRY_GROW_REGROW, nullptr, 0, nullptr, regrow, 256, &left);
  uint32_t regrown = 0;
  for (uint32_t i = 0; i < back; ++i) regrown += regrow[i].regrown ? 1 : 0;
  std::printf("cleared %u within 6 m; regrew %u (volume left %u)\n", found, back, left);
  CHECK(found > 0, "the clearing found things");
  CHECK(back > 0 && regrown == back && left == 0, "regrowth fills the clearing and flags it regrown");

  // A save: restoring into a fresh region works; one with overlapping footprints is refused.
  foundry_region again = foundry_region_create(rt, &rd);
  CHECK(foundry_region_restore(rt, again, placed.data(), static_cast<uint32_t>(placed.size())) == FOUNDRY_OK,
        "a save restores");
  uint32_t more = 0;
  foundry_placement extra[8];
  CHECK(foundry_region_grow(rt, again, FOUNDRY_GROW_SEED, nullptr, 0, nullptr, extra, 8, &more) == 0 && more == 0,
        "a restored full region has nothing to add");
  foundry_region broken = foundry_region_create(rt, &rd);
  std::vector<foundry_placement> doubled = placed;
  for (auto &p : doubled)
    if (p.footprint_cells) {
      doubled.push_back(p);
      doubled.back().slot.index += 100;  // a second thing on the same footprint
      break;
    }
  CHECK(foundry_region_restore(rt, broken, doubled.data(), static_cast<uint32_t>(doubled.size())) == FOUNDRY_STATUS_FAILED,
        "an invalid save is refused");

  // The ground shrinks to a radius of 6: the slots whose footprints left are reported.
  std::vector<foundry_hex_region> small = ground;
  small[0].radius = 6;
  foundry_slot lost[512];
  const uint32_t lost_n = foundry_region_set_regions(rt, again, small.data(), static_cast<uint32_t>(small.size()), lost, 512);
  std::printf("shrinking the ground lost %u of %zu\n", lost_n, placed.size());
  CHECK(lost_n > 0 && foundry_region_count(rt, again) == placed.size() - lost_n, "lost slots are reported and forgotten");

  foundry_region_destroy(rt, glen);
  foundry_region_destroy(rt, again);
  foundry_region_destroy(rt, broken);
  CHECK(foundry_region_count(rt, glen) == 0, "a destroyed region is stale");
  for (foundry_instance h : handles)
    if (h) foundry_destroy(rt, h);
  foundry_runtime_destroy(rt);
  CHECK(ps.GetNumBodies() == 0, "bodies balance");
  if (failures == 0) std::printf("region_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
