#include "physics/walker.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "physics/jolt.h"
#include "physics/toy.h"

namespace physics {

namespace {

// Layers: one per purpose (as the runtime is handed them), the walker's own ground, and the walker.
enum : uint16_t { kSolid = 0, kSoft = 1, kQuery = 2, kSensor = 3, kGround = 4, kMoving = 5, kLayers = 6 };
const JPH::BroadPhaseLayer kStaticBp(0), kMovingBp(1);

class BroadPhase final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == kMoving ? kMovingBp : kStaticBp;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer == kMovingBp ? "moving" : "static";
  }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
    return layer == kMoving || bp == kMovingBp;  // static never tests static
  }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (a != kMoving && b != kMoving) return false;
    const JPH::ObjectLayer other = a == kMoving ? b : a;
    return other == kSolid || other == kGround;  // soft bodies part, queries and sensors don't stop it
  }
};

constexpr float kGravity = -18.0f;  // a game's, heavier than Earth's so a jump doesn't float
constexpr float kWalk = 4.0f, kRun = 8.0f, kJump = 6.5f;

}  // namespace

struct WalkerWorld {
  BroadPhase broad;
  ObjectVsBroadPhase object_vs_broad;
  ObjectPairs pairs;
  JPH::PhysicsSystem system;
  JPH::TempAllocatorImpl temp{1 << 20};
  foundry_physics desc{};
  std::vector<JPH::BodyID> owned;  // the ground, floors, and models it added
  std::vector<JPH::BodyID> floors;  // of those, the wide floors
  JPH::Ref<JPH::CharacterVirtual> character;
  JPH::RVec3 spawn_at = JPH::RVec3::sZero();
  bool bodies_changed = false;
  JPH::uint optimized_bodies = 0;  // the world's body count when its broadphase was last optimized
};

Walker::Walker() {
  init_jolt();
  w_ = std::make_unique<WalkerWorld>();
  w_->system.Init(65536, 0, 4096, 4096, w_->broad, w_->object_vs_broad, w_->pairs);
  w_->desc.tag = FOUNDRY_PHYSICS_JOLT;
  w_->desc.jolt_version = runtime::physics::jolt_version();
  w_->desc.system = &w_->system;
  w_->desc.layers[FOUNDRY_PURPOSE_SOLID] = kSolid;
  w_->desc.layers[FOUNDRY_PURPOSE_SOFT] = kSoft;
  w_->desc.layers[FOUNDRY_PURPOSE_QUERY] = kQuery;
  w_->desc.layers[FOUNDRY_PURPOSE_SENSOR] = kSensor;
}

Walker::~Walker() {
  w_->character = nullptr;
  clear();
}

const foundry_physics *Walker::desc() const { return &w_->desc; }

void Walker::add_triangles(const std::vector<float> &xyz) {
  JPH::TriangleList tris;
  for (size_t i = 0; i + 8 < xyz.size(); i += 9) {
    const float *v = &xyz[i];
    // Skip slivers: Jolt's mesh drops them anyway, and warns.
    const float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2], bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
    const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    if (cx * cx + cy * cy + cz * cz < 1e-12f) continue;
    tris.push_back(JPH::Triangle(JPH::Float3(v[0], v[1], v[2]), JPH::Float3(v[3], v[4], v[5]), JPH::Float3(v[6], v[7], v[8])));
  }
  if (tris.empty()) return;
  JPH::MeshShapeSettings settings(tris);
  JPH::ShapeSettings::ShapeResult r = settings.Create();
  if (r.HasError()) return;
  JPH::BodyCreationSettings body(r.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kGround);
  const JPH::BodyID id = w_->system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
  if (!id.IsInvalid()) w_->owned.push_back(id);
  w_->bodies_changed = true;
}

void Walker::add_bodies(const std::vector<runtime::CollisionBody> &bodies, const float at[3], float yaw) {
  if (bodies.empty()) return;
  std::string error;
  auto shapes = runtime::physics::make_shapes(bodies, error);
  if (!shapes) return;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float rows[12] = {c, 0, s, at[0], 0, 1, 0, at[1], -s, 0, c, at[2]};
  for (uint32_t v : runtime::physics::add(&w_->system, *shapes, w_->desc.layers, rows, 0)) w_->owned.push_back(JPH::BodyID(v));
  w_->bodies_changed = true;
}

