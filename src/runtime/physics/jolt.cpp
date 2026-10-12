#include "physics/jolt.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>

namespace runtime::physics {

struct Shapes {
  struct Body {
    JPH::RefConst<JPH::Shape> shape;
    uint32_t purpose = CollisionBody::kSolid;
  };
  std::vector<Body> bodies;
};

uint64_t jolt_version() {
  using namespace JPH;  // the macro names JPH's integer types unqualified
  return JPH_VERSION_ID;
}

bool check(const void *system, uint64_t version, std::string &error) {
  if (!system) {
    error = "the physics pointer is null";
    return false;
  }
  if (version != jolt_version()) {
    error = "the game's Jolt Physics was built with another version or configuration than the runtime's (JPH_VERSION_ID differs)";
    return false;
  }
  return true;
}

namespace {

JPH::Quat quat_of(const float axis_angle[3]) {
  const JPH::Vec3 v(axis_angle[0], axis_angle[1], axis_angle[2]);
  const float angle = v.Length();
  return angle > 1e-6f ? JPH::Quat::sRotation(v / angle, angle) : JPH::Quat::sIdentity();
}

// One shape, centred at its own origin, or null with an error.
JPH::RefConst<JPH::Shape> make_one(const CollisionShape &s, std::string &error) {
  JPH::ShapeSettings::ShapeResult r;
  const float tiny = 0.001f;
  switch (s.type) {
    case CollisionShape::kSphere:
      r = JPH::SphereShapeSettings(std::max(s.radius, tiny)).Create();
      break;
    case CollisionShape::kBox: {
      const JPH::Vec3 h(std::max(s.half_extents[0], tiny), std::max(s.half_extents[1], tiny), std::max(s.half_extents[2], tiny));
      r = JPH::BoxShapeSettings(h, std::min(JPH::cDefaultConvexRadius, h.ReduceMin() * 0.5f)).Create();
      break;
    }
    case CollisionShape::kCapsule:
      r = JPH::CapsuleShapeSettings(std::max(s.half_height, tiny), std::max(s.radius, tiny)).Create();
      break;
    case CollisionShape::kTaperedCapsule:
      r = JPH::TaperedCapsuleShapeSettings(std::max(s.half_height, tiny), std::max(s.radius_top, tiny), std::max(s.radius, tiny))
              .Create();
      break;
    case CollisionShape::kCylinder: {
      const float hh = std::max(s.half_height, tiny), rr = std::max(s.radius, tiny);
      r = JPH::CylinderShapeSettings(hh, rr, std::min(JPH::cDefaultConvexRadius, std::min(hh, rr) * 0.5f)).Create();
      break;
    }
    case CollisionShape::kConvexHull: {
      JPH::Array<JPH::Vec3> pts;
      for (size_t i = 0; i + 2 < s.points.size(); i += 3) pts.push_back(JPH::Vec3(s.points[i], s.points[i + 1], s.points[i + 2]));
      if (pts.size() < 4) {
        error = "a convex hull needs at least 4 points";
        return nullptr;
      }
      r = JPH::ConvexHullShapeSettings(pts, 0.02f).Create();
      break;
    }
    default:
      error = "unknown collision shape type " + std::to_string(s.type);
      return nullptr;
  }
  if (r.HasError()) {
    error = std::string("collision shape: ") + r.GetError().c_str();
    return nullptr;
  }
  return r.Get();
}

void transform_of(const float rows[12], JPH::RVec3 &pos, JPH::Quat &rot) {
  pos = JPH::RVec3(rows[3], rows[7], rows[11]);
  // The rotation, with any scale divided out of its columns.
  JPH::Vec3 c0(rows[0], rows[4], rows[8]), c1(rows[1], rows[5], rows[9]), c2(rows[2], rows[6], rows[10]);
  c0 = c0.NormalizedOr(JPH::Vec3::sAxisX());
  c1 = c1.NormalizedOr(JPH::Vec3::sAxisY());
  c2 = c2.NormalizedOr(JPH::Vec3::sAxisZ());
  JPH::Mat44 m(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1));
  rot = m.GetQuaternion().Normalized();
}

JPH::BodyID id_of(uint32_t v) { return JPH::BodyID(v); }

}  // namespace

