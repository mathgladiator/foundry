#include "physics/toy.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace physics {

namespace {

// The toy world's layers: one per purpose, all static.
const JPH::BroadPhaseLayer kStatic(0);

class BroadPhase final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return kStatic; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "static"; }
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

struct Toy {
  BroadPhase broad;
  ObjectVsBroadPhase object_vs_broad;
  ObjectPairs pairs;
  JPH::PhysicsSystem system;
  std::mutex mutex;  // one check at a time
  Toy() { system.Init(4096, 0, 4096, 1024, broad, object_vs_broad, pairs); }
};

Toy &toy() {
  init_jolt();
  static Toy t;
  return t;
}

const char *purpose_name(uint32_t p) {
  static const char *names[] = {"solid", "soft", "query", "sensor"};
  return p < 4 ? names[p] : "unknown";
}

}  // namespace

void init_jolt() {
  static std::once_flag once;
  std::call_once(once, [] {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  });
}

void wireframe(const Collision &c, std::vector<viewport::Line> &out, foundry::vec3 at, float yaw) {
  static const float kColor[4][3] = {{1.0f, 0.3f, 0.25f}, {1.0f, 0.62f, 0.2f}, {0.35f, 0.85f, 1.0f}, {1.0f, 0.9f, 0.3f}};
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  auto place = [&](const float *p) {
    return foundry::vec3{at.x + p[0] * cy + p[2] * sy, at.y + p[1], at.z - p[0] * sy + p[2] * cy};
  };
  for (const auto &s : c.surfaces) {
    const float *col = kColor[std::min<uint32_t>(s.purpose, 3)];
    // Each triangle's edges.
    for (size_t i = 0; i + 8 < s.triangles.size(); i += 9) {
      const float *v = &s.triangles[i];
      for (int e = 0; e < 3; ++e) {
        const float *a = v + e * 3, *b = v + ((e + 1) % 3) * 3;
        out.push_back({place(a), place(b), col[0], col[1], col[2], 0.85f});
      }
    }
  }
}

std::vector<runtime::CollisionBody> bodies_from_json(const nlohmann::json &collision) {
  std::vector<runtime::CollisionBody> out;
  if (!collision.is_array()) return out;
  auto v3 = [](const nlohmann::json &j, float *dst) {
    for (int i = 0; i < 3; ++i) dst[i] = j.is_array() && j.size() == 3 ? j[i].get<float>() : 0.0f;
  };
  // Enum fields come as their names (the generated JSON's convention), or as numbers.
  auto enum_of = [](const nlohmann::json &v, std::initializer_list<const char *> names) -> uint32_t {
    if (v.is_number()) return v.get<uint32_t>();
    if (v.is_string()) {
      uint32_t i = 0;
      for (const char *n : names) {
        if (v.get<std::string>() == n) return i;
        ++i;
      }
    }
    return 0;
  };
  for (const auto &b : collision) {
    runtime::CollisionBody body;
    body.purpose = enum_of(b.value("purpose", nlohmann::json()), {"solid", "soft", "query", "sensor"});
    body.name = b.value("name", std::string());
    for (const auto &s : b.value("shapes", nlohmann::json::array())) {
      runtime::CollisionShape sh;
      sh.type = enum_of(s.value("type", nlohmann::json()),
                        {"sphere", "box", "capsule", "tapered_capsule", "cylinder", "convex_hull"});
      v3(s.value("position", nlohmann::json()), sh.position);
      v3(s.value("rotation", nlohmann::json()), sh.rotation);
      sh.radius = s.value("radius", 0.0f);
      sh.radius_top = s.value("radius_top", 0.0f);
      sh.half_height = s.value("half_height", 0.0f);
      v3(s.value("half_extents", nlohmann::json()), sh.half_extents);
      for (const auto &p : s.value("points", nlohmann::json::array()))
        for (int i = 0; i < 3; ++i) sh.points.push_back(p[i].get<float>());
      body.shapes.push_back(std::move(sh));
    }
    out.push_back(std::move(body));
  }
  return out;
}

std::shared_ptr<const Collision> analyze(const std::vector<runtime::CollisionBody> &bodies, float conformance_radius,
                                         float conformance_height) {
  auto out = std::make_shared<Collision>();
  out->bodies = bodies;
  nlohmann::json r = nlohmann::json::object();
  size_t shapes = 0;
  nlohmann::json purposes = nlohmann::json::array();
  for (const auto &b : bodies) {
    shapes += b.shapes.size();
    purposes.push_back(purpose_name(b.purpose));
  }
  r["bodies"] = bodies.size();
  r["shapes"] = shapes;
  r["purposes"] = purposes;
  if (bodies.empty()) {
    r["jolt"] = "nothing collides";
    out->report = r;
    return out;
  }
  Toy &t = toy();
  std::lock_guard<std::mutex> lock(t.mutex);
  std::string error;
  auto made = runtime::physics::make_shapes(bodies, error);
  if (!made) {
    out->ok = false;
    r["jolt"] = error;
    out->report = r;
    return out;
  }
  out->surfaces = runtime::physics::surfaces(*made);
  // Into the toy world and out again, as the runtime does for a game.
  const uint16_t layers[4] = {0, 1, 2, 3};
  const float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  const uint32_t before = t.system.GetNumBodies();
  auto ids = runtime::physics::add(&t.system, *made, layers, rows, 0);
  const uint32_t added = t.system.GetNumBodies() - before;
  runtime::physics::remove(&t.system, ids);
  const bool balanced = t.system.GetNumBodies() == before;
  out->ok = added == runtime::physics::body_count(*made) && balanced;
  r["jolt"] = out->ok ? "ok" : "the toy world didn't add and remove every body";
  // Inside the conformance: every surface point within its radius and below its height
  // (anything below the ground is the model's buried base, and is ignored).
  float reach = 0, top = 0;
  for (const auto &s : out->surfaces)
    for (size_t i = 0; i + 2 < s.triangles.size(); i += 3) {
      reach = std::max(reach, std::hypot(s.triangles[i], s.triangles[i + 2]));
      top = std::max(top, s.triangles[i + 1]);
    }
  r["reach_m"] = std::round(reach * 1000) / 1000;
  r["top_m"] = std::round(top * 1000) / 1000;
  if (conformance_radius > 0) {
    // Jolt rounds convex shapes' corners by a convex radius of a few centimetres: allow for it.
    const float slack = 0.03f;
    out->inside = reach <= conformance_radius * 1.01f + slack && top <= std::max(conformance_height, 0.0f) * 1.01f + slack;
    r["inside_conformance"] = out->inside;
  } else {
    r["inside_conformance"] = "no conformance";
  }
  out->report = r;
  return out;
}

}  // namespace physics
