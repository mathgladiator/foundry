// waterfront_build: a building that comes off land into water (design/planner.md,
// "Waterfronts"): on a lot whose back two rows are land and the rest water (+z is the water).
//
// 1. The lot: small (rect3x4), medium (rect4x5), or large (rect5x6), anchored on the land row nearest
//    the water; the shore runs between it and the first water row. The water lies `drop` below the
//    lot's ground.
// 2. The shore: a quay wall along it (stone for a warehouse or a boathouse, timber otherwise).
// 3. By kind:
//    - a pier: a plank deck on posts out over the water, boats moored along it;
//    - a boathouse: a shed across the shore on posts, its gable open to the water, a boat inside;
//    - a warehouse: a big shed on the land, its doors to the quay, a crane whose jib swings over the
//      water, and a cargo boat alongside;
//    - a fishery: a hut on the land, a short jetty, racks of nets drying (they sway), and boats.
// 4. Barrels and crates on the quay, and a lamp at the water's end, as the type says.
// 5. Boats rock about their keels (house_bone sway along the keel), nets and the crane's jib
//    sway, the lamp flickers: animated by house_animate. Snow on what faces up in winter.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "condition.h"
#include "theme.h"
#include "channels/waterfront_model.h"
#include "channels/waterfront_spec.h"
#include "packer/waterfront_model.h"
#include "packer/waterfront_spec.h"

