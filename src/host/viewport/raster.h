// A small software renderer for the workspace viewport and for screenshots.
//
// It draws a mesh in the house cel look (style.md section 6: banded light,
// warm light and cool shadow, a rim band, and ink outlines from depth edges),
// together with the yardsticks from style.md section 2: the asset's
// conformance capsule, the 1.8 m hero capsule, the hex, and a ground grid.
//
// The GPU renderer (gpu.h) draws the same look with SDL's GPU API when the
// foundry has a window; this CPU renderer remains for headless runs.
#pragma once

#include <memory>

#include "fire/fire.h"
#include "render/look.h"
#include "render/mesh.h"
#include "sky/sky.h"
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
  float pan[3] = {0, 0, 0};  // metres the view's target moves from what it frames: along the ground (x, z), and up (y)
  // Free look (an observatory's sky): stand at eye and look at an azimuth (radians from north, -Z,
  // toward east, +X) and elevation, with a vertical field of view. Overrides the view above.
  bool free = false;
  float eye[3] = {0, 1.6f, 3.0f};
  float azimuth = 0, elevation = 0.5f, fov = 1.1f;
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

// An observatory's sky over the scene (design/observatory.md): the sky behind it, its light on it,
// and on the GPU its shadows (with a ground to catch them).
struct SkyScene {
  std::shared_ptr<const runtime::sky::Observatory> observatory;
  runtime::sky::State state;
  double time = 0;     // seconds, for the clouds' drift, twinkling, and the aurora
  bool ground = true;  // a disc of the observatory's ground under the scene
};

// The ground a sky puts under a scene: a disc around a centre, at y = 0.
runtime::Mesh ground_disc(float cx, float cz, float radius, const float color[3]);

struct Scene {
  const runtime::Mesh *mesh = nullptr;
  const SkyScene *sky = nullptr;  // null: the workspace's backdrop and fixed light
  std::vector<Line> collision;  // drawn when overlays.collision is on
  std::vector<Line> lines;      // always drawn, and included in the framing (a region's hexes)
  bool has_conformance = false;
  float conformance_radius = 0, conformance_height = 0;
  bool has_box = false;  // a box conformance (a building's lot), drawn with the capsule
  foundry::vec3 box_min{0, 0, 0}, box_max{0, 0, 0};
  Overlays overlays;
  Camera camera;
  bool silhouette = false;  // draw the mesh as a solid black shape (style.md section 3)
  // Fire (design/submit.md, "Fire"): the fire system's sprites, drawn after the ink, and its lights
  // on the mesh (the GPU's; the CPU draws the sprites only).
  std::vector<runtime::fire::Sprite> sprites;
  std::vector<runtime::fire::Light> fire_lights;
};

// The look every render uses (render/look.h; design/submit.md, "The look"): the game's grade for
// accessibility and effects, applied after the ink, before overlay lines; its edges add ink between
// colours. Thread-safe; look_version bumps on every change.
void set_look(const runtime::Look &look);
runtime::Look look();
int look_version();

// Renders at width x height (supersampled internally): on the GPU when a GPU renderer is set
// (gpu.h), otherwise on the CPU.
Image render(const Scene &scene, int width, int height);
// The CPU renderer alone.
Image render_cpu(const Scene &scene, int width, int height);
// While one is alive, render() on this thread draws on the CPU (to compare the two renderers).
struct ForceCpu {
  ForceCpu();
  ~ForceCpu();
};
// Which renderer the last render() on this thread used: "gpu" or "cpu".
const char *last_renderer();

// Where a scene's camera stands for a render of width x height pixels, shared by both renderers:
// a point projects to (cx + x * scale / z, cy - y * scale / z) with x, y, z its offsets from the eye
// along right, up, and forward (or without the division by z when orthographic).
struct Framing {
  foundry::vec3 eye{0, 0, 0}, right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
  bool ortho = true;
  float scale = 1;  // pixels per metre (orthographic), or the focal length in pixels
  float cx = 0, cy = 0;
  float asset_radius = 0;  // the asset's reach from its axis (places the hero capsule)
  float radius = 1;        // the framed bounds' radius
};
Framing frame(const Scene &scene, int width, int height);

// The overlay lines a scene draws over its mesh, in order: the grid, the hex, the hero and
// conformance capsules, the scene's lines, and its collision. Each fades to `hidden` where the
// mesh is in front of it.
struct OverlaySegment {
  foundry::vec3 a, b;
  float r = 1, g = 1, bl = 1, alpha = 1, hidden = 0;
};
std::vector<OverlaySegment> overlay_segments(const Scene &scene, const Framing &f);

View parse_view(const std::string &name);
const char *view_name(View v);

}  // namespace viewport
