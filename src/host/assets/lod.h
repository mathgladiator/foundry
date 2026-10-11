// Levels of detail from a progressive mesh, GPU index preparation (optimized
// triangle lists), and mesh statistics.
#pragma once

#include <nlohmann/json.hpp>

#include "assets/meshio.h"

namespace assets {

// Extracts progressive level `level` (0 = full detail): replays its collapse
// steps, maps vertices through the collapses, drops degenerate triangles and
// unused vertices, and recomputes normals (smooth across seams, keeping hard
// edges sharper than 60 degrees).
runtime::Mesh extract_level(const ProgressiveMesh &pm, size_t level);
// The level with the most triangles that is at or under `triangles`.
size_t level_for(const ProgressiveMesh &pm, uint32_t triangles);
runtime::Mesh extract_lod(const ProgressiveMesh &pm, uint32_t triangles);

// The triangle-ordering transform (meshoptimizer): the triangle list
// reordered for the GPU's post-transform vertex cache, with vertices
// reordered for fetch locality. (Strips were measured and lost; see
// design/transforms.md.)
runtime::Mesh optimize_list(const runtime::Mesh &mesh);

// Statistics: counts, welded vertices, bounds, size, surface area, volume,
// open (boundary) and non-manifold edges, degenerate triangles, index count,
// and GPU memory (28-byte vertices, 32-bit indices).
nlohmann::json mesh_stats(const runtime::Mesh &mesh);

}  // namespace assets
