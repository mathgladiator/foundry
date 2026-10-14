// castle_build_v1: one part of a castle (design/planner.md, "Castles"). A castle is a plan's keep
// ward: a small village of these parts round a bailey inside the keep's wall, with an outer ward
// beyond; walls and gates are wall_v1 and gate_v1, laid on the plan's lines.
//
// 1. The lot: small (rect6x5), medium (rect8x6), large (rect10x7), or keep (rect8x8), anchored at
//    the cell nearest its middle, its front (+z) toward the bailey.
// 2. By kind:
//    - a keep: a great square tower of storeys on a battered plinth, windows by storey (slits low,
//      the part's windows high), a parapet and battlements, corner turrets rising above it with
//      their own roofs, a roof inside the parapet, and a forebuilding stair up its front to the
//      first-floor door;
//    - a hall: a tall stone hall along the lot, buttresses between its windows, a steep roof with a
//      louvre on its ridge, a porch at its door, a chimney's smoke;
//    - a chapel: a nave toward the back, an apse, arched windows, a bellcote on its front gable
//      (the bell swings);
//    - a range: a long, low building against the lot's back (barracks, stables, a kitchen,
//      storehouses), its doors along its front (a stable's row of half doors, an arch, or doors),
//      chimneys smoking;
//    - a tower: round, standing alone, slits, a cone roof or battlements.
// 3. Banners on its tops that sway (house_v1_bone, animated by house_animate_v1).
// 4. Its lot's cells, box, every lot as guides, the doorway, the marker, and collision.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "channels/castle_v1_model.h"
#include "channels/castle_v1_spec.h"
#include "condition.h"
#include "lot.h"
#include "theme.h"
#include "packer/castle_v1_model.h"
#include "packer/castle_v1_spec.h"