void Walker::add_floor(float y) {
  JPH::BodyCreationSettings body(new JPH::BoxShape(JPH::Vec3(4000, 0.5f, 4000)), JPH::RVec3(0, y - 0.5f, 0),
                                 JPH::Quat::sIdentity(), JPH::EMotionType::Static, kGround);
  const JPH::BodyID id = w_->system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
  if (!id.IsInvalid()) {
    w_->owned.push_back(id);
    w_->floors.push_back(id);
  }
  w_->bodies_changed = true;
}

void Walker::clear() {
  JPH::BodyInterface &bi = w_->system.GetBodyInterface();
  for (const JPH::BodyID &id : w_->owned) {
    bi.RemoveBody(id);
    bi.DestroyBody(id);
  }
  w_->owned.clear();
  w_->floors.clear();
}

namespace {

// The capsule with its feet at the origin.
JPH::RefConst<JPH::Shape> capsule() {
  const float half = 0.5f * Walker::kHeight - Walker::kRadius;
  return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, half + Walker::kRadius, 0), JPH::Quat::sIdentity(),
                                             new JPH::CapsuleShape(half, Walker::kRadius))
      .Create()
      .Get();
}

}  // namespace

bool Walker::find_spawn(float x, float z, float radius, bool on_floor, float feet[3], bool solid_ground) {
  if (w_->bodies_changed) {
    w_->system.OptimizeBroadPhase();
    w_->bodies_changed = false;
  }
  const JPH::NarrowPhaseQuery &query = w_->system.GetNarrowPhaseQuery();
  const auto broad = w_->system.GetDefaultBroadPhaseLayerFilter(kMoving);
  const auto layers = w_->system.GetDefaultLayerFilter(kMoving);  // what stops the walker: solid bodies and ground
  const JPH::AABox bounds = w_->system.GetBounds();
  if (!bounds.IsValid()) return false;
  // Within a sane height range: one unbounded body (a plane, an empty shape) would make the world's
  // box endless, and a ray across it loses every bit of precision.
  const float top = std::min(static_cast<float>(bounds.mMax.GetY()), 5000.0f) + 1;
  const float depth = top - std::max(static_cast<float>(bounds.mMin.GetY()), -2000.0f) + 1;
  const JPH::RefConst<JPH::Shape> shape = capsule();
  const float walkable = std::cos(JPH::DegreesToRadians(50.0f));  // the character's steepest slope

  // Whether the capsule stands at (px, pz): the lowest thing there is ground it can stand on, with
  // nothing above it (no roof, no canopy, no trunk's top: open sky), and standing there (a little
  // above it) it touches nothing. Within half a metre of the ground counts as the ground (a road's
  // skin over a field).
  const auto is_floor = [&](const JPH::BodyID &id) { return std::find(w_->floors.begin(), w_->floors.end(), id) != w_->floors.end(); };
  auto fits = [&](float px, float pz, float out[3]) {
    JPH::RRayCast ray(JPH::RVec3(px, top, pz), JPH::Vec3(0, -depth, 0));
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;  // a roof from either side; a box only where it starts
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> hits;
    query.CastRay(ray, settings, hits, broad, layers);
    const JPH::RayCastResult *ground = nullptr;
    float highest = 2;  // fraction of the topmost hit that isn't the floor
    for (const JPH::RayCastResult &h : hits.mHits) {
      const bool floor = is_floor(h.mBodyID);
      if (!floor) highest = std::min(highest, h.mFraction);
      if (floor && !on_floor) continue;
      if (!ground || (is_floor(ground->mBodyID) && !floor) || (floor == is_floor(ground->mBodyID) && h.mFraction > ground->mFraction))
        ground = &h;  // the lowest, and real ground before the floor
    }
    if (!ground) return false;
    if (!is_floor(ground->mBodyID) && (ground->mFraction - highest) * depth > 0.5f) return false;  // something overhead
    if (is_floor(ground->mBodyID) && highest < 2) return false;  // the floor, under something
    const JPH::RVec3 at = ray.GetPointOnRay(ground->mFraction);
    {
      JPH::BodyLockRead lock(w_->system.GetBodyLockInterface(), ground->mBodyID);
      if (!lock.Succeeded()) return false;
      const JPH::ObjectLayer layer = lock.GetBody().GetObjectLayer();
      if (layer != kGround && !(solid_ground && layer == kSolid)) return false;  // not a trunk's top or a roof
      if (lock.GetBody().GetWorldSpaceSurfaceNormal(ground->mSubShapeID2, at).GetY() < walkable) return false;
    }
    const JPH::RVec3 stand = at + JPH::Vec3(0, 0.1f, 0);
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> touch;
    query.CollideShape(shape, JPH::Vec3::sReplicate(1), JPH::RMat44::sTranslation(stand + shape->GetCenterOfMass()), JPH::CollideShapeSettings(),
                       JPH::RVec3::sZero(), touch, broad, layers);
    if (touch.HadHit()) return false;
    out[0] = float(stand.GetX());
    out[1] = float(stand.GetY());
    out[2] = float(stand.GetZ());
    return true;
  };
  constexpr float kStep = 0.5f;
  if (fits(x, z, feet)) return true;
  for (int ring = 1; ring * kStep <= radius; ++ring) {
    const float r = ring * kStep;
    const int n = std::max(6, int(std::ceil(2 * JPH::JPH_PI * r / kStep)));
    for (int i = 0; i < n; ++i) {
      const float a = 2 * JPH::JPH_PI * float(i) / float(n);
      if (fits(x + r * std::cos(a), z + r * std::sin(a), feet)) return true;
    }
  }
  return false;
}

