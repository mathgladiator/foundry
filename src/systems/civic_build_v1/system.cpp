// civic_build_v1: a civic building (design/planner.md, "Civic buildings"): what a settlement holds
// in common beyond its homes and trades: a court, a bank, a church or temple, a town or guild hall, a
// library, a market hall, a mage's tower, and the centres a square's road rings round (a well, a
// market cross, a fountain, a maypole, a statue, a shrine).
//
// 1. The lot: centre (flower7), small (rect6x5), large (rect9x7), or great (rect12x9), anchored at
//    the cell nearest its middle, its front (+z) toward its road; the building stands in a
//    rectangle inside its cells, set back a step from the front.
// 2. By kind:
//    - a hall: a plinth, walls with rows of windows, a gable roof along the front, a door up steps,
//      a portico of columns and a pediment (columns), a cupola on the ridge with a clock and a bell;
//    - a church: a nave toward the back, an apse, tall arched windows, a tower at the front with a
//      belfry (its bell swings) and a spire, the door at the tower's foot;
//    - a temple: three steps, a cella, a portico of columns with a pediment, a dome;
//    - a bank: a rusticated stone block with a cornice and parapet, columns by an iron-strapped door,
//      barred windows, a dome;
//    - a tower: a round tower with a cone roof and glowing windows, an annex at its foot;
//    - a market hall: an upper hall on arcades of columns over stalls, a cupola;
//    - a centre: a well under its little roof (its bucket swings), a market cross on its steps, a
//      fountain's basins, a maypole (ribbons stream), a statue on its plinth, a shrine with a candle.
// 3. Grandeur (plain, fine, grand) scales it and adds banners, lamps, and columns.
// 4. The emblem over the door: a sun, scales, coins, a book, a hammer, a star, a tree, a crown, a wheel.
// 5. Its lot's cells, box, every lot as guides, the doorway, the marker, and collision (walls solid,
//    the doorway a sensor). Bones are house_v1_bone, animated by house_animate_v1.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "channels/civic_v1_model.h"
#include "channels/civic_v1_spec.h"
#include "condition.h"
#include "lot.h"
#include "theme.h"
#include "packer/civic_v1_model.h"
#include "packer/civic_v1_spec.h"

