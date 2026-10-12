// A toy Jolt Physics world, made the way a game makes its own (design/api.md,
// "Physics"): the stress tool plays the game, so it owns the world and hands
// the runtime a tagged pointer to it.
#pragma once

#include "runtime.h"

namespace stress {

// Object layers: one per collision purpose, plus the game's moving things.
enum ToyLayer : uint16_t { kToySolid = 0, kToySoft = 1, kToyQuery = 2, kToySensor = 3, kToyMoving = 4, kToyLayers = 5 };

struct ToyPhysics;
// Initializes Jolt (once per process) and creates a world; null on failure.
ToyPhysics *toy_physics_create(uint32_t max_bodies);
void toy_physics_destroy(ToyPhysics *p);
// The description the runtime takes.
const foundry_physics *toy_physics_desc(ToyPhysics *p);
uint32_t toy_physics_bodies(ToyPhysics *p);  // bodies in the world now
// Rebuilds the broad phase, as a game's PhysicsSystem::Update does each step: until then, Jolt
// keeps the tree nodes of removed bodies, so a drain measures memory only after settling.
void toy_physics_settle(ToyPhysics *p);

}  // namespace stress
