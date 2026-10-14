// gate_build_v1: a gate in a wall (design/planner.md, "Gates"). A wall's network (wall_v1) leaves an
// opening at each gate node or gate link and reports where; the gate stands in it, in the line's
// frame: the line along +x, the opening from -width / 2 to width / 2, the inside toward +z. It takes the wall's own kind,
// so its height, thickness, stone, and crown match, and its piers or towers stand just into the
// opening and over the wall's ends (so no face lies on another's).
//
// 1. By type: a gate (two piers or great posts, the wall's height, a lintel or a fighting platform
//    over a palisade's); an arch (a raised section with an arched opening, its voussoirs picked
//    out); a gatehouse (two towers of the wall's kind either side of a vaulted passage, battlements
//    over it, a portcullis half raised at its outer face); a tower gate (a tower over the passage);
//    or a postern (a small door in a thickened section).
// 2. Doors: one or two leaves of planks with iron straps, hinged to swing inward by door_time
//    (house_animate_v1's hinge).
// 3. Banners on its towers or piers that sway, torches either side of its outer face that flicker.
// 4. Collision: piers, towers, and the section over the passage solid; the passage a sensor named
//    gateway. It reports the passage's clear width and height.
// 5. Guides for the viewer: the wall's line through it, the opening's ends, the wall's top beyond.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "condition.h"
#include "channels/gate_v1_model.h"
#include "channels/gate_v1_spec.h"
#include "packer/gate_v1_model.h"
#include "packer/gate_v1_spec.h"