namespace civic_build_v1 {

using channels::civic_v1_model;
using channels::civic_v1_spec;
using channels::civic_v1_type;
using channels::house_v1_bone;

namespace {

using B = Builder<civic_v1_model>;
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
const vec3 kPane = {0.12f, 0.13f, 0.17f};
const vec3 kWater = {0.32f, 0.52f, 0.74f};
const vec3 kIron = {0.2f, 0.2f, 0.22f};
const vec3 kWood = {0.47f, 0.33f, 0.2f};

constexpr int kLotSizes = 4;
const int kLotW[kLotSizes] = {0, 6, 9, 12}, kLotH[kLotSizes] = {1, 5, 7, 9};

using Th = channels::village_v1_theme;

struct Ctx {
  B &b;
  const civic_v1_type &t;
  const Lot &lot;
  float g;       // grandeur's scale
  int grand;     // 0 plain, 1 fine, 2 grand (and wondrous)
  uint32_t season, seed;
  const Th *th = nullptr;  // the village's theme
  bool wonder = false;     // wondrous: the exotic touches on top of grand
  // A hall's ridge (for ornaments on its gable ends): its ends' x, its height, its z; y 0 for none.
  float ridge_x0 = 0, ridge_x1 = 0, ridge_y = 0, ridge_z = 0;
  vec3 door = v3(0, 0, 0);
  float top = 0;
  float pick(uint32_t salt) const { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); }
  // The building's rectangle: x0..x1, z0..z1 (the front at z1), set back from the lot's front.
  float x0() const { return lot.cx - lot.hx * g + 0.35f; }
  float x1() const { return lot.cx + lot.hx * g - 0.35f; }
  float z1() const { return lot.cz + lot.hz - 1.2f; }
  float z0() const { return lot.cz + lot.hz - 1.2f - (2 * lot.hz - 1.6f) * g; }
};

float pitch_rise(const civic_v1_type &t, float half) { return half * foundry::det::sin(t.roof_pitch * kDeg) / foundry::det::cos(t.roof_pitch * kDeg); }

// A door on the face z = z (facing +z) at x: leaves hinged to swing in, a frame, steps below.
void door(Ctx &c, float x, float z, float w, float h, bool arched, int steps) {
  B &b = c.b;
  const int leaves = w > 1.3f ? 2 : 1;
  const float lw = w / static_cast<float>(leaves);
  for (int l = 0; l < leaves; ++l) {
    const bool left = l == 0;
    const float hx = left ? x - w * 0.5f : x + w * 0.5f, dirx = left ? 1.0f : -1.0f;
    const uint32_t bone = b.bone(0, v3(hx, 0, z), v3(0, left ? 1.0f : -1.0f, 0), house_v1_bone::kMotionHinge, 0, 1.4f, 0, 0);
    b.box(v3(hx + dirx * lw * 0.5f, h * 0.5f, z + 0.02f), v3(lw * 0.5f - 0.02f, h * 0.5f, 0.04f), lerp(kWood, c.t.trim_color, 0.25f), bone);
    if (b.detail < 2)
      for (float y : {h * 0.25f, h * 0.75f}) b.box(v3(hx + dirx * lw * 0.5f, y, z + 0.07f), v3(lw * 0.5f - 0.08f, 0.04f, 0.01f), kIron, bone, false);
  }
  b.box(v3(x, h + 0.08f, z + 0.05f), v3(w * 0.5f + 0.14f, 0.08f, 0.06f), c.t.trim_color);
  for (int s = -1; s <= 1; s += 2) b.box(v3(x + static_cast<float>(s) * (w * 0.5f + 0.07f), h * 0.5f, z + 0.05f), v3(0.07f, h * 0.5f, 0.06f), c.t.trim_color);
  if (arched) b.triangle(v3(x - w * 0.5f, h + 0.16f, z + 0.06f), v3(x + w * 0.5f, h + 0.16f, z + 0.06f), v3(x, h + 0.16f + w * 0.45f, z + 0.06f), c.t.trim_color, 0, false, v3(0, 0, 1));
  for (int s = 0; s < steps; ++s)
    b.block(x - w * 0.5f - 0.4f - 0.15f * static_cast<float>(s), x + w * 0.5f + 0.4f + 0.15f * static_cast<float>(s), 0, 0.15f * static_cast<float>(steps - s),
            z + 0.35f * static_cast<float>(s), z + 0.35f * static_cast<float>(s + 1), c.t.base_color);
  c.door = v3(x, 0, z + 0.35f * static_cast<float>(steps) + 0.5f);
}

// The emblem over a door at (x, y, z), facing +z.
void emblem(Ctx &c, float x, float y, float z) {
  B &b = c.b;
  const vec3 m = c.t.metal_color, n = v3(0, 0, 1);
  const vec3 p = v3(x, y, z + 0.06f);
  b.box(p, v3(0.45f, 0.38f, 0.04f), c.t.trim_color * 0.9f);
  const vec3 q = p + v3(0, 0, 0.05f);
  switch (c.t.symbol) {
    case civic_v1_type::kSymbolSun:
      b.frustum(q, q + n * 0.03f, 0.16f, 0.16f, 10, m);
      for (int k = 0; k < 8; ++k) {
        const float a = kPi * 0.25f * static_cast<float>(k);
        const vec3 d = v3(foundry::det::cos(a), foundry::det::sin(a), 0);
        b.beam(q + d * 0.19f, q + d * 0.31f, 0.02f, m);
      }
      break;
    case civic_v1_type::kSymbolScales:
      b.beam(q - v3(0, 0.25f, 0), q + v3(0, 0.2f, 0), 0.02f, m);
      b.beam(q + v3(-0.28f, 0.18f, 0), q + v3(0.28f, 0.18f, 0), 0.02f, m);
      for (int s = -1; s <= 1; s += 2) b.box(q + v3(0.28f * static_cast<float>(s), -0.02f, 0), v3(0.1f, 0.02f, 0.02f), m);
      break;
    case civic_v1_type::kSymbolCoins:
      for (int k = 0; k < 3; ++k) b.frustum(q + v3(-0.15f + 0.15f * static_cast<float>(k), -0.1f + 0.08f * static_cast<float>(k % 2), 0), q + v3(-0.15f + 0.15f * static_cast<float>(k), -0.1f + 0.08f * static_cast<float>(k % 2), 0.03f), 0.1f, 0.1f, 8, m);
      break;
    case civic_v1_type::kSymbolBook:
      for (int s = -1; s <= 1; s += 2) b.box(q + v3(0.13f * static_cast<float>(s), 0, 0), v3(0.12f, 0.18f, 0.02f), s < 0 ? m : m * 0.85f);
      break;
    case civic_v1_type::kSymbolHammer:
      b.beam(q + v3(-0.2f, -0.25f, 0), q + v3(0.15f, 0.2f, 0), 0.025f, kWood);
      b.box(q + v3(0.15f, 0.2f, 0), v3(0.14f, 0.06f, 0.03f), m);
      break;
    case civic_v1_type::kSymbolStar:
      b.triangle(q + v3(0, 0.3f, 0), q + v3(-0.26f, -0.15f, 0), q + v3(0.26f, -0.15f, 0), m, 0, false, n);
      b.triangle(q + v3(0, -0.3f, 0.01f), q + v3(0.26f, 0.15f, 0.01f), q + v3(-0.26f, 0.15f, 0.01f), m, 0, false, n);
      break;
    case civic_v1_type::kSymbolTree:
      b.beam(q - v3(0, 0.28f, 0), q, 0.03f, kWood);
      b.triangle(q + v3(-0.22f, -0.05f, 0), q + v3(0.22f, -0.05f, 0), q + v3(0, 0.3f, 0), v3(0.3f, 0.55f, 0.25f), 0, false, n);
      break;
    case civic_v1_type::kSymbolCrown:
      b.box(q - v3(0, 0.1f, 0), v3(0.25f, 0.06f, 0.02f), m);
      for (int k = -1; k <= 1; ++k) b.triangle(q + v3(0.25f * static_cast<float>(k) - 0.08f, -0.04f, 0), q + v3(0.25f * static_cast<float>(k) + 0.08f, -0.04f, 0), q + v3(0.25f * static_cast<float>(k), 0.18f, 0), m, 0, false, n);
      break;
    case civic_v1_type::kSymbolWheel:
      for (int k = 0; k < 4; ++k) {
        const float a = kPi * 0.25f * static_cast<float>(k);
        const vec3 d = v3(foundry::det::cos(a), foundry::det::sin(a), 0);
        b.beam(q - d * 0.26f, q + d * 0.26f, 0.018f, m);
      }
      break;
    default: break;
  }
}

// Rows of windows along a wall: on the face z = z facing nz (+1 or -1), from x0 to x1.
void window_row(Ctx &c, float x0, float x1, float z, float nz, float y, float h, float spacing, bool arched) {
  const float len = x1 - x0;
  const int n = static_cast<int>(len / spacing);
  for (int i = 0; i < n; ++i) {
    const float x = x0 + len * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    c.b.window(v3(x, y, z), v3(0, 0, nz), minf(0.9f, spacing * 0.45f), h, c.t.trim_color, kPane, arched);
  }
}
void window_col(Ctx &c, float z0, float z1, float x, float nx, float y, float h, float spacing, bool arched) {
  const float len = z1 - z0;
  const int n = static_cast<int>(len / spacing);
  for (int i = 0; i < n; ++i) {
    const float z = z0 + len * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    c.b.window(v3(x, y, z), v3(nx, 0, 0), minf(0.9f, spacing * 0.45f), h, c.t.trim_color, kPane, arched);
  }
}

// A bell on a sway bone hanging at `at`, swinging across `axis`.
void bell(Ctx &c, vec3 at, vec3 axis, float size) {
  const uint32_t bone = c.b.bone(0, at, axis, house_v1_bone::kMotionSway, 0.7f, 0.35f, 0.15f, c.pick(77));
  c.b.frustum(at - v3(0, size * 1.1f, 0), at - v3(0, size * 0.1f, 0), size, size * 0.45f, 8, c.t.metal_color, bone);
}

// A clock face on a wall facing n at p.
void clock(Ctx &c, vec3 p, vec3 n, float r) {
  c.b.frustum(p, p + n * 0.04f, r, r, 12, v3(0.92f, 0.9f, 0.82f));
  const vec3 q = p + n * 0.05f;
  c.b.beam(q, q + v3(0, r * 0.75f, 0), 0.02f, kIron);
  c.b.beam(q, q + cross(n, v3(0, 1, 0)) * (r * 0.55f), 0.02f, kIron);
}

void walls_box(Ctx &c, float x0, float x1, float z0, float z1, float y1) {
  c.b.block(x0 - 0.12f, x1 + 0.12f, -0.4f, 0.45f, z0 - 0.12f, z1 + 0.12f, c.t.base_color, 0, false);
  c.b.block(x0, x1, 0.45f, y1, z0, z1, c.t.wall_color, 0, false);
  // The theme's material over the type's (wattle, drystone, adobe, living wood, marble).
  const uint32_t tm = c.th ? c.th->walls : 0;
  if (tm >= Th::kWallsWattle) {
    theme::rect_material(c.b, (x0 + x1) * 0.5f, (z0 + z1) * 0.5f, (x1 - x0) * 0.5f, (z1 - z0) * 0.5f, 0.45f, y1, tm, c.t.wall_color, c.t.trim_color,
                         (x0 + x1) * 0.5f, 2.0f, c.seed);
  } else if ((tm == Th::kWallsTimber || (tm == Th::kWallsAsType && c.t.walls == civic_v1_type::kWallsTimber)) && c.b.detail < 2) {
    // Timbers on the front and back.
    const int n = maxi_(2, round_to_int((x1 - x0) / 1.4f));
    for (int i = 0; i <= n; ++i) {
      const float x = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(n);
      for (float z : {z0 - 0.02f, z1 + 0.02f}) c.b.box(v3(x, (0.45f + y1) * 0.5f, z), v3(0.08f, (y1 - 0.45f) * 0.5f, 0.02f), c.t.trim_color, 0, false);
    }
  }
  c.b.solid(v3((x0 + x1) * 0.5f, y1 * 0.5f, (z0 + z1) * 0.5f), v3((x1 - x0) * 0.5f, y1 * 0.5f, (z1 - z0) * 0.5f));
}

void columns(Ctx &c, float x0, float x1, float z, float h, int n) {
  for (int i = 0; i < n; ++i) {
    const float x = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(maxi_(n - 1, 1));
    c.b.frustum(v3(x, 0.45f, z), v3(x, h, z), 0.24f, 0.2f, c.b.detail == 0 ? 10 : 6, c.t.trim_color, 0, false, false);
    c.b.box(v3(x, h + 0.08f, z), v3(0.3f, 0.08f, 0.3f), c.t.trim_color);
    c.b.box(v3(x, 0.5f, z), v3(0.3f, 0.06f, 0.3f), c.t.trim_color);
  }
  c.b.solid(v3((x0 + x1) * 0.5f, h * 0.5f, z), v3((x1 - x0) * 0.5f + 0.25f, h * 0.5f, 0.25f), 0);
}

// ---- kinds ----

// A great roundhouse (a round-plan theme's hall): round walls, the theme's roof, the door at its front.
void round_hall(Ctx &c) {
  B &b = c.b;
  const float cx = (c.x0() + c.x1()) * 0.5f, r = minf((c.x1() - c.x0()) * 0.5f, (c.z1() - c.z0()) * 0.5f), cz = c.z1() - r;
  const float wh = c.t.wall_height * (0.8f + 0.1f * static_cast<float>(c.grand));
  const uint32_t mat = c.th->walls != Th::kWallsAsType ? c.th->walls : Th::kWallsStone;
  const float dw = minf(2.0f, r * 0.5f), dh = minf(2.6f, wh * 0.7f);
  theme::frustum(b, v3(cx, -0.4f, cz), v3(cx, 0.45f, cz), r + 0.15f, r + 0.15f, b.sides + 4, c.t.base_color, 0, true);
  theme::round_walls(b, v3(cx, 0, cz), r, 0.45f, wh, mat, c.t.wall_color, c.t.trim_color, 0.0f, dw, dh - 0.45f, c.seed);
  b.solid_cylinder(v3(cx, 0, cz), r, wh);
  uint32_t form = theme::roof_of(*c.th, Th::kRoofCone);
  if (form == Th::kRoofGable || form == Th::kRoofHip) form = Th::kRoofCone;
  theme::Body body;
  body.cx = cx;
  body.cz = cz;
  body.hx = body.hz = r;
  body.round = true;
  c.top = theme::roof(b, body, form, wh, maxf(c.t.roof_pitch, 40.0f), c.t.roof_color, c.t.trim_color, c.t.wall_color, c.seed, false);
  b.solid(v3(cx, wh + (c.top - wh) * 0.35f, cz), v3(r * 0.7f, (c.top - wh) * 0.35f, r * 0.7f));
  for (float a : {1.0f, -1.0f, 2.0f, -2.0f, 3.14159265f}) {
    const vec3 n = theme::around(a);
    theme::window(b, v3(cx, wh * 0.6f, cz) + n * r, n, 0.7f, 0.9f, theme::windows_of(*c.th, Th::kWindowsArched), c.t.trim_color, kPane);
  }
  door(c, cx, cz + r, dw, dh, true, 1);
  if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, cx, dh + 0.7f, cz + r);
}