namespace waterfront_build {

using channels::house_bone;
using channels::waterfront_model;
using channels::waterfront_spec;
using channels::waterfront_type;

namespace {

constexpr float kSqrt3 = 1.7320508f;
const vec3 kStone = {0.56f, 0.55f, 0.52f};
const vec3 kIron = {0.2f, 0.2f, 0.22f};
const vec3 kRope = {0.75f, 0.68f, 0.5f};

struct Lot {
  int w, h, aq, ar;
  uint32_t round;
};
const Lot kLots[3] = {{3, 4, 1, 1, 19}, {4, 5, 1, 1, 37}, {5, 6, 2, 1, 61}};

float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
float cell_z(int r) { return 1.5f * static_cast<float>(r); }
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
float absf_(float v) { return v < 0 ? -v : v; }

// A rowing boat (or a cargo boat when big) at the water, along +z, rocking about its keel.
void boat(Builder<waterfront_model> &b, vec3 at, float len, vec3 paint, float phase, bool big) {
  const float wid = big ? 1.1f : 0.6f, depth = big ? 0.45f : 0.3f;
  const uint32_t bone = b.bone(0, at, v3(0, 0, 1), house_bone::kMotionSway, 0.35f, 0.14f, 0.25f, phase);
  // The hull: a middle box and two narrower ends, the bow raised.
  b.box(at + v3(0, depth * 0.5f, 0), v3(wid * 0.5f, depth * 0.5f, len * 0.3f), paint, bone);
  b.box(at + v3(0, depth * 0.55f, len * 0.4f), v3(wid * 0.32f, depth * 0.5f, len * 0.12f), paint * 0.9f, bone);
  b.box(at + v3(0, depth * 0.5f, -len * 0.38f), v3(wid * 0.38f, depth * 0.48f, len * 0.1f), paint * 0.9f, bone);
  b.box(at + v3(0, depth + 0.01f, 0), v3(wid * 0.42f, 0.02f, len * 0.28f), v3(0.42f, 0.3f, 0.18f), bone, false);  // the floor
  if (big) b.cylinder(at + v3(0, depth, len * 0.1f), at + v3(0, depth + 3.2f, len * 0.1f), 0.06f, v3(0.45f, 0.32f, 0.2f), bone);
}

}  // namespace

bool run(const waterfront_spec &in, waterfront_model &out, std::string &error) {
  // The type, dressed in the village's theme (heartland keeps the type's own).
  waterfront_type t = in.type;
  const channels::village_theme &th = in.theme;
  t.wall_color = theme::dress(t.wall_color, th.wall_color, th);
  t.roof_color = theme::dress(t.roof_color, th.roof_color, th);
  t.wood_color = theme::dress(t.wood_color, th.trim_color, th);
  if (in.condition > waterfront_spec::kConditionRuin) {
    error = "condition out of range";
    return false;
  }
  if (in.size > 2 || in.season > 3 || in.detail > 2 || t.kind > 3) {
    error = "size, season, detail, or kind out of range";
    return false;
  }
  out = waterfront_model();
  Builder<waterfront_model> b{out};
  b.winter = in.season == waterfront_spec::kSeasonWinter;
  b.sides = in.detail == 0 ? 10 : (in.detail == 1 ? 7 : 5);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_bone::kMotionFixed, 0, 0, 0, 0);
  const uint32_t seed = hash_u32(in.seed * 2654435761u ^ hash_str(t.name.c_str()) ^ (in.size * 977u));
  auto pick = [&](uint32_t salt) { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); };
  const bool hi = in.detail == 0;
  const float water = -clampf(in.drop, 0.0f, 4.0f);

  // ---- 1. the lot ----
  const Lot &L = kLots[in.size];
  float land_x0 = -1e9f, land_x1 = 1e9f, wet_x0 = -1e9f, wet_x1 = 1e9f;
  float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
  for (int r = 0; r < L.h; ++r) {
    float lo = 1e9f, hi_x = -1e9f;
    for (int i = 0; i < L.w; ++i) {
      const int q = i - r / 2 - L.aq, rr = r - L.ar;
      out.cells.push_back(q);
      out.cells.push_back(rr);
      if (rr >= 1) {
        out.wet.push_back(q);
        out.wet.push_back(rr);
      }
      const float x = cell_x(q, rr), z = cell_z(rr);
      lo = minf(lo, x);
      hi_x = maxf(hi_x, x);
      x0 = minf(x0, x - 0.5f * kSqrt3);
      x1 = maxf(x1, x + 0.5f * kSqrt3);
      z0 = minf(z0, z - 1.0f);
      z1 = maxf(z1, z + 1.0f);
    }
    // The span every row of the part shares, inset to stay inside the cells.
    if (r - L.ar <= 0) {
      land_x0 = maxf(land_x0, lo - 0.55f);
      land_x1 = minf(land_x1, hi_x + 0.55f);
    } else {
      wet_x0 = maxf(wet_x0, lo - 0.55f);
      wet_x1 = minf(wet_x1, hi_x + 0.55f);
    }
  }
  const float shore = 0.75f, back = cell_z(-1) - 0.6f, front = z1 - 0.7f;
  const float land_half = 0.5f * (land_x1 - land_x0), land_mid = 0.5f * (land_x0 + land_x1);
  const float wet_half = 0.5f * (wet_x1 - wet_x0), wet_mid = 0.5f * (wet_x0 + wet_x1);

  // ---- 2. the shore: a quay wall ----
  const bool stone = t.kind == waterfront_type::kKindWarehouse || t.kind == waterfront_type::kKindBoathouse;
  b.box(v3(land_mid, 0.5f * (water - 0.4f) + 0.03f, shore), v3(land_half, 0.5f * (0.43f - water), 0.18f), stone ? kStone : t.wood_color * 0.8f, 0);
  b.solid(v3(land_mid, 0.5f * (water - 0.4f) + 0.03f, shore), v3(land_half, 0.5f * (0.43f - water), 0.18f));
  // A deck on posts over the water: from z a to z b, half width hw, centred at x.
  auto deck = [&](float x, float za, float zb, float hw) {
    b.box(v3(x, 0.05f, 0.5f * (za + zb)), v3(hw, 0.06f, 0.5f * (zb - za)), t.wood_color, 0);
    const int n = static_cast<int>((zb - za) / 1.5f) + 1;
    for (int i = 0; i <= n; ++i) {
      const float z = za + (zb - za) * static_cast<float>(i) / static_cast<float>(n);
      for (int s = -1; s <= 1; s += 2) b.beam(v3(x + (hw - 0.08f) * static_cast<float>(s), water - 1.5f, z), v3(x + (hw - 0.08f) * static_cast<float>(s), 0.0f, z), 0.07f, t.wood_color * 0.7f, 0, false);
    }
    b.solid(v3(x, 0.05f, 0.5f * (za + zb)), v3(hw, 0.06f, 0.5f * (zb - za)));
  };
  // A shed: walls from y 0 to wh, half sizes hx, hz, a gable roof with its ridge along z (ridge_z) or x.
  auto shed = [&](vec3 c, float hx, float hz, float wh, bool ridge_z, bool open_front) {
    const vec3 wall = t.wall_color;
    if (open_front) {
      // Three walls: back and sides; the front (+z) open to the water.
      b.box(c + v3(0, wh * 0.5f, -hz + 0.06f), v3(hx, wh * 0.5f, 0.06f), wall, 0);
      for (int s = -1; s <= 1; s += 2) b.box(c + v3((hx - 0.06f) * static_cast<float>(s), wh * 0.5f, 0), v3(0.06f, wh * 0.5f, hz), wall * 0.92f, 0);
      for (int s = -1; s <= 1; s += 2) b.solid(c + v3((hx - 0.06f) * static_cast<float>(s), wh * 0.5f, 0), v3(0.06f, wh * 0.5f, hz));
      b.solid(c + v3(0, wh * 0.5f, -hz + 0.06f), v3(hx, wh * 0.5f, 0.06f));
    } else {
      b.box(c + v3(0, wh * 0.5f, 0), v3(hx, wh * 0.5f, hz), wall, 0);
      b.solid(c + v3(0, wh * 0.5f, 0), v3(hx, wh * 0.5f, hz));
      // Big doors to the quay.
      b.box(c + v3(0, 1.1f, hz + 0.02f), v3(minf(1.0f, hx * 0.4f), 1.1f, 0.03f), t.wood_color * 0.7f, 0);
    }
    // The roof: two slopes and the gables.
    const float span = ridge_z ? hx : hz, len = ridge_z ? hz : hx, rise = span * 0.75f;
    const vec3 up = v3(0, 1, 0);
    for (int s = -1; s <= 1; s += 2) {
      const float fs = static_cast<float>(s);
      vec3 e0, e1, r0, r1;
      if (ridge_z) {
        e0 = c + v3(fs * (span + 0.3f), wh, -len - 0.3f);
        e1 = c + v3(fs * (span + 0.3f), wh, len + 0.3f);
        r0 = c + v3(0, wh + rise, -len - 0.3f);
        r1 = c + v3(0, wh + rise, len + 0.3f);
      } else {
        e0 = c + v3(-len - 0.3f, wh, fs * (span + 0.3f));
        e1 = c + v3(len + 0.3f, wh, fs * (span + 0.3f));
        r0 = c + v3(-len - 0.3f, wh + rise, 0);
        r1 = c + v3(len + 0.3f, wh + rise, 0);
      }
      b.quad(e0, e1, r1, r0, t.roof_color, 0, true, up + (ridge_z ? v3(fs, 0, 0) : v3(0, 0, fs)));
      b.quad(e0, r0, r1, e1, t.roof_color * 0.7f, 0, false, -up);
      // The gable at this end.
      const vec3 g0 = ridge_z ? c + v3(-span, wh, fs * len) : c + v3(fs * len, wh, -span);
      const vec3 g1 = ridge_z ? c + v3(span, wh, fs * len) : c + v3(fs * len, wh, span);
      const vec3 gt = ridge_z ? c + v3(0, wh + rise, fs * len) : c + v3(fs * len, wh + rise, 0);
      if (!(open_front && ridge_z && s > 0)) b.triangle(g0, g1, gt, t.wall_color * 0.9f, 0, false, ridge_z ? v3(0, 0, fs) : v3(fs, 0, 0));
    }
  };

  // ---- 3. by kind ----
  const uint32_t want_boats = t.boats < 1 ? 0 : (t.boats > 4 ? 4 : t.boats);
  const uint32_t boats = want_boats == 0 ? 0 : (in.size + 1 < want_boats ? in.size + 1 : want_boats);
  switch (t.kind) {
    case waterfront_type::kKindPier: {
      deck(wet_mid, shore - 0.2f, front, 0.85f);
      for (uint32_t i = 0; i < boats; ++i) {
        const float side = (i % 2) ? 1.0f : -1.0f;
        const float z = shore + 1.4f + 1.9f * static_cast<float>(i / 2);
        if (z > front - 0.8f || absf_(wet_mid + side * 1.6f) > wet_half + absf_(wet_mid) + 0.2f) continue;
        boat(b, v3(wet_mid + side * 1.55f, water, z + 0.4f), 2.6f, t.accent, pick(10 + i), false);
        b.beam(v3(wet_mid + side * 0.85f, 0.1f, z), v3(wet_mid + side * 1.25f, water + 0.3f, z + 0.3f), 0.015f, kRope, 0, false);
      }
      break;
    }
    case waterfront_type::kKindBoathouse: {
      const float hx = minf(land_half, wet_half) - 0.1f, za = back + 0.3f, zb = front - 0.2f;
      // Posts under the part over the water, and a walkway either side of the slip.
      for (int s = -1; s <= 1; s += 2) deck(land_mid + (hx - 0.45f) * static_cast<float>(s), shore, zb, 0.4f);
      shed(v3(land_mid, 0, 0.5f * (za + zb)), hx, 0.5f * (zb - za), 3.0f, true, true);
      if (boats > 0) boat(b, v3(land_mid, water, 0.5f * (shore + zb) + 0.4f), minf(3.2f, zb - shore - 0.6f), t.accent, pick(20), false);
      break;
    }
    case waterfront_type::kKindWarehouse: {
      const float hx = land_half - 0.15f, za = back + 0.15f, zb = shore - 0.55f;
      shed(v3(land_mid, 0, 0.5f * (za + zb)), hx, 0.5f * (zb - za), 4.2f, false, false);
      if (t.crane) {
        const vec3 post = v3(land_x1 - 0.7f, 0, shore - 0.35f);
        b.cylinder(post, post + v3(0, 5.0f, 0), 0.16f, t.wood_color * 0.8f, 0);
        const uint32_t jib = b.bone(0, post + v3(0, 4.6f, 0), v3(0, 1, 0), house_bone::kMotionSway, 0.05f, 0.35f, 0.0f, pick(30));
        b.beam(post + v3(0, 4.6f, 0), post + v3(-0.4f, 4.9f, 3.0f), 0.08f, t.wood_color * 0.8f, jib);
        b.beam(post + v3(-0.4f, 4.9f, 3.0f), post + v3(-0.4f, 2.4f, 3.0f), 0.015f, kRope, jib, false);
        b.box(post + v3(-0.4f, 2.2f, 3.0f), v3(0.25f, 0.2f, 0.25f), t.wood_color, jib);
        b.solid_cylinder(post, 0.2f, 5.0f);
      }
      if (boats > 0) boat(b, v3(wet_mid, water, 0.5f * (shore + front) + 0.3f), minf(5.0f, front - shore - 0.4f), t.accent, pick(31), true);
      break;
    }
    case waterfront_type::kKindFishery: {
      const float hx = minf(1.6f, land_half - 0.3f);
      shed(v3(land_x0 + hx + 0.2f, 0, 0.5f * (back + shore) - 0.3f), hx, minf(1.2f, 0.5f * (shore - back) - 0.4f), 2.4f, false, false);
      deck(wet_x1 - 0.9f, shore - 0.2f, minf(front, shore + 3.0f), 0.6f);
      if (t.nets) {
        // Racks of nets on the land, swaying.
        const float rx = land_x1 - 0.9f;
        for (int k2 = 0; k2 < (hi ? 2 : 1); ++k2) {
          const float z = back + 0.6f + 1.2f * static_cast<float>(k2);
          b.beam(v3(rx - 0.7f, 0, z), v3(rx - 0.7f, 1.8f, z), 0.04f, t.wood_color, 0);
          b.beam(v3(rx + 0.7f, 0, z), v3(rx + 0.7f, 1.8f, z), 0.04f, t.wood_color, 0);
          b.beam(v3(rx - 0.7f, 1.8f, z), v3(rx + 0.7f, 1.8f, z), 0.03f, t.wood_color, 0);
          const uint32_t net = b.bone(0, v3(rx, 1.8f, z), v3(1, 0, 0), house_bone::kMotionSway, 0.8f, 0.3f, 0.6f, pick(40 + static_cast<uint32_t>(k2)));
          b.quad(v3(rx - 0.65f, 1.78f, z), v3(rx + 0.65f, 1.78f, z), v3(rx + 0.6f, 0.5f, z), v3(rx - 0.6f, 0.5f, z), t.accent * 0.8f, net, false, v3(0, 0, 1));
          b.quad(v3(rx - 0.65f, 1.78f, z), v3(rx - 0.6f, 0.5f, z), v3(rx + 0.6f, 0.5f, z), v3(rx + 0.65f, 1.78f, z), t.accent * 0.7f, net, false, v3(0, 0, -1));
        }
      }
      for (uint32_t i = 0; i < boats; ++i) {
        const float x = wet_x1 - 0.9f - 1.3f * static_cast<float>(i + 1);
        if (x < wet_x0 + 0.4f) break;
        boat(b, v3(x, water, shore + 1.6f), 2.4f, t.accent, pick(50 + i), false);
      }
      break;
    }
    default: break;
  }
  // ---- 4. the quay's barrels and crates, and a lamp ----
  if (t.barrels) {
    const int n = 2 + static_cast<int>(in.size);
    for (int i = 0; i < n; ++i) {
      const float x = land_x0 + 0.5f + (land_x1 - land_x0 - 1.0f) * pick(60 + static_cast<uint32_t>(i));
      if (i % 2) b.cylinder(v3(x, 0, shore - 0.5f), v3(x, 0.65f, shore - 0.5f), 0.22f, t.wood_color * 0.9f, 0);
      else b.box(v3(x, 0.22f, shore - 0.5f), v3(0.22f, 0.22f, 0.22f), t.wood_color * 1.1f, 0);
    }
  }
  if (t.lamp) {
    const vec3 p = v3(t.kind == waterfront_type::kKindFishery ? wet_x1 - 0.4f : wet_mid + 0.65f, 0.1f,
                      t.kind == waterfront_type::kKindPier ? front - 0.3f : shore + 0.1f);
    b.cylinder(p, p + v3(0, 2.4f, 0), 0.05f, kIron, 0);
    const uint32_t glow = b.bone(0, p + v3(0, 2.4f, 0), v3(0, 1, 0), house_bone::kMotionFlicker, 3.0f, 0.2f, 0, pick(70));
    b.box(p + v3(0, 2.45f, 0), v3(0.1f, 0.14f, 0.1f), v3(1.0f, 0.82f, 0.45f), glow, false);
    condition::fire(out, p + v3(0, 2.48f, 0), th.torch_fire, 1u);
  }
  theme::moss(out, 0, th.moss * 0.6f, seed);
  // A ruin: broken down, its boats gone, overgrown (condition.h); and the fires it burns from.
  if (in.condition == waterfront_spec::kConditionRuin) condition::ruin(out, seed, th.moss);
  condition::blaze_points(out, th.blaze_fire, seed ^ 0xb1a2e5u, 3 + 2 * static_cast<int>(in.size));

  // ---- size and the lot ----
  float top = 0, reach = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
  }
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z));
  out.height = top;
  out.radius = reach;
  out.footprint = in.size;
  out.footprint_cells = L.round;
  out.footprint_radius = reach;
  out.lot_min = v3(x0, water - 2.0f, z0);
  out.lot_max = v3(x1, top, z1);
  return true;
}

}  // namespace waterfront_build

FOUNDRY_SYSTEM(waterfront_build, channels::waterfront_spec, channels::waterfront_model)
