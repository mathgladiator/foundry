// A mesh builder for this system's model (positions, normals, packed colours, skinning by one bone
// per vertex, bones as house_bone, collision as physics_body). Each system carries its own
// copy (spine.md: write everything twice). Snow whitens what faces up in winter.
#pragma once

#include <vector>

#include "channels/house_bone.h"
#include "channels/physics_body.h"
#include "math.h"

namespace waterfront_build {

using namespace waterfront_math;

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
  void box(vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, vec3 col, uint32_t bone, bool snowy = true) {
    const vec3 X = ax * h.x, Y = ay * h.y, Z = az * h.z;
    auto k = [&](float sx, float sy, float sz) { return c + X * sx + Y * sy + Z * sz; };
    quad(k(1, -1, -1), k(1, 1, -1), k(1, 1, 1), k(1, -1, 1), col * 0.9f, bone, snowy, ax);
    quad(k(-1, -1, 1), k(-1, 1, 1), k(-1, 1, -1), k(-1, -1, -1), col * 0.8f, bone, snowy, -ax);
    quad(k(-1, 1, -1), k(-1, 1, 1), k(1, 1, 1), k(1, 1, -1), col, bone, snowy, ay);
    quad(k(-1, -1, 1), k(1, -1, 1), k(1, 1, 1), k(-1, 1, 1), col * 0.95f, bone, snowy, az);
    quad(k(1, -1, -1), k(-1, -1, -1), k(-1, 1, -1), k(1, 1, -1), col * 0.75f, bone, snowy, -az);
  }
  void box(vec3 c, vec3 h, vec3 col, uint32_t bone = 0, bool snowy = true) { box(c, v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1), h, col, bone, snowy); }
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
  void cylinder(vec3 a, vec3 b, float r, vec3 col, uint32_t bone, bool caps = true, bool snowy = true) {
    const vec3 ax = normalize(b - a), u = perpendicular(ax), w = cross(ax, u);
    std::vector<uint32_t> lo, hi;
    for (int k = 0; k <= sides; ++k) {
      const float t = 2 * kPi * static_cast<float>(k) / static_cast<float>(sides);
      const vec3 d = u * foundry::det::cos(t) + w * foundry::det::sin(t);
      lo.push_back(vertex(a + d * r, d, col, bone, snowy));
      hi.push_back(vertex(b + d * r, d, col, bone, snowy));
    }
    for (int k = 0; k < sides; ++k) {
      tri(lo[k], hi[k], hi[k + 1]);
      tri(lo[k], hi[k + 1], lo[k + 1]);
    }
    if (!caps) return;
    for (int e = 0; e < 2; ++e) {
      const vec3 c = e == 0 ? a : b, nn = e == 0 ? -ax : ax;
      const uint32_t mid = vertex(c, nn, col, bone, snowy);
      std::vector<uint32_t> ring;
      for (int k = 0; k <= sides; ++k) {
        const float t = 2 * kPi * static_cast<float>(k) / static_cast<float>(sides);
        ring.push_back(vertex(c + (u * foundry::det::cos(t) + w * foundry::det::sin(t)) * r, nn, col, bone, snowy));
      }
      for (int k = 0; k < sides; ++k) {
        if (e == 0) tri(mid, ring[k + 1], ring[k]);
        else tri(mid, ring[k], ring[k + 1]);
      }
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
  void solid(vec3 c, vec3 h, float yaw = 0, uint32_t purpose = channels::physics_body::kPurposeSolid) {
    channels::physics_shape sh;
    sh.type = channels::physics_shape::kTypeBox;
    sh.position = c;
    sh.rotation = v3(0, yaw, 0);
    sh.half_extents = h;
    channels::physics_body body;
    body.purpose = purpose;
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

}  // namespace waterfront_build