std::shared_ptr<Shapes> make_shapes(const std::vector<CollisionBody> &bodies, std::string &error) {
  auto out = std::make_shared<Shapes>();
  for (const CollisionBody &b : bodies) {
    if (b.shapes.empty()) continue;
    JPH::RefConst<JPH::Shape> shape;
    if (b.shapes.size() == 1) {
      const CollisionShape &s = b.shapes[0];
      JPH::RefConst<JPH::Shape> one = make_one(s, error);
      if (!one) return nullptr;
      const JPH::Vec3 p(s.position[0], s.position[1], s.position[2]);
      const JPH::Quat q = quat_of(s.rotation);
      if (p.IsNearZero() && q.IsClose(JPH::Quat::sIdentity())) {
        shape = one;
      } else {
        auto r = JPH::RotatedTranslatedShapeSettings(p, q, one).Create();
        if (r.HasError()) {
          error = std::string("collision body: ") + r.GetError().c_str();
          return nullptr;
        }
        shape = r.Get();
      }
    } else {
      JPH::StaticCompoundShapeSettings compound;
      for (const CollisionShape &s : b.shapes) {
        JPH::RefConst<JPH::Shape> one = make_one(s, error);
        if (!one) return nullptr;
        compound.AddShape(JPH::Vec3(s.position[0], s.position[1], s.position[2]), quat_of(s.rotation), one);
      }
      auto r = compound.Create();
      if (r.HasError()) {
        error = std::string("collision body: ") + r.GetError().c_str();
        return nullptr;
      }
      shape = r.Get();
    }
    out->bodies.push_back({shape, std::min<uint32_t>(b.purpose, CollisionBody::kSensor)});
  }
  return out;
}

size_t body_count(const Shapes &shapes) { return shapes.bodies.size(); }

std::vector<Surface> surfaces(const Shapes &shapes) {
  std::vector<Surface> out;
  for (const Shapes::Body &b : shapes.bodies) {
    Surface s;
    s.purpose = b.purpose;
    // Only leaf shapes give triangles: collect the body's leaves, placed in body space, first.
    const JPH::AABox box(JPH::Vec3::sReplicate(-1e4f), JPH::Vec3::sReplicate(1e4f));
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    b.shape->CollectTransformedShapes(box, b.shape->GetCenterOfMass(), JPH::Quat::sIdentity(), JPH::Vec3::sOne(),
                                      JPH::SubShapeIDCreator(), leaves, JPH::ShapeFilter());
    for (const JPH::TransformedShape &leaf : leaves.mHits) {
      JPH::Shape::GetTrianglesContext ctx;
      leaf.GetTrianglesStart(ctx, box, JPH::RVec3::sZero());
      JPH::Float3 v[JPH::Shape::cGetTrianglesMinTrianglesRequested * 3];
      for (;;) {
        const int n = leaf.GetTrianglesNext(ctx, JPH::Shape::cGetTrianglesMinTrianglesRequested, v);
        if (n <= 0) break;
        for (int i = 0; i < n * 3; ++i) {
          s.triangles.push_back(v[i].x);
          s.triangles.push_back(v[i].y);
          s.triangles.push_back(v[i].z);
        }
      }
    }
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<uint32_t> add(void *system, const Shapes &shapes, const uint16_t layers[4], const float rows[12],
                          uint64_t handle) {
  std::vector<uint32_t> ids;
  auto *ps = static_cast<JPH::PhysicsSystem *>(system);
  JPH::BodyInterface &bi = ps->GetBodyInterface();
  JPH::RVec3 pos;
  JPH::Quat rot;
  transform_of(rows, pos, rot);
  for (const Shapes::Body &b : shapes.bodies) {
    JPH::BodyCreationSettings settings(b.shape, pos, rot, JPH::EMotionType::Static, layers[b.purpose]);
    settings.mIsSensor = b.purpose == CollisionBody::kSensor;
    settings.mUserData = handle;
    const JPH::BodyID id = bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    if (!id.IsInvalid()) ids.push_back(id.GetIndexAndSequenceNumber());
  }
  return ids;
}

void move(void *system, const std::vector<uint32_t> &bodies, const float rows[12]) {
  if (bodies.empty()) return;
  JPH::BodyInterface &bi = static_cast<JPH::PhysicsSystem *>(system)->GetBodyInterface();
  JPH::RVec3 pos;
  JPH::Quat rot;
  transform_of(rows, pos, rot);
  for (uint32_t v : bodies) bi.SetPositionAndRotation(id_of(v), pos, rot, JPH::EActivation::DontActivate);
}

void remove(void *system, const std::vector<uint32_t> &bodies) {
  if (bodies.empty()) return;
  JPH::BodyInterface &bi = static_cast<JPH::PhysicsSystem *>(system)->GetBodyInterface();
  for (uint32_t v : bodies) {
    bi.RemoveBody(id_of(v));
    bi.DestroyBody(id_of(v));
  }
}

}  // namespace runtime::physics