void hall(Ctx &c, bool market) {
  if (!market && c.th && theme::round_plan(*c.th)) {
    round_hall(c);
    return;
  }
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z1 = c.z1() - (c.t.columns || c.grand == 2 ? 1.4f : 0.0f), z0 = c.z0();
  const float wh = c.t.wall_height * (0.9f + 0.1f * static_cast<float>(c.grand));
  const float floor = market ? 3.2f : 0.0f;
  const float top = floor + wh;
  if (market) {
    // Arcades: columns in rows under the hall, stalls among them.
    const int nx = maxi_(2, round_to_int((x1 - x0) / 2.4f)), nz = maxi_(2, round_to_int((z1 - z0) / 2.6f));
    for (int i = 0; i <= nx; ++i)
      for (int j = 0; j <= nz; ++j) {
        const float x = x0 + 0.25f + (x1 - x0 - 0.5f) * static_cast<float>(i) / static_cast<float>(nx);
        const float z = z0 + 0.25f + (z1 - z0 - 0.5f) * static_cast<float>(j) / static_cast<float>(nz);
        if (i > 0 && i < nx && j > 0 && j < nz) continue;
        b.frustum(v3(x, 0, z), v3(x, floor, z), 0.24f, 0.22f, b.detail == 0 ? 8 : 5, c.t.base_color, 0, true, false);
        b.solid_cylinder(v3(x, 0, z), 0.25f, floor);
      }
    b.block(x0, x1, -0.3f, 0.05f, z0, z1, c.t.base_color * 0.9f, 0, false);
    b.block(x0 - 0.1f, x1 + 0.1f, floor - 0.3f, floor, z0 - 0.1f, z1 + 0.1f, c.t.trim_color);
    if (b.detail == 0)
      for (int s = 0; s < 3; ++s) {
        const float x = x0 + (x1 - x0) * (0.25f + 0.25f * static_cast<float>(s));
        b.block(x - 0.6f, x + 0.6f, 0, 0.85f, (z0 + z1) * 0.5f - 0.35f, (z0 + z1) * 0.5f + 0.35f, kWood);
        b.block(x - 0.5f, x + 0.5f, 0.85f, 0.95f, (z0 + z1) * 0.5f - 0.3f, (z0 + z1) * 0.5f + 0.3f, lerp(c.t.accent, v3(0.8f, 0.6f, 0.3f), 0.5f));
      }
    b.block(x0, x1, floor, top, z0, z1, c.t.wall_color, 0, false);
    b.solid(v3((x0 + x1) * 0.5f, (floor + top) * 0.5f, (z0 + z1) * 0.5f), v3((x1 - x0) * 0.5f, (top - floor) * 0.5f, (z1 - z0) * 0.5f));
    window_row(c, x0, x1, z1, 1, floor + wh * 0.5f, wh * 0.4f, 1.6f, false);
    window_row(c, x0, x1, z0, -1, floor + wh * 0.5f, wh * 0.4f, 1.6f, false);
    c.door = v3((x0 + x1) * 0.5f, 0, z1 + 0.6f);
  } else {
    walls_box(c, x0, x1, z0, z1, top);
    const int storeys = wh > 5.0f ? 2 : 1;
    for (int s = 0; s < storeys; ++s) {
      const float y = 0.45f + (top - 0.45f) * (static_cast<float>(s) + 0.55f) / static_cast<float>(storeys);
      const float h = (top - 0.45f) / static_cast<float>(storeys) * 0.45f;
      window_row(c, x0 + 0.4f, x1 - 0.4f, z1, 1, y, h, 1.7f, c.grand == 2);
      window_row(c, x0 + 0.4f, x1 - 0.4f, z0, -1, y, h, 1.7f, c.grand == 2);
      window_col(c, z0 + 0.4f, z1 - 0.4f, x0, -1, y, h, 1.9f, false);
      window_col(c, z0 + 0.4f, z1 - 0.4f, x1, 1, y, h, 1.9f, false);
    }
    door(c, (x0 + x1) * 0.5f, z1, minf(2.0f, (x1 - x0) * 0.25f), minf(2.8f, wh * 0.6f), c.grand == 2, c.t.columns ? 1 : 2);
    if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, (x0 + x1) * 0.5f, minf(2.8f, wh * 0.6f) + 0.75f, z1);
  }
  // The roof, ridge along x: the type's gable, or the theme's form.
  const float hz = (z1 - z0) * 0.5f;
  float rise = pitch_rise(c.t, hz);
  const uint32_t form = c.th ? theme::roof_of(*c.th, Th::kRoofGable) : Th::kRoofGable;
  if (form == Th::kRoofGable) {
    b.gable(v3((x0 + x1) * 0.5f, top, (z0 + z1) * 0.5f), v3(1, 0, 0), (x1 - x0) * 0.5f, hz, rise, 0.35f, c.t.roof_color, c.t.wall_color);
  } else {
    theme::Body body;
    body.cx = (x0 + x1) * 0.5f;
    body.cz = (z0 + z1) * 0.5f;
    body.hx = (x1 - x0) * 0.5f;
    body.hz = hz;
    rise = theme::roof(b, body, form, top, c.t.roof_pitch, c.t.roof_color, c.t.trim_color, c.t.wall_color, c.seed, false) - top;
  }
  if (form == Th::kRoofGable || form == Th::kRoofTurf || form == Th::kRoofSweep) {
    c.ridge_x0 = x0 - 0.3f;
    c.ridge_x1 = x1 + 0.3f;
    c.ridge_y = top + rise;
    c.ridge_z = (z0 + z1) * 0.5f;
  }
  b.solid(v3((x0 + x1) * 0.5f, top + rise * 0.4f, (z0 + z1) * 0.5f), v3((x1 - x0) * 0.5f, rise * 0.4f, hz * 0.7f));
  c.top = top + rise;
  // A portico at the front.
  if (!market && (c.t.columns || c.grand == 2)) {
    const float pz = c.z1() - 0.25f;
    const float pw = minf((x1 - x0) * 0.7f, 8.0f), cx = (x0 + x1) * 0.5f;
    const int n = pw > 5 ? 6 : 4;
    columns(c, cx - pw * 0.5f, cx + pw * 0.5f, pz, top * 0.82f, n);
    b.block(cx - pw * 0.5f - 0.35f, cx + pw * 0.5f + 0.35f, top * 0.82f + 0.16f, top * 0.82f + 0.6f, pz - 0.4f, z1 + 0.05f, c.t.trim_color);
    const float pr = (pw * 0.5f + 0.35f) * 0.35f;
    b.triangle(v3(cx - pw * 0.5f - 0.35f, top * 0.82f + 0.6f, pz + 0.4f), v3(cx + pw * 0.5f + 0.35f, top * 0.82f + 0.6f, pz + 0.4f),
               v3(cx, top * 0.82f + 0.6f + pr, pz + 0.4f), c.t.trim_color, 0, false, v3(0, 0, 1));
    b.gable(v3(cx, top * 0.82f + 0.6f, (pz - 0.4f + z1) * 0.5f), v3(0, 0, 1), (z1 - pz + 0.45f) * 0.5f, pw * 0.5f + 0.35f, pr, 0.1f, c.t.roof_color, c.t.trim_color);
    b.block(cx - pw * 0.5f - 0.5f, cx + pw * 0.5f + 0.5f, 0, 0.45f, pz - 0.5f, z1, c.t.base_color);
  }
  // A cupola on the ridge with a clock and a bell.
  if (c.t.clock || c.t.bell || c.grand == 2) {
    const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f, s = minf(1.2f, hz * 0.35f), y0 = top + rise * 0.6f;
    b.block(cx - s, cx + s, y0, top + rise + 1.6f, cz - s, cz + s, c.t.wall_color, 0, false);
    b.pyramid(v3(cx, top + rise + 1.6f, cz), v3(1, 0, 0), v3(0, 0, 1), s + 0.15f, s + 0.15f, s * 1.6f, c.t.metal_color);
    if (c.t.clock) clock(c, v3(cx, top + rise + 0.9f, cz + s + 0.01f), v3(0, 0, 1), s * 0.6f);
    if (c.t.bell) bell(c, v3(cx, top + rise + 1.45f, cz), v3(1, 0, 0), s * 0.4f);
    c.top = top + rise + 1.6f + s * 1.6f;
  }
}

