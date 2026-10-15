// A village theme's architecture (design/planner.md, "Themes"): round plans, roof forms (hip,
// cone, dome, flat, turf, sweep), wall materials (wattle, drystone, adobe, living wood, marble
// besides the types' own), window shapes, foundations, and ornaments. Templated on the system's
// builder, using only its triangle, box, bone, detail, and out, so houses, civic buildings,
// castles, and waterfronts share one look. Each system carries its own copy (spine.md).
#pragma once

#include <cstdint>
#include <vector>

#include "boundary.h"
#include "channels/village_theme.h"
#include "math.h"

namespace gate_build {

using namespace gate_math;

namespace theme {

using Theme = channels::village_theme;
constexpr float kTau = 6.2831853f;

inline vec3 tmix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float cosr(float a) { return foundry::det::cos(a); }
inline float sinr(float a) { return foundry::det::sin(a); }
inline vec3 around(float a) { return v3(sinr(a), 0, cosr(a)); }  // angle 0 faces +z (the front)
inline float tan_(float a) { return foundry::det::sin(a) / foundry::det::cos(a); }
inline int maxi_t(int a, int b) { return a > b ? a : b; }
inline float hash01(uint32_t h) { return static_cast<float>(hash_u32(h) >> 8) * (1.0f / 16777216.0f); }

// The theme's material, roof, and windows over a type's own (theme enums throughout: a type's
// walls 0..4 are the theme's 1..5).
inline uint32_t walls_of(const Theme &th, uint32_t type_walls) { return th.walls != Theme::kWallsAsType ? th.walls : type_walls + 1; }
inline uint32_t roof_of(const Theme &th, uint32_t fallback) { return th.roof != Theme::kRoofAsType ? th.roof : fallback; }
inline uint32_t windows_of(const Theme &th, uint32_t fallback) { return th.windows != Theme::kWindowsAsType ? th.windows : fallback; }
inline bool round_plan(const Theme &th, bool fallback = false) { return th.plan == Theme::kPlanAsType ? fallback : th.plan == Theme::kPlanRound; }
inline vec3 dress(vec3 c, vec3 to, const Theme &th) { return tmix(c, to, clampf(th.mix, 0, 1)); }

// ---- primitives (triangles only, so every builder takes them) ----

// A frustum from a (radius r0) to c (r1) about the axis a->c, n sides, optionally capped.
template <typename B>
void frustum(B &b, vec3 a, vec3 c, float r0, float r1, int n, vec3 col, uint32_t bone, bool caps, bool snow = true, float turn = 0) {
  const vec3 ax = normalize(c - a);
  const vec3 u = absf(ax.y) > 0.9f ? v3(1, 0, 0) : normalize(cross(v3(0, 1, 0), ax));
  const vec3 w = cross(ax, u);
  for (int k = 0; k < n; ++k) {
    const float a0 = turn + kTau * static_cast<float>(k) / static_cast<float>(n), a1 = turn + kTau * static_cast<float>(k + 1) / static_cast<float>(n);
    const vec3 d0 = u * cosr(a0) + w * sinr(a0), d1 = u * cosr(a1) + w * sinr(a1);
    const vec3 out = normalize(d0 + d1);
    const vec3 p0 = a + d0 * r0, p1 = a + d1 * r0, q0 = c + d0 * r1, q1 = c + d1 * r1;
    if (r1 > 1e-4f) {
      b.triangle(p0, p1, q1, col, bone, snow, out);
      b.triangle(p0, q1, q0, col, bone, snow, out);
    } else {
      b.triangle(p0, p1, c, col, bone, snow, out + ax * (r0 / maxf(length(c - a), 1e-3f)));
    }
    if (caps) {
      b.triangle(a, p1, p0, col * 0.8f, bone, false, -ax);
      if (r1 > 1e-4f) b.triangle(c, q0, q1, col, bone, snow, ax);
    }
  }
}

// A dome over a circle (centre c at its base height), `rise` high; stepped: corbelled courses.
template <typename B>
void dome(B &b, vec3 c, float r, float rise, int sides, int rings, vec3 col, uint32_t bone, bool stepped) {
  for (int i = 0; i < rings; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(rings), t1 = static_cast<float>(i + 1) / static_cast<float>(rings);
    const float a0 = t0 * kTau * 0.25f, a1 = t1 * kTau * 0.25f;
    const float ra = r * cosr(a0), rb = r * cosr(a1), ya = rise * sinr(a0), yb = rise * sinr(a1);
    const vec3 shade = col * (0.94f + 0.06f * static_cast<float>(i % 2));
    if (stepped) {
      // A course: a short wall and a ledge in.
      frustum(b, c + v3(0, ya, 0), c + v3(0, yb, 0), ra, ra, sides, shade, bone, false);
      if (rb > 0.02f) frustum(b, c + v3(0, yb, 0), c + v3(0, yb + 0.001f, 0), ra, rb, sides, shade * 1.05f, bone, false);
      else frustum(b, c + v3(0, yb, 0), c + v3(0, yb + 0.02f, 0), ra, 0.0f, sides, shade, bone, false);
    } else {
      frustum(b, c + v3(0, ya, 0), c + v3(0, yb, 0), ra, maxf(rb, 0.0f), sides, shade, bone, false);
    }
  }
}

// A thick sloped slab a b c d (around, its top), thickness down: for roof planes.
template <typename B>
void plane(B &b, vec3 a, vec3 c1, vec3 c2, vec3 d, float thick, vec3 col, uint32_t bone, vec3 up) {
  const vec3 n = normalize(cross(c1 - a, d - a));
  const vec3 nn = dot(n, up) < 0 ? -n : n;
  const vec3 o = nn * -thick;
  b.triangle(a, c1, c2, col, bone, true, nn);
  b.triangle(a, c2, d, col, bone, true, nn);
  b.triangle(a + o, c2 + o, c1 + o, col * 0.7f, bone, false, -nn);
  b.triangle(a + o, d + o, c2 + o, col * 0.7f, bone, false, -nn);
  const vec3 e[4] = {a, c1, c2, d};
  for (int k = 0; k < 4; ++k) {
    const vec3 p = e[k], q = e[(k + 1) % 4];
    const vec3 mid = (p + q) * 0.5f, ctr = (a + c1 + c2 + d) * 0.25f;
    const vec3 side = mid - ctr;
    b.triangle(p, q, q + o, col * 0.85f, bone, false, side);
    b.triangle(p, q + o, p + o, col * 0.85f, bone, false, side);
  }
}

// ---- roofs ----

// A frame: the body's rectangle (cx, cz, hx, hz), or its circle (radius r at cx, cz).
struct Body {
  float cx = 0, cz = 0, hx = 1, hz = 1;
  bool round = false;
  float r() const { return minf(hx, hz); }
};

// The roof over a body at eaves height y0; returns its top. `pitch_deg` for pitched forms.
template <typename B>
float roof(B &b, const Body &body, uint32_t form, float y0, float pitch_deg, vec3 col, vec3 trim, vec3 gable_col, uint32_t seed, bool along_z) {
  const vec3 up = v3(0, 1, 0);
  const float t = tan_(pitch_deg * 0.017453292f);
  const int sides = b.detail == 0 ? 16 : (b.detail == 1 ? 10 : 7);
  if (body.round) {
    const float r = body.r();
    const vec3 c = v3(body.cx, y0, body.cz);
    if (form == Theme::kRoofDome) {
      dome(b, c, r + 0.06f, r * 0.95f, sides, b.detail == 2 ? 3 : 6, col, 0, true);
      b.box(c + v3(0, r * 0.95f + 0.08f, 0), v3(0.18f, 0.08f, 0.18f), trim, 0);  // the capstone
      return y0 + r * 0.95f + 0.16f;
    }
    if (form == Theme::kRoofFlat) {
      frustum(b, c, c + v3(0, 0.12f, 0), r + 0.05f, r + 0.05f, sides, col, 0, true);
      frustum(b, c + v3(0, 0.12f, 0), c + v3(0, 0.45f, 0), r + 0.05f, r + 0.05f, sides, trim, 0, false);
      return y0 + 0.45f;
    }
    // Cone (thatch), sweep (a flared cone), turf (a grassed cone).
    const float over = 0.45f, rise = (r + over) * maxf(t, 1.1f);
    if (form == Theme::kRoofSweep) {
      frustum(b, c + v3(0, -0.12f, 0), c + v3(0, 0.25f, 0), r + over + 0.3f, r + over * 0.4f, sides, col * 0.95f, 0, false);
      frustum(b, c + v3(0, 0.25f, 0), c + v3(0, rise, 0), r + over * 0.4f, 0.0f, sides, col, 0, false);
    } else {
      const vec3 cc = form == Theme::kRoofTurf ? v3(0.36f, 0.5f, 0.24f) : col;
      frustum(b, c + v3(0, -over * 0.4f, 0), c + v3(0, rise, 0), r + over, 0.0f, sides, cc, 0, false);
      frustum(b, c + v3(0, -over * 0.4f - 0.15f, 0), c + v3(0, -over * 0.4f, 0), r + over, r + over, sides, cc * 0.8f, 0, false, false);
    }
    // The eaves' underside.
    frustum(b, c, c + v3(0, -over * 0.4f, 0), r, r + over, sides, col * 0.6f, 0, false, false);
    (void)seed;
    return y0 + rise;
  }
  // Rectangular forms, ridge along x unless along_z.
  const float hu = along_z ? body.hz : body.hx, hv = along_z ? body.hx : body.hz;
  auto at = [&](float u, float y, float v) { return along_z ? v3(body.cx + v, y, body.cz + u) : v3(body.cx + u, y, body.cz + v); };
  const float over = 0.32f;
  if (form == Theme::kRoofFlat) {
    b.box(v3(body.cx, y0 + 0.08f, body.cz), v3(body.hx + 0.08f, 0.08f, body.hz + 0.08f), col, 0);
    // The parapet.
    const float py = y0 + 0.16f, ph = 0.4f;
    b.box(v3(body.cx, py + ph * 0.5f, body.cz - body.hz - 0.04f), v3(body.hx + 0.12f, ph * 0.5f, 0.1f), trim, 0);
    b.box(v3(body.cx, py + ph * 0.5f, body.cz + body.hz + 0.04f), v3(body.hx + 0.12f, ph * 0.5f, 0.1f), trim, 0);
    b.box(v3(body.cx - body.hx - 0.04f, py + ph * 0.5f, body.cz), v3(0.1f, ph * 0.5f, body.hz), trim, 0);
    b.box(v3(body.cx + body.hx + 0.04f, py + ph * 0.5f, body.cz), v3(0.1f, ph * 0.5f, body.hz), trim, 0);
    return py + ph;
  }
  if (form == Theme::kRoofDome) {
    // A flat roof with a dome over its middle.
    b.box(v3(body.cx, y0 + 0.08f, body.cz), v3(body.hx + 0.06f, 0.08f, body.hz + 0.06f), trim, 0);
    const float r = minf(body.hx, body.hz) * 0.85f;
    frustum(b, v3(body.cx, y0 + 0.16f, body.cz), v3(body.cx, y0 + 0.5f, body.cz), r, r, sides, col * 0.95f, 0, false);
    dome(b, v3(body.cx, y0 + 0.5f, body.cz), r, r * 0.9f, sides, b.detail == 2 ? 3 : 5, col, 0, false);
    b.box(v3(body.cx, y0 + 0.5f + r * 0.9f + 0.12f, body.cz), v3(0.05f, 0.14f, 0.05f), trim, 0);
    return y0 + 0.5f + r * 0.9f + 0.26f;
  }
  const float rise = hv * t;
  const float top = y0 + rise;
  const float u0 = -hu - over * 0.7f, u1 = hu + over * 0.7f, d = hv + over, drop = over * t;
  if (form == Theme::kRoofHip || form == Theme::kRoofCone) {
    // A hip (a cone over a rectangle is a pyramid: the ridge shrinks to a point).
    const float ridge = form == Theme::kRoofCone ? 0.0f : maxf(hu - hv, 0.0f);
    const float ty = form == Theme::kRoofCone ? y0 + maxf(hu, hv) * maxf(t, 1.0f) : top;
    const vec3 r0 = at(-ridge, ty, 0), r1 = at(ridge, ty, 0);
    const float ey = y0 - drop;
    const vec3 e00 = at(u0, ey, -d), e10 = at(u1, ey, -d), e11 = at(u1, ey, d), e01 = at(u0, ey, d);
    const vec3 ctr = at(0, (ty + ey) * 0.5f, 0);
    auto face = [&](vec3 p, vec3 q, vec3 s, vec3 tt) {
      const vec3 mid = (p + q + s + tt) * 0.25f;
      b.triangle(p, q, s, col, 0, true, mid - ctr + up * 0.5f);
      b.triangle(p, s, tt, col, 0, true, mid - ctr + up * 0.5f);
    };
    face(e00, e10, r1, r0);
    face(e11, e01, r0, r1);
    b.triangle(e01, e00, r0, col * 0.95f, 0, true, at(-1, 0.5f, 0) - at(0, 0, 0));
    b.triangle(e10, e11, r1, col * 0.95f, 0, true, at(1, 0.5f, 0) - at(0, 0, 0));
    // The underside, so the eaves don't show through.
    b.triangle(e00, e01, e11, col * 0.55f, 0, false, -up);
    b.triangle(e00, e11, e10, col * 0.55f, 0, false, -up);
    return ty;
  }
  // Gable-like: gable, turf, sweep.
  const bool turf = form == Theme::kRoofTurf, sweep = form == Theme::kRoofSweep;
  const vec3 cover = turf ? v3(0.36f, 0.5f, 0.24f) : col;
  const float thick = turf ? 0.3f : 0.12f;
  for (int s = -1; s <= 1; s += 2) {
    const float vs = static_cast<float>(s);
    const vec3 a0 = at(u0, top + thick * 0.6f, 0), a1 = at(u1, top + thick * 0.6f, 0);
    if (sweep) {
      // Steep to a knee, then flatter out past the walls to eaves that sweep up at their corners.
      const float kv = hv * 0.6f, ky = top - kv * t + thick * 0.6f;
      const vec3 k0 = at(u0, ky, vs * kv), k1 = at(u1, ky, vs * kv);
      plane(b, a0, a1, k1, k0, thick, cover, 0, up);
      const float ey = ky - (d + 0.25f - kv) * t * 0.3f;
      const vec3 m0 = at(u0, ey, vs * (d + 0.25f)), m1 = at(u1, ey, vs * (d + 0.25f));
      plane(b, k0, k1, m1, m0, thick, cover * 0.95f, 0, up);
      // The upturned corners: horns of roof reaching out and up.
      const vec3 f0 = at(u0 - 0.55f, ey + 0.55f, vs * (d + 0.6f)), f1 = at(u1 + 0.55f, ey + 0.55f, vs * (d + 0.6f));
      b.triangle(m0, f0, k0, cover * 0.9f, 0, true, up);
      b.triangle(m0, k0, f0, cover * 0.7f, 0, false, -up);
      b.triangle(m1, k1, f1, cover * 0.9f, 0, true, up);
      b.triangle(m1, f1, k1, cover * 0.7f, 0, false, -up);
      b.triangle(m0, at(u0, ey, vs * (d * 0.5f)), f0, cover * 0.85f, 0, false, at(-1, 0, 0) - at(0, 0, 0));
      b.triangle(m1, f1, at(u1, ey, vs * (d * 0.5f)), cover * 0.85f, 0, false, at(1, 0, 0) - at(0, 0, 0));
    } else {
      const vec3 e0 = at(u0, y0 - drop + thick * 0.6f, vs * d), e1 = at(u1, y0 - drop + thick * 0.6f, vs * d);
      plane(b, a0, a1, e1, e0, thick, cover, 0, up);
      if (turf && b.detail <= 1) {
        // Grass tufts and the log that holds the turf along the eaves.
        b.box((e0 + e1) * 0.5f + v3(0, 0.05f, 0), along_z ? v3(0, 0, 1) : v3(1, 0, 0), v3(0, 1, 0), along_z ? v3(1, 0, 0) : v3(0, 0, 1),
              v3(length(e1 - e0) * 0.5f, 0.09f, 0.09f), trim, 0);
        const int tufts = b.detail == 0 ? 10 : 5;
        for (int k = 0; k < tufts; ++k) {
          const float u = hash01(seed + static_cast<uint32_t>(k * 2 + (s > 0 ? 1 : 0)) * 977u), v = hash01(seed ^ (static_cast<uint32_t>(k) * 7919u + (s > 0 ? 3u : 5u)));
          const vec3 p = a0 + (a1 - a0) * (0.1f + 0.8f * u) + (e0 - a0) * (0.15f + 0.7f * v);
          frustum(b, p, p + v3(0, 0.22f, 0), 0.1f, 0.0f, 4, v3(0.42f, 0.58f, 0.26f), 0, false);
        }
      }
    }
  }
  // The ridge cap and the gable ends.
  b.box(at(0, top + thick * 0.7f, 0), along_z ? v3(0, 0, 1) : v3(1, 0, 0), v3(0, 1, 0), along_z ? v3(1, 0, 0) : v3(0, 0, 1),
        v3((u1 - u0) * 0.5f, 0.06f, 0.08f), turf ? cover * 0.85f : col * 0.7f, 0);
  for (int s = -1; s <= 1; s += 2) {
    const float u = static_cast<float>(s) * hu;
    b.triangle(at(u, y0, -hv), at(u, y0, hv), at(u, top, 0), gable_col, 0, false, at(static_cast<float>(s), 0, 0) - at(0, 0, 0));
    // A sweep's ridge ends curl up into horns.
    if (sweep) frustum(b, at(static_cast<float>(s) * (hu + 0.2f), top + thick, 0), at(static_cast<float>(s) * (hu + 0.75f), top + thick + 0.6f, 0), 0.09f, 0.0f, 5, trim, 0, false);
  }
  return top + thick * 0.7f;
}

// ---- walls ----

// Windows: square, round, arched, or a slit; on a face at `at` looking along n.
template <typename B>
void window(B &b, vec3 at, vec3 n, float w, float h, uint32_t shape, vec3 frame, vec3 glass) {
  const vec3 up = v3(0, 1, 0), side = normalize(cross(up, n));
  if (shape == Theme::kWindowsSlit) {
    b.box(at + n * 0.02f, side, up, n, v3(0.07f, h * 0.55f, 0.03f), glass, 0, false);
    b.box(at + n * 0.04f + up * (h * 0.55f + 0.04f), side, up, n, v3(0.14f, 0.04f, 0.04f), frame, 0, false);
    return;
  }
  if (shape == Theme::kWindowsRound) {
    const float r = minf(w, h) * 0.5f;
    frustum(b, at, at + n * 0.03f, r, r, b.detail == 2 ? 6 : 10, glass, 0, true, false);
    frustum(b, at + n * 0.02f, at + n * 0.06f, r + 0.07f, r + 0.07f, b.detail == 2 ? 6 : 10, frame, 0, false, false);
    return;
  }
  b.box(at + n * 0.02f, side, up, n, v3(w * 0.5f, h * 0.5f, 0.03f), glass, 0, false);
  if (shape == Theme::kWindowsArched) {
    // The arch: a half disc over the opening, and its frame.
    const int seg = b.detail == 2 ? 3 : 6;
    const vec3 c = at + up * (h * 0.5f) + n * 0.02f;
    for (int k = 0; k < seg; ++k) {
      const float a0 = 3.14159265f * static_cast<float>(k) / static_cast<float>(seg), a1 = 3.14159265f * static_cast<float>(k + 1) / static_cast<float>(seg);
      const vec3 p0 = c + side * (cosr(a0) * w * 0.5f) + up * (sinr(a0) * w * 0.5f), p1 = c + side * (cosr(a1) * w * 0.5f) + up * (sinr(a1) * w * 0.5f);
      b.triangle(c + n * 0.015f, p0 + n * 0.015f, p1 + n * 0.015f, glass, 0, false, n);
      const vec3 q0 = c + side * (cosr(a0) * (w * 0.5f + 0.08f)) + up * (sinr(a0) * (w * 0.5f + 0.08f));
      const vec3 q1 = c + side * (cosr(a1) * (w * 0.5f + 0.08f)) + up * (sinr(a1) * (w * 0.5f + 0.08f));
      b.triangle(p0 + n * 0.04f, q0 + n * 0.04f, q1 + n * 0.04f, frame, 0, false, n);
      b.triangle(p0 + n * 0.04f, q1 + n * 0.04f, p1 + n * 0.04f, frame, 0, false, n);
    }
  }
  b.box(at + n * 0.05f - up * (h * 0.5f + 0.03f), side, up, n, v3(w * 0.5f + 0.06f, 0.035f, 0.07f), frame, 0);
}

// A round wall from y0 to y1, in its material, its door at angle `door_a` (0 faces +z), its gap
// `door_w` wide, `door_h` high.
template <typename B>
void round_walls(B &b, vec3 c, float r, float y0, float y1, uint32_t material, vec3 col, vec3 trim, float door_a, float door_w, float door_h,
                 uint32_t seed) {
  const int sides = b.detail == 0 ? 16 : (b.detail == 1 ? 12 : 8);
  const float gap = door_w / r;  // radians the door takes
  for (int k = 0; k < sides; ++k) {
    const float a0 = kTau * static_cast<float>(k) / static_cast<float>(sides), a1 = kTau * static_cast<float>(k + 1) / static_cast<float>(sides);
    const vec3 d0 = around(a0), d1 = around(a1), out = around((a0 + a1) * 0.5f);
    float da = (a0 + a1) * 0.5f - door_a;
    while (da > 3.14159265f) da -= kTau;
    while (da < -3.14159265f) da += kTau;
    const bool door_here = door_h > 0 && absf(da) < gap * 0.5f + kTau / static_cast<float>(sides) * 0.5f;
    const float lo = door_here ? y0 + door_h : y0;
    const vec3 shade = col * (0.95f + 0.05f * static_cast<float>(k % 2));
    const vec3 p0 = c + d0 * r + v3(0, lo, 0), p1 = c + d1 * r + v3(0, lo, 0), q0 = c + d0 * r + v3(0, y1, 0), q1 = c + d1 * r + v3(0, y1, 0);
    if (y1 > lo) {
      b.triangle(p0, p1, q1, shade, 0, false, out);
      b.triangle(p0, q1, q0, shade, 0, false, out);
    }
    if (door_here) {
      // The door's recess: a dark back so the gap reads as a way in.
      const vec3 i0 = c + d0 * (r - 0.25f) + v3(0, y0, 0), i1 = c + d1 * (r - 0.25f) + v3(0, y0, 0);
      b.triangle(i0, i1, i1 + v3(0, door_h, 0), col * 0.3f, 0, false, out);
      b.triangle(i0, i1 + v3(0, door_h, 0), i0 + v3(0, door_h, 0), col * 0.3f, 0, false, out);
    }
  }
  // The inside, darker, so a ruin or a gap never shows through.
  frustum(b, c + v3(0, y0, 0), c + v3(0, y1, 0), r - 0.12f, r - 0.12f, sides, col * 0.55f, 0, false, false);
  const bool hi = b.detail == 0, mid = b.detail <= 1;
  if (!mid) return;
  if (material == Theme::kWallsWattle) {
    // Woven bands near the foot and a daub skin above; stakes between.
    for (int k = 0; k < sides; ++k) {
      const vec3 d = around(kTau * static_cast<float>(k) / static_cast<float>(sides));
      float da = kTau * static_cast<float>(k) / static_cast<float>(sides) - door_a;
      while (da > 3.14159265f) da -= kTau;
      while (da < -3.14159265f) da += kTau;
      if (absf(da) < gap * 0.6f) continue;
      b.box(c + d * (r + 0.02f) + v3(0, (y0 + y1) * 0.5f, 0), normalize(cross(v3(0, 1, 0), d)), v3(0, 1, 0), d, v3(0.04f, (y1 - y0) * 0.5f, 0.03f),
            trim, 0, false);
    }
    for (float y = y0 + 0.15f; y < y0 + 0.75f; y += 0.18f)
      frustum(b, c + v3(0, y, 0), c + v3(0, y + 0.07f, 0), r + 0.035f, r + 0.035f, sides, trim * 1.15f, 0, false, false);
  } else if (material == Theme::kWallsDrystone || material == Theme::kWallsStone) {
    // Courses and stones standing proud.
    const float step = material == Theme::kWallsDrystone ? 0.22f : 0.34f;
    for (float y = y0 + step; y < y1 - 0.05f; y += step)
      frustum(b, c + v3(0, y, 0), c + v3(0, y + 0.03f, 0), r + 0.015f, r + 0.015f, sides, col * 0.78f, 0, false, false);
    if (hi)
      for (int k = 0; k < 18; ++k) {
        const float a = kTau * hash01(seed + static_cast<uint32_t>(k) * 31u), y = y0 + 0.2f + (y1 - y0 - 0.4f) * hash01(seed ^ (static_cast<uint32_t>(k) * 7477u));
        float da = a - door_a;
        while (da > 3.14159265f) da -= kTau;
        while (da < -3.14159265f) da += kTau;
        if (absf(da) < gap && y < y0 + door_h + 0.2f) continue;
        const vec3 d = around(a);
        b.box(c + d * (r + 0.02f) + v3(0, y, 0), normalize(cross(v3(0, 1, 0), d)), v3(0, 1, 0), d, v3(0.15f, 0.08f, 0.03f),
              col * (0.82f + 0.15f * hash01(seed + static_cast<uint32_t>(k))), 0, false);
      }
  } else if (material == Theme::kWallsLog || material == Theme::kWallsPlank || material == Theme::kWallsLiving) {
    // Upright posts (a log or plank hut) or living trunks round it.
    const int n = material == Theme::kWallsPlank ? sides : sides / 2;
    for (int k = 0; k < n; ++k) {
      const float a = kTau * (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
      float da = a - door_a;
      while (da > 3.14159265f) da -= kTau;
      while (da < -3.14159265f) da += kTau;
      if (absf(da) < gap * 0.7f) continue;
      const vec3 d = around(a);
      frustum(b, c + d * (r + 0.03f) + v3(0, y0, 0), c + d * (r + 0.03f) + v3(0, y1 + 0.1f, 0), 0.09f, 0.08f, 5, trim, 0, false);
    }
    if (material == Theme::kWallsLiving)
      for (int k = 0; k < (hi ? 10 : 5); ++k) {
        const float a = kTau * hash01(seed + 1000u + static_cast<uint32_t>(k));
        const vec3 d = around(a);
        frustum(b, c + d * (r + 0.1f) + v3(0, y1 - 0.25f, 0), c + d * (r + 0.1f) + v3(0, y1 + 0.15f, 0), 0.28f, 0.1f, 5, v3(0.34f, 0.5f, 0.24f), 0, true);
      }
  } else if (material == Theme::kWallsMarble) {
    for (int k = 0; k < sides; k += 2) {
      const float a = kTau * static_cast<float>(k) / static_cast<float>(sides);
      float da = a - door_a;
      while (da > 3.14159265f) da -= kTau;
      while (da < -3.14159265f) da += kTau;
      if (absf(da) < gap) continue;
      const vec3 d = around(a);
      frustum(b, c + d * (r + 0.05f) + v3(0, y0, 0), c + d * (r + 0.05f) + v3(0, y1, 0), 0.1f, 0.1f, 6, trim, 0, true);
    }
    frustum(b, c + v3(0, y1 - 0.12f, 0), c + v3(0, y1, 0), r + 0.12f, r + 0.12f, sides, trim, 0, true);
  } else if (material == Theme::kWallsAdobe) {
    // Beam ends (vigas) poking out under the roof.
    for (int k = 0; k < sides; k += 2) {
      const vec3 d = around(kTau * (static_cast<float>(k) + 0.5f) / static_cast<float>(sides));
      frustum(b, c + d * (r - 0.1f) + v3(0, y1 - 0.25f, 0), c + d * (r + 0.35f) + v3(0, y1 - 0.25f, 0), 0.07f, 0.07f, 5, trim, 0, true);
    }
  } else if (material == Theme::kWallsTimber) {
    for (int k = 0; k < sides; k += 2) {
      const float a = kTau * static_cast<float>(k) / static_cast<float>(sides);
      float da = a - door_a;
      while (da > 3.14159265f) da -= kTau;
      while (da < -3.14159265f) da += kTau;
      if (absf(da) < gap * 0.7f) continue;
      const vec3 d = around(a);
      b.box(c + d * (r + 0.02f) + v3(0, (y0 + y1) * 0.5f, 0), normalize(cross(v3(0, 1, 0), d)), v3(0, 1, 0), d, v3(0.05f, (y1 - y0) * 0.5f, 0.03f),
            trim, 0, false);
    }
    frustum(b, c + v3(0, y1 - 0.1f, 0), c + v3(0, y1, 0), r + 0.03f, r + 0.03f, sides, trim, 0, false, false);
  }
}

// What the new materials add to a rectangular wall (the body box is the system's own).
template <typename B>
void rect_material(B &b, float cx, float cz, float hx, float hz, float y0, float y1, uint32_t material, vec3 col, vec3 trim, float door_x,
                   float door_w, uint32_t seed) {
  const bool hi = b.detail == 0, mid = b.detail <= 1;
  const vec3 X = v3(1, 0, 0), Y = v3(0, 1, 0), Z = v3(0, 0, 1);
  const float x0 = cx - hx, x1 = cx + hx, z0 = cz - hz, z1 = cz + hz;
  auto off_door = [&](float x, float z) { return !(z > z1 - 0.05f && x > door_x - door_w * 0.5f - 0.1f && x < door_x + door_w * 0.5f + 0.1f); };
  if (material == Theme::kWallsWattle) {
    // Corner posts, a woven skirt, a daub skin.
    for (int sx = 0; sx < 2; ++sx)
      for (int sz = 0; sz < 2; ++sz) b.box(v3(sx ? x1 : x0, (y0 + y1) * 0.5f, sz ? z1 : z0), v3(0.09f, (y1 - y0) * 0.5f, 0.09f), trim, 0, false);
    if (!mid) return;
    for (float y = y0 + 0.12f; y < y0 + 0.7f; y += 0.16f) {
      for (int s = 0; s < 2; ++s) {
        b.box(v3(cx, y, s ? z1 + 0.02f : z0 - 0.02f), v3(hx, 0.05f, 0.02f), trim * 1.15f, 0, false);
        b.box(v3(s ? x1 + 0.02f : x0 - 0.02f, y, cz), v3(0.02f, 0.05f, hz), trim * 1.15f, 0, false);
      }
    }
  } else if (material == Theme::kWallsDrystone) {
    if (!mid) return;
    for (float y = y0 + 0.22f; y < y1 - 0.05f; y += 0.22f)
      for (int s = 0; s < 2; ++s) {
        b.box(v3(cx, y, s ? z1 + 0.01f : z0 - 0.01f), v3(hx, 0.015f, 0.01f), col * 0.75f, 0, false);
        b.box(v3(s ? x1 + 0.01f : x0 - 0.01f, y, cz), v3(0.01f, 0.015f, hz), col * 0.75f, 0, false);
      }
    if (!hi) return;
    for (int k = 0; k < 22; ++k) {
      const float u = hash01(seed + static_cast<uint32_t>(k) * 13u), v = hash01(seed ^ (static_cast<uint32_t>(k) * 7919u + 1u));
      const int face = static_cast<int>(hash_u32(seed + static_cast<uint32_t>(k) * 101u) % 4u);
      const float y = y0 + 0.15f + v * (y1 - y0 - 0.3f);
      vec3 at, n;
      if (face < 2) {
        at = v3(x0 + 0.2f + u * (2 * hx - 0.4f), y, face ? z1 : z0);
        n = v3(0, 0, face ? 1.0f : -1.0f);
      } else {
        at = v3(face == 3 ? x1 : x0, y, z0 + 0.2f + u * (2 * hz - 0.4f));
        n = v3(face == 3 ? 1.0f : -1.0f, 0, 0);
      }
      if (!off_door(at.x, at.z) && y < 2.2f) continue;
      b.box(at, normalize(cross(Y, n)), Y, n, v3(0.12f + 0.08f * u, 0.07f + 0.04f * v, 0.03f), col * (0.8f + 0.18f * u), 0, false);
    }
  } else if (material == Theme::kWallsAdobe) {
    // Rounded buttresses at the corners and vigas under the roof.
    for (int sx = 0; sx < 2; ++sx)
      for (int sz = 0; sz < 2; ++sz)
        frustum(b, v3(sx ? x1 : x0, y0, sz ? z1 : z0), v3(sx ? x1 : x0, y1 + 0.1f, sz ? z1 : z0), 0.28f, 0.2f, b.detail == 2 ? 5 : 8, col * 0.97f, 0, true);
    if (!mid) return;
    const int n = static_cast<int>(2 * hx / 0.9f);
    for (int k = 1; k < n; ++k) {
      const float x = x0 + 2 * hx * static_cast<float>(k) / static_cast<float>(n);
      for (int s = 0; s < 2; ++s) {
        const float z = s ? z1 : z0;
        frustum(b, v3(x, y1 - 0.25f, z - (s ? 0.1f : -0.1f)), v3(x, y1 - 0.25f, z + (s ? 0.35f : -0.35f)), 0.07f, 0.07f, 5, trim, 0, true);
      }
    }
  } else if (material == Theme::kWallsLiving) {
    // Trunks grown at the corners and along the walls, their crowns leafing over the eaves.
    const int n = maxi_t(2, static_cast<int>(2 * hx / 1.6f));
    for (int k = 0; k <= n; ++k)
      for (int s = 0; s < 2; ++s) {
        const float x = x0 + 2 * hx * static_cast<float>(k) / static_cast<float>(n), z = s ? z1 + 0.06f : z0 - 0.06f;
        if (!off_door(x, z)) continue;
        frustum(b, v3(x, y0, z), v3(x, y1 + 0.2f, z), 0.14f, 0.09f, 6, trim, 0, false);
        if (mid) frustum(b, v3(x, y1 - 0.1f, z), v3(x, y1 + 0.55f, z), 0.4f, 0.12f, 6, v3(0.32f, 0.5f, 0.24f) * (0.9f + 0.2f * hash01(seed + static_cast<uint32_t>(k * 2 + s))), 0, true);
      }
    if (!hi) return;
    for (int k = 0; k < 8; ++k) {  // vines
      const float x = x0 + 2 * hx * hash01(seed + 77u + static_cast<uint32_t>(k));
      const float z = k % 2 ? z1 + 0.03f : z0 - 0.03f;
      if (!off_door(x, z)) continue;
      b.box(v3(x, (y0 + y1) * 0.5f, z), X, Y, Z, v3(0.05f, (y1 - y0) * 0.45f, 0.02f), v3(0.3f, 0.46f, 0.22f), 0, false);
    }
  } else if (material == Theme::kWallsMarble) {
    // Pilasters, a plinth, and a cornice.
    const int n = maxi_t(2, static_cast<int>(2 * hx / 1.4f));
    for (int k = 0; k <= n; ++k)
      for (int s = 0; s < 2; ++s) {
        const float x = x0 + 2 * hx * static_cast<float>(k) / static_cast<float>(n), z = s ? z1 + 0.05f : z0 - 0.05f;
        if (!off_door(x, z)) continue;
        b.box(v3(x, (y0 + y1) * 0.5f, z), v3(0.11f, (y1 - y0) * 0.5f, 0.05f), trim, 0, false);
      }
    b.box(v3(cx, y1 - 0.08f, cz), v3(hx + 0.12f, 0.08f, hz + 0.12f), trim, 0);
    b.box(v3(cx, y0 + 0.12f, cz), v3(hx + 0.06f, 0.12f, hz + 0.06f), trim * 0.95f, 0);
  }
}

// ---- foundations and ornaments ----

// Stilts: posts under a raised deck, and steps down from the door. The building above was lifted
// by `lift` (see lift()).
template <typename B>
void stilts(B &b, float cx, float cz, float hx, float hz, float lift, float door_x, float front, vec3 wood) {
  b.box(v3(cx, lift - 0.06f, cz), v3(hx + 0.3f, 0.06f, hz + 0.3f), wood * 1.05f, 0);
  const int nx = maxi_t(2, static_cast<int>(2 * hx / 1.4f) + 1), nz = maxi_t(2, static_cast<int>(2 * hz / 1.4f) + 1);
  for (int i = 0; i < nx; ++i)
    for (int k = 0; k < nz; ++k) {
      if (i != 0 && i != nx - 1 && k != 0 && k != nz - 1) continue;
      const float x = cx - hx - 0.15f + (2 * hx + 0.3f) * static_cast<float>(i) / static_cast<float>(nx - 1);
      const float z = cz - hz - 0.15f + (2 * hz + 0.3f) * static_cast<float>(k) / static_cast<float>(nz - 1);
      frustum(b, v3(x, -0.1f, z), v3(x, lift - 0.1f, z), 0.08f, 0.07f, 5, wood * 0.85f, 0, false);
    }
  // Steps.
  const int steps = maxi_t(1, static_cast<int>(lift / 0.2f));
  for (int k = 0; k < steps; ++k) {
    const float y = lift * static_cast<float>(k + 1) / static_cast<float>(steps + 1);
    b.box(v3(door_x, y - 0.03f, front + 0.3f + 0.28f * static_cast<float>(steps - k)), v3(0.45f, 0.03f, 0.14f), wood, 0);
  }
  for (int s = -1; s <= 1; s += 2)
    b.box(v3(door_x + 0.47f * static_cast<float>(s), lift * 0.5f + 0.25f, front + 0.35f), v3(0.03f, lift * 0.5f + 0.25f, 0.03f), wood * 0.9f, 0);
}

// Everything built since vertex v0, bone k0, and fire point f0 rises by dy.
template <typename M>
void lift(M &m, size_t v0, size_t k0, size_t f0, float dy) {
  for (size_t v = v0; v < m.positions.size(); ++v) m.positions[v].y += dy;
  for (size_t f = f0; f < m.fire_points.size(); ++f) m.fire_points[f].y += dy;
  for (size_t k = k0; k < m.bones.size(); ++k) m.bones[k].pivot.y += dy;
}

// Moss and green on everything facing up since vertex v0.
template <typename M>
void moss(M &m, size_t v0, float amount, uint32_t seed) {
  if (amount <= 0) return;
  const vec3 green = v3(0.32f, 0.46f, 0.22f);
  for (size_t v = v0; v < m.colors.size(); ++v) {
    const float up = m.normals[v].y;
    if (up < 0.3f) continue;
    const uint32_t c = m.colors[v];
    vec3 col = v3(static_cast<float>(c & 255u) / 255.0f, static_cast<float>((c >> 8) & 255u) / 255.0f, static_cast<float>((c >> 16) & 255u) / 255.0f);
    const float n = noise(m.positions[v] * 1.3f, seed);
    col = tmix(col, green, clampf(amount * up * (0.4f + 0.8f * n), 0, 0.85f));
    auto byte = [](float x) { return static_cast<uint32_t>(clampf(x, 0, 1) * 255.0f + 0.5f); };
    m.colors[v] = byte(col.x) | (byte(col.y) << 8) | (byte(col.z) << 16) | (c & 0xff000000u);
  }
}

// A standing stone: a tall, slightly leaning slab.
template <typename B>
void standing_stone(B &b, vec3 at, float h, vec3 col, uint32_t seed) {
  const float lean = (hash01(seed) - 0.5f) * 0.12f, yaw = hash01(seed + 1u) * 3.0f;
  const vec3 ax = v3(cosr(yaw), 0, -sinr(yaw)), az = v3(sinr(yaw), 0, cosr(yaw));
  const vec3 ay = normalize(v3(0, 1, 0) + ax * lean);
  b.box(at + ay * (h * 0.5f - 0.15f), ax, ay, az, v3(0.32f, h * 0.5f, 0.16f), col * (0.9f + 0.15f * hash01(seed + 2u)), 0);
  if (b.detail == 0) b.box(at + ay * (h - 0.1f), ax, ay, az, v3(0.26f, 0.1f, 0.13f), col * 1.05f, 0);
}

// A dragon's head on a gable's peak, looking out along `dir`.
template <typename B>
void dragon_head(B &b, vec3 peak, vec3 dir, vec3 col, vec3 eye) {
  const vec3 up = v3(0, 1, 0);
  // The crossed bargeboards rising past the peak, curving out into the neck and head.
  const vec3 side = normalize(cross(up, dir));
  for (int s = -1; s <= 1; s += 2)
    b.box(peak + up * 0.3f + side * (0.12f * static_cast<float>(s)) + dir * 0.05f, side, normalize(up - side * (0.4f * static_cast<float>(s))), dir,
          v3(0.06f, 0.4f, 0.05f), col, 0);
  const vec3 neck = peak + up * 0.65f + dir * 0.2f;
  frustum(b, peak + up * 0.3f, neck, 0.1f, 0.08f, 5, col, 0, false);
  frustum(b, neck, neck + dir * 0.45f - up * 0.05f, 0.11f, 0.03f, 5, col, 0, true);
  if (b.detail <= 1) {
    for (int s = -1; s <= 1; s += 2) {
      b.box(neck + side * (0.08f * static_cast<float>(s)) + dir * 0.08f + up * 0.04f, v3(0.025f, 0.025f, 0.025f), eye, 0, false);
      frustum(b, neck - dir * 0.05f + side * (0.05f * static_cast<float>(s)), neck - dir * 0.25f + up * 0.2f + side * (0.08f * static_cast<float>(s)), 0.03f,
              0.0f, 4, col * 0.85f, 0, false);  // horns
    }
  }
}

// A cluster of crystals, glowing in the accent colour.
template <typename B>
void crystals(B &b, vec3 at, float s, vec3 col, uint32_t seed) {
  const int n = b.detail == 2 ? 2 : 4;
  for (int k = 0; k < n; ++k) {
    const float a = kTau * hash01(seed + static_cast<uint32_t>(k)), lean = 0.15f + 0.3f * hash01(seed + 10u + static_cast<uint32_t>(k));
    const vec3 d = normalize(v3(0, 1, 0) + around(a) * lean);
    const float h = s * (0.6f + 0.6f * hash01(seed + 20u + static_cast<uint32_t>(k)));
    frustum(b, at, at + d * h, s * 0.18f, 0.0f, 5, col * (1.0f + 0.25f * static_cast<float>(k % 2)), 0, true, false);
  }
}

// A string of prayer flags from a to c on one sway bone.
template <typename B>
void prayer_flags(B &b, vec3 a, vec3 c, uint32_t bone, uint32_t seed) {
  const vec3 cols[5] = {v3(0.2f, 0.4f, 0.8f), v3(0.95f, 0.95f, 0.92f), v3(0.8f, 0.2f, 0.18f), v3(0.25f, 0.6f, 0.3f), v3(0.95f, 0.8f, 0.2f)};
  const float len = length(c - a);
  const int n = maxi_t(3, static_cast<int>(len / 0.45f));
  const vec3 d = normalize(c - a), side = normalize(cross(v3(0, 1, 0), d));
  for (int k = 0; k < n; ++k) {
    const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
    const vec3 p = a + (c - a) * t - v3(0, 0.3f * 4 * t * (1 - t), 0);
    b.box(p - v3(0, 0.12f, 0), d, v3(0, 1, 0), side, v3(0.13f, 0.12f, 0.005f), cols[(k + seed) % 5], bone, false);
  }
}

// A lantern hanging from a bracket; returns where its flame burns.
template <typename B>
vec3 lantern(B &b, vec3 wall, vec3 out, vec3 iron, vec3 glass) {
  const vec3 up = v3(0, 1, 0);
  const vec3 at = wall + out * 0.3f;
  b.box((wall + at) * 0.5f + up * 0.18f, normalize(cross(up, out)), up, out, v3(0.015f, 0.015f, 0.16f), iron, 0, false);
  b.box(at, v3(0.08f, 0.11f, 0.08f), glass, 0, false);
  b.box(at + up * 0.13f, v3(0.1f, 0.02f, 0.1f), iron, 0);
  return at;
}

}  // namespace theme

}  // namespace gate_build
