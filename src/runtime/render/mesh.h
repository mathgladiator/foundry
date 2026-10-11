// The engine's in-memory mesh: what the renderer uploads and draws.
#pragma once

#include <cstdint>
#include <vector>

#include "boundary.h"

namespace runtime {

struct Mesh {
  std::vector<foundry::vec3> positions;
  std::vector<foundry::vec3> normals;  // empty, or one per position
  std::vector<uint32_t> colors;        // packed RGBA (red in the lowest byte); empty, or one per position
  std::vector<uint32_t> indices;       // a triangle list

  size_t triangles() const { return indices.size() / 3; }
};

}  // namespace runtime
