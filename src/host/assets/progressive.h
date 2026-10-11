// The progressive mesh transform (design/transforms.md): volume-preserving
// edge collapses from full detail down to the coarsest mesh that stays valid.
#pragma once

#include <functional>
#include <string>

#include "assets/meshio.h"

namespace assets {

// Each collapse places the surviving vertex where the quadric error is
// smallest subject to keeping the enclosed volume unchanged (Lindstrom and
// Turk), after welding seam vertices so UV and color seams don't tear.
// Collapses that flip, fold, or sliver a triangle, or that would make the
// surface non-manifold, are rejected.
bool build_progressive(const runtime::Mesh &mesh, ProgressiveMesh &out, std::string &error,
                       const std::function<void(float)> &progress = nullptr);

}  // namespace assets
