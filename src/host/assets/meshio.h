// The foundry's own mesh files (design/transforms.md): meshes and progressive
// meshes between pipeline transforms, and the stamped levels of detail.
//
// Layout, little-endian: magic "FDYD", format version, a format name string
// ("foundry_mesh" or "foundry_progressive"), then the fields in order.
// Arrays are a u32 count followed by the elements.
#pragma once

#include <string>
#include <vector>

#include "render/mesh.h"

namespace assets {

// A progressive mesh (see progressive.h for how it's built and read).
struct ProgressiveMesh {
  std::vector<foundry::vec3> positions;  // original positions, in progressive order
  std::vector<foundry::vec3> normals;
  std::vector<uint32_t> colors;
  std::vector<uint32_t> indices;          // full detail
  std::vector<uint32_t> collapse;         // per vertex: the lower-indexed vertex it collapses into (or itself)
  std::vector<uint32_t> welded;           // per vertex: welded position id (seams share one)
  std::vector<uint32_t> level_vertices;   // vertices kept at each level
  std::vector<uint32_t> level_triangles;  // triangles at each level (non-increasing)
  std::vector<uint32_t> step_welded;      // per collapse step: the surviving welded vertex
  std::vector<foundry::vec3> step_position;  // per collapse step: its new position
  std::string report;
};

bool save_mesh(const std::string &path, const runtime::Mesh &mesh);
bool load_mesh(const std::string &path, runtime::Mesh &mesh);
bool save_progressive(const std::string &path, const ProgressiveMesh &pm);
bool load_progressive_file(const std::string &path, ProgressiveMesh &pm);

}  // namespace assets
