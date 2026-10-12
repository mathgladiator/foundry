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
  float frame = 0;     // > 0: frame a box this many metres tall and wide instead of the content, so
                       // several renders (a contact sheet) share one scale
};

struct Overlays {
  bool conformance = true;
  bool collision = true;  // the scene's collision wireframes (Scene::collision)
  bool hero = true;
  bool hex = true;
  bool grid = true;
};

// A line in world space, for overlays the scene's owner computes: collision wireframes, a region's hexes.
struct Line {
  foundry::vec3 a, b;
  float r = 1, g = 1, bl = 1, alpha = 0.9f;
};

struct Scene {
  const runtime::Mesh *mesh = nullptr;
  std::vector<Line> collision;  // drawn when overlays.collision is on
  std::vector<Line> lines;      // always drawn, and included in the framing (a region's hexes)
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
