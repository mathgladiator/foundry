// The runtime as a game uses it (design/api.md): a tree in a Jolt world, felled,
// its fall reported through its update result and the changed list, then turned
// into a stump, whose collision body replaces the trunk's. Bodies balance at the end.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>

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
  bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};

bool wait(foundry_runtime *rt, foundry_instance h, bool (*done)(const foundry_tree_v1_result &), bool &listed) {
  listed = false;
  for (int i = 0; i < 4000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // builds run on their own thread
    foundry_update(rt, 1.0f / 60.0f);
    const foundry_instance *changed = nullptr;
    const uint32_t n = foundry_changed(rt, &changed);
    for (uint32_t k = 0; k < n; ++k) listed = listed || changed[k] == h;
    foundry_tree_v1_result r;
    if (foundry_tree_v1_get_result(rt, h, &r) == FOUNDRY_OK && done(r)) return true;
  }
  return false;
}

// The height of the body's bounds: the trunk is metres tall, the stump well under one.
float body_top(JPH::PhysicsSystem &ps) {
  JPH::BodyIDVector ids;
  ps.GetBodies(ids);
  if (ids.empty()) return -1;
  JPH::BodyLockRead lock(ps.GetBodyLockInterface(), ids[0]);
  return lock.Succeeded() ? lock.GetBody().GetWorldSpaceBounds().mMax.GetY() : -1;
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
  ps.Init(1024, 0, 1024, 1024, broad, obp, pairs);

  foundry_physics phys = {};
  phys.tag = FOUNDRY_PHYSICS_JOLT;
  {
    using namespace JPH;
    phys.jolt_version = JPH_VERSION_ID;
  }
  phys.system = &ps;
  foundry_runtime_desc desc = {};
  desc.physics = &phys;
  desc.update_threads = 1;
  desc.build_threads = 1;

  // A wrong version is refused, with a reason.
  {
    foundry_physics bad = phys;
    bad.jolt_version ^= 1;
    foundry_runtime_desc d = desc;
    d.physics = &bad;
    foundry_runtime *rt = foundry_runtime_create(&d);
    CHECK(rt == nullptr, "a mismatched Jolt version is refused");
    if (rt) foundry_runtime_destroy(rt);
  }

  foundry_runtime *rt = foundry_runtime_create(&desc);
  if (!rt) {
    std::printf("FAIL: runtime: %s\n", foundry_last_error());
    return 1;
  }
  foundry_tree_v1_params p;
  foundry_tree_v1_default_params(&p);
  p.species = FOUNDRY_TREE_V1_SPECIES_OAK;
  p.wind_speed = 0;
  const float at[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 0, 3, 1};
  foundry_instance tree = foundry_tree_v1_spawn(rt, &p, at);

  CHECK(tree != FOUNDRY_NO_INSTANCE, "the tree spawns");
  bool listed = false;
  CHECK(wait(rt, tree, [](const foundry_tree_v1_result &r) { return r.ready; }, listed), "the tree's build is ready");
  {
    foundry_diag_instance d;
    foundry_diag_instance_get(rt, tree, &d);
    if (d.failed || !d.ready) std::printf("instance: ready %d failed %d error %s\n", d.ready, d.failed, d.error);
  }
  CHECK(listed, "ready shows in the changed list");
  CHECK(ps.GetNumBodies() == 1, "the tree's trunk is one body in the world");
  const float trunk_top = body_top(ps);
  CHECK(trunk_top > 2.0f, "the trunk body is metres tall");

  foundry_tree_v1_start_break(rt, tree);
  foundry_tree_v1_result r;
  foundry_tree_v1_get_result(rt, tree, &r);
  CHECK(!r.break_over, "break_over is false as the fall starts");
  CHECK(wait(rt, tree, [](const foundry_tree_v1_result &r) { return r.break_over; }, listed), "the fall comes to rest");
  CHECK(listed, "break_over shows in the changed list");

  // The game declares it a stump; the rebuild swaps the trunk's body for the stump's.
  foundry_tree_v1_set_stump(rt, tree, true);
  CHECK(wait(rt, tree, [](const foundry_tree_v1_result &r) { return r.ready; }, listed), "the stump's build is ready");
  CHECK(ps.GetNumBodies() == 1, "still one body: the stump");
  const float stump_top = body_top(ps);
  CHECK(stump_top > 0 && stump_top < 1.0f, "the stump's body is under a metre");
  std::printf("trunk body top %.2f m, stump body top %.2f m\n", trunk_top, stump_top);

  foundry_diag_counters c;
  foundry_diag_counters_get(rt, &c);
  CHECK(c.bodies == 1, "the runtime counts one body");
  foundry_destroy(rt, tree);
  CHECK(ps.GetNumBodies() == 0, "destroying the tree removes its body");
  foundry_runtime_destroy(rt);
  CHECK(ps.GetNumBodies() == 0, "bodies balance");

  if (failures == 0) std::printf("results_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
