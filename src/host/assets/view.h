// What the viewport shows for a workspace item: concept art as an image; a
// Meshy mesh item at full fidelity (after sizing) with its conformance and,
// once built, its progressive mesh for any level of detail; or a stamped
// fixed asset's LOD0.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "assets/meshio.h"
#include "core/pipeline.h"
#include "fire/fire.h"
#include "viewport/image.h"
#include "viewport/raster.h"
#include "workspace/workspace.h"

namespace assets {

struct ViewContent {
  std::string label;
  std::shared_ptr<const runtime::Mesh> mesh;
  bool has_conformance = false;
  float radius = 0, height = 0;
  bool has_box = false;  // a box conformance too (a building's lot)
  foundry::vec3 box_min{0, 0, 0}, box_max{0, 0, 0};
  std::shared_ptr<const viewport::Image> image;
  std::shared_ptr<const ProgressiveMesh> progressive;
  std::vector<std::shared_ptr<const runtime::Mesh>> lods;  // a fixed asset's stamped LODs, LOD0 first
  std::vector<viewport::Line> collision;  // collision wireframes (a pipeline's), world space
  std::vector<viewport::Line> lines;      // other overlays that frame the view (a region's hexes)
  // Fire (design/submit.md, "Fire"): its fire points (the model's draw.fires, in its space), the
  // clock and wind they burn in, and how much it burns (0: only its lit fires and chimneys).
  std::vector<runtime::FirePoint> fires;
  float fire_time = 0, wind_speed = 0, wind_heading = 0, burning = 0;
  bool own_ground = false;  // it draws its own ground (a boundary's terrain): no sky ground disc under it
  uint32_t full_triangles() const { return mesh ? static_cast<uint32_t>(mesh->triangles()) : 0; }
  bool empty() const { return !mesh && !image; }
};

ViewContent load_view(const workspace::Workspace &ws, const std::string &id);

// The fire configs views burn with: the workspace's config/fire_configs/, reread when it changes.
void set_fire_root(const std::string &workspace_root);
std::vector<runtime::fire::Config> fire_configs();
// The fire system's sprites and lights for fire points (model space) at a moment: a still, stepped
// from nothing (fire::Sim::snapshot).
void fire_snapshot(const std::vector<runtime::FirePoint> &fires, float burning, float time, float wind_speed, float wind_heading,
                   std::vector<runtime::fire::Sprite> &sprites, std::vector<runtime::fire::Light> &lights);

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