void church(Ctx &c) {
  B &b = c.b;
  const float cx = (c.x0() + c.x1()) * 0.5f, nw = minf(c.x1() - c.x0() - 0.4f, 7.5f * c.g);
  const float x0 = cx - nw * 0.5f, x1 = cx + nw * 0.5f;
  const float tw = minf(nw * 0.5f, 3.2f);
  const float zf = c.z1(), zt0 = zf - tw;  // the tower's footprint at the front
  const float zb = c.z0() + nw * 0.3f;     // the nave's back, the apse beyond
  const float wh = c.t.wall_height * (0.9f + 0.1f * static_cast<float>(c.grand));
  walls_box(c, x0, x1, zb, zt0 + 0.05f, wh);
  // The apse: a half-octagon at the back.
  b.frustum(v3(cx, -0.4f, zb), v3(cx, wh * 0.9f, zb), nw * 0.42f, nw * 0.42f, 8, c.t.wall_color, 0, true, false, kPi / 8.0f);
  b.frustum(v3(cx, wh * 0.9f, zb), v3(cx, wh * 0.9f + nw * 0.35f, zb), nw * 0.46f, 0, 8, c.t.roof_color, 0, false, true, kPi / 8.0f);
  // The nave's roof, ridge along z.
  const float rise = pitch_rise(c.t, nw * 0.5f);
  b.gable(v3(cx, wh, (zb + zt0) * 0.5f), v3(0, 0, 1), (zt0 - zb) * 0.5f, nw * 0.5f, rise, 0.35f, c.t.roof_color, c.t.wall_color);
  b.solid(v3(cx, wh + rise * 0.4f, (zb + zt0) * 0.5f), v3(nw * 0.35f, rise * 0.4f, (zt0 - zb) * 0.5f));
  // Tall arched windows down the nave.
  window_col(c, zb + 0.6f, zt0 - 0.3f, x0, -1, wh * 0.55f, wh * 0.45f, 2.0f, true);
  window_col(c, zb + 0.6f, zt0 - 0.3f, x1, 1, wh * 0.55f, wh * 0.45f, 2.0f, true);
  // The tower.
  const float th = wh * (c.t.spire ? 1.9f : 1.6f) + static_cast<float>(c.grand);
  b.block(cx - tw * 0.5f, cx + tw * 0.5f, -0.4f, th, zt0, zf, c.t.wall_color * 0.98f, 0, false);
  b.block(cx - tw * 0.5f - 0.06f, cx + tw * 0.5f + 0.06f, th, th + 0.12f, zt0 - 0.06f, zf + 0.06f, c.t.trim_color);
  b.solid(v3(cx, th * 0.5f, (zt0 + zf) * 0.5f), v3(tw * 0.5f, th * 0.5f, tw * 0.5f));
  // The belfry: openings on each face, the bell inside.
  for (int s = 0; s < 4; ++s) {
    const vec3 n = s == 0 ? v3(0, 0, 1) : (s == 1 ? v3(0, 0, -1) : (s == 2 ? v3(1, 0, 0) : v3(-1, 0, 0)));
    const vec3 p = v3(cx, th - 1.1f, (zt0 + zf) * 0.5f) + n * (tw * 0.5f + 0.01f);
    b.window(p, n, tw * 0.4f, 1.1f, c.t.trim_color, kPane * 0.5f, true);
  }
  if (c.t.bell || c.grand > 0) bell(c, v3(cx, th - 0.5f, (zt0 + zf) * 0.5f), v3(1, 0, 0), tw * 0.18f);
  if (c.t.clock) clock(c, v3(cx, th - 2.6f, zf + 0.01f), v3(0, 0, 1), tw * 0.28f);
  const bool domed = c.th && (c.th->roof == Th::kRoofDome || c.th->roof == Th::kRoofFlat);
  if (domed) {
    theme::dome(b, v3(cx, th + 0.12f, (zt0 + zf) * 0.5f), tw * 0.55f, tw * 0.6f, b.sides, b.detail == 2 ? 3 : 5, c.t.metal_color, 0, false);
    c.top = th + 0.12f + tw * 0.6f;
  } else if (c.t.spire) {
    b.frustum(v3(cx, th + 0.12f, (zt0 + zf) * 0.5f), v3(cx, th + 0.12f + tw * (2.4f + 0.4f * static_cast<float>(c.grand)), (zt0 + zf) * 0.5f), tw * 0.72f, 0, 4, c.t.roof_color, 0, true, true, kPi * 0.25f);
    c.top = th + 0.12f + tw * (2.4f + 0.4f * static_cast<float>(c.grand));
  } else {
    b.merlons(v3(cx - tw * 0.5f, th + 0.12f, zf - 0.15f), v3(cx + tw * 0.5f, th + 0.12f, zf - 0.15f), 0.4f, 0.4f, 0.3f, c.t.wall_color);
    b.merlons(v3(cx - tw * 0.5f, th + 0.12f, zt0 + 0.15f), v3(cx + tw * 0.5f, th + 0.12f, zt0 + 0.15f), 0.4f, 0.4f, 0.3f, c.t.wall_color);
    c.top = th + 0.6f;
  }
  door(c, cx, zf, minf(1.6f, tw * 0.55f), 2.6f, true, 2);
  if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, cx, 3.9f, zf);
}

void temple(Ctx &c) {
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z0 = c.z0(), z1 = c.z1();
  const float wh = c.t.wall_height * (0.9f + 0.1f * static_cast<float>(c.grand));
  // Three steps, the cella inside a ring of columns at the front.
  for (int s = 0; s < 3; ++s) {
    const float in = 0.3f * static_cast<float>(s);
    b.block(x0 + in, x1 - in, -0.4f + (s == 0 ? 0.0f : 0.2f * static_cast<float>(s) + 0.4f), 0.2f * static_cast<float>(s + 1), z0 + in, z1 - in, c.t.base_color * (1.0f - 0.04f * static_cast<float>(s)));
  }
  const float y0 = 0.6f, deep = (z1 - z0) * 0.3f;
  const float cx0 = x0 + 1.2f, cx1 = x1 - 1.2f, cz0 = z0 + 1.0f, cz1 = z1 - minf(deep, 3.0f);
  b.block(cx0, cx1, y0, y0 + wh, cz0, cz1, c.t.wall_color, 0, false);
  b.solid(v3((cx0 + cx1) * 0.5f, (y0 + wh) * 0.5f, (cz0 + cz1) * 0.5f), v3((cx1 - cx0) * 0.5f, (y0 + wh) * 0.5f, (cz1 - cz0) * 0.5f));
  const int n = maxi_(4, round_to_int((x1 - x0 - 1.2f) / 1.6f) + 1);
  columns(c, x0 + 0.7f, x1 - 0.7f, z1 - 0.7f, y0 + wh, n);
  if (c.grand == 2) {
    columns(c, x0 + 0.7f, x1 - 0.7f, z0 + 0.7f, y0 + wh, n);
  }
  // The entablature and the pediment, ridge front to back.
  b.block(x0 + 0.3f, x1 - 0.3f, y0 + wh + 0.16f, y0 + wh + 0.7f, z0 + 0.3f, z1 - 0.3f, c.t.trim_color);
  const float hw = (x1 - x0) * 0.5f - 0.3f, rise = hw * 0.3f, cx = (x0 + x1) * 0.5f;
  b.gable(v3(cx, y0 + wh + 0.7f, (z0 + z1) * 0.5f), v3(0, 0, 1), (z1 - z0) * 0.5f - 0.3f, hw, rise, 0.2f, c.t.roof_color, c.t.trim_color);
  c.top = y0 + wh + 0.7f + rise;
  if (c.t.dome) {
    const float r = minf(cx1 - cx0, cz1 - cz0) * 0.42f;
    const float dy = y0 + wh + 0.7f + rise * 0.6f;
    b.frustum(v3(cx, dy - 0.3f, (cz0 + cz1) * 0.5f), v3(cx, dy + 0.4f, (cz0 + cz1) * 0.5f), r, r, 12, c.t.wall_color, 0, false, false);
    float prev_r = r, prev_y = dy + 0.4f;
    for (int k = 1; k <= 4; ++k) {
      const float a = kPi * 0.5f * static_cast<float>(k) / 4.0f;
      const float rr = r * foundry::det::cos(a), yy = dy + 0.4f + r * foundry::det::sin(a);
      b.frustum(v3(cx, prev_y, (cz0 + cz1) * 0.5f), v3(cx, yy, (cz0 + cz1) * 0.5f), prev_r, k == 4 ? 0.0f : rr, 12, c.t.metal_color, 0, false, true);
      prev_r = rr;
      prev_y = yy;
    }
    c.top = maxf(c.top, dy + 0.4f + r);
  }
  door(c, cx, cz1, minf(2.0f, (cx1 - cx0) * 0.35f), minf(3.2f, wh * 0.7f), false, 0);
  c.door = v3(cx, 0, z1 + 0.5f);
  if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, cx, y0 + wh + 1.1f, z1 - 0.3f);
}