namespace gate_build_v1 {

using channels::gate_v1_model;
using channels::gate_v1_spec;
using channels::gate_v1_type;
using channels::house_v1_bone;
using channels::wall_v1_kind;

namespace {

using B = Builder<gate_v1_model>;
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
const vec3 kIron = {0.2f, 0.2f, 0.22f};
const vec3 kDark = {0.06f, 0.05f, 0.05f};

struct Ctx {
  const gate_v1_type &t;
  const wall_v1_kind &w;
  float W, H, T, bt, F;  // opening, wall height, thickness, batter, footing
  bool logs() const { return w.material == wall_v1_kind::kMaterialLogs; }
  bool earth() const { return w.material == wall_v1_kind::kMaterialEarth; }
  float half() const { return T * 0.5f + bt; }
};

// Battlements along x from x0 to x1 at height y across depth d (centred on z = zc).
void crenels(B &b, const Ctx &c, float x0, float x1, float y, float zc, float d) {
  if (b.detail == 2 || !c.w.crenellated) return;
  const float cw = 0.5f, len = x1 - x0;
  const int n = static_cast<int>(len / (2 * cw));
  const float pad = (len - static_cast<float>(n) * 2 * cw + cw) * 0.5f;
  for (int i = 0; i < n; ++i) {
    const float x = x0 + pad + 2 * cw * static_cast<float>(i) + cw * 0.5f;
    b.box(v3(x, y + 0.35f, zc), v3(cw * 0.5f, 0.35f, d * 0.5f), c.w.color * 0.97f);
  }
}

// A solid section: x0..x1, from the footing to `top`, half depth hd; logs as pointed logs.
void section(B &b, const Ctx &c, float x0, float x1, float top, float hd, uint32_t seed) {
  if (c.logs()) {
    const int n = maxi_(1, round_to_int((x1 - x0) / (c.T * 1.05f)));
    const float r = (x1 - x0) / static_cast<float>(n) * 0.5f;
    for (int i = 0; i < n; ++i) {
      const float x = x0 + (static_cast<float>(i) + 0.5f) * (x1 - x0) / static_cast<float>(n);
      const float h = top * (0.96f + 0.05f * unit(hash_u32(seed + static_cast<uint32_t>(i) * 7919u)));
      b.frustum(v3(x, -c.F, 0), v3(x, h, 0), r, r * 0.95f, 6, c.w.color, 0, false, false);
      b.frustum(v3(x, h, 0), v3(x, h + r * 2.4f, 0), r * 0.95f, 0, 6, lerp(c.w.color, c.w.trim_color, 0.5f), 0, false, true);
    }
    return;
  }
  const vec3 col = c.earth() ? c.w.trim_color : c.w.color;
  b.block(x0, x1, -c.F, top, -hd, hd, col, 0, false);
  b.block(x0 - 0.04f, x1 + 0.04f, top, top + 0.08f, -hd - 0.04f, hd + 0.04f, c.w.trim_color);
}

// A tower of the wall's kind at x, rising to `top`.
void tower(B &b, const Ctx &c, float x, float R, float top, uint32_t seed) {
  const bool round = c.w.tower == wall_v1_kind::kTowerRound;
  const vec3 col = c.earth() ? c.w.trim_color : c.w.color;
  if (c.logs()) {
    b.block(x - R, x + R, -c.F, top, -R, R, c.w.color * 0.95f, 0, false);
    b.block(x - R - 0.1f, x + R + 0.1f, top, top + 0.1f, -R - 0.1f, R + 0.1f, c.w.trim_color);
    for (int s = 0; s < 4; ++s) b.beam(v3(x + (s & 1 ? R : -R) * 0.85f, top, (s & 2 ? R : -R) * 0.85f), v3(x + (s & 1 ? R : -R) * 0.85f, top + 1.1f, (s & 2 ? R : -R) * 0.85f), 0.06f, c.w.trim_color);
  } else if (round) {
    const int n = b.detail == 0 ? 14 : (b.detail == 1 ? 10 : 7);
    b.frustum(v3(x, -c.F, 0), v3(x, top, 0), R + c.bt * 0.6f, R, n, col, 0, true, false);
  } else {
    b.block(x - R, x + R, -c.F, top, -R, R, col, 0, false);
  }
  // Slits facing out.
  if (b.detail < 2 && !c.logs())
    for (int l = 0; l < 2; ++l) b.window(v3(x, top * (0.4f + 0.3f * static_cast<float>(l)), -R - 0.01f), v3(0, 0, -1), 0.14f, 0.7f, c.w.trim_color, v3(0.08f, 0.07f, 0.07f), false);
  // The top: the type's roof, else battlements.
  const vec3 roof = c.t.roof_color;
  switch (c.t.roof) {
    case gate_v1_type::kRoofCone: b.frustum(v3(x, top, 0), v3(x, top + R * 1.7f, 0), R * (round ? 1.12f : 1.45f), 0, round ? 12 : 4, roof, 0, true, true, round ? 0.0f : kPi * 0.25f); break;
    case gate_v1_type::kRoofPyramid: b.pyramid(v3(x, top, 0), v3(1, 0, 0), v3(0, 0, 1), R + 0.2f, R + 0.2f, R * 1.3f, roof); break;
    case gate_v1_type::kRoofGable: b.gable(v3(x, top, 0), v3(0, 0, 1), R, R, R * 0.9f, 0.25f, roof, col); break;
    default:
      if (!c.logs() && b.detail < 2 && c.w.crenellated) {
        for (int s = -1; s <= 1; s += 2) {
          b.box(v3(x + static_cast<float>(s) * (R - 0.25f), top + 0.35f, -R + 0.25f), v3(0.25f, 0.35f, 0.25f), col * 0.97f);
          b.box(v3(x + static_cast<float>(s) * (R - 0.25f), top + 0.35f, R - 0.25f), v3(0.25f, 0.35f, 0.25f), col * 0.97f);
          b.box(v3(x, top + 0.35f, static_cast<float>(s) * (R - 0.25f)), v3(0.25f, 0.35f, 0.25f), col * 0.97f);
        }
      }
      break;
  }
  if (c.t.banners) {
    const float rt = c.t.roof == gate_v1_type::kRoofCone ? R * 1.7f : (c.t.roof == gate_v1_type::kRoofPyramid ? R * 1.3f : (c.t.roof == gate_v1_type::kRoofGable ? R * 0.9f : 0.0f));
    b.banner(v3(x, top + rt - 0.1f, 0), 1.5f, v3(1, 0, 0), c.t.accent, unit(hash_u32(seed ^ 0xba11u)));
  }
  if (round && !c.logs()) b.solid_cylinder(v3(x, 0, 0), R, top);
  else b.solid(v3(x, top * 0.5f, 0), v3(R, top * 0.5f, R));
}

// Door leaves across [-w/2, w/2] at z, ph tall: planks and straps, hinged to swing toward +z.
void doors(B &b, const Ctx &c, float w, float ph, float z) {
  const int leaves = c.t.leaves >= 2 && w > 1.4f ? 2 : 1;
  const float lw = w / static_cast<float>(leaves);
  for (int l = 0; l < leaves; ++l) {
    const bool left = l == 0;
    const float hx = left ? -w * 0.5f : w * 0.5f;  // the hinge's x
    const float dirx = left ? 1.0f : -1.0f;      // from the hinge into the leaf
    const uint32_t bone = b.bone(0, v3(hx, 0, z), v3(0, left ? -1.0f : 1.0f, 0), house_v1_bone::kMotionHinge, 0, 1.45f, 0, 0);
    const float cx = hx + dirx * (lw * 0.5f - 0.01f);
    b.box(v3(cx, ph * 0.5f, z), v3(lw * 0.5f - 0.02f, ph * 0.5f, 0.05f), c.t.door_color, bone);
    if (b.detail == 0) {
      const int planks = maxi_(2, round_to_int(lw / 0.25f));
      for (int p = 1; p < planks; ++p) {
        const float x = hx + dirx * (lw * static_cast<float>(p) / static_cast<float>(planks));
        b.box(v3(x, ph * 0.5f, z - 0.055f), v3(0.012f, ph * 0.5f - 0.05f, 0.006f), c.t.door_color * 0.7f, bone, false);
      }
    }
    if (b.detail < 2)
      for (float y : {ph * 0.2f, ph * 0.75f}) b.box(v3(cx, y, z - 0.06f), v3(lw * 0.5f - 0.06f, 0.05f, 0.01f), c.t.iron_color, bone, false);
  }
}

void torches(B &b, float x, float z, uint32_t seed) {
  for (int s = -1; s <= 1; s += 2) {
    const vec3 at = v3(static_cast<float>(s) * x, 2.3f, z);
    b.beam(at - v3(0, 0.3f, 0), at, 0.03f, kIron);
    const uint32_t glow = b.bone(0, at + v3(0, 0.12f, 0), v3(0, 1, 0), house_v1_bone::kMotionFlicker, 2.6f + unit(hash_u32(seed + static_cast<uint32_t>(s + 5))), 0.3f, 0, unit(hash_u32(seed ^ static_cast<uint32_t>(s + 9))));
    b.box(at + v3(0, 0.12f, 0), v3(0.07f, 0.11f, 0.07f), v3(1.0f, 0.62f, 0.22f), glow, false);
    condition::fire(b.out, at + v3(0, 0.2f, 0), 2u, 1u);  // a torch
  }
}

// The arched top of an opening [-r, r] springing at y0: fill above the arc up to y1, stepped.
void arch_fill(B &b, const Ctx &c, float r, float y0, float y1, float hd, vec3 col) {
  const int n = b.detail == 0 ? 8 : (b.detail == 1 ? 6 : 4);
  for (int i = 0; i < n; ++i) {
    const float xa = -r + 2 * r * static_cast<float>(i) / static_cast<float>(n), xb = -r + 2 * r * static_cast<float>(i + 1) / static_cast<float>(n);
    const float xo = absf(xa) > absf(xb) ? absf(xa) : absf(xb);
    const float yi = y0 + foundry::det::sqrt(maxf(r * r - xo * xo, 0));
    if (y1 - yi > 0.01f) b.block(xa, xb, yi, y1, -hd, hd, col, 0, false);
    if (b.detail == 0) {
      // The voussoirs, picked out on both faces.
      const float xm = (xa + xb) * 0.5f, ym = y0 + foundry::det::sqrt(maxf(r * r - xm * xm, 0));
      for (int s = -1; s <= 1; s += 2) b.box(v3(xm, ym + 0.15f, static_cast<float>(s) * (hd + 0.02f)), v3((xb - xa) * 0.45f, 0.15f, 0.02f), c.w.trim_color, 0, false);
    }
  }
}

}  // namespace

bool run(const gate_v1_spec &in, gate_v1_model &out, std::string &error) {
  const gate_v1_type &t = in.type;
  const wall_v1_kind &w = in.wall;
  if (in.season > 3 || in.detail > 2 || t.kind > 4 || t.roof > 3 || w.material > wall_v1_kind::kMaterialLiving || w.tower > 2 ||
      in.condition > gate_v1_spec::kConditionRuin) {
    error = "season, detail, the type's kind or roof, or the wall's material or tower out of range";
    return false;
  }
  if (!(in.width > 0.5f && in.width < 30.0f)) {
    error = "a gate needs a width between 0.5 and 30 m";
    return false;
  }
  out = gate_v1_model();
  B b{out};
  b.winter = in.season == gate_v1_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 10 : (in.detail == 1 ? 7 : 5);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_v1_bone::kMotionFixed, 0, 0, 0, 0);
  const Ctx c{t, w, in.width, clampf(w.height, 1.5f, 14.0f), clampf(w.thickness, 0.25f, 4.0f), clampf(w.batter, 0, 1.5f), clampf(w.footing, 0.2f, 3.0f)};
  const uint32_t seed = hash_u32(in.seed * 2654435761u ^ hash_str(t.name.c_str()) ^ hash_str(w.name.c_str()) ^ static_cast<uint32_t>(round_to_int(in.width * 100.0f)));
  const float hw = c.W * 0.5f - 0.03f;  // just into the opening, over the wall's ends
  const float rise = clampf(t.rise, 0, 12);
  const float D = maxf(c.T * clampf(t.depth, 1, 6), c.half() * 2 + 0.3f);
  float ph = 2.6f, pw = c.W - 0.06f, reach_x = hw;
  switch (t.kind) {
    case gate_v1_type::kKindGate: {
      // Two piers (great posts in a palisade) and the doors between.
      const float p = c.logs() ? 0.32f : maxf(c.half() + 0.15f, 0.5f);
      const float top = c.H + 0.3f + rise * 0.5f;
      if (c.logs()) {
        for (int s = -1; s <= 1; s += 2) {
          const float x = static_cast<float>(s) * (hw + p);
          b.frustum(v3(x, -c.F, 0), v3(x, top, 0), p, p * 0.95f, 7, w.color * 0.95f, 0, false, false);
          b.frustum(v3(x, top, 0), v3(x, top + 0.7f, 0), p * 0.95f, 0, 7, w.trim_color, 0, false, true);
          b.solid_cylinder(v3(x, 0, 0), p, top);
        }
        // A lintel and, over a walled palisade, a platform across.
        b.box(v3(0, top - 0.25f, 0), v3(hw + p, 0.15f, 0.15f), w.trim_color);
        if (w.walkway) b.block(-hw - p, hw + p, c.H - 1.4f, c.H - 1.3f, 0.2f, 1.2f, w.trim_color);
        ph = minf(top - 0.45f, 3.6f);
      } else {
        for (int s = -1; s <= 1; s += 2) {
          const float x0 = s < 0 ? -hw - 2 * p : hw, x1 = s < 0 ? -hw : hw + 2 * p;
          section(b, c, x0, x1, top, c.half() + 0.15f, seed);
          if (w.crenellated && b.detail < 2) b.box(v3((x0 + x1) * 0.5f, top + 0.4f, 0), v3(p * 0.6f, 0.32f, p * 0.6f), w.color * 0.97f);
          b.solid(v3((x0 + x1) * 0.5f, top * 0.5f, 0), v3(p, top * 0.5f, c.half() + 0.15f));
          if (t.banners) b.banner(v3((x0 + x1) * 0.5f, top + 0.08f, 0), 1.4f, v3(1, 0, 0), t.accent, unit(hash_u32(seed + static_cast<uint32_t>(s + 3))));
        }
        ph = minf(c.H, 3.6f);
      }
      reach_x = hw + 2 * p;
      doors(b, c, c.W - 0.06f, ph, 0.0f);
      if (t.lamps) torches(b, hw + 0.3f, -c.half() - 0.22f, seed);
      break;
    }
    case gate_v1_type::kKindArch:
    case gate_v1_type::kKindTowerGate: {
      const bool tg = t.kind == gate_v1_type::kKindTowerGate;
      const float S = maxf(1.2f, c.T);
      const float top = c.H + maxf(rise, tg ? 3.0f : 1.0f);
      const float hd = tg ? maxf(D * 0.5f, hw + 0.5f) : c.half() + 0.12f;
      const float r = hw;
      ph = minf(top - 1.0f, r + maxf(2.4f, 3.4f - r * 0.3f));
      const float spring = ph - r;
      section(b, c, -hw - S, -hw, top, hd, seed);
      section(b, c, hw, hw + S, top, hd, seed);
      if (!c.logs()) {
        b.block(-hw, hw, ph, top, -hd, hd, w.color, 0, false);
        arch_fill(b, c, r, spring, ph, hd, w.color);
        b.block(-hw - S - 0.04f, hw + S + 0.04f, top, top + 0.08f, -hd - 0.04f, hd + 0.04f, w.trim_color);
      } else {
        b.block(-hw, hw, ph, ph + 0.4f, -0.3f, 0.3f, w.trim_color);
      }
      if (tg) {
        const vec3 col = w.color;
        switch (t.roof) {
          case gate_v1_type::kRoofGable: b.gable(v3(0, top + 0.08f, 0), v3(1, 0, 0), hw + S, hd, (hd) * 0.9f, 0.3f, t.roof_color, col); break;
          case gate_v1_type::kRoofPyramid:
          case gate_v1_type::kRoofCone: b.pyramid(v3(0, top + 0.08f, 0), v3(1, 0, 0), v3(0, 0, 1), hw + S + 0.2f, hd + 0.2f, (hd + hw) * 0.7f, t.roof_color); break;
          default:
            crenels(b, c, -hw - S, hw + S, top + 0.08f, -hd + 0.2f, 0.4f);
            crenels(b, c, -hw - S, hw + S, top + 0.08f, hd - 0.2f, 0.4f);
            break;
        }
        if (b.detail < 2)
          for (int s = -1; s <= 1; s += 2) b.window(v3(0, top - 1.4f, static_cast<float>(s) * (hd + 0.01f)), v3(0, 0, static_cast<float>(s)), 0.6f, 1.0f, w.trim_color, kDark, true);
      } else {
        crenels(b, c, -hw - S, hw + S, top + 0.08f, -c.half() + 0.3f, 0.5f);
      }
      if (t.portcullis) {
        const float z = -hd + 0.25f, y0 = ph * 0.55f;
        const int bars = maxi_(3, round_to_int(c.W / 0.35f));
        for (int i = 0; i < bars; ++i) {
          const float x = -hw + 0.1f + (2 * hw - 0.2f) * (static_cast<float>(i) + 0.5f) / static_cast<float>(bars);
          b.box(v3(x, (y0 + ph) * 0.5f + 0.2f, z), v3(0.03f, (ph - y0) * 0.5f + 0.2f, 0.03f), t.iron_color, 0, false);
          b.frustum(v3(x, y0 - 0.0f, z), v3(x, y0 - 0.18f, z), 0.03f, 0, 4, t.iron_color);
        }
        for (float y = y0 + 0.2f; y < ph; y += 0.4f) b.box(v3(0, y, z), v3(hw - 0.05f, 0.03f, 0.03f), t.iron_color, 0, false);
      }
      for (int s = -1; s <= 1; s += 2) b.solid(v3(static_cast<float>(s) * (hw + S * 0.5f), top * 0.5f, 0), v3(S * 0.5f, top * 0.5f, hd));
      b.solid(v3(0, (ph + top) * 0.5f, 0), v3(hw, (top - ph) * 0.5f, hd));
      doors(b, c, c.W - 0.06f, maxf(spring + r * 0.35f, 2.0f), hd - 0.3f);
      if (t.banners) b.banner(v3(0, top + 0.08f, -hd + 0.3f), 1.8f, v3(1, 0, 0), t.accent, unit(hash_u32(seed + 11u)));
      if (t.lamps) torches(b, hw + 0.35f, -hd - 0.12f, seed);
      reach_x = hw + S;
      break;
    }
    case gate_v1_type::kKindGatehouse: {
      const float R = c.logs() ? maxf(c.T, 1.4f) : maxf(maxf(w.tower_radius, c.half() + 0.5f), 1.5f);
      const float top = c.H + maxf(rise, 2.0f);
      const float hd = maxf(D * 0.5f, R);
      ph = minf(c.H + rise * 0.4f - 0.6f, hw + 3.2f);
      ph = maxf(ph, 2.6f);
      for (int s = -1; s <= 1; s += 2) tower(b, c, static_cast<float>(s) * (hw + R), R, top, seed + static_cast<uint32_t>(s + 2));
      // The passage's vault and the fighting floor over it.
      const float over = minf(top - 0.4f, ph + 2.2f);
      if (c.logs()) {
        b.block(-hw, hw, ph, over, -hd * 0.8f, hd * 0.8f, w.color * 0.9f, 0, false);
        b.block(-hw - 0.1f, hw + 0.1f, over, over + 0.1f, -hd * 0.8f - 0.1f, hd * 0.8f + 0.1f, w.trim_color);
      } else {
        b.block(-hw, hw, ph, over, -hd * 0.85f, hd * 0.85f, w.color, 0, false);
        if (b.detail == 0) arch_fill(b, c, hw, ph - hw * 0.5f, ph, hd * 0.85f, w.color * 0.98f);
        crenels(b, c, -hw, hw, over, -hd * 0.85f + 0.2f, 0.4f);
        b.block(-hw - 0.02f, hw + 0.02f, over, over + 0.08f, -hd * 0.85f, hd * 0.85f, w.trim_color);
      }
      if (t.portcullis) {
        const float z = -hd * 0.85f + 0.3f, y0 = ph * 0.6f;
        const int bars = maxi_(3, round_to_int(c.W / 0.35f));
        for (int i = 0; i < bars; ++i) {
          const float x = -hw + 0.1f + (2 * hw - 0.2f) * (static_cast<float>(i) + 0.5f) / static_cast<float>(bars);
          b.box(v3(x, (y0 + ph) * 0.5f + 0.15f, z), v3(0.03f, (ph - y0) * 0.5f + 0.15f, 0.03f), t.iron_color, 0, false);
          b.frustum(v3(x, y0, z), v3(x, y0 - 0.18f, z), 0.03f, 0, 4, t.iron_color);
        }
        for (float y = y0 + 0.2f; y < ph; y += 0.4f) b.box(v3(0, y, z), v3(hw - 0.05f, 0.03f, 0.03f), t.iron_color, 0, false);
      }
      b.solid(v3(0, (ph + over) * 0.5f, 0), v3(hw, (over - ph) * 0.5f, hd * 0.85f));
      doors(b, c, c.W - 0.06f, ph - 0.1f, hd * 0.85f - 0.25f);
      if (t.lamps) torches(b, hw - 0.15f, -hd * 0.85f - 0.12f, seed);
      reach_x = hw + 2 * R;
      break;
    }
    default: {  // a postern: a small door in a thickened section
      const float S = 0.6f;
      const float top = c.H + 0.1f + rise * 0.3f;
      const float hd = c.half() + 0.08f;
      const float dw = minf(c.W - 0.2f, 1.1f);
      ph = 2.1f;
      pw = dw;
      section(b, c, -hw - S, -dw * 0.5f, top, hd, seed);
      section(b, c, dw * 0.5f, hw + S, top, hd, seed);
      if (!c.logs()) b.block(-dw * 0.5f, dw * 0.5f, ph, top, -hd, hd, w.color, 0, false);
      else b.block(-dw * 0.5f, dw * 0.5f, ph, ph + 0.3f, -0.25f, 0.25f, w.trim_color);
      crenels(b, c, -hw - S, hw + S, top + 0.08f, 0, c.T);
      for (int s = -1; s <= 1; s += 2) {
        const float x0 = s < 0 ? -hw - S : dw * 0.5f, x1 = s < 0 ? -dw * 0.5f : hw + S;
        b.solid(v3((x0 + x1) * 0.5f, top * 0.5f, 0), v3((x1 - x0) * 0.5f, top * 0.5f, hd));
      }
      doors(b, c, dw, ph, hd - 0.15f);
      if (t.lamps) torches(b, dw * 0.5f + 0.35f, -hd - 0.12f, seed);
      reach_x = hw + S;
      break;
    }
  }
  b.solid(v3(0, ph * 0.5f, 0), v3(maxf(pw * 0.5f - 0.1f, 0.2f), ph * 0.5f, maxf(D * 0.5f, 0.5f)), 0, channels::physics_v1_body::kPurposeSensor, "gateway");
  out.passage = pw;
  out.passage_height = ph;
  // ---- guides ----
  auto line = [&](vec3 a, vec3 cc, vec3 col) {
    out.guide_lines.push_back(a);
    out.guide_lines.push_back(cc);
    out.guide_colors.push_back(pack(col));
  };
  const vec3 kLine = {1, 1, 1}, kFaint = {0.55f, 0.6f, 0.65f}, kJoin = {1.0f, 0.8f, 0.3f};
  const float gy = 0.05f, m = 3.0f;
  line(v3(-c.W * 0.5f - m, gy, 0), v3(c.W * 0.5f + m, gy, 0), kLine);
  for (int s = -1; s <= 1; s += 2) {
    const float x = static_cast<float>(s) * c.W * 0.5f;
    line(v3(x, gy, -c.half() - 0.4f), v3(x, gy, c.half() + 0.4f), kJoin);
    // The wall beyond: its top, as the next module would stand.
    const float xe = x + static_cast<float>(s) * m;
    for (float z : {-c.T * 0.5f, c.T * 0.5f}) line(v3(x, c.H, z), v3(xe, c.H, z), kFaint);
    line(v3(xe, c.H, -c.T * 0.5f), v3(xe, c.H, c.T * 0.5f), kFaint);
  }
  // ---- size ----
  // A ruin: its doors and portcullis gone, broken down, overgrown (condition.h); the points it burns from.
  if (in.condition == gate_v1_spec::kConditionRuin) condition::ruin(out, seed ^ 0x5eedu, 0.5f);
  condition::blaze_points(out, 4u, seed ^ 0xb1a2e5u, 3);
  float top = 0, reach = 0, hwid = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
    hwid = maxf(hwid, absf(p.z));
  }
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      const float r = sh.type == channels::physics_v1_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
      top = maxf(top, sh.position.y + (sh.type == channels::physics_v1_shape::kTypeBox ? sh.half_extents.y : sh.half_height));
    }
  out.height = top;
  out.radius = reach;
  out.length = 2 * reach_x;
  out.half_width = hwid;
  return true;
}

}  // namespace gate_build_v1

FOUNDRY_SYSTEM(gate_build_v1, channels::gate_v1_spec, channels::gate_v1_model)
