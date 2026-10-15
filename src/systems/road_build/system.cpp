// road_build: one road cell (design/planner.md, "Roads"): a hex of paving and what stands at its
// edges, by its kind's rules. A road is a way through: it never builds a fence (fence runs along
// the edges the spec marks) or a square's centre (a civic building the square's road rings).
//
// 1. The paving, by surface: dirt (with cart ruts along the way), gravel, cobble, flagstone, or
//    planks, broken into stones by the level of detail, and a skirt down its sides so steps between
//    cells never show a gap. With paving none, a terrain draws the surface and the cell keeps only
//    boards (planks, a boardwalk, a bridge's deck).
// 2. Its flow: the edges to road and gate neighbours, less the two a half takes. A half (where the
//    plan gives one) is the half of the hex that holds edges h and h + 1 whole, cut through the
//    middles of edges h + 2 and h + 5; it holds the kind's edge piece (a market stall, a bench,
//    crates, a planter, a hand cart) facing the free half, which keeps every way through open.
// 3. Each edge, by its neighbour (the mask), except one facing a door (kept clear: a cell in front of
//    a door takes no half either, and no post stands on its corners): open land gets a verge (grass, a kerb, or a ditch) and
//    stays clear where a fence runs; a building's front may get a planter against it, its side or
//    back crates; water gets a stone quay with a bollard, or a grassy bank. Each stays within a
//    strip along its edge, never across the way.
// 4. Posts: a lamp post on a corner by a building or open land, by the kind's rate per cell (its
//    lantern flickers), and a sign post at one junction in four (three or four ways), an arm along each way.
// 5. A bridge is a plank deck on posts with rails along the water.
// 6. Guides for the viewer: each neighbour's outline coloured by what it is, arrows along the flow,
//    the half's outline and cut, and dashes along fence edges.
//
// Every choice hashes the seed and the edge, so a cell's neighbours never reshuffle it. Bones are
// house_bone, animated by house_animate.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "condition.h"
#include "channels/road_model.h"
#include "channels/road_spec.h"
#include "packer/road_model.h"
#include "packer/road_spec.h"