void bank(Ctx &c) {
  B &b = c.b;
  const float x0 = c.x0() + 0.3f, x1 = c.x1() - 0.3f, z0 = c.z0() + 0.4f, z1 = c.z1();
  const float wh = c.t.wall_height * (0.9f + 0.1f * static_cast<float>(c.grand));
  walls_box(c, x0, x1, z0, z1, wh);
  // Rustication: a proud band of blocks at its foot, a cornice, a parapet.
  b.block(x0 - 0.05f, x1 + 0.05f, 0.45f, 1.5f, z0 - 0.05f, z1 + 0.05f, c.t.base_color * 1.05f, 0, false);
  b.block(x0 - 0.15f, x1 + 0.15f, wh, wh + 0.2f, z0 - 0.15f, z1 + 0.15f, c.t.trim_color);
  b.block(x0, x1, wh + 0.2f, wh + 0.8f, z0, z1, c.t.wall_color * 0.97f, 0, true);
  c.top = wh + 0.8f;
  const float cx = (x0 + x1) * 0.5f;
  // Barred windows.
  const float y = wh * 0.55f;
  for (int s = -1; s <= 1; s += 2)
    for (int i = 1; i <= 2; ++i) {
      const float x = cx + static_cast<float>(s) * (x1 - x0) * 0.17f * static_cast<float>(i);
      b.window(v3(x, y, z1), v3(0, 0, 1), 0.7f, 1.3f, c.t.trim_color, kPane, false);
      if (b.detail < 2)
        for (int k = -1; k <= 1; ++k) b.box(v3(x + 0.2f * static_cast<float>(k), y, z1 + 0.06f), v3(0.015f, 0.65f, 0.015f), kIron, 0, false);
    }
  // Columns by the door, the door, the emblem.
  const float dw = 1.6f, dh = minf(2.8f, wh * 0.6f);
  for (int s = -1; s <= 1; s += 2) {
    const float x = cx + static_cast<float>(s) * (dw * 0.5f + 0.55f);
    b.frustum(v3(x, 0.45f, z1 + 0.35f), v3(x, wh, z1 + 0.35f), 0.25f, 0.22f, b.detail == 0 ? 10 : 6, c.t.trim_color, 0, false, false);
    b.solid_cylinder(v3(x, 0, z1 + 0.35f), 0.26f, wh);
  }
  door(c, cx, z1, dw, dh, false, 2);
  if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, cx, dh + 0.75f, z1);
  if (c.t.dome) {
    const float r = minf(x1 - x0, z1 - z0) * 0.25f;
    b.frustum(v3(cx, wh + 0.8f, (z0 + z1) * 0.5f), v3(cx, wh + 0.8f + r * 0.6f, (z0 + z1) * 0.5f), r, r * 0.65f, 12, c.t.metal_color);
    b.frustum(v3(cx, wh + 0.8f + r * 0.6f, (z0 + z1) * 0.5f), v3(cx, wh + 0.8f + r, (z0 + z1) * 0.5f), r * 0.65f, 0, 12, c.t.metal_color);
    c.top = wh + 0.8f + r;
  }
}

void tower(Ctx &c) {
  B &b = c.b;
  const float cx = (c.x0() + c.x1()) * 0.5f, cz = (c.z0() + c.z1()) * 0.5f - 0.3f;
  const float r = minf(c.x1() - c.x0(), c.z1() - c.z0()) * 0.32f;
  const float h = c.t.wall_height * (2.4f + 0.3f * static_cast<float>(c.grand));
  const int n = b.detail == 0 ? 14 : (b.detail == 1 ? 10 : 7);
  b.frustum(v3(cx, -0.4f, cz), v3(cx, h, cz), r * 1.06f, r, n, c.t.wall_color, 0, true, false);
  b.frustum(v3(cx, h, cz), v3(cx, h + 0.15f, cz), r + 0.2f, r + 0.2f, n, c.t.trim_color);
  b.frustum(v3(cx, h + 0.15f, cz), v3(cx, h + 0.15f + r * 2.2f, cz), r + 0.25f, 0, n, c.t.roof_color);
  b.beam(v3(cx, h + 0.1f + r * 2.2f, cz), v3(cx, h + 0.8f + r * 2.2f, cz), 0.03f, c.t.metal_color);
  c.top = h + 0.8f + r * 2.2f;
  b.solid_cylinder(v3(cx, 0, cz), r * 1.06f, h);
  // Glowing windows climbing round it.
  for (int k = 0; k < 5; ++k) {
    const float a = 0.9f * static_cast<float>(k) + c.pick(5) * kPi;
    const vec3 nn = v3(foundry::det::sin(a), 0, foundry::det::cos(a));
    const vec3 p = v3(cx, h * (0.25f + 0.14f * static_cast<float>(k)), cz) + nn * (r + 0.02f);
    b.window(p, nn, 0.35f, 0.7f, c.t.trim_color, lerp(c.t.accent, v3(1, 1, 1), 0.2f), true);
    if (k == 4) {
      const uint32_t glow = b.bone(0, p, v3(0, 1, 0), house_v1_bone::kMotionFlicker, 1.4f, 0.35f, 0, c.pick(9));
      b.box(p + nn * 0.05f, v3(0.12f, 0.25f, 0.02f), c.t.accent, glow, false);
    }
  }
  // An annex at its foot with the door.
  const float az0 = cz + r * 0.6f, az1 = minf(c.z1(), cz + r + 1.6f);
  b.block(cx - 1.3f, cx + 1.3f, -0.4f, 2.8f, az0, az1, c.t.wall_color * 0.97f, 0, false);
  b.gable(v3(cx, 2.8f, (az0 + az1) * 0.5f), v3(0, 0, 1), (az1 - az0) * 0.5f, 1.3f, 1.0f, 0.2f, c.t.roof_color, c.t.wall_color);
  b.solid(v3(cx, 1.4f, (az0 + az1) * 0.5f), v3(1.3f, 1.4f, (az1 - az0) * 0.5f));
  door(c, cx, az1, 1.0f, 2.1f, true, 1);
  if (c.t.symbol != civic_v1_type::kSymbolNone) emblem(c, cx, 2.95f, az1 - 0.05f);
}

// ---- centres: on the lot's middle, the road ringing them ----