namespace castle_build_v1 {

using channels::castle_v1_model;
using channels::castle_v1_part;
using channels::castle_v1_spec;
using channels::house_v1_bone;

namespace {

using B = Builder<castle_v1_model>;
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
const vec3 kPane = {0.1f, 0.1f, 0.13f};
const vec3 kIron = {0.2f, 0.2f, 0.22f};

constexpr int kLotSizes = 4;
const int kLotW[kLotSizes] = {6, 8, 10, 8}, kLotH[kLotSizes] = {5, 6, 7, 8};

using Th = channels::village_v1_theme;

struct Ctx {
  B &b;
  const castle_v1_part &p;
  const Lot &lot;
  uint32_t season, seed;
  const Th *th = nullptr;  // the village's theme
  uint32_t form() const { return th ? th->roof : 0u; }
  vec3 door = v3(0, 0, 0);
  float top = 0;
  float pick(uint32_t salt) const { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); }
  float x0() const { return lot.cx - lot.hx + 0.3f; }
  float x1() const { return lot.cx + lot.hx - 0.3f; }
  float z0() const { return lot.cz - lot.hz + 0.3f; }
  float z1() const { return lot.cz + lot.hz - 0.9f; }
  float storey() const { return clampf(p.story_height, 2.5f, 6.0f); }
  int storeys() const { return static_cast<int>(p.stories < 1 ? 1 : (p.stories > 5 ? 5 : p.stories)); }
};

void stone_block(Ctx &c, float x0, float x1, float z0, float z1, float y1, float batter) {
  B &b = c.b;
  if (batter > 0) {
    b.block(x0 - batter, x1 + batter, -0.5f, 1.2f, z0 - batter, z1 + batter, c.p.stone_color * 0.93f, 0, false);
    b.block(x0 - batter * 0.5f, x1 + batter * 0.5f, 1.2f, 1.45f, z0 - batter * 0.5f, z1 + batter * 0.5f, c.p.trim_color, 0, false);
  }
  b.block(x0, x1, -0.4f, y1, z0, z1, c.p.stone_color, 0, false);
  b.solid(v3((x0 + x1) * 0.5f, y1 * 0.5f, (z0 + z1) * 0.5f), v3((x1 - x0) * 0.5f + batter, y1 * 0.5f, (z1 - z0) * 0.5f + batter));
}

// Windows round a block's four faces, a row per storey: slits low, the part's windows above.
void windows(Ctx &c, float x0, float x1, float z0, float z1, int from_storey, bool low_slits) {
  B &b = c.b;
  if (b.detail == 2) return;
  const float sh = c.storey();
  for (int s = from_storey; s < c.storeys(); ++s) {
    const bool slit = low_slits && s == 0;
    const float y = sh * (static_cast<float>(s) + 0.55f);
    const float w = slit ? 0.14f : (c.p.windows == castle_v1_part::kWindowsTracery ? 0.8f : 0.55f);
    const float h = slit ? 0.8f : sh * (c.p.windows == castle_v1_part::kWindowsTracery ? 0.6f : 0.42f);
    const bool arched = !slit && c.p.windows != castle_v1_part::kWindowsSlits;
    for (int f = 0; f < 4; ++f) {
      const bool along_x = f < 2;
      const float len = along_x ? x1 - x0 : z1 - z0;
      const int n = maxi_(1, static_cast<int>(len / 2.4f));
      for (int i = 0; i < n; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        vec3 p, nn;
        if (along_x) {
          nn = v3(0, 0, f == 0 ? 1.0f : -1.0f);
          p = v3(x0 + len * u, y, f == 0 ? z1 : z0);
        } else {
          nn = v3(f == 2 ? 1.0f : -1.0f, 0, 0);
          p = v3(f == 2 ? x1 : x0, y, z0 + len * u);
        }
        b.window(p, nn, w, h, c.p.trim_color, kPane, arched);
      }
    }
  }
}

// A parapet with battlements round a rectangle at height y.
void parapet(Ctx &c, float x0, float x1, float z0, float z1, float y) {
  B &b = c.b;
  b.block(x0 - 0.1f, x1 + 0.1f, y, y + 0.12f, z0 - 0.1f, z1 + 0.1f, c.p.trim_color);
  if (!c.p.crenellated) return;
  const float t = 0.35f, h = 0.6f;
  b.block(x0, x1, y + 0.12f, y + 0.5f, z1 - t, z1, c.p.stone_color * 0.97f, 0, true);
  b.block(x0, x1, y + 0.12f, y + 0.5f, z0, z0 + t, c.p.stone_color * 0.97f, 0, true);
  b.block(x0, x0 + t, y + 0.12f, y + 0.5f, z0 + t, z1 - t, c.p.stone_color * 0.97f, 0, true);
  b.block(x1 - t, x1, y + 0.12f, y + 0.5f, z0 + t, z1 - t, c.p.stone_color * 0.97f, 0, true);
  if (b.detail == 2) return;
  b.merlons(v3(x0, y + 0.5f, z1 - t * 0.5f), v3(x1, y + 0.5f, z1 - t * 0.5f), 0.5f, h, t, c.p.stone_color * 0.97f);
  b.merlons(v3(x0, y + 0.5f, z0 + t * 0.5f), v3(x1, y + 0.5f, z0 + t * 0.5f), 0.5f, h, t, c.p.stone_color * 0.97f);
  b.merlons(v3(x0 + t * 0.5f, y + 0.5f, z0 + t), v3(x0 + t * 0.5f, y + 0.5f, z1 - t), 0.5f, h, t, c.p.stone_color * 0.97f);
  b.merlons(v3(x1 - t * 0.5f, y + 0.5f, z0 + t), v3(x1 - t * 0.5f, y + 0.5f, z1 - t), 0.5f, h, t, c.p.stone_color * 0.97f);
}

// A roof over a rectangle at height y: a gable along its longer side, or (keep) a pyramid.
float roof(Ctx &c, float x0, float x1, float z0, float z1, float y, bool pyramid) {
  if (c.p.roof == castle_v1_part::kRoofNone) return 0;
  const float hx = (x1 - x0) * 0.5f, hz = (z1 - z0) * 0.5f;
  const vec3 mid = v3((x0 + x1) * 0.5f, y, (z0 + z1) * 0.5f);
  // The theme's form (a dome, a flat roof, turf, a sweep) over the part's own.
  const uint32_t form = c.form();
  if (form == Th::kRoofDome || form == Th::kRoofFlat || form == Th::kRoofTurf || form == Th::kRoofSweep || form == Th::kRoofCone) {
    theme::Body body;
    body.cx = mid.x;
    body.cz = mid.z;
    body.hx = hx;
    body.hz = hz;
    const float rise = theme::roof(c.b, body, pyramid && form == Th::kRoofSweep ? Th::kRoofHip : form, y, 45.0f, c.p.roof_color, c.p.trim_color, c.p.stone_color,
                                   c.seed, hz > hx) - y;
    c.b.solid(mid + v3(0, rise * 0.4f, 0), v3(hx * 0.8f, maxf(rise * 0.4f, 0.1f), hz * 0.8f));
    return rise;
  }
  if (pyramid) {
    const float rise = minf(hx, hz) * 1.1f;
    c.b.pyramid(mid, v3(1, 0, 0), v3(0, 0, 1), hx, hz, rise, c.p.roof_color);
    return rise;
  }
  const bool along_x = hx >= hz;
  const float rise = (along_x ? hz : hx) * 1.1f;
  c.b.gable(mid, along_x ? v3(1, 0, 0) : v3(0, 0, 1), along_x ? hx : hz, along_x ? hz : hx, rise, 0.3f, c.p.roof_color, c.p.stone_color);
  c.b.solid(mid + v3(0, rise * 0.4f, 0), v3(hx * 0.8f, rise * 0.4f, hz * 0.8f));
  return rise;
}

// Doors on the face z = z at x: by the part's doors.
void doors(Ctx &c, float x, float z, float w, float y0) {
  B &b = c.b;
  const float h = c.p.doors == castle_v1_part::kDoorsArch ? 3.0f : 2.3f;
  const uint32_t bone = b.bone(0, v3(x - w * 0.5f, y0, z), v3(0, 1, 0), house_v1_bone::kMotionHinge, 0, 1.4f, 0, 0);
  b.box(v3(x, y0 + h * 0.5f, z + 0.03f), v3(w * 0.5f - 0.02f, h * 0.5f, 0.04f), c.p.door_color, bone);
  if (b.detail < 2)
    for (float y : {h * 0.25f, h * 0.75f}) b.box(v3(x, y0 + y, z + 0.08f), v3(w * 0.5f - 0.08f, 0.04f, 0.01f), kIron, bone, false);
  b.box(v3(x, y0 + h + 0.1f, z + 0.05f), v3(w * 0.5f + 0.15f, 0.1f, 0.06f), c.p.trim_color);
  if (c.p.doors == castle_v1_part::kDoorsArch)
    b.triangle(v3(x - w * 0.5f - 0.15f, y0 + h + 0.2f, z + 0.06f), v3(x + w * 0.5f + 0.15f, y0 + h + 0.2f, z + 0.06f), v3(x, y0 + h + 0.2f + w * 0.5f, z + 0.06f), c.p.trim_color, 0, false, v3(0, 0, 1));
}

void turret(Ctx &c, float x, float z, float r, float y0, float y1) {
  B &b = c.b;
  const int n = b.detail == 0 ? 10 : 7;
  b.frustum(v3(x, y0, z), v3(x, y1, z), r, r, n, c.p.stone_color * 0.98f, 0, false, false);
  b.frustum(v3(x, y1, z), v3(x, y1 + 0.12f, z), r + 0.12f, r + 0.12f, n, c.p.trim_color);
  if (c.p.roof != castle_v1_part::kRoofNone && (c.form() == Th::kRoofDome || c.form() == Th::kRoofFlat))
    theme::dome(b, v3(x, y1 + 0.12f, z), r + 0.1f, r * 1.4f, n, b.detail == 2 ? 3 : 5, c.p.roof_color, 0, c.form() == Th::kRoofFlat);
  else if (c.p.roof != castle_v1_part::kRoofNone && c.form() == Th::kRoofSweep) {
    b.frustum(v3(x, y1 + 0.02f, z), v3(x, y1 + 0.4f, z), r + 0.45f, r * 0.6f, n, c.p.roof_color);
    b.frustum(v3(x, y1 + 0.4f, z), v3(x, y1 + 0.4f + r * 2.4f, z), r * 0.6f, 0, n, c.p.roof_color);
  } else if (c.p.roof != castle_v1_part::kRoofNone) b.frustum(v3(x, y1 + 0.12f, z), v3(x, y1 + 0.12f + r * 2.2f, z), r + 0.15f, 0, n, c.p.roof_color);
  else if (b.detail < 2)
    for (int k = 0; k < 4; ++k) {
      const float a = kPi * 0.5f * static_cast<float>(k) + kPi * 0.25f;
      b.box(v3(x + (r - 0.1f) * foundry::det::cos(a), y1 + 0.42f, z + (r - 0.1f) * foundry::det::sin(a)), v3(0.18f, 0.3f, 0.18f), c.p.stone_color);
    }
}

// ---- kinds ----

void keep(Ctx &c) {
  B &b = c.b;
  const float cx = c.lot.cx, cz = c.lot.cz - 0.4f;
  const float s = minf(c.lot.hx, c.lot.hz) - 1.1f;
  const float x0 = cx - s, x1 = cx + s, z0 = cz - s, z1 = cz + s;
  const float H = c.storey() * static_cast<float>(c.storeys()) + 0.5f;
  stone_block(c, x0, x1, z0, z1, H, 0.35f);
  windows(c, x0, x1, z0, z1, 0, true);
  parapet(c, x0, x1, z0, z1, H);
  float top = H + 1.1f;
  if (c.p.roof != castle_v1_part::kRoofNone) top = maxf(top, H + 0.12f + roof(c, x0 + 0.5f, x1 - 0.5f, z0 + 0.5f, z1 - 0.5f, H + 0.12f, true));
  if (c.p.turrets) {
    const float r = s * 0.22f;
    for (int k = 0; k < 4; ++k) turret(c, k & 1 ? x1 : x0, k & 2 ? z1 : z0, r, 0.0f, H + 2.4f);
    top = maxf(top, H + 2.6f + (c.p.roof != castle_v1_part::kRoofNone ? r * 2.2f : 0.7f));
  }
  if (c.p.chimneys) {
    b.block(x1 - 1.4f, x1 - 0.8f, H, H + 1.8f, z0 + 0.8f, z0 + 1.4f, c.p.stone_color * 0.9f);
    condition::fire(b.out, v3(x1 - 1.1f, H + 1.9f, z0 + 1.1f), c.th->hearth_fire, 2u);
  }
  if (c.p.banners) b.banner(v3(cx, maxf(H + 0.12f, top - 0.2f), cz), 2.0f, v3(1, 0, 0), c.p.accent, c.pick(3));
  // The forebuilding: a stair up the front to the first-floor door.
  const float sh = c.storey();
  const int steps = maxi_(4, round_to_int(sh / 0.3f));
  const float run = minf(s * 1.4f, 5.0f);
  for (int k = 0; k < steps; ++k) {
    const float xa = x0 + 0.4f + run * static_cast<float>(k) / static_cast<float>(steps), xb = x0 + 0.4f + run * static_cast<float>(k + 1) / static_cast<float>(steps);
    b.block(xa, xb, -0.3f, sh * static_cast<float>(k + 1) / static_cast<float>(steps), z1, z1 + 1.2f, c.p.stone_color * 0.92f, 0, true);
  }
  b.block(x0 + 0.4f + run, x0 + 0.4f + run + 1.4f, -0.3f, sh, z1, z1 + 1.2f, c.p.stone_color * 0.92f, 0, true);
  b.solid(v3(x0 + 0.4f + (run + 1.4f) * 0.5f, sh * 0.5f, z1 + 0.6f), v3((run + 1.4f) * 0.5f, sh * 0.5f, 0.6f));
  doors(c, x0 + 0.4f + run + 0.7f, z1, 1.2f, sh);
  c.door = v3(x0 + 0.4f, 0, z1 + 1.8f);
  c.top = top + 2.0f;
}

void hall(Ctx &c) {
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z1 = c.z1() - 0.8f, z0 = maxf(c.z0(), z1 - 9.0f);
  const float H = c.storey() * static_cast<float>(c.storeys());
  stone_block(c, x0, x1, z0, z1, H, 0.0f);
  windows(c, x0 + 0.6f, x1 - 0.6f, z0, z1, c.storeys() > 1 ? 1 : 0, false);
  // Buttresses between the windows.
  const int n = maxi_(2, static_cast<int>((x1 - x0) / 2.4f));
  for (int i = 0; i <= n; ++i) {
    const float x = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(n);
    for (int s = -1; s <= 1; s += 2) {
      const float z = s > 0 ? z1 : z0;
      b.box(v3(x, H * 0.4f, z + static_cast<float>(s) * 0.3f), v3(0.28f, H * 0.4f + 0.4f, 0.3f), c.p.stone_color * 0.93f);
    }
  }
  if (c.p.crenellated) parapet(c, x0, x1, z0, z1, H);
  const float rise = roof(c, x0, x1, z0, z1, H, false);
  // A louvre on the ridge, and a chimney's smoke.
  const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
  if (rise > 0) {
    b.block(cx - 0.6f, cx + 0.6f, H + rise * 0.8f, H + rise + 0.7f, cz - 0.6f, cz + 0.6f, c.p.trim_color * 0.8f);
    b.pyramid(v3(cx, H + rise + 0.7f, cz), v3(1, 0, 0), v3(0, 0, 1), 0.8f, 0.8f, 0.7f, c.p.roof_color);
  }
  if (c.p.chimneys) {
    b.block(x1 - 1.0f, x1 - 0.3f, H * 0.5f, H + rise + 0.8f, z0 + 0.4f, z0 + 1.1f, c.p.stone_color * 0.9f);
    condition::fire(b.out, v3(x1 - 0.65f, H + rise + 0.9f, z0 + 0.75f), c.th->hearth_fire, 2u);
  }
  // A porch at the door.
  const float dz = z1;
  b.block(cx - 1.4f, cx + 1.4f, -0.3f, 3.4f, dz, dz + 1.3f, c.p.stone_color * 0.96f, 0, false);
  b.gable(v3(cx, 3.4f, dz + 0.65f), v3(0, 0, 1), 0.65f, 1.4f, 1.2f, 0.15f, c.p.roof_color, c.p.stone_color);
  b.block(cx - 0.75f, cx + 0.75f, 0, 2.7f, dz + 1.3f, dz + 1.32f, kPane);
  doors(c, cx, dz, 1.4f, 0.0f);
  c.door = v3(cx, 0, dz + 1.9f);
  if (c.p.banners) {
    b.banner(v3(x0 + 0.6f, H + 0.12f, z1 - 0.3f), 1.6f, v3(1, 0, 0), c.p.accent, c.pick(6));
    b.banner(v3(x1 - 0.6f, H + 0.12f, z1 - 0.3f), 1.6f, v3(1, 0, 0), c.p.accent, c.pick(7));
  }
  c.top = H + rise + 1.4f;
}

void chapel(Ctx &c) {
  B &b = c.b;
  const float cx = c.lot.cx, nw = minf(c.x1() - c.x0() - 0.6f, 6.0f);
  const float x0 = cx - nw * 0.5f, x1 = cx + nw * 0.5f, z1 = c.z1(), z0 = c.z0() + nw * 0.35f;
  const float H = c.storey() * static_cast<float>(c.storeys());
  stone_block(c, x0, x1, z0, z1, H, 0.0f);
  b.frustum(v3(cx, -0.4f, z0), v3(cx, H * 0.85f, z0), nw * 0.42f, nw * 0.42f, 8, c.p.stone_color, 0, true, false, kPi / 8.0f);
  b.frustum(v3(cx, H * 0.85f, z0), v3(cx, H * 0.85f + nw * 0.35f, z0), nw * 0.46f, 0, 8, c.p.roof_color, 0, false, true, kPi / 8.0f);
  const float rise = nw * 0.5f * 1.2f;
  b.gable(v3(cx, H, (z0 + z1) * 0.5f), v3(0, 0, 1), (z1 - z0) * 0.5f, nw * 0.5f, rise, 0.3f, c.p.roof_color, c.p.stone_color);
  if (b.detail < 2)
    for (int s = -1; s <= 1; s += 2) {
      const int n = maxi_(1, static_cast<int>((z1 - z0 - 1.0f) / 2.0f));
      for (int i = 0; i < n; ++i) {
        const float z = z0 + 0.8f + (z1 - z0 - 1.6f) * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        b.window(v3(s < 0 ? x0 : x1, H * 0.55f, z), v3(static_cast<float>(s), 0, 0), 0.55f, H * 0.45f, c.p.trim_color, kPane, true);
      }
    }
  // The bellcote on the front gable, its bell swinging.
  const float by = H + rise * 0.75f;
  b.block(cx - 0.55f, cx + 0.55f, by - 0.3f, by + 1.4f, z1 - 0.3f, z1 + 0.05f, c.p.stone_color * 0.97f, 0, false);
  b.triangle(v3(cx - 0.65f, by + 1.4f, z1 + 0.06f), v3(cx + 0.65f, by + 1.4f, z1 + 0.06f), v3(cx, by + 2.1f, z1 + 0.06f), c.p.stone_color, 0, false, v3(0, 0, 1));
  const uint32_t bell = b.bone(0, v3(cx, by + 1.2f, z1 - 0.1f), v3(1, 0, 0), house_v1_bone::kMotionSway, 0.7f, 0.35f, 0.15f, c.pick(8));
  b.frustum(v3(cx, by + 0.6f, z1 + 0.08f), v3(cx, by + 1.15f, z1 + 0.08f), 0.28f, 0.13f, 8, v3(0.7f, 0.55f, 0.25f), bell);
  b.solid(v3(cx, H + rise * 0.4f, (z0 + z1) * 0.5f), v3(nw * 0.35f, rise * 0.4f, (z1 - z0) * 0.5f));
  doors(c, cx, z1, 1.3f, 0.0f);
  c.door = v3(cx, 0, z1 + 0.6f);
  c.top = by + 2.2f;
}

void range(Ctx &c) {
  B &b = c.b;
  const float x0 = c.x0(), x1 = c.x1(), z0 = c.z0(), z1 = minf(c.z1() - 0.6f, z0 + 6.5f);
  const float H = c.storey() * static_cast<float>(c.storeys());
  stone_block(c, x0, x1, z0, z1, H, 0.0f);
  const float rise = roof(c, x0, x1, z0, z1, H, false);
  if (c.p.crenellated && rise == 0) parapet(c, x0, x1, z0, z1, H);
  // Doors along its front.
  const int n = c.p.doors == castle_v1_part::kDoorsStable ? maxi_(2, static_cast<int>((x1 - x0) / 1.9f)) : maxi_(1, static_cast<int>((x1 - x0) / 4.5f));
  for (int i = 0; i < n; ++i) {
    const float x = x0 + (x1 - x0) * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    if (c.p.doors == castle_v1_part::kDoorsStable) {
      // A half door, its top open on the dark.
      const uint32_t bone = b.bone(0, v3(x - 0.55f, 0, z1), v3(0, 1, 0), house_v1_bone::kMotionHinge, 0, 1.3f, 0, 0);
      b.box(v3(x, 0.6f, z1 + 0.03f), v3(0.55f, 0.6f, 0.04f), c.p.door_color, bone);
      b.box(v3(x, 1.65f, z1 + 0.01f), v3(0.55f, 0.45f, 0.01f), kPane, 0, false);
      b.box(v3(x, 2.2f, z1 + 0.05f), v3(0.68f, 0.08f, 0.06f), c.p.trim_color);
    } else {
      doors(c, x, z1, c.p.doors == castle_v1_part::kDoorsArch ? 2.0f : 1.1f, 0.0f);
    }
  }
  windows(c, x0 + 0.5f, x1 - 0.5f, z0, z1, c.storeys() > 1 ? 1 : 0, false);
  if (c.p.chimneys)
    for (int s = 0; s < 2; ++s) {
      const float x = x0 + (x1 - x0) * (0.25f + 0.5f * static_cast<float>(s));
      b.block(x - 0.35f, x + 0.35f, H, H + rise + 0.9f, z0 + 0.3f, z0 + 1.0f, c.p.stone_color * 0.9f);
      condition::fire(b.out, v3(x, H + rise + 1.0f, z0 + 0.65f), c.th->hearth_fire, 2u);
    }
  if (c.p.banners) b.banner(v3(x0 + 0.5f, H + rise * 0.2f, z1 - 0.2f), 1.5f, v3(1, 0, 0), c.p.accent, c.pick(12));
  c.door = v3((x0 + x1) * 0.5f, 0, z1 + 0.6f);
  c.top = H + rise + 1.2f;
}

void tower(Ctx &c) {
  B &b = c.b;
  const float cx = c.lot.cx, cz = c.lot.cz - 0.3f;
  const float r = minf(c.lot.hx, c.lot.hz) - 0.9f;
  const float H = c.storey() * static_cast<float>(c.storeys()) + 1.0f;
  const int n = b.detail == 0 ? 14 : (b.detail == 1 ? 10 : 7);
  b.frustum(v3(cx, -0.4f, cz), v3(cx, H, cz), r + 0.3f, r, n, c.p.stone_color, 0, true, false);
  b.solid_cylinder(v3(cx, 0, cz), r + 0.3f, H);
  if (b.detail < 2)
    for (int s = 0; s < c.storeys(); ++s)
      for (int k = 0; k < 3; ++k) {
        const float a = kPi * (0.33f + 0.67f * static_cast<float>(k)) + 0.5f * static_cast<float>(s);
        const vec3 nn = v3(foundry::det::sin(a), 0, foundry::det::cos(a));
        b.window(v3(cx, c.storey() * (static_cast<float>(s) + 0.6f), cz) + nn * (r + 0.05f), nn, 0.14f, 0.8f, c.p.trim_color, kPane, false);
      }
  if (c.p.roof != castle_v1_part::kRoofNone && (c.form() == Th::kRoofDome || c.form() == Th::kRoofFlat)) {
    b.frustum(v3(cx, H, cz), v3(cx, H + 0.12f, cz), r + 0.3f, r + 0.3f, n, c.p.trim_color);
    theme::dome(b, v3(cx, H + 0.12f, cz), r + 0.25f, r * 1.2f, n, b.detail == 2 ? 3 : 6, c.p.roof_color, 0, c.form() == Th::kRoofFlat);
    c.top = H + r * 1.2f + 0.2f;
  } else if (c.p.roof != castle_v1_part::kRoofNone) {
    b.frustum(v3(cx, H, cz), v3(cx, H + 0.12f, cz), r + 0.3f, r + 0.3f, n, c.p.trim_color);
    b.frustum(v3(cx, H + 0.12f, cz), v3(cx, H + 0.12f + r * 2.0f, cz), r + 0.35f, 0, n, c.p.roof_color);
    c.top = H + r * 2.0f + 0.2f;
  } else {
    b.frustum(v3(cx, H, cz), v3(cx, H + 0.12f, cz), r + 0.1f, r + 0.1f, n, c.p.trim_color);
    if (b.detail < 2)
      for (int k = 0; k < 8; ++k) {
        const float a = kPi * 0.25f * static_cast<float>(k);
        b.box(v3(cx + (r - 0.2f) * foundry::det::cos(a), H + 0.45f, cz + (r - 0.2f) * foundry::det::sin(a)), v3(0.3f, 0.33f, 0.3f), c.p.stone_color);
      }
    c.top = H + 0.8f;
  }
  if (c.p.banners) b.banner(v3(cx, c.top - 0.2f, cz), 1.8f, v3(1, 0, 0), c.p.accent, c.pick(14));
  doors(c, cx, cz + r + 0.3f, 1.1f, 0.0f);
  c.door = v3(cx, 0, cz + r + 0.9f);
  c.top += 1.8f;
}

// Torches either side of the door, and the theme's ornaments.
void dress_door(Ctx &c) {
  B &b = c.b;
  const vec3 up = v3(0, 1, 0);
  for (int s = -1; s <= 1; s += 2) {
    const vec3 wall = v3(c.door.x + static_cast<float>(s) * 1.1f, 2.4f, c.door.z - 0.55f);
    b.beam(wall, wall + v3(0, 0.1f, 0.25f), 0.03f, kIron);
    b.frustum(wall + v3(0, 0.05f, 0.25f), wall + v3(0, 0.3f, 0.3f), 0.04f, 0.08f, 5, v3(0.35f, 0.25f, 0.15f));
    condition::fire(b.out, wall + v3(0, 0.33f, 0.3f), c.th->torch_fire, 1u);
  }
  switch (c.th->ornament) {
    case Th::kOrnamentStandingStones:
      for (int s = -1; s <= 1; s += 2) {
        const float x = s < 0 ? c.x0() + 0.4f : c.x1() - 0.4f;
        theme::standing_stone(b, v3(x, 0, c.z1() + 0.5f), 2.6f, v3(0.56f, 0.56f, 0.53f), c.seed + static_cast<uint32_t>(s + 2));
        b.solid(v3(x, 1.3f, c.z1() + 0.5f), v3(0.35f, 1.3f, 0.25f));
      }
      break;
    case Th::kOrnamentCrystals:
      theme::crystals(b, v3(c.lot.cx, c.top - 1.6f, c.lot.cz), 1.4f, c.th->accent, c.seed);
      break;
    case Th::kOrnamentLanterns:
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = theme::lantern(b, v3(c.door.x + static_cast<float>(s) * 1.8f, 3.2f, c.door.z - 0.55f), v3(0, 0, 1), kIron, v3(1.0f, 0.86f, 0.45f));
        condition::fire(b.out, at, c.th->torch_fire, 1u);
      }
      break;
    case Th::kOrnamentDragonHeads:
      theme::dragon_head(b, v3(c.door.x, 4.2f, c.door.z - 0.5f), v3(0, 0, 1), c.p.trim_color, c.th->accent);
      break;
    case Th::kOrnamentCarvings:
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = v3(c.door.x + static_cast<float>(s) * 1.5f, 0, c.door.z - 0.4f);
        theme::frustum(b, at, at + up * 3.2f, 0.12f, 0.1f, 6, c.p.trim_color, 0, true);
      }
      break;
    case Th::kOrnamentPrayerFlags: {
      const vec3 from = v3(c.lot.cx, c.top - 1.8f, c.lot.cz);
      const vec3 to = v3(c.x1(), 2.4f, c.z1() + 0.3f);
      b.beam(v3(to.x, 0, to.z), to, 0.04f, v3(0.45f, 0.34f, 0.22f));
      const uint32_t fb = b.bone(0, from, normalize(to - from), house_v1_bone::kMotionSway, 0.8f, 0.25f, 0.9f, c.pick(70));
      theme::prayer_flags(b, from, to, fb, c.seed);
      break;
    }
    default: break;
  }
}

}  // namespace

