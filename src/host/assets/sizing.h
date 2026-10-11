// The sizing transforms and the hull estimator (design/transforms.md).
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "render/mesh.h"

namespace assets {

// Hull estimator: bounds, height, the footprint's convex hull and smallest
// enclosing circle (its centre and radius), volume, and how the mesh sits
// against each conformance shape (whether its size fits now, and the largest
// uniform scale that would fit).
nlohmann::json hull_report(const std::string &root, const runtime::Mesh &mesh);

// Applies sizing operations to a mesh. Each is a JSON object with "op":
//   scale     {"by": s} or {"by": [sx, sy, sz]}
//   rotate    {"axis": "x"|"y"|"z", "degrees": d}
//   translate {"by": [x, y, z]}
//   ground    lowest point to y = 0
//   center    footprint (smallest enclosing circle) centre to x = z = 0
//   height    {"meters": h}: uniform scale to height h, then ground
//   fit       {"conformance": name, "fill": 0.95, "height": optional}: center,
//             ground, and uniformly scale as large as the conformance allows
//             (or to "height"); sets state["conformance"]
bool apply_ops(const std::string &root, runtime::Mesh &mesh, const nlohmann::json &ops, nlohmann::json &state,
               std::string &error);

}  // namespace assets