void centre(Ctx &c) {
  B &b = c.b;
  const float R = minf(c.lot.round_r, 2.6f) * (0.85f + 0.08f * static_cast<float>(c.grand));
  const vec3 st = c.t.wall_color, trim = c.t.trim_color;
  switch (c.t.kind) {
    case civic_v1_type::kKindWell: {
      b.frustum(v3(0, -0.2f, 0), v3(0, 0.8f, 0), 0.85f, 0.85f, b.sides, st, 0, false, false);
      b.frustum(v3(0, 0.75f, 0), v3(0, 0.82f, 0), 0.95f, 0.95f, b.sides, trim);
      b.frustum(v3(0, 0.6f, 0), v3(0, 0.62f, 0), 0.7f, 0.7f, b.sides, kWater * 0.7f, 0, true, false);
      for (int s = -1; s <= 1; s += 2) b.beam(v3(0.8f * static_cast<float>(s), 0.8f, 0), v3(0.8f * static_cast<float>(s), 2.3f, 0), 0.06f, kWood);
      b.beam(v3(-0.85f, 2.0f, 0), v3(0.85f, 2.0f, 0), 0.05f, kWood);
      b.gable(v3(0, 2.3f, 0), v3(1, 0, 0), 1.0f, 0.7f, 0.6f, 0.15f, c.t.roof_color, kWood);
      const uint32_t rope = b.bone(0, v3(0, 2.0f, 0), v3(1, 0, 0), house_v1_bone::kMotionSway, 0.8f, 0.15f, 0.4f, c.pick(3));
      b.beam(v3(0, 2.0f, 0), v3(0, 1.15f, 0), 0.01f, v3(0.75f, 0.68f, 0.5f), rope);
      b.frustum(v3(0, 0.9f, 0), v3(0, 1.15f, 0), 0.13f, 0.15f, 6, kWood, rope);
      b.solid_cylinder(v3(0, 0, 0), 0.95f, 2.4f);
      c.top = 3.0f;
      break;
    }
    case civic_v1_type::kKindCross: {
      const float rs[3] = {R * 0.75f, R * 0.55f, R * 0.35f};
      for (int s = 0; s < 3; ++s) b.frustum(v3(0, -0.2f + 0.3f * static_cast<float>(s), 0), v3(0, 0.3f * static_cast<float>(s + 1), 0), rs[s], rs[s], 8, st * (1.0f - 0.04f * static_cast<float>(s)), 0, true, true, kPi / 8.0f);
      b.frustum(v3(0, 0.9f, 0), v3(0, 4.2f, 0), 0.17f, 0.13f, 8, st);
      b.box(v3(0, 4.3f, 0), v3(0.5f, 0.09f, 0.09f), trim);
      b.box(v3(0, 4.45f, 0), v3(0.09f, 0.45f, 0.09f), trim);
      b.solid_cylinder(v3(0, 0, 0), rs[0], 0.9f);
      b.solid_cylinder(v3(0, 0.9f, 0), 0.2f, 3.9f);
      c.top = 4.9f;
      break;
    }
    case civic_v1_type::kKindFountain: {
      b.frustum(v3(0, -0.2f, 0), v3(0, 0.6f, 0), R * 0.85f, R * 0.85f, 8, st, 0, false, false, kPi / 8.0f);
      b.frustum(v3(0, 0.55f, 0), v3(0, 0.65f, 0), R * 0.9f, R * 0.9f, 8, trim, 0, true, true, kPi / 8.0f);
      b.frustum(v3(0, 0.5f, 0), v3(0, 0.52f, 0), R * 0.78f, R * 0.78f, 16, kWater, 0, true, false);
      b.frustum(v3(0, 0.5f, 0), v3(0, 1.8f, 0), 0.28f, 0.2f, 8, st);
      b.frustum(v3(0, 1.6f, 0), v3(0, 1.85f, 0), 0.3f, 0.85f, 12, st);
      b.frustum(v3(0, 1.82f, 0), v3(0, 1.84f, 0), 0.75f, 0.75f, 12, kWater, 0, true, false);
      b.frustum(v3(0, 1.85f, 0), v3(0, 2.6f, 0), 0.12f, 0.08f, 6, c.t.metal_color);
      // Water falling from the bowl's rim.
      if (b.detail < 2)
        for (int k = 0; k < 6; ++k) {
          const float a = kPi / 3.0f * static_cast<float>(k);
          const vec3 d = v3(foundry::det::cos(a), 0, foundry::det::sin(a)), t = cross(v3(0, 1, 0), d);
          b.quad(d * 0.8f + t * 0.08f + v3(0, 1.8f, 0), d * 0.8f - t * 0.08f + v3(0, 1.8f, 0), d * 1.0f - t * 0.1f + v3(0, 0.55f, 0), d * 1.0f + t * 0.1f + v3(0, 0.55f, 0), lerp(kWater, v3(1, 1, 1), 0.4f), 0, false, d);
        }
      b.solid_cylinder(v3(0, 0, 0), R * 0.85f, 0.65f);
      b.solid_cylinder(v3(0, 0.65f, 0), 0.3f, 1.95f);
      c.top = 2.6f;
      break;
    }
    case civic_v1_type::kKindMaypole: {
      b.frustum(v3(0, -0.3f, 0), v3(0, 7.0f, 0), 0.12f, 0.08f, 8, v3(0.92f, 0.9f, 0.84f), 0, true, false);
      const bool bloom = c.season == civic_v1_spec::kSeasonSpring || c.season == civic_v1_spec::kSeasonSummer;
      for (int k = 0; k < 8; ++k) {
        const float a = kPi * 0.25f * static_cast<float>(k);
        b.box(v3(0.35f * foundry::det::cos(a), 6.4f, 0.35f * foundry::det::sin(a)), v3(0.12f, 0.1f, 0.12f), bloom ? v3(0.9f, 0.4f, 0.6f) : v3(0.35f, 0.5f, 0.3f));
      }
      const vec3 ribbon[6] = {v3(0.85f, 0.2f, 0.2f), v3(0.2f, 0.4f, 0.85f), v3(0.95f, 0.8f, 0.2f), v3(0.3f, 0.7f, 0.3f), c.t.accent, v3(0.95f, 0.95f, 0.95f)};
      const int n = b.detail == 2 ? 3 : 6;
      for (int r = 0; r < n; ++r) {
        const float a = 2 * kPi * static_cast<float>(r) / static_cast<float>(n);
        const vec3 d = v3(foundry::det::cos(a), 0, foundry::det::sin(a));
        const vec3 axis = cross(v3(0, 1, 0), d);
        const uint32_t rb = b.bone(0, v3(0, 6.2f, 0), axis, house_v1_bone::kMotionSway, 0.9f, 0.35f, 0.6f, 0.17f * static_cast<float>(r));
        b.quad(v3(0, 6.2f, 0), v3(0, 6.2f, 0) + d * 0.06f + v3(0, -0.06f, 0), d * (R * 0.75f) + v3(0, 1.2f, 0), d * (R * 0.75f - 0.08f) + v3(0, 1.25f, 0), ribbon[r], rb, false, axis);
      }
      b.solid_cylinder(v3(0, 0, 0), 0.14f, 7.0f);
      c.top = 7.1f;
      break;
    }
    case civic_v1_type::kKindStatue: {
      b.block(-R * 0.45f, R * 0.45f, -0.3f, 0.4f, -R * 0.45f, R * 0.45f, c.t.base_color);
      b.block(-R * 0.32f, R * 0.32f, 0.4f, 1.6f, -R * 0.32f, R * 0.32f, st);
      b.block(-R * 0.36f, R * 0.36f, 1.6f, 1.72f, -R * 0.36f, R * 0.36f, trim);
      const vec3 m = c.t.metal_color;
      const float y = 1.72f;
      for (int s = -1; s <= 1; s += 2) b.box(v3(0.14f * static_cast<float>(s), y + 0.45f, 0), v3(0.09f, 0.45f, 0.1f), m);
      b.box(v3(0, y + 1.2f, 0), v3(0.24f, 0.32f, 0.13f), m);
      b.box(v3(0, y + 1.68f, 0), v3(0.12f, 0.14f, 0.12f), m);
      b.beam(v3(0.26f, y + 1.45f, 0), v3(0.45f, y + 1.95f, 0.05f), 0.06f, m);
      b.beam(v3(-0.26f, y + 1.45f, 0), v3(-0.35f, y + 0.95f, 0.08f), 0.06f, m);
      b.beam(v3(0.45f, y + 1.95f, 0.05f), v3(0.5f, y + 2.6f, 0.05f), 0.025f, m * 1.1f);
      b.solid(v3(0, 1.2f, 0), v3(R * 0.45f, 1.2f, R * 0.45f));
      c.top = y + 2.6f;
      break;
    }
    default: {  // a shrine
      b.block(-0.9f, 0.9f, -0.3f, 0.6f, -0.7f, 0.7f, c.t.base_color);
      b.block(-0.7f, 0.7f, 0.6f, 2.0f, -0.5f, 0.5f, st);
      b.window(v3(0, 1.3f, 0.5f), v3(0, 0, 1), 0.7f, 0.9f, trim, kPane * 0.6f, true);
      b.gable(v3(0, 2.0f, 0), v3(1, 0, 0), 0.8f, 0.6f, 0.6f, 0.15f, c.t.roof_color, st);
      const uint32_t glow = b.bone(0, v3(0, 1.0f, 0.45f), v3(0, 1, 0), house_v1_bone::kMotionFlicker, 3.2f, 0.3f, 0, c.pick(4));
      b.box(v3(0, 0.95f, 0.42f), v3(0.04f, 0.1f, 0.04f), v3(0.95f, 0.92f, 0.8f), 0);
      b.box(v3(0, 1.1f, 0.42f), v3(0.04f, 0.05f, 0.04f), v3(1.0f, 0.75f, 0.3f), glow, false);
      condition::fire(b.out, v3(0, 1.12f, 0.42f), 7u, 1u);  // a candle
      const bool bloom = c.season == civic_v1_spec::kSeasonSpring || c.season == civic_v1_spec::kSeasonSummer;
      if (bloom)
        for (int s = -1; s <= 1; s += 2) b.box(v3(0.55f * static_cast<float>(s), 0.7f, 0.6f), v3(0.15f, 0.1f, 0.1f), c.t.accent);
      b.solid(v3(0, 1.0f, 0), v3(0.9f, 1.0f, 0.7f));
      c.top = 2.8f;
      break;
    }
  }
  c.door = v3(0, 0, minf(R + 0.4f, c.lot.round_r - 0.5f));  // in front, inside the lot
}

void lamps(Ctx &c) {
  const float z = c.door.z - 0.3f, x = c.door.x;
  for (int s = -1; s <= 1; s += 2) {
    const vec3 p = v3(x + static_cast<float>(s) * 1.5f, 0, z);
    c.b.cylinder(p, p + v3(0, 2.6f, 0), 0.05f, kIron);
    const uint32_t glow = c.b.bone(0, p + v3(0, 2.7f, 0), v3(0, 1, 0), house_v1_bone::kMotionFlicker, 2.8f, 0.2f, 0, c.pick(20 + static_cast<uint32_t>(s + 1)));
    c.b.box(p + v3(0, 2.75f, 0), v3(0.1f, 0.14f, 0.1f), v3(1.0f, 0.82f, 0.45f), glow, false);
    c.b.solid_cylinder(p, 0.08f, 2.6f);
    condition::fire(c.b.out, p + v3(0, 2.78f, 0), c.th ? c.th->torch_fire : 2u, 1u);
  }
}

// ---- wondrous: the exotic, on top of grand ----

const vec3 kGold = {0.95f, 0.78f, 0.30f};

// A round tower with a dome and a crystal finial, standing at (x, z) up to h.
void wonder_tower(Ctx &c, float x, float z, float r, float h) {
  B &b = c.b;
  b.frustum(v3(x, -0.4f, z), v3(x, h, z), r * 1.05f, r, b.sides, c.t.wall_color * 1.02f, 0, true, false);
  b.frustum(v3(x, h, z), v3(x, h + 0.2f, z), r + 0.15f, r + 0.15f, b.sides, c.t.trim_color);
  theme::dome(b, v3(x, h + 0.2f, z), r, r * 1.1f, b.sides, b.detail == 2 ? 3 : 5, c.t.metal_color, 0, false);
  theme::crystals(b, v3(x, h + 0.2f + r * 1.1f, z), 1.3f, c.th ? c.th->accent : c.t.accent, c.seed + static_cast<uint32_t>(static_cast<int>(x * 10.0f)));
  b.solid_cylinder(v3(x, 0, z), r * 1.05f, h);
  c.top = maxf(c.top, h + 0.2f + r * 1.1f + 1.3f);
}

// A gilded figure on a plinth (a statue's colossus, a temple's crown), facing +z, `s` tall.
void figure(Ctx &c, vec3 at, float s, vec3 m) {
  B &b = c.b;
  for (int k = -1; k <= 1; k += 2) b.box(at + v3(0.08f * s * static_cast<float>(k), 0.22f * s, 0), v3(0.05f * s, 0.22f * s, 0.06f * s), m);
  b.box(at + v3(0, 0.6f * s, 0), v3(0.14f * s, 0.18f * s, 0.08f * s), m);
  b.box(at + v3(0, 0.88f * s, 0), v3(0.07f * s, 0.08f * s, 0.07f * s), m);
  b.beam(at + v3(0.15f * s, 0.72f * s, 0), at + v3(0.26f * s, 1.05f * s, 0.03f * s), 0.035f * s, m);
  b.beam(at + v3(-0.15f * s, 0.72f * s, 0), at + v3(-0.2f * s, 0.45f * s, 0.05f * s), 0.035f * s, m);
}