void Walker::spawn(const float feet[3]) {
  w_->spawn_at = JPH::RVec3(feet[0], feet[1], feet[2]);
  if (!w_->character) {
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = capsule();
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -kRadius);  // contacts below the lower sphere's centre carry it
    w_->character = new JPH::CharacterVirtual(settings, w_->spawn_at, JPH::Quat::sIdentity(), 0, &w_->system);
  }
  w_->character->SetPosition(w_->spawn_at);
  w_->character->SetLinearVelocity(JPH::Vec3::sZero());
}

void Walker::step(float dt, const Input &in) {
  if (!w_->character || dt <= 0) return;
  // A runtime adds and removes bodies one by one (a map streaming its chunks), which leaves the
  // broadphase's trees unbalanced and every query slow: rebuild them when the world has changed.
  tidy();
  JPH::CharacterVirtual &ch = *w_->character;
  const JPH::Vec3 up = JPH::Vec3::sAxisY(), gravity(0, kGravity, 0);
  // What the stick asks: along the heading and across it, no faster than full speed diagonally.
  const JPH::Vec3 fwd(std::sin(in.heading), 0, -std::cos(in.heading)), right(std::cos(in.heading), 0, std::sin(in.heading));
  JPH::Vec3 want = fwd * in.forward + right * in.right;
  if (want.LengthSq() > 1) want = want.Normalized();
  want *= (in.run ? kRun : kWalk) * std::clamp(in.speed, 0.1f, 20.0f);

  ch.UpdateGroundVelocity();
  const JPH::Vec3 velocity = ch.GetLinearVelocity();
  const JPH::Vec3 vertical = up * velocity.Dot(up), ground = ch.GetGroundVelocity();
  const bool landed = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && vertical.GetY() - ground.GetY() < 0.1f;
  JPH::Vec3 next = landed ? ground : vertical;
  if (landed && in.jump) next += up * kJump;
  next += gravity * dt;
  // On the ground it goes where it's steered; in the air it keeps its run and steers a little.
  const JPH::Vec3 flat = velocity - vertical;
  next += landed ? want : flat + (want - flat) * std::min(1.0f, 2.5f * dt);
  ch.SetLinearVelocity(next);

  JPH::CharacterVirtual::ExtendedUpdateSettings settings;
  settings.mWalkStairsStepUp = JPH::Vec3(0, 0.45f, 0);
  settings.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0);
  ch.ExtendedUpdate(dt, gravity, settings, w_->system.GetDefaultBroadPhaseLayerFilter(kMoving),
                    w_->system.GetDefaultLayerFilter(kMoving), {}, {}, w_->temp);
  if (ch.GetPosition().GetY() < -60) spawn(std::array<float, 3>{float(w_->spawn_at.GetX()), float(w_->spawn_at.GetY()), float(w_->spawn_at.GetZ())}.data());
}

