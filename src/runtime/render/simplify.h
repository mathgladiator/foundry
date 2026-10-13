// The runtime's far level of detail (design/submit.md, "Level of detail"): a
// build's coarsest mesh, simplified by clustering its vertices on a grid, for
// things a few pixels tall. Every pipeline gets it without a level of its own:
// a cluster keeps one of its vertices' bones, so the instance's pose drives it.
#pragma once

#include "core/pipeline.h"

namespace runtime {

// Clusters the mesh's vertices into cells `cells` across its bounding box's largest side (vertices
// of clearly different colours stay apart), drops the triangles that collapse, and averages each
// cluster's position, normal, and colour.
void simplify(DrawMesh &mesh, int cells);

}  // namespace runtime
