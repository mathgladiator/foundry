// The runtime's side of the game's physics (design/api.md, "Physics"): Jolt
// Physics, linked beside the runtime. The game owns the Jolt world and hands
// the runtime a blind pointer to it; the runtime keeps each instance's
// collision bodies there, by handle.
//
// No Jolt type crosses this header, so world.h and the C API stay free of Jolt.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/pipeline.h"

namespace runtime::physics {

// The Jolt version and build configuration this runtime was compiled against (JPH_VERSION_ID):
// a world built with other flags would misbehave, so the game's must match.
uint64_t jolt_version();

// Checks the game's pointer: it must be non-null, and its version must match ours.
bool check(const void *system, uint64_t version, std::string &error);

// The Jolt shapes of a build's collision bodies, made once per build and shared by its instances.
struct Shapes;
std::shared_ptr<Shapes> make_shapes(const std::vector<CollisionBody> &bodies, std::string &error);
size_t body_count(const Shapes &shapes);

// The surface of each body as Jolt sees it, in body space: xyz per vertex, three vertices per
// triangle (for drawing collision as a wireframe in the foundry, and proof that Jolt took it).
struct Surface {
  uint32_t purpose = 0;
  std::vector<float> triangles;
};
std::vector<Surface> surfaces(const Shapes &shapes);

// Adds a build's bodies to the world at an instance's transform (3 rows of 4, row-major, as the
// world keeps it), each on the object layer for its purpose. The bodies' user data is the
// instance's handle. Returns the bodies' IDs (index and sequence).
std::vector<uint32_t> add(void *system, const Shapes &shapes, const uint16_t layers[4], const float rows[12],
                          uint64_t handle);
void move(void *system, const std::vector<uint32_t> &bodies, const float rows[12]);
void remove(void *system, const std::vector<uint32_t> &bodies);

}  // namespace runtime::physics
