#include "physics.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <mutex>

namespace stress {

namespace {

// Two broad phase layers: what never moves (everything the runtime adds) and what does.
const JPH::BroadPhaseLayer kStatic(0), kMoving(1);

class BroadPhase final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == kToyMoving ? kMoving : kStatic;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer == kMoving ? "moving" : "static";
  }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
    return layer == kToyMoving || bp == kMoving;  // static never tests static
  }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (a != kToyMoving && b != kToyMoving) return false;
    const JPH::ObjectLayer other = a == kToyMoving ? b : a;
    return other != kToyQuery;  // queries are found by rays only
  }
};

std::once_flag g_jolt_once;

}  // namespace

struct ToyPhysics {
  BroadPhase broad;
  ObjectVsBroadPhase object_vs_broad;
  ObjectPairs pairs;
  JPH::PhysicsSystem system;
  foundry_physics desc{};
};

ToyPhysics *toy_physics_create(uint32_t max_bodies) {
  std::call_once(g_jolt_once, [] {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  });
  auto *p = new ToyPhysics();
  p->system.Init(max_bodies, 0, max_bodies, max_bodies / 4 + 1024, p->broad, p->object_vs_broad, p->pairs);
  p->desc.tag = FOUNDRY_PHYSICS_JOLT;
  {
    using namespace JPH;  // the macro names JPH's integer types unqualified
    p->desc.jolt_version = JPH_VERSION_ID;
  }
  p->desc.system = &p->system;
  p->desc.layers[FOUNDRY_PURPOSE_SOLID] = kToySolid;
  p->desc.layers[FOUNDRY_PURPOSE_SOFT] = kToySoft;
  p->desc.layers[FOUNDRY_PURPOSE_QUERY] = kToyQuery;
  p->desc.layers[FOUNDRY_PURPOSE_SENSOR] = kToySensor;
  return p;
}

void toy_physics_destroy(ToyPhysics *p) { delete p; }

const foundry_physics *toy_physics_desc(ToyPhysics *p) { return &p->desc; }

uint32_t toy_physics_bodies(ToyPhysics *p) { return p->system.GetNumBodies(); }

void toy_physics_settle(ToyPhysics *p) { p->system.OptimizeBroadPhase(); }

}  // namespace stress