bool run(const castle_v1_spec &in, castle_v1_model &out, std::string &error) {
  // The part, dressed in the village's theme (heartland keeps the part's own).
  castle_v1_part p = in.part;
  const Th &th = in.theme;
  p.stone_color = theme::dress(p.stone_color, th.wall_color, th);
  p.trim_color = theme::dress(p.trim_color, th.trim_color, th);
  p.roof_color = theme::dress(p.roof_color, th.roof_color, th);
  p.accent = theme::dress(p.accent, th.accent, th);
  if (in.stage > castle_v1_spec::kStageRuin) {
    error = "stage out of range";
    return false;
  }
  if (in.size > 3 || in.season > 3 || in.detail > 2 || p.kind > 4 || p.size > 3 || p.roof > 4 || p.windows > 2 || p.doors > 2) {
    error = "size, season, detail, or the part's kind, size, roof, windows, or doors out of range";
    return false;
  }
  out = castle_v1_model();
  B b{out};
  b.winter = in.season == castle_v1_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 12 : (in.detail == 1 ? 8 : 6);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_v1_bone::kMotionFixed, 0, 0, 0, 0);
  const Lot lot = make_lot(kLotW[in.size], kLotH[in.size]);
  Ctx c{b, p, lot, in.season, hash_u32(in.seed * 2654435761u ^ hash_str(p.name.c_str()) ^ (in.size * 977u))};
  c.th = &th;
  switch (p.kind) {
    case castle_v1_part::kKindKeep: keep(c); break;
    case castle_v1_part::kKindHall: hall(c); break;
    case castle_v1_part::kKindChapel: chapel(c); break;
    case castle_v1_part::kKindRange: range(c); break;
    default: tower(c); break;
  }
  dress_door(c);
  theme::moss(out, 0, th.moss * 0.6f, c.seed);
  // ---- the stage: planned, half built, finished, or a ruin (condition.h); the fires it burns from ----
  if (in.stage == castle_v1_spec::kStagePlanning) condition::planning(out, c.seed);
  else if (in.stage == castle_v1_spec::kStageHalf) condition::half_built(out, c.seed);
  else if (in.stage == castle_v1_spec::kStageRuin) condition::ruin(out, c.seed, th.moss);
  if (in.stage == castle_v1_spec::kStageFinished || in.stage == castle_v1_spec::kStageRuin)
    condition::blaze_points(out, th.blaze_fire, c.seed ^ 0xb1a2e5u, 4 + 2 * static_cast<int>(in.size));
  b.solid(v3(c.door.x, 0.9f, c.door.z - 0.2f), v3(0.6f, 0.9f, 0.4f), 0, channels::physics_v1_body::kPurposeSensor, "doorway");
  out.doorway = c.door;
  out.doorway_heading = 0;
  float top = 0, reach = 0, ground = 0;
  for (const vec3 &q : out.positions) {
    top = maxf(top, q.y);
    const float d = foundry::det::hypot(q.x, q.z);
    reach = maxf(reach, d);
    if (q.y < 0.4f) ground = maxf(ground, d);
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
  {
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
  out.marker = v3(lot.cx, maxf(c.top, 2.0f) + 0.6f, lot.cz);
  return true;
}

}  // namespace castle_build_v1

FOUNDRY_SYSTEM(castle_build_v1, channels::castle_v1_spec, channels::castle_v1_model)