void wondrous(Ctx &c) {
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z0 = c.z0(), z1 = c.z1(), cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
  const uint32_t torch = c.th ? c.th->torch_fire : 2u;
  switch (c.t.kind) {
    case civic_v1_type::kKindHall:
    case civic_v1_type::kKindMarketHall:
    case civic_v1_type::kKindBank: {
      // Twin towers at the front corners, domed, crowned with crystals.
      const float r = clampf((x1 - x0) * 0.13f, 1.0f, 1.8f), h = maxf(c.top * 0.9f, 7.0f);
      wonder_tower(c, x0 + r + 0.05f, z1 - r - 0.05f, r, h);
      wonder_tower(c, x1 - r - 0.05f, z1 - r - 0.05f, r, h);
      break;
    }
    case civic_v1_type::kKindChurch: {
      // Flying buttresses down the nave, a rose window on the tower, a gilded finial.
      const float nw = minf(x1 - x0 - 0.4f, 7.5f * c.g), tw = minf(nw * 0.5f, 3.2f), zb = z0 + nw * 0.3f, zt0 = z1 - tw;
      const float wh = c.t.wall_height * (0.9f + 0.1f * static_cast<float>(c.grand));
      const float out = minf(nw * 0.5f + 1.1f, (x1 - x0) * 0.5f + 0.25f);
      for (int s = -1; s <= 1; s += 2)
        for (float z = zb + 1.0f; z < zt0 - 0.5f; z += 2.4f) {
          const float px = cx + static_cast<float>(s) * out;
          b.block(px - 0.25f, px + 0.25f, -0.4f, wh * 0.85f, z - 0.3f, z + 0.3f, c.t.wall_color * 0.95f, 0, true);
          b.frustum(v3(px, wh * 0.85f, z), v3(px, wh * 0.85f + 1.0f, z), 0.22f, 0.0f, 4, c.t.roof_color, 0, false, true, kPi * 0.25f);
          b.beam(v3(px, wh * 0.8f, z), v3(cx + static_cast<float>(s) * nw * 0.5f, wh * 0.95f, z), 0.12f, c.t.wall_color * 0.92f);
          b.solid(v3(px, wh * 0.42f, z), v3(0.25f, wh * 0.42f, 0.3f));
        }
      const float th = wh * (c.t.spire ? 1.9f : 1.6f) + static_cast<float>(c.grand);
      theme::window(b, v3(cx, th * 0.62f, z1 + 0.01f), v3(0, 0, 1), tw * 0.5f, tw * 0.5f, Th::kWindowsRound, c.t.trim_color,
                    lerp(c.t.accent, v3(0.6f, 0.3f, 0.7f), 0.5f));
      b.box(v3(cx, c.top + 0.15f, (zt0 + z1) * 0.5f), v3(0.15f, 0.15f, 0.15f), kGold);
      b.box(v3(cx, c.top + 0.6f, (zt0 + z1) * 0.5f), v3(0.04f, 0.35f, 0.04f), kGold);
      c.top += 0.95f;
      break;
    }
    case civic_v1_type::kKindTemple: {
      // Braziers burning either side of the steps, and a gilded figure on the pediment.
      for (int s = -1; s <= 1; s += 2) {
        const vec3 p = v3(s < 0 ? x0 + 0.35f : x1 - 0.35f, 0, z1 - 0.3f);
        b.frustum(p, p + v3(0, 1.1f, 0), 0.12f, 0.09f, 6, c.t.trim_color);
        b.frustum(p + v3(0, 1.1f, 0), p + v3(0, 1.4f, 0), 0.15f, 0.35f, 8, c.t.metal_color);
        b.solid_cylinder(p, 0.3f, 1.4f);
        condition::fire(b.out, p + v3(0, 1.38f, 0), 8u, 1u);
      }
      figure(c, v3(cx, c.top - 0.1f, z1 - 0.5f), 1.6f, kGold);
      c.top += 1.6f;
      break;
    }
    case civic_v1_type::kKindTower: {
      // A ring of crystals circling the tower's crown, and a spirit fire at its point.
      const float tcz = (c.z0() + c.z1()) * 0.5f - 0.3f;
      const float r = minf(c.x1() - c.x0(), c.z1() - c.z0()) * 0.32f;
      const float h = c.t.wall_height * (2.4f + 0.3f * static_cast<float>(c.grand));
      const vec3 hub = v3(cx, h * 0.92f, tcz);
      const uint32_t ring = b.bone(0, hub, v3(0, 1, 0), house_v1_bone::kMotionSpin, 0.12f, 0, 0, c.pick(41));
      const vec3 glow = c.th && c.th->name != "heartland" ? c.th->accent : lerp(c.t.accent, v3(0.7f, 0.85f, 1.0f), 0.5f);
      for (int k = 0; k < 8; ++k) {
        const float a = kPi * 0.25f * static_cast<float>(k);
        const vec3 at = hub + v3(foundry::det::cos(a) * (r + 1.3f), 0.35f * static_cast<float>(k % 2), foundry::det::sin(a) * (r + 1.3f));
        b.frustum(at, at + v3(0, 0.75f, 0), 0.26f, 0.0f, 5, glow, ring, true, false);
        b.frustum(at, at - v3(0, 0.45f, 0), 0.26f, 0.0f, 5, glow * 0.85f, ring, false, false);
      }
      // An orb over the point.
      theme::dome(b, v3(cx, c.top + 0.35f, tcz), 0.35f, 0.35f, 8, 3, glow, 0, false);
      theme::dome(b, v3(cx, c.top + 0.35f, tcz), 0.35f, -0.35f, 8, 3, glow, 0, false);
      condition::fire(b.out, v3(cx, c.top + 0.75f, tcz), torch == 2u ? 6u : torch, 1u);
      c.top += 1.3f;
      break;
    }
    case civic_v1_type::kKindWell: {
      // A gilded dome on four columns over the well.
      for (int k = 0; k < 4; ++k) {
        const float a = kPi * 0.5f * static_cast<float>(k) + kPi * 0.25f;
        const vec3 p = v3(1.35f * foundry::det::cos(a), 0, 1.35f * foundry::det::sin(a));
        b.frustum(p, p + v3(0, 3.0f, 0), 0.12f, 0.1f, 6, c.t.trim_color);
        b.solid_cylinder(p, 0.12f, 3.0f);
      }
      b.frustum(v3(0, 3.0f, 0), v3(0, 3.15f, 0), 1.6f, 1.6f, b.sides, c.t.trim_color);
      theme::dome(b, v3(0, 3.15f, 0), 1.5f, 1.2f, b.sides, b.detail == 2 ? 3 : 5, kGold, 0, false);
      c.top = 4.5f;
      break;
    }
    case civic_v1_type::kKindCross: {
      b.frustum(v3(0, 4.9f, 0), v3(0, 5.5f, 0), 0.0f, 0.0f, 4, kGold);
      theme::crystals(b, v3(0, 4.9f, 0), 0.7f, c.th ? c.th->accent : c.t.accent, c.seed);
      c.top = 5.7f;
      break;
    }
    case civic_v1_type::kKindFountain: {
      figure(c, v3(0, 2.6f, 0), 1.3f, kGold);
      c.top = 3.9f;
      break;
    }
    case civic_v1_type::kKindMaypole: {
      b.frustum(v3(0, 6.6f, 0), v3(0, 6.75f, 0), 0.8f, 0.8f, b.sides, kGold, 0, true, false);
      c.top = 7.1f;
      break;
    }
    case civic_v1_type::kKindStatue: {
      // The figure raises a burning torch.
      condition::fire(b.out, v3(0.5f, 1.72f + 2.65f, 0.05f), torch, 1u);
      c.top += 0.4f;
      break;
    }
    default: {  // a shrine: an eternal flame in a bowl before it, crystals on its roof
      b.frustum(v3(0, 0, 1.0f), v3(0, 0.7f, 1.0f), 0.1f, 0.08f, 6, c.t.trim_color);
      b.frustum(v3(0, 0.7f, 1.0f), v3(0, 0.9f, 1.0f), 0.1f, 0.25f, 8, c.t.metal_color);
      condition::fire(b.out, v3(0, 0.88f, 1.0f), c.th ? c.th->hearth_fire : 1u, 1u);
      theme::crystals(b, v3(0, 2.55f, 0), 0.5f, c.th ? c.th->accent : c.t.accent, c.seed);
      c.top = maxf(c.top, 3.1f);
      break;
    }
  }
  (void)cz;
}

// ---- the theme's ornaments ----

