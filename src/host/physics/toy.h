// The foundry's toy Jolt Physics world (design/plotter.md, "Footprints and
// conformance"): the host builds its own world, as a game does, to check and
// show what pipelines output as collision. Jolt is initialized once, here.
#pragma once

#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/pipeline.h"
#include "physics/jolt.h"
#include "viewport/raster.h"

namespace physics {

// A build's collision, checked: Jolt made its shapes, they went into the toy world and out
// again, and they stay inside the conformance.
struct Collision {
  std::vector<runtime::CollisionBody> bodies;
  std::vector<runtime::physics::Surface> surfaces;  // body space, for the viewport's wireframe
  bool ok = true;              // Jolt took every shape, and the toy world added and removed them
  bool inside = true;          // inside the conformance (true when there's no conformance to check)
  nlohmann::json report;       // for pipeline_set and the inspector
};

// Registers Jolt's allocator, factory, and types, once for the process.
void init_jolt();

// conformance_radius 0: no conformance to check against.
std::shared_ptr<const Collision> analyze(const std::vector<runtime::CollisionBody> &bodies, float conformance_radius,
                                         float conformance_height);

// The collision's surfaces as wireframe lines, coloured by purpose (solid red, soft orange, query cyan,
// sensor yellow), turned by yaw about +Y and moved to `at` (a placement in a region).
void wireframe(const Collision &c, std::vector<viewport::Line> &out, foundry::vec3 at = {0, 0, 0}, float yaw = 0);

// Collision bodies from a model's `collision` field, decoded as JSON (the physics_v1_body convention).
std::vector<runtime::CollisionBody> bodies_from_json(const nlohmann::json &collision);

}  // namespace physics