namespace road_build {

using channels::house_bone;
using channels::road_kind;
using channels::road_model;
using channels::road_spec;

namespace {

constexpr float kHalfFlat = 0.8660254f;  // the hex's inner radius
// The edges' directions (toward each neighbour), degrees from +x toward +z: 0, -60, -120, 180, 120, 60.
constexpr float kDirX[6] = {1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f};
constexpr float kDirZ[6] = {0.0f, -0.8660254f, -0.8660254f, 0.0f, 0.8660254f, 0.8660254f};
// Corners at 30 + 60 k degrees; edge d (toward its neighbour at angle t) runs between the corners at
// t - 30 and t + 30: corners (6 - d + 5) % 6 and (6 - d) % 6.
constexpr float kCornerX[6] = {0.8660254f, 0.0f, -0.8660254f, -0.8660254f, 0.0f, 0.8660254f};
constexpr float kCornerZ[6] = {0.5f, 1.0f, 0.5f, -0.5f, -1.0f, -0.5f};

enum Edge : uint32_t { kRoad = 0, kFront = 1, kOpen = 2, kWater = 3, kWall = 4, kGate = 5, kWaterfront = 6, kSide = 7 };

const vec3 kGrass = {0.38f, 0.58f, 0.27f};
const vec3 kStone = {0.58f, 0.57f, 0.54f};
const vec3 kWood = {0.47f, 0.33f, 0.2f};
const vec3 kDarkWood = {0.32f, 0.22f, 0.14f};
const vec3 kIron = {0.2f, 0.2f, 0.22f};

vec3 corner(int k, float y) { return v3(kCornerX[((k % 6) + 6) % 6], y, kCornerZ[((k % 6) + 6) % 6]); }
vec3 dir(int d) { return v3(kDirX[d], 0, kDirZ[d]); }
// The edge's two corners, and its yaw (the edge's own x along it, as physics_shape turns a box).
int edge_corner(int d) { return (6 - d) % 6; }
void edge_corners(int d, float y, vec3 &a, vec3 &b) {
  a = corner(edge_corner(d) + 5, y);
  b = corner(edge_corner(d), y);
}
// A yaw that turns +x onto the edge's tangent (b - a): tangent angle = direction + 90 degrees, and
// physics_shape's yaw turns +x toward -z as it grows, so yaw = -(angle).
float edge_yaw(int d) {
  static const float deg[6] = {0, -60, -120, 180, 120, 60};
  return -(deg[d] + 90.0f) * kPi / 180.0f;
}

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
// A collision box's yaw that turns +x onto a ground direction t (physics_shape's yaw turns +x
// toward -z as it grows): the nearest of the twelve directions a half's cut can take.
float half_yaw(vec3 t) {
  int best = 0;
  float best_d = -2;
  for (int i = 0; i < 12; ++i) {
    const float a = kPi * static_cast<float>(i) / 6.0f;
    const float d = t.x * foundry::det::cos(a) - t.z * foundry::det::sin(a);
    if (d > best_d) {
      best_d = d;
      best = i;
    }
  }
  return kPi * static_cast<float>(best) / 6.0f;
}
// The viewer's colour for each neighbour: road, front, open, water, wall, gate, waterfront, side.
const vec3 kEdgeColor[8] = {{0.78f, 0.7f, 0.56f}, {1.0f, 0.8f, 0.3f}, {0.45f, 0.85f, 0.4f}, {0.35f, 0.6f, 1.0f},
                            {0.72f, 0.72f, 0.78f}, {0.3f, 0.95f, 1.0f}, {0.3f, 0.85f, 0.8f}, {0.9f, 0.55f, 0.35f}};

}  // namespace

bool run(const road_spec &in, road_model &out, std::string &error) {
  const road_kind &k = in.kind;
  if (in.role > 1 || in.season > 3 || in.detail > 2 || in.half > 6 || k.surface > road_kind::kSurfaceBoardwalk || k.verge > 2 || k.half > 5 ||
      in.condition > road_spec::kConditionRuin || in.paving > 1) {
    error = "role, season, detail, half, or the kind's surface, verge, or half out of range";
    return false;
  }
  out = road_model();
  Builder<road_model> b{out};
  b.winter = in.season == road_spec::kSeasonWinter;
  b.sides = in.detail == 0 ? 10 : (in.detail == 1 ? 7 : 5);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_bone::kMotionFixed, 0, 0, 0, 0);
  const uint32_t seed = hash_u32(in.seed * 2654435761u ^ hash_str(k.name.c_str()) ^ (in.mask * 40503u) ^ (in.half * 977u) ^ (in.fences * 7919u));
  auto pick = [&](uint32_t salt) { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); };
  uint32_t edge[6];
  bool way[6], fenced[6], door[6];
  int ways = 0;
  for (int d = 0; d < 6; ++d) {
    edge[d] = (in.mask >> (3 * d)) & 7u;
    way[d] = edge[d] == kRoad || edge[d] == kGate;
    fenced[d] = ((in.fences >> d) & 1u) != 0 && edge[d] == kOpen;
    door[d] = ((in.doors >> d) & 1u) != 0;
    ways += way[d] ? 1 : 0;
  }
  const bool bridge = in.role == road_spec::kRoleBridge;
  // The half: it takes edges h and h + 1, so they leave the flow.
  // A cell in front of a door is a strict way: it takes no half.
  bool any_door = false;
  for (int d = 0; d < 6; ++d) any_door = any_door || door[d];
  const int h = in.half < 6 && k.half != road_kind::kHalfNone && !bridge && !any_door ? static_cast<int>(in.half) : -1;
  bool taken[6] = {false, false, false, false, false, false};
  if (h >= 0) {
    taken[h] = true;
    taken[(h + 1) % 6] = true;
  }
  uint32_t flow = 0;
  for (int d = 0; d < 6; ++d)
    if (way[d] && !taken[d]) flow |= 1u << d;
  out.flow = flow;
  const bool hi = in.detail == 0, mid = in.detail <= 1;
  const float top = 0.03f;
  // Paving shows half-whitened in winter (trodden snow), so mix rather than replace.
  const vec3 pave = b.winter ? lerp(k.surface_color, b.snow, 0.45f) : k.surface_color;
  const vec3 joint = k.detail_color;

  // ---- 1. the paving ----
  // The way through: the mean direction of the flow, for ruts and planks.
  vec3 wayv = v3(0, 0, 0);
  for (int d = 0; d < 6; ++d)
    if ((flow >> d) & 1u) {
      // Opposite edges add along one line, not cancel: fold each into the half-plane of edge 0..2.
      const vec3 v = d < 3 ? dir(d) : -dir(d);
      wayv = wayv + v;
    }
  if (length(wayv) < 1e-3f) wayv = v3(1, 0, 0);
  wayv = normalize(wayv);
  const bool boards = bridge || k.surface == road_kind::kSurfacePlanks || k.surface == road_kind::kSurfaceBoardwalk;
  // paving none: a terrain draws the surface (a map's streets), so only boards are this cell's own.
  const bool paved = in.paving == 0 || boards;
  if (k.surface == road_kind::kSurfaceBoardwalk && !bridge) {
    // A boardwalk stands on piles over the mud: a pile at each corner, and the mud under it.
    for (int s = 0; s < 6; s += 2) {
      const vec3 p = corner(s, 0) * 0.82f;
      b.cylinder(p + v3(0, -0.6f, 0), p + v3(0, top - 0.06f, 0), 0.07f, kWood * 0.7f, 0);
    }
    for (int s = 0; s < 6; ++s) b.triangle(v3(0, -0.12f, 0), corner(s + 5, -0.12f), corner(s, -0.12f), joint * 0.7f, 0, false, v3(0, 1, 0));
  }
  if (boards) {
    // Planks across the way.
    const vec3 across = cross(v3(0, 1, 0), wayv);
    const int n = hi ? 9 : (mid ? 6 : 3);
    for (int i = 0; i < n; ++i) {
      const float t = -0.9f + 1.8f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
      const float shade = 0.85f + 0.2f * pick(100 + static_cast<uint32_t>(i));
      b.box(wayv * t + v3(0, top - 0.04f, 0), wayv, v3(0, 1, 0), across, v3(0.9f / static_cast<float>(n) - 0.01f, 0.05f, 0.95f), kWood * shade, 0);
    }
  } else if (paved) {
    // Six triangles, each split into n x n stones of their own shade.
    const bool soft = k.surface == road_kind::kSurfaceDirt || k.surface == road_kind::kSurfaceSand || k.surface == road_kind::kSurfaceMossStones;
    const int n = soft ? 1 : (hi ? (k.surface == road_kind::kSurfaceFlagstone ? 2 : 4) : (mid ? 2 : 1));
    const float spread = soft ? 0.04f : (k.surface == road_kind::kSurfaceGravel ? 0.16f : 0.12f);
    for (int s = 0; s < 6; ++s) {
      const vec3 A = v3(0, top, 0), B = corner(s + 5, top), C = corner(s, top);
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n - i; ++j) {
          auto P = [&](float u, float v) { return A + (B - A) * (u / static_cast<float>(n)) + (C - A) * (v / static_cast<float>(n)); };
          const float sh0 = 1.0f + spread * (2 * pick(1000 + static_cast<uint32_t>(s * 97 + i * 13 + j)) - 1);
          b.triangle(P(static_cast<float>(i), static_cast<float>(j)), P(static_cast<float>(i + 1), static_cast<float>(j)),
                     P(static_cast<float>(i), static_cast<float>(j + 1)), pave * sh0, 0, false, v3(0, 1, 0));
          if (j + 1 < n - i) {
            const float sh1 = 1.0f + spread * (2 * pick(2000 + static_cast<uint32_t>(s * 97 + i * 13 + j)) - 1);
            b.triangle(P(static_cast<float>(i + 1), static_cast<float>(j)), P(static_cast<float>(i + 1), static_cast<float>(j + 1)),
                       P(static_cast<float>(i), static_cast<float>(j + 1)), pave * sh1, 0, false, v3(0, 1, 0));
          }
        }
    }
    // Stepping stones in moss: flat stones from the middle out along each way.
    if (k.surface == road_kind::kSurfaceMossStones) {
      b.box(v3(0, top + 0.02f, 0), v3(0.28f, 0.03f, 0.24f), joint, 0);
      for (int d = 0; d < 6; ++d) {
        if (!((flow >> d) & 1u) && flow != 0) continue;
        for (int i = 1; i <= 2; ++i) {
          const vec3 at = dir(d) * (0.32f * static_cast<float>(i)) + v3(0, top + 0.02f, 0);
          const float yaw = pick(300 + static_cast<uint32_t>(d * 7 + i)) * 3.0f;
          const vec3 ax = v3(foundry::det::cos(yaw), 0, -foundry::det::sin(yaw)), az = v3(-ax.z, 0, ax.x);
          b.box(at, ax, v3(0, 1, 0), az, v3(0.17f + 0.05f * pick(320 + static_cast<uint32_t>(d * 7 + i)), 0.03f, 0.13f), joint * (0.9f + 0.2f * pick(340 + static_cast<uint32_t>(d))), 0);
        }
      }
    }
    // Sand: wind ripples across it.
    if (k.surface == road_kind::kSurfaceSand && mid) {
      for (int i = -3; i <= 3; ++i) {
        const vec3 c = v3(0.22f * static_cast<float>(i), top + 0.003f, 0.0f);
        b.quad(c + v3(-0.03f, 0, -0.6f), c + v3(0.03f, 0, -0.6f), c + v3(0.03f, 0, 0.6f), c + v3(-0.03f, 0, 0.6f), joint, 0, false, v3(0, 1, 0));
      }
    }
    // Ruts: two darker strips along the way.
    if (k.ruts && flow != 0) {
      const vec3 across = cross(v3(0, 1, 0), wayv);
      for (int s = -1; s <= 1; s += 2) {
        const vec3 c = across * (0.32f * static_cast<float>(s)) + v3(0, top + 0.004f, 0);
        b.quad(c - wayv * 0.82f - across * 0.07f, c + wayv * 0.82f - across * 0.07f, c + wayv * 0.82f + across * 0.07f,
               c - wayv * 0.82f + across * 0.07f, joint, 0, false, v3(0, 1, 0));
      }
    }
  }
  // The skirt: down the sides, so steps between cells never show a gap.
  for (int d = 0; d < 6 && paved; ++d) {
    vec3 a, c;
    edge_corners(d, top, a, c);
    b.quad(a, c, c - v3(0, bridge ? 0.3f : 0.6f, 0), a - v3(0, bridge ? 0.3f : 0.6f, 0), joint * 0.8f, 0, false, dir(d));
  }

  // ---- 2. the half's edge piece, facing the free half ----
  if (h >= 0) {
    // u: from the middle toward the half's far corner; t: along the cut.
    const int cm = (5 - h + 6) % 6;
    const vec3 u = normalize(corner(cm, 0));
    const vec3 t = cross(u, v3(0, 1, 0));
    const vec3 base = u * 0.33f + v3(0, top, 0);
    switch (k.half) {
      case road_kind::kHalfStall: {
        // A table with goods, two posts at its back, and an awning over the table and the cut.
        b.standing(base, t, v3(0.55f, 0.42f, 0.22f), kWood, 0);
        for (int g = 0; g < (hi ? 4 : 2); ++g) {
          const float s = -0.4f + 0.8f * (static_cast<float>(g) + 0.5f) / static_cast<float>(hi ? 4 : 2);
          const float hh = 0.03f + 0.04f * pick(50 + static_cast<uint32_t>(g));
          const vec3 gc = lerp(v3(0.8f, 0.6f, 0.25f), v3(0.5f, 0.7f, 0.3f), pick(60 + static_cast<uint32_t>(g)));
          b.standing(base + t * s + v3(0, 0.84f, 0), t, v3(0.07f, hh, 0.08f), gc, 0, false);
        }
        const vec3 back_l = base + u * 0.28f - t * 0.55f + v3(0, 1.85f, 0), back_r = base + u * 0.28f + t * 0.55f + v3(0, 1.85f, 0);
        b.beam(back_l - v3(0, 1.85f, 0), back_l, 0.03f, kDarkWood, 0);
        b.beam(back_r - v3(0, 1.85f, 0), back_r, 0.03f, kDarkWood, 0);
        const uint32_t aw = b.bone(0, (back_l + back_r) * 0.5f, t, house_bone::kMotionSway, 1.3f, 0.2f, 0.5f, pick(70));
        const vec3 fl = back_l - u * 0.75f - v3(0, 0.3f, 0), fr = back_r - u * 0.75f - v3(0, 0.3f, 0);
        const int stripes = hi ? 4 : 1;
        for (int s2 = 0; s2 < stripes; ++s2) {
          const float t0 = static_cast<float>(s2) / static_cast<float>(stripes), t1 = static_cast<float>(s2 + 1) / static_cast<float>(stripes);
          const vec3 col = (s2 % 2) ? v3(0.95f, 0.93f, 0.86f) : k.half_color;
          b.quad(lerp(back_l, back_r, t0), lerp(back_l, back_r, t1), lerp(fl, fr, t1), lerp(fl, fr, t0), col, aw, true, v3(0, 1, 0));
          b.quad(lerp(back_l, back_r, t0), lerp(fl, fr, t0), lerp(fl, fr, t1), lerp(back_l, back_r, t1), col * 0.8f, aw, false, v3(0, -1, 0));
        }
        b.solid(base + v3(0, 0.42f, 0), v3(0.55f, 0.42f, 0.22f), half_yaw(t));
        break;
      }
      case road_kind::kHalfBench: {
        b.standing(base + v3(0, 0.0f, 0), t, v3(0.6f, 0.22f, 0.18f), kWood, 0);
        b.standing(base + v3(0, 0.44f, 0), t, v3(0.6f, 0.03f, 0.2f), k.half_color, 0);
        b.standing(base + u * 0.17f + v3(0, 0.44f, 0), t, v3(0.6f, 0.25f, 0.03f), kWood * 0.9f, 0);
        b.solid(base + v3(0, 0.3f, 0), v3(0.6f, 0.3f, 0.2f), half_yaw(t));
        break;
      }
      case road_kind::kHalfCrates: {
        b.standing(base - t * 0.3f, t, v3(0.25f, 0.25f, 0.25f), kWood * 1.05f, 0);
        b.standing(base - t * 0.3f + v3(0, 0.5f, 0), t, v3(0.2f, 0.2f, 0.2f), kWood * 0.92f, 0);
        b.cylinder(base + t * 0.3f, base + t * 0.3f + v3(0, 0.7f, 0), 0.24f, kWood * 0.85f, 0);
        b.solid(base + v3(0, 0.35f, 0), v3(0.6f, 0.35f, 0.27f), half_yaw(t));
        break;
      }
      case road_kind::kHalfPlanter: {
        b.standing(base, t, v3(0.6f, 0.25f, 0.2f), kStone * 0.95f, 0);
        const bool bloom = in.season == road_spec::kSeasonSpring || in.season == road_spec::kSeasonSummer;
        const vec3 top_c = in.season == road_spec::kSeasonFall ? v3(0.85f, 0.5f, 0.15f) : (bloom ? v3(0.85f, 0.35f, 0.55f) : v3(0.35f, 0.3f, 0.22f));
        b.standing(base + v3(0, 0.5f, 0), t, v3(0.55f, 0.08f, 0.16f), bloom ? kGrass : top_c, 0);
        if (bloom && hi)
          for (int f = 0; f < 5; ++f) b.box(base + t * (-0.45f + 0.225f * static_cast<float>(f)) + v3(0, 0.64f, 0), v3(0.06f, 0.05f, 0.06f), top_c, 0);
        b.solid(base + v3(0, 0.3f, 0), v3(0.6f, 0.3f, 0.2f), half_yaw(t));
        break;
      }
      case road_kind::kHalfCart: {
        b.standing(base + v3(0, 0.35f, 0), t, v3(0.55f, 0.15f, 0.3f), k.half_color, 0);
        for (int s = -1; s <= 1; s += 2) {
          const vec3 wc = base - t * 0.15f + u * (0.33f * static_cast<float>(s)) + v3(0, 0.3f, 0);
          b.cylinder(wc - u * 0.03f * static_cast<float>(s), wc + u * 0.03f * static_cast<float>(s), 0.3f, kDarkWood, 0);
        }
        b.beam(base + t * 0.55f + v3(0, 0.45f, 0), base + t * 0.85f + v3(0, 0.15f, 0), 0.03f, kWood, 0);
        b.solid(base + v3(0, 0.35f, 0), v3(0.6f, 0.35f, 0.33f), half_yaw(t));
        break;
      }
      default: break;
    }
  }

  // ---- 3. the edges ----
  const float band = (1.0f - clampf(k.paved, 0.4f, 1.0f)) * kHalfFlat;  // the verge's depth from the edge
  int lamp_edge = -1;
  for (int d = 0; d < 6; ++d) {
    vec3 a, c;
    edge_corners(d, top, a, c);
    const vec3 inward = -dir(d), along = normalize(c - a), mid_e = (a + c) * 0.5f;
    const uint32_t e = edge[d];
    if (e == kOpen && !bridge) {
      // The verge (a fence, where one runs, stands on the edge itself).
      const vec3 a2 = a + inward * band * 1.15f, c2 = c + inward * band * 1.15f;
      if (band > 0.01f) {
        const vec3 vc = k.verge == road_kind::kVergeDitch ? kGrass * 0.7f : kGrass;
        const float lift = k.verge == road_kind::kVergeDitch ? -0.05f : 0.006f;
        b.quad(a + v3(0, lift, 0), c + v3(0, lift, 0), c2 + v3(0, lift, 0), a2 + v3(0, lift, 0), vc, 0, true, v3(0, 1, 0));
      }
      if (k.verge == road_kind::kVergeKerb) b.beam(a2 + v3(0, 0.05f, 0), c2 + v3(0, 0.05f, 0), 0.06f, kStone * 0.95f, 0);
      if (lamp_edge < 0 && !fenced[d]) lamp_edge = d;
    } else if (e == kFront && !bridge && !taken[d] && !door[d]) {
      // A planter at the foot of the front.
      if (pick(40 + static_cast<uint32_t>(d)) < k.planters) {
        const vec3 base = mid_e + inward * 0.16f;
        b.standing(base, along, v3(0.32f, 0.2f, 0.11f), kWood * 0.9f, 0);
        const bool bloom = in.season == road_spec::kSeasonSpring || in.season == road_spec::kSeasonSummer;
        const vec3 top_c = in.season == road_spec::kSeasonFall ? v3(0.85f, 0.5f, 0.15f) : (bloom ? v3(0.85f, 0.35f, 0.55f) : v3(0.35f, 0.3f, 0.22f));
        b.standing(base + v3(0, 0.4f, 0), along, v3(0.28f, 0.06f, 0.09f), top_c, 0);
      }
      if (lamp_edge < 0) lamp_edge = d;
    } else if (e == kSide && !bridge && !taken[d]) {
      if (pick(90 + static_cast<uint32_t>(d)) < k.clutter) {
        // A barrel and a crate against the wall.
        const vec3 base = mid_e + inward * 0.2f;
        b.cylinder(base - along * 0.2f, base - along * 0.2f + v3(0, 0.6f, 0), 0.18f, kWood * 0.9f, 0);
        b.standing(base + along * 0.2f, along, v3(0.17f, 0.17f, 0.17f), kWood * 1.1f, 0);
        b.solid(base + v3(0, 0.3f, 0), v3(0.4f, 0.3f, 0.19f), edge_yaw(d));
      }
    } else if (e == kWater && !bridge) {
      if (k.quay) {
        // A stone quay edge and a bollard.
        b.standing(mid_e + inward * 0.12f - v3(0, 0.8f, 0), along, v3(0.5f, 0.42f, 0.12f), kStone * 0.9f, 0);
        if (pick(95 + static_cast<uint32_t>(d)) < 0.5f) {
          const vec3 bo = mid_e + inward * 0.25f;
          b.cylinder(bo, bo + v3(0, 0.45f, 0), 0.1f, kIron, 0);
        }
      } else {
        const vec3 a2 = a + inward * 0.3f, c2 = c + inward * 0.3f;
        b.quad(a + v3(0, -0.15f, 0), c + v3(0, -0.15f, 0), c2 + v3(0, 0.008f, 0), a2 + v3(0, 0.008f, 0), kGrass * 0.9f, 0, true, v3(0, 1, 0));
      }
    }
  }
  // ---- 4. posts: a lamp on a corner of the chosen edge, a sign post at a junction ----
  auto corner_free = [&](int cn) {
    // Neither edge meeting at the corner is taken by the half or fenced.
    const int e1 = (6 - cn) % 6, e2 = (5 - cn + 6) % 6;  // the edges whose corners include cn
    return !taken[e1] && !taken[e2] && !fenced[e1] && !fenced[e2] && !door[e1] && !door[e2];
  };
  if (lamp_edge >= 0 && pick(30) < k.lamps) {  // one roll per cell
    int cn = edge_corner(lamp_edge);
    if (!corner_free(cn)) cn = (cn + 5) % 6;
    if (corner_free(cn)) {
      const vec3 lc = corner(cn, 0);
      const vec3 p = lc * 0.72f + v3(0, top, 0);
      b.cylinder(p, p + v3(0, 3.0f, 0), 0.05f, kIron, 0);
      b.beam(p + v3(0, 2.9f, 0), p + v3(0, 2.9f, 0) - lc * 0.3f, 0.025f, kIron, 0);
      const vec3 lantern = p + v3(0, 2.62f, 0) - lc * 0.3f;
      const uint32_t glow = b.bone(0, lantern, v3(0, 1, 0), house_bone::kMotionFlicker, 3.0f, 0.2f, 0, pick(31));
      b.box(lantern, v3(0.1f, 0.15f, 0.1f), k.lamp_color, glow, false);
      b.solid_cylinder(p, 0.08f, 3.0f);
    }
  }
  // A sign post at one junction cell in four (a wide street's every cell has three ways or more).
  if (k.signs && ways >= 3 && ways <= 4 && !bridge && pick(35) < 0.25f) {
    // The corner touching the fewest ways (and not the half or a fence).
    int best = -1, best_n = 9;
    for (int cn = 0; cn < 6; ++cn) {
      if (!corner_free(cn)) continue;
      const int e1 = (6 - cn) % 6, e2 = (5 - cn + 6) % 6;
      const int n = (way[e1] ? 1 : 0) + (way[e2] ? 1 : 0);
      if (n < best_n) {
        best_n = n;
        best = cn;
      }
    }
    if (best >= 0) {
      const vec3 p = corner(best, 0) * 0.72f + v3(0, top, 0);
      b.cylinder(p, p + v3(0, 2.4f, 0), 0.05f, kDarkWood, 0);
      int arm = 0;
      for (int d = 0; d < 6 && arm < 4; ++d) {
        if (!((flow >> d) & 1u)) continue;
        const vec3 dd = dir(d);
        const vec3 side = cross(v3(0, 1, 0), dd);
        const vec3 c = p + v3(0, 2.15f - 0.22f * static_cast<float>(arm), 0) + dd * 0.28f;
        b.box(c, dd, v3(0, 1, 0), side, v3(0.3f, 0.08f, 0.02f), lerp(kWood, v3(0.9f, 0.85f, 0.7f), 0.3f), 0);
        ++arm;
      }
      b.solid_cylinder(p, 0.08f, 2.4f);
    }
  }
  // ---- 5. a bridge: a plank deck on posts, rails along the water ----
  if (bridge) {
    for (int c2 = 0; c2 < 6; c2 += 2) {
      const vec3 p = corner(c2, 0) * 0.8f;
      b.beam(p + v3(0, -2.0f, 0), p + v3(0, top - 0.05f, 0), 0.08f, kDarkWood, 0, false);
    }
    for (int d = 0; d < 6; ++d) {
      if (edge[d] != kWater && edge[d] != kOpen) continue;
      vec3 a, c;
      edge_corners(d, top, a, c);
      const vec3 in2 = -dir(d) * 0.08f;
      b.beam(a + in2, a + in2 + v3(0, 1.0f, 0), 0.04f, kWood, 0);
      b.beam(c + in2, c + in2 + v3(0, 1.0f, 0), 0.04f, kWood, 0);
      b.beam(a + in2 + v3(0, 0.95f, 0), c + in2 + v3(0, 0.95f, 0), 0.035f, kWood, 0);
      b.solid((a + c) * 0.5f + in2 + v3(0, 0.5f, 0), v3(0.5f, 0.5f, 0.04f), edge_yaw(d));
    }
    b.solid(v3(0, top - 0.12f, 0), v3(kHalfFlat, 0.12f, kHalfFlat));
  }

  // A ruin: its stalls, lamps, and posts fallen, weeds through its paving, overgrown (condition.h).
  if (in.condition == road_spec::kConditionRuin) {
    condition::ruin_over(out, seed ^ 0x5eedu, 0.6f, [](float, float) { return 0.0f; }, 2.5f, false);
    const int tufts = hi ? 9 : (mid ? 5 : 2);
    for (int i = 0; i < tufts; ++i) {
      const float a = pick(400 + static_cast<uint32_t>(i)) * 6.2831853f, r = 0.15f + 0.7f * pick(420 + static_cast<uint32_t>(i));
      const vec3 at = v3(r * foundry::det::cos(a), top, r * foundry::det::sin(a));
      const float th = 0.18f + 0.15f * pick(440 + static_cast<uint32_t>(i));
      for (int s = 0; s < 3; ++s) {
        const float sa = a + 2.1f * static_cast<float>(s);
        b.triangle(at + v3(0.07f * foundry::det::cos(sa), 0, 0.07f * foundry::det::sin(sa)), at - v3(0.07f * foundry::det::cos(sa), 0, 0.07f * foundry::det::sin(sa)), at + v3(0, th, 0), v3(0.38f, 0.52f, 0.26f), 0, false, v3(foundry::det::sin(sa), 0.3f, -foundry::det::cos(sa)));
      }
    }
  }
  // ---- size ----
  float hgt = 0, reach = 0;
  for (const vec3 &p : out.positions) {
    hgt = maxf(hgt, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
  }
  // The conformance holds the collision too.
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      const float r = sh.type == channels::physics_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
      hgt = maxf(hgt, sh.position.y + (sh.type == channels::physics_shape::kTypeBox ? sh.half_extents.y : sh.half_height));
    }
  out.height = hgt;
  out.radius = reach;

  // ---- 6. guides for the viewer ----
  auto line = [&](vec3 a, vec3 c, vec3 col) {
    out.guide_lines.push_back(a);
    out.guide_lines.push_back(c);
    out.guide_colors.push_back(pack(col));
  };
  const float gy = 0.06f;
  for (int d = 0; d < 6; ++d) {
    const vec3 centre = dir(d) * (2.0f * kHalfFlat);
    const vec3 col = kEdgeColor[edge[d]];
    for (int s = 0; s < 6; ++s) {
      line(centre + corner(s, gy) * 0.96f, centre + corner(s + 1, gy) * 0.96f, col);
      line(centre + corner(s, gy) * 0.55f, centre + corner(s + 1, gy) * 0.55f, col * 0.8f);
    }
  }
  const vec3 kFlowC = {0.3f, 0.95f, 1.0f};
  for (int d = 0; d < 6; ++d) {
    if (!((flow >> d) & 1u)) continue;
    const vec3 tip = dir(d) * 1.25f + v3(0, gy, 0), side = cross(v3(0, 1, 0), dir(d));
    line(dir(d) * 0.15f + v3(0, gy, 0), tip, kFlowC);
    line(tip, tip - dir(d) * 0.22f + side * 0.13f, kFlowC);
    line(tip, tip - dir(d) * 0.22f - side * 0.13f, kFlowC);
  }
  if (h >= 0) {
    const vec3 kHalfC = {1.0f, 0.55f, 0.15f};
    const vec3 m0 = dir((h + 5) % 6) * kHalfFlat + v3(0, gy, 0), m1 = dir((h + 2) % 6) * kHalfFlat + v3(0, gy, 0);
    line(m0, m1, kHalfC);
    vec3 prev = m0;
    for (int cn : {(6 - h) % 6, (5 - h + 6) % 6, (4 - h + 6) % 6}) {
      const vec3 p = corner(cn, gy);
      line(prev, p, kHalfC);
      prev = p;
    }
    line(prev, m1, kHalfC);
  }
  for (int d = 0; d < 6; ++d) {
    if (!door[d]) continue;
    vec3 a, c;
    edge_corners(d, gy + 0.03f, a, c);
    line(a, c, v3(1.0f, 0.3f, 0.3f));
    line(a - dir(d) * 0.15f, c - dir(d) * 0.15f, v3(1.0f, 0.3f, 0.3f));
  }
  for (int d = 0; d < 6; ++d) {
    if (!fenced[d]) continue;
    vec3 a, c;
    edge_corners(d, gy + 0.02f, a, c);
    for (int s = 0; s < 6; s += 2) line(lerp(a, c, static_cast<float>(s) / 6.0f), lerp(a, c, static_cast<float>(s + 1) / 6.0f), v3(1, 1, 1));
  }
  return true;
}

}  // namespace road_build

FOUNDRY_SYSTEM(road_build, channels::road_spec, channels::road_model)
