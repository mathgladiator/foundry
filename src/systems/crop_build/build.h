// A mesh builder for this system's model (positions, normals, packed colours, skinning by one bone
// per vertex, bones as house_bone, collision as physics_body). Each system carries its own
// copy (spine.md: write everything twice). Snow whitens what faces up in winter.
#pragma once

#include <string>
#include <vector>

#include "channels/house_bone.h"
#include "channels/physics_body.h"
#include "math.h"

namespace crop_build {

using namespace crop_math;

inline uint32_t pack(vec3 c) {
  auto byte = [](float v) {
    v = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(v);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

template <typename Model>
struct Builder {
  Model &out;
  bool winter = false;
  int sides = 8;
  int detail = 0;  // 0 high, 1 medium, 2 low
  vec3 snow = {0.92f, 0.94f, 0.98f};

  uint32_t vertex(vec3 p, vec3 n, vec3 c, uint32_t bone, bool snowy) {
    if (winter && snowy && n.y > 0.55f) c = snow;
    out.positions.push_back(p);
    out.normals.push_back(n);
    out.colors.push_back(pack(c));
    out.bone_a.push_back(bone);
    out.bone_b.push_back(bone);
    out.bone_w.push_back(0);
    return static_cast<uint32_t>(out.positions.size() - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  }
  void triangle(vec3 a, vec3 b, vec3 c, vec3 col, uint32_t bone, bool snowy, vec3 facing) {
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(n, facing) < 0) n = -n;
    const uint32_t i0 = vertex(a, n, col, bone, snowy);
    const uint32_t i1 = vertex(b, n, col, bone, snowy);
    const uint32_t i2 = vertex(c, n, col, bone, snowy);
    if (dot(cross(b - a, c - a), n) >= 0) tri(i0, i1, i2);
    else tri(i0, i2, i1);
  }
  void quad(vec3 a, vec3 b, vec3 c, vec3 d, vec3 col, uint32_t bone, bool snowy, vec3 facing) {
    triangle(a, b, c, col, bone, snowy, facing);
    triangle(a, c, d, col, bone, snowy, facing);
  }
  // A box with its own axes (unit, orthogonal) and half sizes along them.
  void box(vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, vec3 col, uint32_t bone = 0, bool snowy = true) {
    const vec3 X = ax * h.x, Y = ay * h.y, Z = az * h.z;
    auto k = [&](float sx, float sy, float sz) { return c + X * sx + Y * sy + Z * sz; };
    quad(k(1, -1, -1), k(1, 1, -1), k(1, 1, 1), k(1, -1, 1), col * 0.9f, bone, snowy, ax);
    quad(k(-1, -1, 1), k(-1, 1, 1), k(-1, 1, -1), k(-1, -1, -1), col * 0.8f, bone, snowy, -ax);
    quad(k(-1, 1, -1), k(-1, 1, 1), k(1, 1, 1), k(1, 1, -1), col, bone, snowy, ay);
    quad(k(-1, -1, 1), k(1, -1, 1), k(1, 1, 1), k(-1, 1, 1), col * 0.95f, bone, snowy, az);
    quad(k(1, -1, -1), k(-1, -1, -1), k(-1, 1, -1), k(1, 1, -1), col * 0.75f, bone, snowy, -az);
  }
  void box(vec3 c, vec3 h, vec3 col, uint32_t bone = 0, bool snowy = true) { box(c, v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1), h, col, bone, snowy); }
  // A box from y0 to y1 between x0..x1 and z0..z1 (axis-aligned).
  void block(float x0, float x1, float y0, float y1, float z0, float z1, vec3 col, uint32_t bone = 0, bool snowy = true) {
    box(v3((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f), v3((x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f), col, bone, snowy);
  }
  // A box standing on `base`, its x along `along` (on the ground), half sizes h.
  void standing(vec3 base, vec3 along, vec3 h, vec3 col, uint32_t bone = 0, bool snowy = true) {
    vec3 ax = normalize(v3(along.x, 0, along.z));
    const vec3 az = cross(ax, v3(0, 1, 0));
    box(base + v3(0, h.y, 0), ax, v3(0, 1, 0), az, h, col, bone, snowy);
  }
  // A beam from a to b, square in section.
  void beam(vec3 a, vec3 b, float half, vec3 col, uint32_t bone = 0, bool snowy = true) {
    const vec3 d = b - a;
    const float len = length(d);
    if (len < 1e-4f) return;
    const vec3 ay = d * (1 / len);
    const vec3 ax = perpendicular(ay);
    box((a + b) * 0.5f, ax, ay, cross(ax, ay), v3(half, len * 0.5f, half), col, bone, snowy);
  }
  // A frustum (a cone when r1 is 0) from a (radius r0) to b (radius r1), with n sides.
  void frustum(vec3 a, vec3 b, float r0, float r1, int n, vec3 col, uint32_t bone = 0, bool caps = true, bool snowy = true, float turn = 0) {
    const vec3 ax = normalize(b - a), u = perpendicular(ax), w = cross(ax, u);
    std::vector<vec3> lo, hi;
    for (int k = 0; k <= n; ++k) {
      const float t = turn + 2 * kPi * static_cast<float>(k) / static_cast<float>(n);
      const vec3 d = u * foundry::det::cos(t) + w * foundry::det::sin(t);
      lo.push_back(a + d * r0);
      hi.push_back(b + d * r1);
    }
    for (int k = 0; k < n; ++k) {
      const vec3 mid = (lo[k] + lo[k + 1] + hi[k] + hi[k + 1]) * 0.25f - (a + b) * 0.5f;
      if (r1 > 1e-4f) quad(lo[k], lo[k + 1], hi[k + 1], hi[k], col, bone, snowy, mid);
      else triangle(lo[k], lo[k + 1], b, col, bone, snowy, mid);
    }
    if (!caps) return;
    for (int k = 1; k + 1 < n; ++k) triangle(lo[0], lo[k], lo[k + 1], col * 0.7f, bone, false, -ax);
    if (r1 > 1e-4f)
      for (int k = 1; k + 1 < n; ++k) triangle(hi[0], hi[k], hi[k + 1], col, bone, snowy, ax);
  }
  void cylinder(vec3 a, vec3 b, float r, vec3 col, uint32_t bone = 0, bool caps = true, bool snowy = true) {
    frustum(a, b, r, r, sides, col, bone, caps, snowy);
  }
  // A pyramid over a rectangle centred at c (its base's middle), axes ax and az on the ground.
  void pyramid(vec3 c, vec3 ax, vec3 az, float hx, float hz, float rise, vec3 col, uint32_t bone = 0) {
    const vec3 apex = c + v3(0, rise, 0);
    const vec3 p[4] = {c + ax * hx + az * hz, c - ax * hx + az * hz, c - ax * hx - az * hz, c + ax * hx - az * hz};
    for (int k = 0; k < 4; ++k) {
      const vec3 a = p[k], b = p[(k + 1) % 4];
      triangle(a, b, apex, col * (k % 2 ? 0.85f : 1.0f), bone, true, (a + b) * 0.5f - c + v3(0, rise * 0.3f, 0));
    }
  }
  // A gable roof: ridge along ax over a rectangle centred at c (the eaves' middle), half sizes hx
  // along the ridge and hz across, rising `rise`; overhang o; gable ends filled with `wall`.
  void gable(vec3 c, vec3 ax, float hx, float hz, float rise, float o, vec3 col, vec3 wall, uint32_t bone = 0) {
    const vec3 az = cross(ax, v3(0, 1, 0)) * -1.0f;
    const vec3 r0 = c - ax * (hx + o) + v3(0, rise, 0), r1 = c + ax * (hx + o) + v3(0, rise, 0);
    const float drop = rise * o / maxf(hz, 0.1f);
    const vec3 e00 = c - ax * (hx + o) + az * (hz + o) - v3(0, drop, 0), e01 = c + ax * (hx + o) + az * (hz + o) - v3(0, drop, 0);
    const vec3 e10 = c - ax * (hx + o) - az * (hz + o) - v3(0, drop, 0), e11 = c + ax * (hx + o) - az * (hz + o) - v3(0, drop, 0);
    quad(e00, e01, r1, r0, col, bone, true, az + v3(0, 1, 0));
    quad(e10, e11, r1, r0, col * 0.85f, bone, true, v3(0, 1, 0) - az);
    // Undersides, so the eaves read from below.
    quad(e00, e01, r1, r0, col * 0.5f, bone, false, (az + v3(0, 1, 0)) * -1.0f);
    quad(e10, e11, r1, r0, col * 0.45f, bone, false, (v3(0, 1, 0) - az) * -1.0f);
    for (int s = -1; s <= 1; s += 2) {
      const vec3 e = c + ax * (hx * static_cast<float>(s));
      triangle(e + az * hz, e - az * hz, e + v3(0, rise, 0), wall, bone, false, ax * static_cast<float>(s));
    }
  }
  // Battlements along a to b (at their height): merlons `w` long every 2w, `h` tall, `t` thick.
  void merlons(vec3 a, vec3 b, float w, float h, float t, vec3 col, uint32_t bone = 0) {
    const vec3 d = b - a;
    const float len = length(v3(d.x, 0, d.z));
    if (len < w) return;
    const vec3 ax = normalize(v3(d.x, 0, d.z)), az = cross(ax, v3(0, 1, 0));
    const int n = static_cast<int>(len / (2 * w));
    const float pad = (len - static_cast<float>(n) * 2 * w + w) * 0.5f;
    for (int k = 0; k < n; ++k) {
      const float s = pad + 2 * w * static_cast<float>(k) + w * 0.5f;
      const vec3 base = a + d * (s / len);
      box(base + v3(0, h * 0.5f, 0), ax, v3(0, 1, 0), az, v3(w * 0.5f, h * 0.5f, t * 0.5f), col, bone);
    }
  }
  // A window on a wall facing n (unit, on the ground): a dark pane just proud of the wall, a sill,
  // and (arched) a pointed head.
  void window(vec3 c, vec3 n, float w, float h, vec3 frame, vec3 pane, bool arched, uint32_t bone = 0) {
    const vec3 t = cross(v3(0, 1, 0), n);
    const vec3 p = c + n * 0.03f;
    const vec3 A = p - t * (w * 0.5f) - v3(0, h * 0.5f, 0), B = p + t * (w * 0.5f) - v3(0, h * 0.5f, 0);
    const vec3 C = p + t * (w * 0.5f) + v3(0, h * 0.5f, 0), D = p - t * (w * 0.5f) + v3(0, h * 0.5f, 0);
    quad(A, B, C, D, pane, bone, false, n);
    if (arched) triangle(D, C, p + v3(0, h * 0.5f + w * 0.55f, 0), pane, bone, false, n);
    if (detail == 0) box(p - v3(0, h * 0.5f + 0.04f, 0) + n * 0.03f, t, v3(0, 1, 0), n, v3(w * 0.5f + 0.06f, 0.04f, 0.06f), frame, bone);
  }
  // A banner: a pole and a cloth that sways in the wind about the pole's top.
  void banner(vec3 base, float pole, vec3 along, vec3 col, float phase) {
    beam(base, base + v3(0, pole, 0), 0.03f, v3(0.3f, 0.25f, 0.2f));
    const vec3 top = base + v3(0, pole - 0.05f, 0);
    const uint32_t b = bone(0, top, along, channels::house_bone::kMotionSway, 0.9f, 0.35f, 0.7f, phase);
    const vec3 a = normalize(v3(along.x, 0, along.z));
    quad(top, top + a * 0.7f, top + a * 0.7f - v3(0, 0.55f, 0), top - v3(0, 0.55f, 0), col, b, false, cross(a, v3(0, 1, 0)));
    quad(top, top - v3(0, 0.55f, 0), top + a * 0.7f - v3(0, 0.55f, 0), top + a * 0.7f, col * 0.85f, b, false, cross(v3(0, 1, 0), a));
  }
  // Smoke: puffs rising from `top` on rise bones.
  void smoke(vec3 top, float amount, int puffs, float phase0) {
    const vec3 col = amount > 0.7f ? v3(0.45f, 0.45f, 0.47f) : v3(0.82f, 0.82f, 0.84f);
    const float r = 0.14f + 0.12f * amount;
    for (int k = 0; k < puffs; ++k) {
      float phase = phase0 + static_cast<float>(k) / static_cast<float>(puffs);
      phase -= static_cast<float>(static_cast<int>(phase));
      const uint32_t b = bone(0, top, v3(0, 1, 0), channels::house_bone::kMotionRise, 0.22f + 0.12f * amount, 1.3f + 1.8f * amount, 1.0f, phase);
      if (detail == 2 && k > 0) continue;
      box(top, v3(r, r * 0.85f, r), col, b, false);
    }
  }
  uint32_t bone(int32_t parent, vec3 pivot, vec3 axis, uint32_t motion, float rate, float reach, float drag, float phase) {
    channels::house_bone b;
    b.parent = parent;
    b.pivot = pivot;
    b.axis = normalize(axis);
    b.motion = motion;
    b.rate = rate;
    b.reach = reach;
    b.drag = drag;
    b.phase = phase;
    out.bones.push_back(b);
    return static_cast<uint32_t>(out.bones.size() - 1);
  }
  // Collision: a box turned by yaw about +y (physics_shape's rotation), or a cylinder.
  void solid(vec3 c, vec3 h, float yaw = 0, uint32_t purpose = channels::physics_body::kPurposeSolid, const char *name = "") {
    channels::physics_shape sh;
    sh.type = channels::physics_shape::kTypeBox;
    sh.position = c;
    sh.rotation = v3(0, yaw, 0);
    sh.half_extents = h;
    channels::physics_body body;
    body.purpose = purpose;
    body.name = name;
    body.shapes.push_back(sh);
    out.collision.push_back(body);
  }
  void solid_cylinder(vec3 base, float r, float h) {
    channels::physics_shape sh;
    sh.type = channels::physics_shape::kTypeCylinder;
    sh.position = base + v3(0, h * 0.5f, 0);
    sh.rotation = v3(0, 0, 0);
    sh.radius = r;
    sh.half_height = h * 0.5f;
    channels::physics_body body;
    body.purpose = channels::physics_body::kPurposeSolid;
    body.shapes.push_back(sh);
    out.collision.push_back(body);
  }
};

}  // namespace crop_build
