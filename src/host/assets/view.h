// What the viewport shows for a workspace item: concept art as an image; a
// Meshy mesh item at full fidelity (after sizing) with its conformance and,
// once built, its progressive mesh for any level of detail; or a stamped
// fixed asset's LOD0.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "assets/meshio.h"
#include "viewport/image.h"
#include "viewport/raster.h"
#include "workspace/workspace.h"

namespace assets {

struct ViewContent {
  std::string label;
  std::shared_ptr<const runtime::Mesh> mesh;
  bool has_conformance = false;
  float radius = 0, height = 0;
  std::shared_ptr<const viewport::Image> image;
  std::shared_ptr<const ProgressiveMesh> progressive;
  std::vector<std::shared_ptr<const runtime::Mesh>> lods;  // a fixed asset's stamped LODs, LOD0 first
  std::vector<viewport::Line> collision;  // collision wireframes (a pipeline's), world space
  std::vector<viewport::Line> lines;      // other overlays that frame the view (a region's hexes)
  uint32_t full_triangles() const { return mesh ? static_cast<uint32_t>(mesh->triangles()) : 0; }
  bool empty() const { return !mesh && !image; }
};

ViewContent load_view(const workspace::Workspace &ws, const std::string &id);

// Pipelines and config entries show the live pipeline instance; the pipeline slice
// provides it (so assets doesn't depend on pipelines). Returns false to fall back.
using LiveProvider = std::function<bool(const workspace::Item &item, ViewContent &out)>;
void set_live_provider(LiveProvider provider);

// The mesh shown at a triangle budget (the full mesh when 0 or no progressive
// mesh). For a fixed asset, the stamped LOD nearest that count (LOD0 when 0).
std::shared_ptr<const runtime::Mesh> mesh_at(const ViewContent &content, uint32_t triangles);

// triangles > 0 shows the progressive level at or under that count; sky puts it under an observatory.
viewport::Image render_view(const ViewContent &content, const viewport::Camera &camera,
                            const viewport::Overlays &overlays, bool silhouette, int width, int height,
                            uint32_t triangles = 0, const viewport::SkyScene *sky = nullptr);

}  // namespace assets