void ornaments(Ctx &c) {
  if (!c.th || c.th->ornament == Th::kOrnamentNone) return;
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z1 = c.z1();
  const vec3 up = v3(0, 1, 0);
  switch (c.th->ornament) {
    case Th::kOrnamentStandingStones:
      for (int s = -1; s <= 1; s += 2) {
        const float x = s < 0 ? x0 + 0.4f : x1 - 0.4f;
        theme::standing_stone(b, v3(x, 0, z1 + 0.7f), 2.2f + 0.5f * c.pick(60 + static_cast<uint32_t>(s + 1)), v3(0.56f, 0.56f, 0.53f), c.seed + static_cast<uint32_t>(s + 2));
        b.solid(v3(x, 1.1f, z1 + 0.7f), v3(0.35f, 1.1f, 0.25f));
      }
      break;
    case Th::kOrnamentLanterns:
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = theme::lantern(b, v3(c.door.x + static_cast<float>(s) * 1.0f, 2.4f, z1 + 0.02f), v3(0, 0, 1), kIron, v3(1.0f, 0.86f, 0.45f));
        condition::fire(b.out, at, c.th->torch_fire, 1u);
      }
      break;
    case Th::kOrnamentCrystals:
      theme::crystals(b, v3(c.lot.cx, c.top - 0.1f, (c.z0() + z1) * 0.5f), 1.0f, c.th->accent, c.seed);
      c.top += 1.0f;
      break;
    case Th::kOrnamentDragonHeads:
      if (c.ridge_y > 0) {
        theme::dragon_head(b, v3(c.ridge_x0, c.ridge_y - 0.1f, c.ridge_z), v3(-1, 0, 0), c.t.trim_color, c.th->accent);
        theme::dragon_head(b, v3(c.ridge_x1, c.ridge_y - 0.1f, c.ridge_z), v3(1, 0, 0), c.t.trim_color, c.th->accent);
      }
      break;
    case Th::kOrnamentPrayerFlags: {
      const vec3 from = v3(c.lot.cx, c.top - 0.3f, (c.z0() + z1) * 0.5f);
      for (int s = -1; s <= 1; s += 2) {
        const vec3 to = v3(s < 0 ? x0 : x1, 2.2f, z1 + 0.4f);
        b.beam(v3(to.x, 0, to.z), to, 0.04f, v3(0.45f, 0.34f, 0.22f));
        const uint32_t fb = b.bone(0, from, normalize(to - from), house_v1_bone::kMotionSway, 0.8f, 0.25f, 0.9f, c.pick(70 + static_cast<uint32_t>(s + 1)));
        theme::prayer_flags(b, from, to, fb, c.seed + static_cast<uint32_t>(s + 1));
      }
      break;
    }
    case Th::kOrnamentCarvings:
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = v3(c.door.x + static_cast<float>(s) * 1.2f, 0, z1 + 0.15f);
        theme::frustum(b, at, at + up * 3.0f, 0.1f, 0.08f, 6, c.t.trim_color, 0, true);
        b.box(at + up * 3.1f, v3(0.1f, 0.1f, 0.1f), c.t.trim_color * 1.1f);
      }
      break;
    default: break;
  }
}

void banners(Ctx &c, float x0, float x1, float z, float y) {
  for (int s = -1; s <= 1; s += 2) {
    const float x = (x0 + x1) * 0.5f + static_cast<float>(s) * (x1 - x0) * 0.3f;
    c.b.banner(v3(x, y, z + 0.08f), 1.4f, v3(0, 0, 1), c.t.accent, c.pick(30 + static_cast<uint32_t>(s + 1)));
  }
}

}  // namespace

bool run(const civic_v1_spec &in, civic_v1_model &out, std::string &error) {
  // The type, dressed in the village's theme (heartland keeps the type's own).
  civic_v1_type t = in.type;
  const Th &th = in.theme;
  t.wall_color = theme::dress(t.wall_color, th.wall_color, th);
  t.trim_color = theme::dress(t.trim_color, th.trim_color, th);
  t.roof_color = theme::dress(t.roof_color, th.roof_color, th);
  t.accent = theme::dress(t.accent, th.accent, th);
  if (in.stage > civic_v1_spec::kStageRuin) {
    error = "stage out of range";
    return false;
  }
  if (in.size > 3 || in.grandeur > 3 || in.season > 3 || in.detail > 2 || t.kind > 11 || t.size > 3 || t.walls > 3 || t.roof > 4 || t.symbol > 9) {
    error = "size, grandeur, season, detail, or the type's kind, size, walls, roof, or symbol out of range";
    return false;
  }
  out = civic_v1_model();
  B b{out};
  b.winter = in.season == civic_v1_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 12 : (in.detail == 1 ? 8 : 6);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_v1_bone::kMotionFixed, 0, 0, 0, 0);
  const Lot lot = make_lot(kLotW[in.size], kLotH[in.size]);
  // Grandeur scales the building within its lot (on a centre lot, never past plain's, so a hall's
  // eaves stay inside it).
  const float g = in.grandeur == 0 || in.size == civic_v1_spec::kSizeCentre ? 0.88f : (in.grandeur == 1 ? 0.95f : 1.0f);
  Ctx c{b, t, lot, g, static_cast<int>(in.grandeur > 2 ? 2 : in.grandeur), in.season, hash_u32(in.seed * 2654435761u ^ hash_str(t.name.c_str()) ^ (in.size * 977u))};
  c.th = &th;
  c.wonder = in.grandeur == civic_v1_spec::kGrandeurWondrous;
  const bool is_centre = t.kind >= civic_v1_type::kKindWell;
  switch (t.kind) {
    case civic_v1_type::kKindHall: hall(c, false); break;
    case civic_v1_type::kKindMarketHall: hall(c, true); break;
    case civic_v1_type::kKindChurch: church(c); break;
    case civic_v1_type::kKindTemple: temple(c); break;
    case civic_v1_type::kKindBank: bank(c); break;
    case civic_v1_type::kKindTower: tower(c); break;
    default: centre(c); break;
  }
  if (!is_centre && in.size != civic_v1_spec::kSizeCentre) {  // a centre lot has no room for them
    if (t.lamps || in.grandeur >= 2) lamps(c);
    if (t.banners || in.grandeur >= 2) banners(c, c.x0(), c.x1(), c.z1(), 0.0f);
    if (in.grandeur >= 1) ornaments(c);
  }
  if (c.wonder && (is_centre || in.size != civic_v1_spec::kSizeCentre)) wondrous(c);
  theme::moss(out, 0, th.moss * 0.6f, c.seed);
  // ---- the stage: planned, half built, finished, or a ruin (condition.h); the fires it burns from ----
  if (in.stage == civic_v1_spec::kStagePlanning) condition::planning(out, c.seed);
  else if (in.stage == civic_v1_spec::kStageHalf) condition::half_built(out, c.seed);
  else if (in.stage == civic_v1_spec::kStageRuin) condition::ruin(out, c.seed, th.moss);
  if (!is_centre && (in.stage == civic_v1_spec::kStageFinished || in.stage == civic_v1_spec::kStageRuin))
    condition::blaze_points(out, th.blaze_fire, c.seed ^ 0xb1a2e5u, 4 + 2 * static_cast<int>(in.size));
  // The doorway: a sensor just outside the door, no taller than what stands (a ruined centre is low).
  float standing = 0.6f;
  for (const vec3 &p : out.positions) standing = maxf(standing, p.y);
  const float dh = minf(0.9f, standing * 0.5f);
  b.solid(v3(c.door.x, dh, c.door.z - 0.2f), v3(0.6f, dh, 0.4f), 0, channels::physics_v1_body::kPurposeSensor, "doorway");
  out.doorway = c.door;
  out.doorway_heading = 0;
  // ---- size, the lot, guides, the marker ----
  float top = 0, reach = 0, ground = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    const float d = foundry::det::hypot(p.x, p.z);
    reach = maxf(reach, d);
    if (p.y < 0.4f) ground = maxf(ground, d);
  }
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      const float r = sh.type == channels::physics_v1_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
    }
  out.height = top;
  out.radius = reach;
  out.footprint = in.size;
  out.footprint_cells = lot.round;
  out.footprint_radius = ground;
  out.cells = std::vector<int32_t>(lot.cells.begin(), lot.cells.end());
  if (!is_centre) {  // a centre stands in its square, its road all round it
    const std::vector<int> doors = door_cells(lot, c.door.x);
    out.doors = std::vector<int32_t>(doors.begin(), doors.end());
  }
  lot_box(lot, top, out.lot_min, out.lot_max);
  for (uint32_t s = 0; s < kLotSizes; ++s) {
    const Lot other = make_lot(kLotW[s], kLotH[s]);
    for (size_t k = 0; k + 1 < other.cells.size(); k += 2) {
      out.guide_cells.push_back(static_cast<int32_t>(s));
      out.guide_cells.push_back(other.cells[k]);
      out.guide_cells.push_back(other.cells[k + 1]);
    }
    out.guide_tones.push_back(s == in.size ? 0u : 2u);
  }
  out.marker = v3(is_centre ? 0.0f : lot.cx, maxf(c.top, 2.0f) + 0.6f, is_centre ? 0.0f : lot.cz);
  return true;
}

}  // namespace civic_build_v1

FOUNDRY_SYSTEM(civic_build_v1, channels::civic_v1_spec, channels::civic_v1_model)
