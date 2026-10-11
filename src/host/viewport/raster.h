// A small software renderer for the workspace viewport and for screenshots.
//
// It draws a mesh in the house cel look (style.md section 6: banded light,
// warm light and cool shadow, a rim band, and ink outlines from depth edges),
// together with the yardsticks from style.md section 2: the asset's
// conformance capsule, the 1.8 m hero capsule, the hex, and a ground grid.
//
// Rendering on the CPU keeps screenshots trivial for the agent (no GPU
// readback) and works without a window. The GPU engine (design/submit.md)
// replaces it in a later milestone.
#pragma once

#include "render/mesh.h"
#include "viewport/image.h"

namespace viewport {

enum class View { Orbit, Front, Side, Top, Iso };

struct Camera {
  View view = View::Orbit;
  float yaw = 0.7f;    // radians, orbit only
  float pitch = 0.3f;  // radians, orbit only
  float zoom = 1.0f;   // > 1 moves closer
  int quarter = 0;     // quarter turns (90 degrees each) around +Y, for every view
  float distance = 0;  // orbit only: > 0 places the camera this many metres from the asset, with the
                       // game camera's 0.9 rad field of view (to see an asset as at a LOD switch)
};

struct Overlays {
  bool conformance = true;
  bool hero = true;
  bool hex = true;
  bool grid = true;
};

struct Scene {
  const runtime::Mesh *mesh = nullptr;
  bool has_conformance = false;
  float conformance_radius = 0, conformance_height = 0;
  Overlays overlays;
  Camera camera;
  bool silhouette = false;  // draw the mesh as a solid black shape (style.md section 3)
};

// Renders at width x height (supersampled internally).
Image render(const Scene &scene, int width, int height);

View parse_view(const std::string &name);
const char *view_name(View v);

}  // namespace viewport