uint32_t Walker::body_count() const { return w_->system.GetNumBodies(); }

void Walker::tidy() {
  if (w_->bodies_changed || w_->system.GetNumBodies() != w_->optimized_bodies) {
    w_->system.OptimizeBroadPhase();
    w_->bodies_changed = false;
    w_->optimized_bodies = w_->system.GetNumBodies();
  }
}

std::string Walker::probe(float x, float z) {
  const JPH::NarrowPhaseQuery &query = w_->system.GetNarrowPhaseQuery();
  const auto broad = w_->system.GetDefaultBroadPhaseLayerFilter(kMoving);
  const auto layers = w_->system.GetDefaultLayerFilter(kMoving);
  const JPH::AABox bounds = w_->system.GetBounds();
  if (!bounds.IsValid()) return "no bounds";
  const float top = std::min(static_cast<float>(bounds.mMax.GetY()), 5000.0f) + 1;
  const float depth = top - std::max(static_cast<float>(bounds.mMin.GetY()), -2000.0f) + 1;
  JPH::RRayCast ray(JPH::RVec3(x, top, z), JPH::Vec3(0, -depth, 0));
  JPH::RayCastSettings settings;
  settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
  JPH::AllHitCollisionCollector<JPH::CastRayCollector> hits;
  query.CastRay(ray, settings, hits, broad, layers);
  hits.Sort();
  std::string out = "bounds y " + std::to_string(bounds.mMin.GetY()) + " to " + std::to_string(bounds.mMax.GetY()) + "; hits:";
  for (const auto &h : hits.mHits) {
    const JPH::RVec3 at = ray.GetPointOnRay(h.mFraction);
    JPH::BodyLockRead lock(w_->system.GetBodyLockInterface(), h.mBodyID);
    if (!lock.Succeeded()) continue;
    out += " [y " + std::to_string(static_cast<float>(at.GetY())) + " layer " + std::to_string(lock.GetBody().GetObjectLayer()) + " ny " +
           std::to_string(lock.GetBody().GetWorldSpaceSurfaceNormal(h.mSubShapeID2, at).GetY()) + "]";
  }
  if (!hits.mHits.empty()) {
    const JPH::RVec3 stand = ray.GetPointOnRay(hits.mHits.front().mFraction) + JPH::Vec3(0, 0.1f, 0);
    const JPH::RefConst<JPH::Shape> shape = capsule();
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> touch;
    query.CollideShape(shape, JPH::Vec3::sReplicate(1), JPH::RMat44::sTranslation(stand + shape->GetCenterOfMass()), JPH::CollideShapeSettings(),
                       JPH::RVec3::sZero(), touch, broad, layers);
    out += "; the capsule there touches " + std::to_string(touch.mHits.size());
    for (const auto &t : touch.mHits) out += " (depth " + std::to_string(t.mPenetrationDepth) + ")";
  }
  return out;
}

void Walker::feet(float out[3]) const {
  const JPH::RVec3 p = w_->character ? w_->character->GetPosition() : w_->spawn_at;
  out[0] = static_cast<float>(p.GetX());
  out[1] = static_cast<float>(p.GetY());
  out[2] = static_cast<float>(p.GetZ());
}

bool Walker::grounded() const {
  return w_->character && w_->character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

bool Walker::spawned() const { return w_->character != nullptr; }

}  // namespace physics
