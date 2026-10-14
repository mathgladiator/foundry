// house_build_v1: a village house, store, or workshop, seen from outside (design/systems.md,
// Village houses).
//
// 1. The lot: sized by the household (solo, family, extended) and shaped by the type (wide, deep,
//    or round): rectWxH brick-laid or a hex flower, and the largest rectangle inside its cells. The
//    house stands at the back of it, wider at each stage, with a front yard; a deep house turns its
//    gable to the street.
// 2. The stage: construction (a tent for each generation and a campfire on a staked plot, a
//    foundation half laid, a scaffold, lumber), basic (one plain room, thatch or planks, the
//    simplest sign), deluxe (the type's walls and roof, chimney, shutters, window boxes, porch or
//    awning, its real sign, its feature and doodads), premium (a stone base, a second storey where
//    the type has one (an extended household has it from deluxe), lanterns, banners, a fence).
// 3. Walls by material (timber framing, planks, logs, stone, brick), a gable roof by covering
//    (thatch, shingle, slate, tile), windows, the door on a hinge bone.
// 4. Moving parts as bones: smoke puffs rising from the chimney, the hanging sign, banners,
//    laundry, the awning's valance and the tents' flaps swinging about their seams in the wind,
//    the waterwheel and windmill sails spinning, the weathervane turning with the wind, the glow
//    of a forge flickering.
// 5. Seasons: snow on what faces up, icicles, a bigger woodpile in winter; flowers and the garden
//    by season; leaves and pumpkins in the fall.
// 6. Yard things placed on the lot by a seeded order, clear of the house, the door's path, and
//    each other.
// 7. Collision: the house and roof solid, big yard things and the fence solid, the doorway a sensor.
//
// Bones never depend on the level of detail (the runtime keeps one pose for every level); detail
// only drops small geometry.
#include <vector>

#include "boundary.h"
#include "channels/house_v1_model.h"
#include "channels/house_v1_spec.h"
#include "condition.h"
#include "math.h"
#include "packer/house_v1_model.h"
#include "theme.h"
#include "packer/house_v1_spec.h"

namespace house_build_v1 {

using namespace house_math;
using channels::house_v1_bone;
using channels::house_v1_model;
using channels::house_v1_spec;
using channels::house_v1_type;

namespace {

constexpr float kSqrt3 = 1.7320508f;
const vec3 kSnow = {0.92f, 0.94f, 0.98f};
const vec3 kGlass = {0.16f, 0.20f, 0.26f};
const vec3 kIron = {0.20f, 0.20f, 0.22f};
const vec3 kStraw = {0.80f, 0.68f, 0.38f};
const vec3 kWood = {0.47f, 0.33f, 0.20f};
const vec3 kDarkWood = {0.30f, 0.21f, 0.13f};
const vec3 kStone = {0.56f, 0.55f, 0.52f};
const vec3 kCloth = {0.86f, 0.82f, 0.72f};
const vec3 kDirt = {0.42f, 0.32f, 0.22f};
const vec3 kWater = {0.30f, 0.52f, 0.72f};
const vec3 kFlame = {1.00f, 0.62f, 0.18f};

vec3 mixv(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
float tanf_(float a) { return foundry::det::sin(a) / foundry::det::cos(a); }
int maxi(int a, int b) { return a > b ? a : b; }

uint32_t pack(vec3 c) {
  auto byte = [](float v) {
    v = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(v);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

// ---- the lot ----

struct Footprint {
  int count;
  int cells[72][2];      // q, r, relative to the cell under the origin
  float cx, cz, hx, hz;  // the largest rectangle inside the cells, shrunk by 0.1 m (measured)
  uint32_t round;        // the round footprint that holds it: 19, 37, 61, 91, or 127 cells
  bool member[25][25];   // whether cell (q, r) is in it, at [q + 12][r + 12]
  bool has(int q, int r) const { return q >= -12 && q <= 12 && r >= -12 && r <= 12 && member[q + 12][r + 12]; }
};

// The lots, by household (solo, family, extended) and shape (wide, deep, round, long, broad):
// rectWxH is H rows of W cells, brick-laid (row r starts at q = -floor(r / 2)), so the plot is
// nearly rectangular; flowerN is a hex of hexes. Each is anchored at the cell nearest its middle,
// and its rectangle was measured by rasterizing the cells (the largest inside them).
struct Lots {
  int w, h;     // rect W x H, or w = 0 and h rings for a flower
  int aq, ar;   // the anchor cell, before moving it to the origin
  float cx, cz, hx, hz;
  uint32_t round;
};
constexpr uint32_t kShapes = 5, kLotCount = 15;
const Lots kLots[kLotCount] = {
    {5, 4, 1, 2, 0.44f, -0.75f, 3.80f, 2.65f, 37},   // solo: rect5x4 (20 cells)
    {4, 5, 1, 2, -0.41f, 0.00f, 2.95f, 3.40f, 37},   //       rect4x5 (20)
    {0, 2, 0, 0, 0.04f, 0.00f, 3.30f, 2.00f, 19},    //       flower19
    {3, 8, -1, 4, 0.44f, -0.75f, 2.03f, 5.65f, 61},  //       rect3x8 (24), long
    {7, 4, 2, 2, 0.45f, -0.75f, 5.50f, 2.65f, 61},   //       rect7x4 (28), broad
    {6, 5, 2, 2, -0.45f, 0.00f, 4.65f, 3.40f, 61},   // family: rect6x5 (30)
    {5, 6, 1, 2, 0.44f, 0.75f, 3.80f, 4.15f, 61},    //         rect5x6 (30)
    {0, 3, 0, 0, 0.00f, 0.00f, 4.15f, 3.50f, 37},    //         flower37
    {4, 9, 0, 4, -0.41f, 0.00f, 2.90f, 6.40f, 61},   //         rect4x9 (36), long
    {9, 5, 3, 2, 0.42f, 0.00f, 7.25f, 3.40f, 91},    //         rect9x5 (45), broad
    {8, 6, 3, 2, -0.43f, 0.75f, 6.40f, 4.15f, 91},   // extended: rect8x6 (48)
    {6, 8, 1, 3, 0.42f, 0.75f, 4.65f, 5.65f, 91},    //           rect6x8 (48)
    {0, 4, 0, 0, 0.02f, 0.00f, 5.10f, 4.95f, 61},    //           flower61
    {5, 11, 0, 5, -0.43f, 0.00f, 3.75f, 7.90f, 91},  //           rect5x11 (55), long
    {10, 6, 4, 2, -0.44f, 0.75f, 8.12f, 4.15f, 127}, //           rect10x6 (60), broad
};

Footprint footprint(uint32_t index) {
  const Lots &l = kLots[index];
  Footprint f{};
  f.count = 0;
  if (l.w > 0) {
    for (int r = 0; r < l.h; ++r)
      for (int i = 0; i < l.w; ++i) {
        f.cells[f.count][0] = i - r / 2 - l.aq;
        f.cells[f.count][1] = r - l.ar;
        ++f.count;
      }
  } else {
    for (int q = -l.h; q <= l.h; ++q)
      for (int r = -l.h; r <= l.h; ++r) {
        const int s = q + r;
        if ((q < 0 ? -q : q) + (r < 0 ? -r : r) + (s < 0 ? -s : s) > 2 * l.h) continue;
        f.cells[f.count][0] = q;
        f.cells[f.count][1] = r;
        ++f.count;
      }
  }
  for (int k = 0; k < f.count; ++k) f.member[f.cells[k][0] + 12][f.cells[k][1] + 12] = true;
  f.cx = l.cx;
  f.cz = l.cz;
  f.hx = l.hx;
  f.hz = l.hz;
  f.round = l.round;
  return f;
}

float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
float cell_z(int r) { return 1.5f * static_cast<float>(r); }

// The cell a point is in (pointy-top, circumradius 1): axial coordinates, rounded in cube space.
void cell_at(float x, float z, int &q, int &r) {
  const float fq = kSqrt3 / 3.0f * x - z / 3.0f, fr = 2.0f / 3.0f * z, fs = -fq - fr;
  float rq = std::floor(fq + 0.5f), rr = std::floor(fr + 0.5f);
  const float rs = std::floor(fs + 0.5f);
  const float dq = rq > fq ? rq - fq : fq - rq, dr = rr > fr ? rr - fr : fr - rr, ds = rs > fs ? rs - fs : fs - rs;
  if (dq > dr && dq > ds) rq = -rr - rs;
  else if (dr > ds) rr = -rq - rs;
  q = static_cast<int>(rq);
  r = static_cast<int>(rr);
}

// The radius a round footprint holds (its inner circle, shrunk by 0.1 m; design/plotter.md): each
// ring adds 1.5 * 2 / sqrt(3) m.
float round_radius(uint32_t cells) {
  return cells <= 7 ? 2.5f : (cells <= 19 ? 4.23f : (cells <= 37 ? 5.96f : (cells <= 61 ? 7.69f : (cells <= 91 ? 9.42f : 11.15f))));
}

struct Lot {
  const Footprint *fp;
  // Inside the cells and inside the round footprint that holds them, so a plotter that only knows
  // round footprints reserves no more than the house's own.
  bool inside(float x, float z) const {
    const float R = round_radius(fp->round);
    if (x * x + z * z > R * R) return false;
    int q, r;
    cell_at(x, z, q, r);
    return fp->has(q, r);
  }
  // Inside, at least `margin` from the lot's edge (checked at twelve points around).
  bool inside(float x, float z, float margin) const {
    if (!inside(x, z)) return false;
    for (int k = 0; k < 12; ++k) {
      const float a = kPi * static_cast<float>(k) / 6.0f;
      if (!inside(x + margin * foundry::det::cos(a), z + margin * foundry::det::sin(a))) return false;
    }
    return true;
  }
};

// An axis-aligned rectangle on the ground.
struct Rect {
  float cx, cz, hx, hz;
  bool contains(float x, float z, float grow) const {
    return x > cx - hx - grow && x < cx + hx + grow && z > cz - hz - grow && z < cz + hz + grow;
  }
};

// ---- geometry ----

struct Icosphere {
  std::vector<vec3> v;
  std::vector<uint32_t> i;
};

Icosphere icosphere(int subdivisions) {
  const float t = 1.618034f;
  Icosphere s;
  if (subdivisions < 0) {
    s.v = {v3(1, 0, 0), v3(-1, 0, 0), v3(0, 1, 0), v3(0, -1, 0), v3(0, 0, 1), v3(0, 0, -1)};
    s.i = {2, 4, 0, 2, 0, 5, 2, 5, 1, 2, 1, 4, 3, 0, 4, 3, 5, 0, 3, 1, 5, 3, 4, 1};
    return s;
  }
  s.v = {v3(-1, t, 0), v3(1, t, 0), v3(-1, -t, 0), v3(1, -t, 0), v3(0, -1, t), v3(0, 1, t),
         v3(0, -1, -t), v3(0, 1, -t), v3(t, 0, -1), v3(t, 0, 1), v3(-t, 0, -1), v3(-t, 0, 1)};
  for (auto &p : s.v) p = normalize(p);
  s.i = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
         3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
  for (int d = 0; d < subdivisions; ++d) {
    std::vector<uint32_t> next;
    std::vector<std::pair<uint64_t, uint32_t>> mids;
    auto mid = [&](uint32_t a, uint32_t b) {
      const uint64_t key = a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
      for (const auto &m : mids)
        if (m.first == key) return m.second;
      s.v.push_back(normalize(s.v[a] + s.v[b]));
      const uint32_t id = static_cast<uint32_t>(s.v.size() - 1);
      mids.push_back({key, id});
      return id;
    };
    for (size_t k = 0; k < s.i.size(); k += 3) {
      const uint32_t a = s.i[k], b = s.i[k + 1], c = s.i[k + 2];
      const uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
      const uint32_t tris[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
      next.insert(next.end(), tris, tris + 12);
    }
    s.i = next;
  }
  return s;
}

struct Builder {
  house_v1_model &out;
  bool winter;
  uint32_t detail;
  Icosphere ico;
  int sides;  // round things' sides at this detail

  uint32_t vertex(vec3 p, vec3 n, vec3 c, uint32_t bone, bool snow) {
    if (winter && snow && n.y > 0.55f) c = kSnow;
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
  // A flat quad (a b c d around), its normal facing `facing` (or from its winding if zero).
  void quad(vec3 a, vec3 b, vec3 c, vec3 d, vec3 col, uint32_t bone, bool snow, vec3 facing = v3(0, 0, 0)) {
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(facing, facing) > 0 && dot(n, facing) < 0) n = -n;
    const uint32_t i0 = vertex(a, n, col, bone, snow), i1 = vertex(b, n, col, bone, snow);
    const uint32_t i2 = vertex(c, n, col, bone, snow), i3 = vertex(d, n, col, bone, snow);
    tri(i0, i1, i2);
    tri(i0, i2, i3);
  }
  void triangle(vec3 a, vec3 b, vec3 c, vec3 col, uint32_t bone, bool snow, vec3 facing) {
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(n, facing) < 0) n = -n;
    // One vertex per statement: argument order is unspecified, and GCC and Clang differ.
    const uint32_t i0 = vertex(a, n, col, bone, snow);
    const uint32_t i1 = vertex(b, n, col, bone, snow);
    const uint32_t i2 = vertex(c, n, col, bone, snow);
    tri(i0, i1, i2);
  }
  // A box with its own axes (unit, orthogonal) and half sizes along them.
  void box(vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, vec3 col, uint32_t bone, bool snow = true) {
    const vec3 X = ax * h.x, Y = ay * h.y, Z = az * h.z;
    auto corner = [&](float sx, float sy, float sz) { return c + X * sx + Y * sy + Z * sz; };
    quad(corner(1, -1, -1), corner(1, 1, -1), corner(1, 1, 1), corner(1, -1, 1), col, bone, snow, ax);
    quad(corner(-1, -1, 1), corner(-1, 1, 1), corner(-1, 1, -1), corner(-1, -1, -1), col, bone, snow, -ax);
    quad(corner(-1, 1, -1), corner(-1, 1, 1), corner(1, 1, 1), corner(1, 1, -1), col, bone, snow, ay);
    quad(corner(-1, -1, 1), corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), col, bone, snow, -ay);
    quad(corner(-1, -1, 1), corner(1, -1, 1), corner(1, 1, 1), corner(-1, 1, 1), col, bone, snow, az);
    quad(corner(1, -1, -1), corner(-1, -1, -1), corner(-1, 1, -1), corner(1, 1, -1), col, bone, snow, -az);
  }
  void box(vec3 c, vec3 h, vec3 col, uint32_t bone = 0, bool snow = true) {
    box(c, v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1), h, col, bone, snow);
  }
  // A beam from a to b, square in section (2 half wide), its sides turned to `side`.
  void beam(vec3 a, vec3 b, float half, vec3 side, vec3 col, uint32_t bone = 0, bool snow = true) {
    const vec3 d = b - a;
    const float len = length(d);
    if (len < 1e-4f) return;
    const vec3 ay = d * (1 / len);
    vec3 ax = side - ay * dot(side, ay);
    ax = length(ax) > 1e-4f ? normalize(ax) : perpendicular(ay);
    box((a + b) * 0.5f, ax, ay, cross(ax, ay), v3(half, len * 0.5f, half), col, bone, snow);
  }
  // A thick slab under the quad a b c d (around), `thick` along its normal away from `up`.
  void slab(vec3 a, vec3 b, vec3 c, vec3 d, float thick, vec3 col, uint32_t bone, bool snow, vec3 up) {
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(n, up) < 0) n = -n;
    const vec3 o = n * -thick;
    quad(a, b, c, d, col, bone, snow, n);
    quad(a + o, b + o, c + o, d + o, col * 0.8f, bone, false, -n);
    const vec3 p[4] = {a, b, c, d};
    const vec3 mid = (a + b + c + d) * 0.25f;
    for (int k = 0; k < 4; ++k) {
      const vec3 p0 = p[k], p1 = p[(k + 1) % 4];
      quad(p0, p1, p1 + o, p0 + o, col * 0.9f, bone, false, (p0 + p1) * 0.5f - mid);
    }
  }
  // A cylinder from a to b; caps optional.
  void cylinder(vec3 a, vec3 b, float r, vec3 col, uint32_t bone, bool caps = true, bool snow = true, int n = 0) {
    if (n == 0) n = sides;
    const vec3 ax = normalize(b - a), u = perpendicular(ax), w = cross(ax, u);
    std::vector<uint32_t> lo, hi;
    for (int k = 0; k <= n; ++k) {
      const float t = 2 * kPi * static_cast<float>(k) / static_cast<float>(n);
      const vec3 d = u * foundry::det::cos(t) + w * foundry::det::sin(t);
      lo.push_back(vertex(a + d * r, d, col, bone, snow));
      hi.push_back(vertex(b + d * r, d, col, bone, snow));
    }
    for (int k = 0; k < n; ++k) {
      tri(lo[k], hi[k], hi[k + 1]);
      tri(lo[k], hi[k + 1], lo[k + 1]);
    }
    if (!caps) return;
    for (int e = 0; e < 2; ++e) {
      const vec3 c = e == 0 ? a : b, nn = e == 0 ? -ax : ax;
      const uint32_t mid = vertex(c, nn, col, bone, snow);
      std::vector<uint32_t> ring;
      for (int k = 0; k <= n; ++k) {
        const float t = 2 * kPi * static_cast<float>(k) / static_cast<float>(n);
        ring.push_back(vertex(c + (u * foundry::det::cos(t) + w * foundry::det::sin(t)) * r, nn, col, bone, snow));
      }
      for (int k = 0; k < n; ++k) tri(mid, ring[k], ring[k + 1]);
    }
  }
  // A cone from a base circle at `base` to `apex`.
  void cone(vec3 base, vec3 apex, float r, vec3 col, uint32_t bone, bool snow = true) {
    const vec3 ax = normalize(apex - base), u = perpendicular(ax), w = cross(ax, u);
    const float slope = r / maxf(length(apex - base), 1e-3f);
    for (int k = 0; k < sides; ++k) {
      const float t0 = 2 * kPi * static_cast<float>(k) / static_cast<float>(sides);
      const float t1 = 2 * kPi * static_cast<float>(k + 1) / static_cast<float>(sides);
      const vec3 d0 = u * foundry::det::cos(t0) + w * foundry::det::sin(t0), d1 = u * foundry::det::cos(t1) + w * foundry::det::sin(t1);
      const vec3 n0 = normalize(d0 + ax * slope), n1 = normalize(d1 + ax * slope), nm = normalize(n0 + n1);
      const uint32_t i0 = vertex(base + d0 * r, n0, col, bone, snow);
      const uint32_t i1 = vertex(base + d1 * r, n1, col, bone, snow);
      const uint32_t i2 = vertex(apex, nm, col, bone, snow);
      tri(i0, i1, i2);
    }
  }
  // A lump: the icosphere scaled, roughened by noise, normals from its faces.
  void blob(vec3 c, vec3 scale, float bump, uint32_t seed, vec3 col, uint32_t bone, bool snow = true) {
    blob(c, v3(scale.x, 0, 0), v3(0, scale.y, 0), v3(0, 0, scale.z), bump, seed, col, bone, snow);
  }
  // The same along three axes of its own (each scaled by the lump's half size along it).
  void blob(vec3 c, vec3 ax, vec3 ay, vec3 az, float bump, uint32_t seed, vec3 col, uint32_t bone, bool snow = true) {
    std::vector<vec3> pos(ico.v.size());
    for (size_t k = 0; k < ico.v.size(); ++k) {
      const vec3 d = ico.v[k];
      const float n = noise(d * 2.3f + v3(static_cast<float>(seed % 97), static_cast<float>(seed % 89), 0), seed);
      pos[k] = c + (ax * d.x + ay * d.y + az * d.z) * (1 + bump * (n - 0.5f) * 2);
    }
    std::vector<vec3> nrm(ico.v.size(), v3(0, 0, 0));
    for (size_t k = 0; k < ico.i.size(); k += 3) {
      const vec3 fn = cross(pos[ico.i[k + 1]] - pos[ico.i[k]], pos[ico.i[k + 2]] - pos[ico.i[k]]);
      for (int j = 0; j < 3; ++j) nrm[ico.i[k + j]] = nrm[ico.i[k + j]] + fn;
    }
    const uint32_t first = static_cast<uint32_t>(out.positions.size());
    for (size_t k = 0; k < pos.size(); ++k) vertex(pos[k], normalize(nrm[k]), col, bone, snow);
    for (size_t k = 0; k < ico.i.size(); k += 3) tri(first + ico.i[k], first + ico.i[k + 1], first + ico.i[k + 2]);
  }

  uint32_t bone(int32_t parent, vec3 pivot, vec3 axis, uint32_t motion, float rate, float reach, float drag, float phase) {
    house_v1_bone b;
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
};

// ---- collision ----

void solid_box(house_v1_model &out, vec3 c, vec3 h, uint32_t purpose = channels::physics_v1_body::kPurposeSolid,
               const char *name = "") {
  channels::physics_v1_shape s;
  s.type = channels::physics_v1_shape::kTypeBox;
  s.position = c;
  s.rotation = v3(0, 0, 0);
  s.half_extents = h;
  channels::physics_v1_body b;
  b.purpose = purpose;
  b.name = name;
  b.shapes.push_back(s);
  out.collision.push_back(b);
}

void solid_cylinder(house_v1_model &out, vec3 base, float r, float h) {
  channels::physics_v1_shape s;
  s.type = channels::physics_v1_shape::kTypeCylinder;
  s.position = base + v3(0, h * 0.5f, 0);
  s.rotation = v3(0, 0, 0);
  s.radius = r;
  s.half_height = h * 0.5f;
  channels::physics_v1_body b;
  b.purpose = channels::physics_v1_body::kPurposeSolid;
  b.shapes.push_back(s);
  out.collision.push_back(b);
}

// ---- the house ----

struct House {
  Rect body;         // the walls' rectangle
  bool along_z = false;  // the ridge runs front to back (a deep house)
  float wall_h = 2.4f;  // eaves height
  float ridge = 3.5f;   // the roof's top
  float door_x = 0, door_w = 0.9f, door_h = 1.9f;
  float front() const { return body.cz + body.hz; }
};

// One window on a wall: the wall's outward normal n, its centre on the wall face, its size.
void window(Builder &bd, vec3 at, vec3 n, float w, float h, const house_v1_type &t, bool shutters, bool box_flowers,
            vec3 flower, bool frames) {
  const vec3 up = v3(0, 1, 0), side = cross(up, n);
  bd.box(at + n * 0.02f, side, up, n, v3(w * 0.5f, h * 0.5f, 0.03f), kGlass, 0, false);
  if (frames) {
    bd.box(at + n * 0.04f, side, up, n, v3(0.03f, h * 0.5f, 0.03f), t.trim_color, 0, false);         // the mullion
    bd.box(at + n * 0.05f - up * (h * 0.5f + 0.03f), side, up, n, v3(w * 0.5f + 0.06f, 0.035f, 0.07f), t.trim_color, 0);  // the sill
    bd.box(at + n * 0.04f + up * (h * 0.5f + 0.03f), side, up, n, v3(w * 0.5f + 0.04f, 0.035f, 0.04f), t.trim_color, 0);
  }
  if (shutters)
    for (int s = -1; s <= 1; s += 2)
      bd.box(at + n * 0.05f + side * (static_cast<float>(s) * (w * 0.5f + w * 0.27f)), side, up, n, v3(w * 0.25f, h * 0.52f, 0.025f),
             t.shutter_color, 0, false);
  if (box_flowers) {
    const vec3 base = at + n * 0.14f - up * (h * 0.5f + 0.14f);
    bd.box(base, side, up, n, v3(w * 0.5f + 0.04f, 0.08f, 0.09f), t.shutter_color * 0.85f, 0);
    if (flower.x >= 0) {
      const int n_fl = bd.detail == 2 ? 2 : 4;
      for (int k = 0; k < n_fl; ++k) {
        const float u = (static_cast<float>(k) + 0.5f) / static_cast<float>(n_fl) - 0.5f;
        bd.blob(base + up * 0.11f + side * (u * w), v3(0.09f, 0.07f, 0.08f), 0.3f, 31u + static_cast<uint32_t>(k),
                k % 2 ? flower : v3(0.30f, 0.52f, 0.24f), 0, false);
      }
    }
  }
}

// Walls of one storey, from y0 to y1, around the body, in the type's material.
void walls(Builder &bd, const House &h, float y0, float y1, const house_v1_type &t, uint32_t material, vec3 colour, Rng &rng) {
  const Rect &b = h.body;
  const float x0 = b.cx - b.hx, x1 = b.cx + b.hx, z0 = b.cz - b.hz, z1 = b.cz + b.hz;
  const bool hi = bd.detail == 0, mid = bd.detail <= 1;
  const vec3 X = v3(1, 0, 0), Y = v3(0, 1, 0), Z = v3(0, 0, 1);
  if (material == house_v1_type::kWallsLog) {
    // Logs stacked, crossing past each other at the corners.
    const float r = 0.13f;
    const int rows = static_cast<int>((y1 - y0) / (2 * r));
    for (int k = 0; k < rows; ++k) {
      const float y = y0 + r + 2 * r * static_cast<float>(k);
      const vec3 c = colour * (0.9f + 0.1f * static_cast<float>(k % 2));
      const float stick = mid ? 0.22f : 0.0f;
      const bool along_x = (k % 2) == 0;
      for (int s = 0; s < 2; ++s) {
        const float zz = s ? z1 - r : z0 + r, xx = s ? x1 - r : x0 + r;
        if (along_x) bd.cylinder(v3(x0 - stick, y, zz), v3(x1 + stick, y, zz), r, c, 0);
        else bd.cylinder(v3(xx, y, z0 - stick), v3(xx, y, z1 + stick), r, c, 0);
      }
      if (!mid) continue;
      for (int s = 0; s < 2; ++s) {  // the other pair this row, so every row closes
        const float zz = s ? z1 - r : z0 + r, xx = s ? x1 - r : x0 + r;
        if (!along_x) bd.cylinder(v3(x0, y + r * 0.3f, zz), v3(x1, y + r * 0.3f, zz), r * 0.95f, c * 0.95f, 0, false);
        else bd.cylinder(v3(xx, y + r * 0.3f, z0), v3(xx, y + r * 0.3f, z1), r * 0.95f, c * 0.95f, 0, false);
      }
    }
    // A wall behind them, so no gap shows at low detail.
    bd.box(v3(b.cx, (y0 + y1) * 0.5f, b.cz), v3(b.hx - 0.1f, (y1 - y0) * 0.5f, b.hz - 0.1f), colour * 0.8f, 0, false);
    return;
  }
  bd.box(v3(b.cx, (y0 + y1) * 0.5f, b.cz), v3(b.hx, (y1 - y0) * 0.5f, b.hz), colour, 0, false);
  if (material == house_v1_type::kWallsTimber) {
    // Framing: corner posts, the sill and top plates, and braces between.
    const float d = 0.07f;
    const vec3 tc = t.trim_color;
    for (int sx = 0; sx < 2; ++sx)
      for (int sz = 0; sz < 2; ++sz)
        bd.box(v3(sx ? x1 : x0, (y0 + y1) * 0.5f, sz ? z1 : z0), v3(d + 0.02f, (y1 - y0) * 0.5f, d + 0.02f), tc, 0, false);
    for (int s = 0; s < 2; ++s) {
      bd.box(v3(b.cx, y0 + 0.08f, s ? z1 : z0), v3(b.hx, 0.08f, d), tc, 0);
      bd.box(v3(b.cx, y1 - 0.06f, s ? z1 : z0), v3(b.hx, 0.06f, d), tc, 0);
      bd.box(v3(s ? x1 : x0, y0 + 0.08f, b.cz), v3(d, 0.08f, b.hz), tc, 0);
      bd.box(v3(s ? x1 : x0, y1 - 0.06f, b.cz), v3(d, 0.06f, b.hz), tc, 0);
    }
    if (mid) {
      // Studs every metre or so on the long walls, braces at the ends.
      const int studs = static_cast<int>(2 * b.hx / 1.1f);
      for (int s = 0; s < 2; ++s) {
        const float zz = s ? z1 + 0.02f : z0 - 0.02f;
        for (int k = 1; k < studs; ++k) {
          const float x = x0 + 2 * b.hx * static_cast<float>(k) / static_cast<float>(studs);
          if (s && x > h.door_x - h.door_w * 0.5f - 0.1f && x < h.door_x + h.door_w * 0.5f + 0.1f) continue;
          bd.box(v3(x, (y0 + y1) * 0.5f, zz), v3(0.05f, (y1 - y0) * 0.5f, 0.03f), tc, 0, false);
        }
        if (hi) {
          const float lean = rng.uniform() < 0.5f ? 1.0f : -1.0f;
          bd.beam(v3(x0 + 0.1f, y0 + 0.2f, zz), v3(x0 + 0.1f + 0.7f, y1 - 0.15f, zz), 0.04f, Z, tc);
          bd.beam(v3(x1 - 0.1f, y0 + 0.2f, zz), v3(x1 - 0.1f - 0.7f * lean * lean, y1 - 0.15f, zz), 0.04f, Z, tc);
        }
      }
      for (int s = 0; s < 2; ++s) {
        const float xx = s ? x1 + 0.02f : x0 - 0.02f;
        bd.box(v3(xx, (y0 + y1) * 0.5f, b.cz), v3(0.03f, (y1 - y0) * 0.5f, 0.05f), tc, 0, false);
      }
    }
  } else if (material == house_v1_type::kWallsPlank) {
    if (mid) {  // battens: horizontal boards, each a little proud of the last
      const float step = hi ? 0.28f : 0.56f;
      for (float y = y0 + step; y < y1 - 0.05f; y += step)
        for (int s = 0; s < 2; ++s) {
          bd.box(v3(b.cx, y, s ? z1 + 0.012f : z0 - 0.012f), v3(b.hx, 0.02f, 0.012f), colour * 0.78f, 0, false);
          bd.box(v3(s ? x1 + 0.012f : x0 - 0.012f, y, b.cz), v3(0.012f, 0.02f, b.hz), colour * 0.78f, 0, false);
        }
    }
    for (int sx = 0; sx < 2; ++sx)
      for (int sz = 0; sz < 2; ++sz)
        bd.box(v3(sx ? x1 : x0, (y0 + y1) * 0.5f, sz ? z1 : z0), v3(0.07f, (y1 - y0) * 0.5f, 0.07f), t.trim_color, 0, false);
  } else {
    // Stone or brick: quoins at the corners, alternating.
    const float step = material == house_v1_type::kWallsStone ? 0.34f : 0.24f;
    const vec3 q = material == house_v1_type::kWallsStone ? colour * 0.85f : t.trim_color;
    int k = 0;
    for (float y = y0 + step * 0.5f; y < y1 && mid; y += step, ++k) {
      const float lx = k % 2 ? 0.22f : 0.14f, lz = k % 2 ? 0.14f : 0.22f;
      for (int sx = 0; sx < 2; ++sx)
        for (int sz = 0; sz < 2; ++sz)
          bd.box(v3(sx ? x1 - lx + 0.03f : x0 + lx - 0.03f, y, sz ? z1 - lz + 0.03f : z0 + lz - 0.03f),
                 v3(lx, step * 0.45f, lz), q * (0.95f + 0.1f * static_cast<float>((k + sx + sz) % 2)), 0, false);
    }
    if (hi && material == house_v1_type::kWallsStone) {
      // A few stones standing proud, so the wall reads as stone.
      for (int s = 0; s < 14; ++s) {
        const float u = rng.uniform();
        const float v = rng.uniform();
        const int face = static_cast<int>(rng.uniform() * 4.0f) % 4;
        const float y = y0 + 0.2f + v * (y1 - y0 - 0.4f);
        vec3 at, n;
        if (face < 2) {
          at = v3(x0 + 0.3f + u * (2 * b.hx - 0.6f), y, face ? z1 : z0);
          n = v3(0, 0, face ? 1.0f : -1.0f);
        } else {
          at = v3(face == 3 ? x1 : x0, y, z0 + 0.3f + u * (2 * b.hz - 0.6f));
          n = v3(face == 3 ? 1.0f : -1.0f, 0, 0);
        }
        if (face == 1 && at.x > h.door_x - h.door_w && at.x < h.door_x + h.door_w) continue;
        const vec3 side = cross(Y, n);
        bd.box(at, side, Y, n, v3(0.16f, 0.1f, 0.02f), colour * (0.82f + 0.12f * u), 0, false);
      }
    }
    (void)X;
  }
}

// A frame over the body with u along the ridge and v across it: the ridge runs along x (a wide
// house, eaves to the street), or along z (a deep house, its gable to the street).
struct RoofFrame {
  const Rect &b;
  bool along_z;
  float hu() const { return along_z ? b.hz : b.hx; }
  float hv() const { return along_z ? b.hx : b.hz; }
  vec3 at(float u, float y, float v) const { return along_z ? v3(b.cx + v, y, b.cz + u) : v3(b.cx + u, y, b.cz + v); }
  vec3 dir(float u, float y, float v) const { return along_z ? v3(v, y, u) : v3(u, y, v); }
};

// The roof: a gable over the body (eaves at y0), its covering by `roof`.
void gable_roof(Builder &bd, const RoofFrame &f, float y0, float pitch_deg, uint32_t roof, vec3 colour, vec3 gable_colour, float &ridge_y) {
  const float over = 0.32f;
  const float hu = f.hu(), hv = f.hv();
  const float d = hv + over;
  const float rise = hv * tanf_(pitch_deg * kDeg);
  ridge_y = y0 + rise;
  const float u0 = -hu - over * 0.7f, u1 = hu + over * 0.7f;
  const float drop = over * tanf_(pitch_deg * kDeg);
  const float thick = roof == house_v1_type::kRoofThatch ? 0.26f : 0.1f;
  const vec3 up = v3(0, 1, 0);
  // The two slopes, from the ridge out past the walls.
  for (int s = -1; s <= 1; s += 2) {
    const float vs = static_cast<float>(s);
    const vec3 r0 = f.at(u0, ridge_y + thick * 0.6f, 0), r1 = f.at(u1, ridge_y + thick * 0.6f, 0);
    const vec3 e0 = f.at(u0, y0 - drop + thick * 0.6f, vs * d), e1 = f.at(u1, y0 - drop + thick * 0.6f, vs * d);
    bd.slab(r0, r1, e1, e0, thick, colour, 0, true, up);
    if (bd.detail == 0 && roof != house_v1_type::kRoofThatch) {
      // Courses: a darker line every row of shingles, slates, or tiles.
      const int rows = roof == house_v1_type::kRoofTile ? 6 : 8;
      for (int k = 1; k < rows; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(rows);
        const vec3 a = r0 + (e0 - r0) * u, c = r1 + (e1 - r1) * u;
        bd.beam(a + v3(0, 0.015f, 0), c + v3(0, 0.015f, 0), 0.018f, f.dir(0, 0, vs), colour * 0.72f);
      }
    }
  }
  // The ridge cap.
  const vec3 cap = roof == house_v1_type::kRoofThatch ? colour * 0.85f : colour * 0.7f;
  bd.beam(f.at(u0, ridge_y + thick * 0.65f, 0), f.at(u1, ridge_y + thick * 0.65f, 0), roof == house_v1_type::kRoofThatch ? 0.16f : 0.07f,
          f.dir(0, 0, 1), cap);
  // Gable ends, in the wall's colour.
  for (int s = -1; s <= 1; s += 2) {
    const float u = static_cast<float>(s) * hu;
    bd.triangle(f.at(u, y0, -hv), f.at(u, y0, hv), f.at(u, ridge_y, 0), gable_colour, 0, false, f.dir(static_cast<float>(s), 0, 0));
  }
}

// A sign's symbol on a face centred at c, with right r, up u, and outward n, about `s` across.
void symbol(Builder &bd, uint32_t kind, vec3 c, vec3 r, vec3 u, vec3 n, float s, uint32_t bone) {
  const vec3 o = c + n * (s * 0.06f);
  const vec3 gold = v3(0.90f, 0.72f, 0.25f), silver = v3(0.80f, 0.82f, 0.86f);
  auto part = [&](vec3 at, vec3 half, vec3 col) { bd.box(o + r * (at.x * s) + u * (at.y * s), r, u, n, half * s, col, bone, false); };
  auto tilted = [&](vec3 a, vec3 b, float half, vec3 col) {
    bd.beam(o + r * (a.x * s) + u * (a.y * s), o + r * (b.x * s) + u * (b.y * s), half * s, n, col, bone, false);
  };
  switch (kind) {
    case house_v1_type::kSymbolMug:
      part(v3(-0.05f, -0.05f, 0), v3(0.2f, 0.28f, 0.08f), v3(0.86f, 0.62f, 0.22f));
      part(v3(-0.05f, 0.28f, 0), v3(0.24f, 0.08f, 0.09f), v3(0.97f, 0.95f, 0.88f));
      part(v3(0.24f, -0.03f, 0), v3(0.07f, 0.15f, 0.07f), v3(0.86f, 0.62f, 0.22f));
      break;
    case house_v1_type::kSymbolAnvil:
      part(v3(0, 0.14f, 0), v3(0.38f, 0.09f, 0.08f), kIron);
      part(v3(-0.42f, 0.17f, 0), v3(0.08f, 0.05f, 0.07f), kIron);
      part(v3(0, -0.04f, 0), v3(0.12f, 0.1f, 0.08f), kIron);
      part(v3(0, -0.2f, 0), v3(0.26f, 0.06f, 0.08f), kIron);
      break;
    case house_v1_type::kSymbolLoaf:
      bd.blob(o, r * (s * 0.38f), u * (s * 0.2f), n * (s * 0.1f), 0.06f, 7u, v3(0.80f, 0.52f, 0.24f), bone, false);
      for (int k = -1; k <= 1; ++k) tilted(v3(0.12f * k - 0.05f, 0.04f, 0), v3(0.12f * k + 0.06f, 0.17f, 0), 0.02f, v3(0.95f, 0.8f, 0.55f));
      break;
    case house_v1_type::kSymbolNeedle:
      tilted(v3(-0.32f, -0.32f, 0), v3(0.32f, 0.32f, 0), 0.025f, silver);
      for (int k = 0; k < 5; ++k) {
        const float t = static_cast<float>(k) / 4.0f;
        part(v3(0.24f - 0.5f * t, 0.12f + 0.12f * foundry::det::sin(t * 6.0f), 0), v3(0.05f, 0.025f, 0.03f), v3(0.82f, 0.18f, 0.2f));
      }
      break;
    case house_v1_type::kSymbolPotion:
      bd.blob(o - u * (s * 0.1f), r * (s * 0.24f), u * (s * 0.24f), n * (s * 0.12f), 0.02f, 9u, v3(0.42f, 0.85f, 0.55f), bone, false);
      part(v3(0, 0.2f, 0), v3(0.07f, 0.1f, 0.06f), v3(0.75f, 0.85f, 0.88f));
      part(v3(0, 0.32f, 0), v3(0.06f, 0.04f, 0.06f), kWood);
      break;
    case house_v1_type::kSymbolCoins:
      for (int k = 0; k < 3; ++k) {
        const vec3 at = o + r * (s * (-0.22f + 0.22f * static_cast<float>(k))) + u * (s * (k == 1 ? 0.12f : -0.08f));
        bd.cylinder(at, at + n * (s * 0.05f), s * 0.17f, gold, bone, true, false);
      }
      break;
    case house_v1_type::kSymbolHammer:
      tilted(v3(-0.28f, -0.32f, 0), v3(0.12f, 0.22f, 0), 0.04f, kWood);
      bd.box(o + r * (s * 0.14f) + u * (s * 0.26f), normalize(r - u), normalize(r + u), n, v3(s * 0.08f, s * 0.2f, s * 0.07f), kIron, bone, false);
      break;
    case house_v1_type::kSymbolWheat:
      for (int k = -1; k <= 1; ++k) {
        tilted(v3(0.0f, -0.35f, 0), v3(0.18f * k, 0.12f, 0), 0.015f, v3(0.75f, 0.62f, 0.3f));
        bd.blob(o + r * (s * 0.2f * k) + u * (s * 0.25f), r * (s * 0.06f), u * (s * 0.15f), n * (s * 0.05f), 0.1f, 11u,
                v3(0.92f, 0.78f, 0.35f), bone, false);
      }
      break;
    case house_v1_type::kSymbolBed:
      part(v3(0, -0.12f, 0), v3(0.38f, 0.1f, 0.07f), kWood);
      part(v3(0.05f, 0.02f, 0), v3(0.3f, 0.07f, 0.08f), v3(0.3f, 0.42f, 0.7f));
      part(v3(-0.28f, 0.04f, 0), v3(0.1f, 0.06f, 0.09f), v3(0.95f, 0.95f, 0.92f));
      break;
    default:
      break;
  }
}

// Smoke: puffs rising from `top` on rise bones; the bones are made at every detail, the puffs drawn
// only as detail allows.
void smoke(Builder &bd, vec3 top, float amount, int puffs, Rng &rng) {
  const vec3 col = amount > 0.7f ? v3(0.45f, 0.45f, 0.47f) : v3(0.82f, 0.82f, 0.84f);
  const float r = 0.14f + 0.12f * amount;
  const float base_phase = rng.uniform();
  for (int k = 0; k < puffs; ++k) {
    const float phase = base_phase + static_cast<float>(k) / static_cast<float>(puffs);
    const uint32_t b = bd.bone(0, top, v3(0, 1, 0), house_v1_bone::kMotionRise, 0.22f + 0.12f * amount, 1.3f + 1.8f * amount, 1.0f,
                               phase - std::floor(phase));
    if (bd.detail == 2 && k > 0) continue;
    bd.blob(top, v3(r, r * 0.85f, r), 0.25f, 40u + static_cast<uint32_t>(k), col, b, false);
  }
}

// A glow (a forge's fire, an oven's mouth, a lantern): a bright lump on a flicker bone.
void glow(Builder &bd, vec3 at, vec3 size, vec3 col, float reach, Rng &rng) {
  const float rate = 2.3f + rng.uniform();
  const float phase = rng.uniform();
  const uint32_t b = bd.bone(0, at, v3(0, 1, 0), house_v1_bone::kMotionFlicker, rate, reach, 0, phase);
  bd.blob(at, size, 0.2f, 61u, col, b, false);
}

// Flowers by season (none in winter): the colour of the blooms, or x < 0 for none.
vec3 bloom(uint32_t season, float pick) {
  switch (season) {
    case house_v1_spec::kSeasonSpring: return pick < 0.5f ? v3(0.96f, 0.62f, 0.78f) : v3(0.98f, 0.86f, 0.32f);
    case house_v1_spec::kSeasonSummer: return pick < 0.5f ? v3(0.86f, 0.22f, 0.25f) : v3(0.62f, 0.36f, 0.82f);
    case house_v1_spec::kSeasonFall: return pick < 0.5f ? v3(0.92f, 0.50f, 0.15f) : v3(0.75f, 0.28f, 0.16f);
    default: return v3(-1, -1, -1);
  }
}

// ---- yard things ----

struct Spot {
  float x, z, r;
};

struct Yard {
  const Lot &lot;
  Rect body;
  float door_x, front;
  std::vector<Spot> taken;
  std::vector<Rect> blocked;
  struct Candidate {
    float x, z, clear, order;
  };
  std::vector<Candidate> cands;

  Yard(const Lot &l, Rect b, float dx, float fz, uint32_t seed) : lot(l), body(b), door_x(dx), front(fz) {
    float lo_x = 1e9f, hi_x = -1e9f, lo_z = 1e9f, hi_z = -1e9f;
    for (int k = 0; k < l.fp->count; ++k) {
      const float x = cell_x(l.fp->cells[k][0], l.fp->cells[k][1]), z = cell_z(l.fp->cells[k][1]);
      lo_x = minf(lo_x, x - 1);
      hi_x = maxf(hi_x, x + 1);
      lo_z = minf(lo_z, z - 1);
      hi_z = maxf(hi_z, z + 1);
    }
    uint32_t n = 0;
    for (float z = lo_z; z <= hi_z; z += 0.3f)
      for (float x = lo_x; x <= hi_x; x += 0.3f, ++n) {
        // Nothing stands under the house, and most of a big lot is well clear of its edge.
        if (b.contains(x, z, 0.15f) || !l.inside(x, z, 0.15f)) continue;
        float clear = 0.15f;
        if (l.inside(x, z, 1.2f)) clear = 1.2f;
        else
          for (float m = 0.3f; m < 1.2f; m += 0.15f) {
            if (!l.inside(x, z, m)) break;
            clear = m;
          }
        cands.push_back({x, z, clear, static_cast<float>(hash_u32(seed ^ hash_u32(n + 77u)) >> 8) / 16777216.0f});
      }
  }
  // Where a thing of radius r can stand: near the walls when `wall`, else anywhere free, by the seed.
  bool find(float r, bool wall, float &x, float &z) {
    float best = 1e9f;
    bool found = false;
    for (const Candidate &c : cands) {
      if (c.clear < r) continue;
      if (body.contains(c.x, c.z, r + 0.05f)) continue;
      if (c.z > front - 0.2f && c.x > door_x - 0.6f - r && c.x < door_x + 0.6f + r) continue;  // the door's path
      bool clash = false;
      for (const Spot &s : taken)
        if ((c.x - s.x) * (c.x - s.x) + (c.z - s.z) * (c.z - s.z) < (r + s.r) * (r + s.r)) clash = true;
      for (const Rect &b : blocked)
        if (b.contains(c.x, c.z, r)) clash = true;
      if (clash) continue;
      float score = c.order;
      if (wall) {
        const float dx = maxf(0, maxf(body.cx - body.hx - c.x, c.x - body.cx - body.hx));
        const float dz = maxf(0, maxf(body.cz - body.hz - c.z, c.z - body.cz - body.hz));
        score = dx + dz + 0.25f * c.order;
      }
      if (score < best) {
        best = score;
        x = c.x;
        z = c.z;
        found = true;
      }
    }
    if (found) taken.push_back({x, z, r});
    return found;
  }
};

// Axes turned by yaw about +Y (x toward -z as yaw grows, as physics_v1_shape's rotation).
void yaw_axes(float yaw, vec3 &ax, vec3 &az) {
  ax = v3(foundry::det::cos(yaw), 0, -foundry::det::sin(yaw));
  az = v3(foundry::det::sin(yaw), 0, foundry::det::cos(yaw));
}

void turned_box(house_v1_model &out, vec3 c, vec3 h, float yaw) {
  solid_box(out, c, h);
  out.collision.back().shapes.back().rotation = v3(0, yaw, 0);
}

// One yard thing standing at (x, z), turned by yaw.
void doodad(Builder &bd, house_v1_model &out, const char *what, float x, float z, float yaw, const house_v1_type &t, uint32_t season,
            Rng &rng) {
  vec3 ax, az;
  yaw_axes(yaw, ax, az);
  const vec3 up = v3(0, 1, 0), p = v3(x, 0, z);
  const bool mid = bd.detail <= 1;
  const uint32_t h = hash_str(what);
  if (h == hash_str("barrels")) {
    const int n = bd.detail == 0 ? 3 : 2;
    for (int k = 0; k < n; ++k) {
      const vec3 c = p + ax * (0.27f * static_cast<float>(k == 2 ? 0 : 2 * k - 1)) + az * (k == 2 ? 0.3f : 0.0f);
      bd.cylinder(c, c + up * 0.72f, 0.25f, kWood * (0.95f + 0.1f * static_cast<float>(k % 2)), 0);
      if (mid)
        for (float y : {0.14f, 0.56f}) bd.cylinder(c + up * y, c + up * (y + 0.05f), 0.262f, kIron, 0, false);
    }
    solid_cylinder(out, p, 0.45f, 0.72f);
  } else if (h == hash_str("crates")) {
    bd.box(p + up * 0.25f, ax, up, az, v3(0.25f, 0.25f, 0.25f), kWood * 1.15f, 0);
    vec3 bx, bz;
    yaw_axes(yaw + 0.35f, bx, bz);
    bd.box(p + up * 0.68f + ax * 0.04f, bx, up, bz, v3(0.18f, 0.18f, 0.18f), kWood * 1.05f, 0);
    if (mid) bd.box(p + up * 0.25f + az * 0.252f, ax, up, az, v3(0.25f, 0.03f, 0.005f), kDarkWood, 0, false);
    turned_box(out, p + up * 0.43f, v3(0.27f, 0.43f, 0.27f), yaw);
  } else if (h == hash_str("sacks")) {
    for (int k = 0; k < 3; ++k)
      bd.blob(p + ax * (0.22f * static_cast<float>(k - 1)) + up * (0.2f + (k == 1 ? 0.15f : 0.0f)) + az * (k == 1 ? -0.08f : 0.0f),
              v3(0.19f, 0.22f, 0.17f), 0.15f, 70u + static_cast<uint32_t>(k), kCloth * 0.92f, 0);
  } else if (h == hash_str("woodpile")) {
    const int rows = season == house_v1_spec::kSeasonWinter ? 4 : 2;
    for (int r = 0; r < rows; ++r)
      for (int k = 0; k < 4; ++k) {
        const vec3 c = p + az * (0.17f * static_cast<float>(k) - 0.26f + (r % 2 ? 0.08f : 0.0f)) + up * (0.09f + 0.15f * static_cast<float>(r));
        bd.cylinder(c - ax * 0.45f, c + ax * 0.45f, 0.08f, mixv(kWood, v3(0.62f, 0.48f, 0.3f), static_cast<float>((r + k) % 2)), 0, true, true,
                    bd.detail == 2 ? 4 : 6);
      }
    turned_box(out, p + up * (0.08f * rows), v3(0.47f, 0.08f * rows, 0.36f), yaw);
  } else if (h == hash_str("bench")) {
    bd.box(p + up * 0.42f, ax, up, az, v3(0.55f, 0.03f, 0.16f), kWood, 0);
    for (int s = -1; s <= 1; s += 2) bd.box(p + up * 0.2f + ax * (0.42f * static_cast<float>(s)), ax, up, az, v3(0.04f, 0.2f, 0.14f), kDarkWood, 0, false);
  } else if (h == hash_str("anvil")) {
    bd.cylinder(p, p + up * 0.5f, 0.22f, kWood * 0.9f, 0);
    bd.box(p + up * 0.62f, ax, up, az, v3(0.24f, 0.06f, 0.09f), kIron, 0);
    bd.box(p + up * 0.54f, ax, up, az, v3(0.09f, 0.05f, 0.07f), kIron, 0);
    bd.box(p + up * 0.64f + ax * 0.3f, ax, up, az, v3(0.08f, 0.035f, 0.05f), kIron, 0);
    solid_cylinder(out, p, 0.3f, 0.66f);
  } else if (h == hash_str("cart")) {
    bd.box(p + up * 0.55f, ax, up, az, v3(0.55f, 0.05f, 0.34f), kWood, 0);
    for (int s = -1; s <= 1; s += 2) {
      bd.box(p + up * 0.72f + az * (0.33f * static_cast<float>(s)), ax, up, az, v3(0.55f, 0.13f, 0.025f), kWood * 0.9f, 0, false);
      const vec3 w = p + up * 0.34f + az * (0.42f * static_cast<float>(s));
      bd.cylinder(w - az * 0.04f, w + az * 0.04f, 0.34f, kDarkWood, 0, true, false, bd.detail == 2 ? 6 : 10);
    }
    bd.beam(p + up * 0.55f + ax * 0.5f + az * 0.2f, p + up * 0.05f + ax * 1.05f + az * 0.2f, 0.03f, az, kWood);
    bd.beam(p + up * 0.55f + ax * 0.5f - az * 0.2f, p + up * 0.05f + ax * 1.05f - az * 0.2f, 0.03f, az, kWood);
    bd.blob(p + up * 0.8f, ax * 0.42f, up * 0.2f, az * 0.25f, 0.2f, 81u, rng.uniform() < 0.5f ? kStraw : kCloth * 0.9f, 0);
    turned_box(out, p + up * 0.45f, v3(0.6f, 0.45f, 0.45f), yaw);
  } else if (h == hash_str("well")) {
    bd.cylinder(p, p + up * 0.7f, 0.55f, kStone, 0, false);
    bd.cylinder(p + up * 0.69f, p + up * 0.72f, 0.55f, kStone * 0.9f, 0);
    bd.cylinder(p + up * 0.6f, p + up * 0.721f, 0.42f, kWater * 0.6f, 0, true, false);
    for (int s = -1; s <= 1; s += 2) bd.beam(p + ax * (0.5f * static_cast<float>(s)), p + ax * (0.5f * static_cast<float>(s)) + up * 1.65f, 0.05f, az, kWood);
    bd.beam(p + up * 1.4f - ax * 0.55f, p + up * 1.4f + ax * 0.55f, 0.04f, az, kWood);
    for (int s = -1; s <= 1; s += 2)
      bd.slab(p + up * 1.95f - ax * 0.7f, p + up * 1.95f + ax * 0.7f, p + up * 1.55f + ax * 0.7f + az * (0.6f * static_cast<float>(s)),
              p + up * 1.55f - ax * 0.7f + az * (0.6f * static_cast<float>(s)), 0.06f, t.roof_color, 0, true, up);
    if (mid) bd.cylinder(p + up * 0.95f, p + up * 1.15f, 0.1f, kWood, 0);
    solid_cylinder(out, p, 0.6f, 0.75f);
  } else if (h == hash_str("stall")) {
    bd.box(p + up * 0.82f, ax, up, az, v3(0.7f, 0.05f, 0.32f), kWood, 0);
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sz = -1; sz <= 1; sz += 2)
        bd.beam(p + ax * (0.66f * static_cast<float>(sx)) + az * (0.28f * static_cast<float>(sz)),
                p + ax * (0.66f * static_cast<float>(sx)) + az * (0.28f * static_cast<float>(sz)) + up * (sz < 0 ? 1.9f : 1.7f), 0.03f, az, kWood);
    const int stripes = bd.detail == 2 ? 1 : 4;
    for (int k = 0; k < stripes; ++k) {
      const float a = -0.75f + 1.5f * static_cast<float>(k) / static_cast<float>(stripes), b = a + 1.5f / static_cast<float>(stripes);
      bd.slab(p + ax * a + up * 1.95f - az * 0.4f, p + ax * b + up * 1.95f - az * 0.4f, p + ax * b + up * 1.72f + az * 0.45f,
              p + ax * a + up * 1.72f + az * 0.45f, 0.02f, k % 2 ? kCloth : t.accent, 0, true, up);
    }
    const vec3 goods[3] = {v3(0.85f, 0.25f, 0.2f), v3(0.95f, 0.75f, 0.25f), v3(0.4f, 0.65f, 0.3f)};
    for (int k = 0; k < (bd.detail == 2 ? 2 : 5); ++k)
      bd.blob(p + up * 0.94f + ax * (-0.5f + 0.25f * static_cast<float>(k)), v3(0.1f, 0.08f, 0.12f), 0.2f, 90u + static_cast<uint32_t>(k),
              goods[k % 3], 0);
    turned_box(out, p + up * 0.45f, v3(0.72f, 0.45f, 0.34f), yaw);
  } else if (h == hash_str("beehives")) {
    bd.box(p + up * 0.2f, ax, up, az, v3(0.4f, 0.04f, 0.2f), kWood, 0);
    for (int s = -1; s <= 1; s += 2) {
      const vec3 c = p + up * 0.24f + ax * (0.22f * static_cast<float>(s));
      bd.cylinder(c, c + up * 0.18f, 0.16f, kStraw, 0, false);
      bd.cone(c + up * 0.18f, c + up * 0.42f, 0.16f, kStraw * 0.95f, 0);
    }
    turned_box(out, p + up * 0.3f, v3(0.42f, 0.3f, 0.22f), yaw);
  } else if (h == hash_str("hay")) {
    bd.box(p + up * 0.22f, ax, up, az, v3(0.42f, 0.22f, 0.24f), kStraw, 0);
    bd.box(p + up * 0.66f + ax * 0.08f, ax, up, az, v3(0.42f, 0.22f, 0.24f), kStraw * 0.95f, 0);
    if (mid) bd.box(p + up * 0.22f + az * 0.245f, ax, up, az, v3(0.42f, 0.02f, 0.005f), kStraw * 0.7f, 0, false);
    turned_box(out, p + up * 0.44f, v3(0.5f, 0.44f, 0.25f), yaw);
  } else if (h == hash_str("trough")) {
    bd.box(p + up * 0.2f, ax, up, az, v3(0.6f, 0.2f, 0.22f), kWood, 0, false);
    bd.box(p + up * 0.36f, ax, up, az, v3(0.55f, 0.03f, 0.17f), season == house_v1_spec::kSeasonWinter ? v3(0.8f, 0.88f, 0.95f) : kWater, 0, false);
    turned_box(out, p + up * 0.2f, v3(0.6f, 0.2f, 0.22f), yaw);
  } else if (h == hash_str("cauldron")) {
    for (int k = 0; k < 3; ++k) {
      const float a = 2 * kPi * static_cast<float>(k) / 3.0f;
      bd.beam(p + ax * (0.45f * foundry::det::cos(a)) + az * (0.45f * foundry::det::sin(a)), p + up * 1.1f, 0.025f, up, kDarkWood);
    }
    bd.blob(p + up * 0.45f, v3(0.3f, 0.24f, 0.3f), 0.05f, 95u, kIron, 0);
    bd.cylinder(p + up * 0.6f, p + up * 0.64f, 0.24f, v3(0.45f, 0.75f, 0.4f), 0, true, false);
    glow(bd, p + up * 0.12f, v3(0.16f, 0.12f, 0.16f), kFlame, 0.25f, rng);
    smoke(bd, p + up * 0.7f, 0.1f, 2, rng);
    solid_cylinder(out, p, 0.35f, 0.7f);
  }
}

// The laundry line: two posts and cloths on sway bones.
void laundry(Builder &bd, const house_v1_type &t, float x, float z, float yaw, Rng &rng) {
  vec3 ax, az;
  yaw_axes(yaw, ax, az);
  const vec3 up = v3(0, 1, 0), p = v3(x, 0, z);
  for (int s = -1; s <= 1; s += 2) bd.beam(p + ax * (0.9f * static_cast<float>(s)), p + ax * (0.9f * static_cast<float>(s)) + up * 1.7f, 0.03f, az, kWood);
  bd.beam(p + up * 1.62f - ax * 0.9f, p + up * 1.62f + ax * 0.9f, 0.008f, az, kCloth * 0.8f, 0, false);
  const vec3 cols[3] = {kCloth, t.accent, t.shutter_color * 1.2f};
  for (int k = 0; k < 3; ++k) {
    const vec3 pin = p + up * 1.62f + ax * (-0.55f + 0.55f * static_cast<float>(k));
    const float rate = 1.1f + 0.3f * rng.uniform();
    const float phase = rng.uniform();
    const uint32_t b = bd.bone(0, pin, ax, house_v1_bone::kMotionSway, rate, 0.8f, 1.2f, phase);
    const float w = 0.2f + 0.04f * static_cast<float>(k % 2), hgt = 0.45f + 0.1f * static_cast<float>(k);
    bd.box(pin - up * (hgt * 0.5f), ax, up, az, v3(w, hgt * 0.5f, 0.012f), cols[k], b, false);
  }
}

// A fence along the lot's edge, pulled in a little, with a gap in front of the door.
void fence(Builder &bd, house_v1_model &out, const Lot &lot, float door_x, float front) {
  const Footprint &fp = *lot.fp;
  const vec3 up = v3(0, 1, 0);
  for (int c = 0; c < fp.count; ++c) {
    const int q = fp.cells[c][0], r = fp.cells[c][1];
    const float cx = cell_x(q, r), cz = cell_z(r);
    for (int e = 0; e < 6; ++e) {
      // The neighbour across edge e (between corners e and e + 1), at twice the edge's midpoint.
      const float am = kPi / 3.0f * static_cast<float>(e + 1);
      const float mx = cx + kSqrt3 * foundry::det::cos(am), mz = cz + kSqrt3 * foundry::det::sin(am);
      const int nr = static_cast<int>(std::floor(2.0f / 3.0f * mz + 0.5f));
      const int nq = static_cast<int>(std::floor(kSqrt3 / 3.0f * mx - mz / 3.0f + 0.5f));
      if (fp.has(nq, nr)) continue;
      const float a0 = kPi / 6.0f + kPi / 3.0f * static_cast<float>(e), a1 = a0 + kPi / 3.0f;
      const vec3 c3 = v3(cx, 0, cz);
      vec3 p0 = v3(cx + foundry::det::cos(a0), 0, cz + foundry::det::sin(a0)), p1 = v3(cx + foundry::det::cos(a1), 0, cz + foundry::det::sin(a1));
      p0 = p0 + (c3 - p0) * 0.12f;
      p1 = p1 + (c3 - p1) * 0.12f;
      // Within the round footprint: corners past its circle come in to it.
      const float R = round_radius(fp.round) - 0.06f;
      for (vec3 *q : {&p0, &p1}) {
        const float d = foundry::det::hypot(q->x, q->z);
        if (d > R) *q = *q * (R / d);
      }
      const vec3 m = (p0 + p1) * 0.5f;
      if (m.z > front && m.x > door_x - 0.8f && m.x < door_x + 0.8f) continue;  // the gate
      bd.beam(p0, p0 + up * 0.85f, 0.04f, v3(1, 0, 0), kWood);
      for (float y : {0.35f, 0.7f}) {
        if (bd.detail == 2 && y < 0.5f) continue;
        bd.beam(p0 + up * y, p1 + up * y, 0.025f, up, kWood * 1.1f);
      }
      // Collision: a thin box along the edge (its direction is 150 + 60 e degrees from +x toward +z).
      const float dir = kPi * (150.0f + 60.0f * static_cast<float>(e)) / 180.0f;
      solid_box(out, m + up * 0.45f, v3(0.5f * length(p1 - p0), 0.45f, 0.04f));
      out.collision.back().shapes.back().rotation = v3(0, -dir, 0);
    }
  }
}

}  // namespace

bool run(const house_v1_spec &in, house_v1_model &out, std::string &error) {
  // The type, dressed in the palette: each part's colour mixed toward the palette's by its mix.
  house_v1_type t = in.type;
  {
    const channels::house_v1_palette &p = in.palette;
    auto mix = [](vec3 &c, vec3 to, float m) { c = mixv(c, to, clampf(m, 0, 1)); };
    mix(t.roof_color, p.roof, p.roof_mix);
    mix(t.wall_color, p.wall, p.wall_mix);
    mix(t.trim_color, p.trim, p.trim_mix);
    mix(t.door_color, p.door, p.door_mix);
    mix(t.shutter_color, p.shutter, p.shutter_mix);
    mix(t.accent, p.accent, p.accent_mix);
    mix(t.sign_color, p.sign, p.sign_mix);
  }
  // The village's theme over the type (heartland keeps the type's own).
  const channels::village_v1_theme &th = in.theme;
  t.wall_color = theme::dress(t.wall_color, th.wall_color, th);
  t.trim_color = theme::dress(t.trim_color, th.trim_color, th);
  t.roof_color = theme::dress(t.roof_color, th.roof_color, th);
  t.accent = theme::dress(t.accent, th.accent, th);
  const uint32_t shape = in.footprint == house_v1_spec::kFootprintAlternate ? t.footprint_alt : t.footprint;
  if (shape >= kShapes || in.household > 2 || in.stage > house_v1_spec::kStageRuin || in.season > 3 || in.detail > 2) {
    error = "lot shape, household, stage, season, or detail out of range";
    return false;
  }
  const uint32_t household = in.household, fpi = household * kShapes + shape;
  const Footprint fp = footprint(fpi);
  const Lot lot{&fp};
  const uint32_t seed = hash_u32(in.seed * 2654435761u ^ hash_str(t.name.c_str()));
  Rng rng(seed, 1u);
  out = house_v1_model();
  Builder bd{out, in.season == house_v1_spec::kSeasonWinter, in.detail, icosphere(in.detail == 0 ? 1 : (in.detail == 1 ? 0 : -1)),
             in.detail == 0 ? 10 : (in.detail == 1 ? 7 : 5)};
  bd.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_v1_bone::kMotionFixed, 0, 0, 0, 0);
  const vec3 up = v3(0, 1, 0), X = v3(1, 0, 0), Z = v3(0, 0, 1);
  // A ruin is a premium house fallen to wreck: built whole, then broken down (condition.h).
  const bool ruined = in.stage == house_v1_spec::kStageRuin;
  const uint32_t stage = ruined ? static_cast<uint32_t>(house_v1_spec::kStagePremium) : in.stage, season = in.season;
  const bool deluxe = stage >= house_v1_spec::kStageDeluxe, premium = stage == house_v1_spec::kStagePremium;
  const bool hi = in.detail == 0, mid = in.detail <= 1;

  // ---- the lot and the body ----
  // The house stands against the back of the lot's rectangle, leaving a front yard (for the path,
  // a porch, a stall) that shrinks as it grows; its width grows with each stage. Construction stakes
  // out the premium house.
  const uint32_t built = stage == house_v1_spec::kStageConstruction ? static_cast<uint32_t>(house_v1_spec::kStagePremium) : stage;
  const float widths[4] = {0.92f, 0.72f, 0.84f, 0.92f}, yards[4] = {1.6f, 2.0f, 1.8f, 1.6f};
  House h;
  h.body.hx = fp.hx * widths[built];
  h.body.cx = fp.cx;
  {
    // Where the lot ends in front of the house's middle (past the rectangle on a flower).
    float lot_front = fp.cz;
    while (lot.inside(fp.cx, lot_front + 0.1f, 0.05f)) lot_front += 0.1f;
    const float back = fp.cz - fp.hz + 0.02f;
    const float body_front = minf(fp.cz + fp.hz, lot_front - yards[built]);
    h.body.hz = 0.5f * (body_front - back);
    h.body.cz = back + h.body.hz;
    // No deeper than 1.25 times its width: on a long lot (a burgage) the house keeps to the front,
    // and the rest is its back garden.
    const float deepest = 1.25f * h.body.hx;
    if (h.body.hz > deepest) {
      h.body.hz = deepest;
      h.body.cz = body_front - deepest;
    }
  }
  // A lean-to (on the right end) or a waterwheel (on the left) needs room beside the house: from
  // deluxe on, the house gives up some of its width and moves over.
  if (stage >= house_v1_spec::kStageDeluxe && (t.feature == house_v1_type::kFeatureLeanTo || t.feature == house_v1_type::kFeatureWaterwheel)) {
    const float need = t.feature == house_v1_type::kFeatureLeanTo ? 1.1f : 0.5f;
    const float give = minf(need - (fp.hx - h.body.hx), h.body.hx * 0.35f);
    if (give > 0) {
      h.body.hx -= give * 0.5f;
      h.body.cx += t.feature == house_v1_type::kFeatureLeanTo ? -give * 0.5f : give * 0.5f;
    }
  }
  // A deep house turns its gable to the street.
  h.along_z = h.body.hz > h.body.hx;
  const float storey = clampf(t.wall_height, 2.0f, 3.4f) * (stage == house_v1_spec::kStageBasic ? 0.9f : 1.0f);
  const bool two = (premium && t.stories >= 2) || (household == house_v1_spec::kHouseholdExtended && deluxe);
  h.wall_h = two ? 2 * storey - 0.15f : storey;
  h.door_w = 0.9f;
  h.door_h = minf(2.05f, storey - 0.3f);
  // The door: the footprint's door cell, the front-row cell nearest the lot's middle, so every type
  // on a lot has its door in one place (a planner keeps the way in front of it clear).
  int door_cell = 0;
  {
    int front_r = -1000;
    for (int k = 0; k < fp.count; ++k) front_r = maxi(front_r, fp.cells[k][1]);
    float best = 1e30f;
    for (int k = 0; k < fp.count; ++k) {
      if (fp.cells[k][1] != front_r) continue;
      const float dx = cell_x(fp.cells[k][0], fp.cells[k][1]) - fp.cx;
      const float d = dx < 0 ? -dx : dx;
      if (d < best - 1e-4f) {
        best = d;
        door_cell = k;
      }
    }
    const float room = maxf(0, h.body.hx - 0.65f);
    const float u = rng.signed_unit();  // kept, so the rest of the house draws as before
    (void)u;
    h.door_x = clampf(cell_x(fp.cells[door_cell][0], fp.cells[door_cell][1]), h.body.cx - room, h.body.cx + room);
  }
  const float front = h.front();

  if (stage == house_v1_spec::kStageConstruction) {
    // ---- construction: the household's tents and campfire on a staked plot ----
    const Rect &b = h.body;
    const float x0 = b.cx - b.hx, x1 = b.cx + b.hx, z0 = b.cz - b.hz, z1 = b.cz + b.hz;
    for (int sx = 0; sx < 2; ++sx)
      for (int sz = 0; sz < 2; ++sz) bd.beam(v3(sx ? x1 : x0, 0, sz ? z1 : z0), v3(sx ? x1 : x0, 0.6f, sz ? z1 : z0), 0.025f, X, kWood);
    const vec3 corner[4] = {v3(x0, 0.45f, z0), v3(x1, 0.45f, z0), v3(x1, 0.45f, z1), v3(x0, 0.45f, z1)};
    for (int k = 0; k < 4; ++k) bd.beam(corner[k], corner[(k + 1) % 4], 0.006f, up, kCloth, 0, false);
    // The foundation: blocks along the back and sides, part laid.
    const float laid = 0.4f + 0.35f * rng.uniform();
    const float perim = 4 * (b.hx + b.hz);
    float walked = 0;
    for (int side = 0; side < 4 && walked < laid * perim; ++side) {
      const vec3 a = v3(corner[side].x, 0, corner[side].z), c = v3(corner[(side + 1) % 4].x, 0, corner[(side + 1) % 4].z);
      const float len = length(c - a);
      const vec3 d = (c - a) * (1 / len);
      for (float u = 0.25f; u < len - 0.2f && walked < laid * perim; u += 0.52f, walked += 0.52f) {
        const vec3 at = a + d * u;
        bd.box(at + up * 0.15f, d, up, cross(d, up), v3(0.25f, 0.15f, 0.18f), kStone * (0.92f + 0.12f * rng.uniform()), 0);
        if (walked < laid * perim * 0.4f) bd.box(at + up * 0.45f, d, up, cross(d, up), v3(0.24f, 0.14f, 0.17f), kStone * 0.95f, 0);
      }
    }
    // A scaffold at the back corner, a lumber pile, and the tents and fire in front.
    {
      const vec3 c = v3(x1 - 0.5f, 0, z0 + 0.4f);
      for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
          bd.beam(c + v3(0.45f * static_cast<float>(sx), 0, 0.3f * static_cast<float>(sz)),
                  c + v3(0.45f * static_cast<float>(sx), 2.6f, 0.3f * static_cast<float>(sz)), 0.035f, X, kWood);
      for (float y : {1.3f, 2.4f}) bd.box(c + up * y, v3(0.55f, 0.025f, 0.32f), kWood * 1.15f, 0);
    }
    if (mid) {
      const vec3 c = v3(x0 + 0.9f, 0, z0 + 0.45f);
      for (int k = 0; k < 6; ++k)
        bd.box(c + up * (0.03f + 0.06f * static_cast<float>(k)) + X * (0.03f * static_cast<float>(k % 2)), v3(0.8f, 0.03f, 0.2f),
               kWood * (1.05f + 0.08f * static_cast<float>(k % 2)), 0);
    }
    // The tents: one for each of the household's generations, side by side along the front, each
    // an A-frame big enough to lie down in (2.6 m long), opening toward the street. One flap is
    // rolled and tied back; the other hangs from its seam on a sway bone.
    const float tw = 1.05f, tl = 1.3f, th = 1.75f;
    const float tz = z1 - tl - 0.1f;
    float first_x = 0;
    int tents = 0;
    for (uint32_t k = 0; k <= household; ++k) {
      const float x = x1 - 0.3f - tw - static_cast<float>(k) * (2 * tw + 0.5f);
      if (x - tw < x0 + 0.2f) break;
      const vec3 tc = v3(x, 0, tz);
      const vec3 colour = k % 2 ? mixv(t.accent, kCloth, 0.5f) : t.accent;
      for (int s = -1; s <= 1; s += 2)
        bd.slab(tc + v3(0, th, -tl), tc + v3(0, th, tl), tc + v3(tw * static_cast<float>(s), 0, tl), tc + v3(tw * static_cast<float>(s), 0, -tl), 0.03f,
                colour, 0, true, v3(static_cast<float>(s), 1, 0));
      bd.triangle(tc + v3(-tw, 0, -tl), tc + v3(tw, 0, -tl), tc + v3(0, th, -tl), colour * 0.85f, 0, false, -Z);
      bd.beam(tc + v3(0, th + 0.05f, -tl - 0.1f), tc + v3(0, th + 0.05f, tl + 0.1f), 0.025f, X, kWood);
      const vec3 ridge = tc + v3(0, th, tl), left = tc + v3(-tw, 0, tl), right = tc + v3(tw, 0, tl), foot = tc + v3(0, 0, tl);
      bd.cylinder(ridge + v3(0.08f, -0.1f, 0.02f), right + v3(-0.02f, 0.12f, 0.02f), 0.07f, colour * 0.9f, 0, true, false, bd.detail == 2 ? 4 : 6);
      {
        const uint32_t flap = bd.bone(0, ridge, left - ridge, house_v1_bone::kMotionSway, 0.7f + 0.1f * static_cast<float>(k), 0.6f, 0.8f, rng.uniform());
        const vec3 lip = v3(0, 0, 0.012f);
        bd.triangle(ridge + lip, left + lip, foot + lip + v3(0, 0.05f, 0), colour * 0.92f, flap, false, Z);
        bd.triangle(ridge, left, foot + v3(0, 0.05f, 0), colour * 0.8f, flap, false, -Z);
      }
      solid_box(out, tc + up * (th * 0.5f), v3(tw, th * 0.5f, tl));
      if (k == 0) first_x = x;
      ++tents;
    }
    // The campfire, in front of the tents: a ring of stones, crossed logs, its flame, and its smoke.
    const vec3 fire = v3(first_x - tw - 0.25f, 0, z1 + 0.7f);
    for (int k = 0; k < (mid ? 7 : 4); ++k) {
      const float a = 2 * kPi * static_cast<float>(k) / (mid ? 7.0f : 4.0f);
      bd.blob(fire + v3(0.3f * foundry::det::cos(a), 0.06f, 0.3f * foundry::det::sin(a)), v3(0.09f, 0.07f, 0.09f), 0.2f, 100u + static_cast<uint32_t>(k),
              kStone, 0);
    }
    bd.beam(fire + v3(-0.22f, 0.06f, -0.1f), fire + v3(0.22f, 0.12f, 0.1f), 0.04f, up, kDarkWood);
    bd.beam(fire + v3(-0.2f, 0.12f, 0.12f), fire + v3(0.2f, 0.06f, -0.12f), 0.04f, up, kDarkWood);
    condition::fire(out, fire + up * 0.12f, in.theme.hearth_fire, 1);
    h.door_x = first_x;
    out.doorway = v3(first_x, 0, z1 + 0.5f);
    out.doorway_heading = 0;
    (void)tents;
    // A cart of planks, if the yard has room.
    Yard yard(lot, Rect{b.cx, b.cz, b.hx, b.hz + 0.5f}, h.door_x, front + 0.5f, seed);
    yard.taken.push_back({fire.x, fire.z, 0.5f});
    float x, z;
    if (yard.find(1.1f, false, x, z)) {
      const float yaw = rng.uniform() * 6.28f;
      doodad(bd, out, "cart", x, z, yaw, t, season, rng);
    }
    if (yard.find(0.56f, true, x, z)) doodad(bd, out, "barrels", x, z, 0, t, season, rng);
    h.ridge = 2.6f;
  } else {
    // ---- the house ----
    // The theme's plan: a round house stands at the door, as wide as the body is deep; where the
    // lot's width leaves room, smaller round huts stand either side (a household as a cluster).
    using Th = channels::village_v1_theme;
    const Rect whole = h.body;
    const bool round = theme::round_plan(th);
    std::vector<theme::Body> huts;
    if (round) {
      float r = minf(h.body.hz, h.body.hx - absf(h.door_x - h.body.cx)), cx = h.door_x;
      if (r < 1.3f) {
        r = minf(h.body.hx, h.body.hz);
        cx = h.body.cx;
      }
      h.body = Rect{cx, front - r, r, r};
      h.along_z = false;
      for (int s = -1; s <= 1; s += 2) {
        const float fs = static_cast<float>(s);
        const float edge = whole.cx + fs * whole.hx, near = cx + fs * (r + 0.35f), room = (edge - near) * fs;
        if (room < 2.2f) continue;
        const float hr = minf(room * 0.5f, r * 0.75f);
        theme::Body hut;
        hut.cx = near + fs * hr;
        hut.cz = whole.cz - whole.hz + hr + 0.05f;
        hut.hx = hut.hz = hr;
        hut.round = true;
        huts.push_back(hut);
      }
    }
    const Rect &b = h.body;
    // The walls' material (the theme's 1..10; a type's own 0..4 is 1..5): a basic house builds in
    // the theme's humble material, or planks.
    uint32_t tmat = theme::walls_of(th, t.walls);
    vec3 wall_col = t.wall_color;
    if (stage == house_v1_spec::kStageBasic) {
      if (th.walls == Th::kWallsMarble) tmat = Th::kWallsStone;
      else if (th.walls == Th::kWallsAsType && t.walls != house_v1_type::kWallsLog) {
        tmat = Th::kWallsPlank;
        wall_col = kWood * 1.1f;
      }
    }
    const bool type_mat = tmat >= Th::kWallsTimber && tmat <= Th::kWallsBrick;
    // What it stands on: a stone base (premium, or stone walls), an earth mound, or stilts.
    const float lift = th.foundation == Th::kFoundationStilts ? 0.8f : (th.foundation == Th::kFoundationMound ? 0.25f : 0.0f);
    const bool base = th.foundation == Th::kFoundationStone && (premium || tmat == Th::kWallsStone);
    const int round_sides = in.detail == 0 ? 16 : (in.detail == 1 ? 12 : 8);
    if (base) {
      if (round) theme::frustum(bd, v3(b.cx, 0, b.cz), v3(b.cx, 0.4f, b.cz), b.hx + 0.08f, b.hx + 0.08f, round_sides, t.base_color, 0, true);
      else bd.box(v3(b.cx, 0.2f, b.cz), v3(b.hx + 0.06f, 0.2f, b.hz + 0.06f), t.base_color, 0);
    }
    if (th.foundation == Th::kFoundationMound) {
      if (round) theme::frustum(bd, v3(b.cx, -0.05f, b.cz), v3(b.cx, lift, b.cz), b.hx + 0.7f, b.hx + 0.1f, round_sides, mixv(kDirt, v3(0.36f, 0.5f, 0.26f), 0.6f), 0, true);
      else bd.box(v3(b.cx, lift * 0.5f - 0.02f, b.cz), v3(b.hx + 0.4f, lift * 0.5f, b.hz + 0.4f), mixv(kDirt, v3(0.36f, 0.5f, 0.26f), 0.6f), 0);
    }
    // Everything from here to the yard rises with the foundation (lift).
    const size_t v0 = out.positions.size(), k0 = out.bones.size(), f0 = out.fire_points.size();
    // Decoration that only some levels of detail draw takes its randoms from its own stream, so every
    // level places the same things and gives its bones the same phases.
    Rng deco(seed, 9u);
    auto storey_walls = [&](float y0, float y1, vec3 col, bool ground) {
      if (round) theme::round_walls(bd, v3(b.cx, 0, b.cz), b.hx, y0, y1, tmat, col, t.trim_color, 0.0f, ground ? h.door_w : 0.0f, ground ? h.door_h : 0.0f,
                                    seed + (ground ? 0u : 7u));
      else if (type_mat) walls(bd, h, y0, y1, t, tmat - 1, col, deco);
      else {
        bd.box(v3(b.cx, (y0 + y1) * 0.5f, b.cz), v3(b.hx, (y1 - y0) * 0.5f, b.hz), col, 0, false);
        theme::rect_material(bd, b.cx, b.cz, b.hx, b.hz, y0, y1, tmat, col, t.trim_color, h.door_x, h.door_w, seed);
      }
    };
    storey_walls(0, storey, wall_col, true);
    if (two) {
      // The upper storey, its floor marked by a belt.
      storey_walls(storey - 0.15f, h.wall_h, wall_col * 1.03f, false);
      if (round) theme::frustum(bd, v3(b.cx, storey - 0.2f, b.cz), v3(b.cx, storey - 0.04f, b.cz), b.hx + 0.05f, b.hx + 0.05f, round_sides, t.trim_color, 0, false);
      else bd.box(v3(b.cx, storey - 0.12f, b.cz), v3(b.hx + 0.05f, 0.08f, b.hz + 0.05f), t.trim_color, 0);
    }
    const uint32_t roof = stage == house_v1_spec::kStageBasic && t.roof != house_v1_type::kRoofThatch ? house_v1_type::kRoofShingle : t.roof;
    const vec3 roof_col = stage == house_v1_spec::kStageBasic && t.roof != house_v1_type::kRoofThatch && th.roof == Th::kRoofAsType ? kWood * 0.85f : t.roof_color;
    const RoofFrame frame{b, h.along_z};
    // The roof's form: the theme's, or the type's gable; a round house takes a cone for a gable.
    uint32_t form = theme::roof_of(th, Th::kRoofGable);
    if (round && (form == Th::kRoofGable || form == Th::kRoofHip)) form = Th::kRoofCone;
    const bool gabled = !round && (form == Th::kRoofGable || form == Th::kRoofTurf || form == Th::kRoofSweep);
    const vec3 gable_col = tmat == Th::kWallsLog ? t.trim_color : wall_col;
    if (!round && form == Th::kRoofGable) gable_roof(bd, frame, h.wall_h, clampf(t.roof_pitch, 20, 60), roof, roof_col, gable_col, h.ridge);
    else {
      theme::Body tb;
      tb.cx = b.cx;
      tb.cz = b.cz;
      tb.hx = b.hx;
      tb.hz = b.hz;
      tb.round = round;
      h.ridge = theme::roof(bd, tb, form, h.wall_h, clampf(t.roof_pitch, 20, 60), roof_col, t.trim_color, gable_col, seed, h.along_z);
    }
    float roof_top = h.ridge;

    // The door, on its hinge (its left edge, seen from outside), and its frame.
    {
      const vec3 hinge = v3(h.door_x - h.door_w * 0.5f, 0, front + 0.03f);
      const uint32_t door = bd.bone(0, hinge, up, house_v1_bone::kMotionHinge, 0, -1.7f, 0, 0);
      bd.box(hinge + v3(h.door_w * 0.5f, h.door_h * 0.5f, 0.02f), v3(h.door_w * 0.5f - 0.02f, h.door_h * 0.5f, 0.03f), t.door_color, door, false);
      if (mid) {
        bd.box(hinge + v3(h.door_w * 0.8f, h.door_h * 0.5f, 0.06f), v3(0.03f, 0.03f, 0.02f), kIron, door, false);
        for (float y : {0.35f, h.door_h - 0.35f})
          bd.box(hinge + v3(h.door_w * 0.35f, y, 0.055f), v3(h.door_w * 0.3f, 0.025f, 0.01f), kIron, door, false);
      }
      bd.box(v3(h.door_x, h.door_h + 0.06f, front + 0.04f), v3(h.door_w * 0.5f + 0.1f, 0.07f, 0.06f), t.trim_color, 0);
      for (int s = -1; s <= 1; s += 2)
        bd.box(v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.05f), h.door_h * 0.5f, front + 0.04f), v3(0.05f, h.door_h * 0.5f, 0.06f),
               t.trim_color, 0, false);
      bd.box(v3(h.door_x, 0.04f, front + 0.25f), v3(h.door_w * 0.5f + 0.1f, 0.04f, 0.22f), t.base_color, 0);  // the step
    }
    // Windows: either side of the door where there's room, on the ends, and the back; and upstairs.
    // The theme's shape (round, arched, slit) replaces the type's square ones and their shutters.
    const float ww = 0.6f, wh = 0.7f, wy = 1.3f;
    const bool frames = deluxe && mid, shutters = deluxe && mid;
    const bool boxes = deluxe && (t.flowerpots || t.garden);
    const vec3 fl = bloom(season, rng.uniform());
    const uint32_t wshape = theme::windows_of(th, Th::kWindowsSquare);
    auto win = [&](vec3 at, vec3 n, float w, float hh, bool flower_box) {
      if (wshape == Th::kWindowsSquare) window(bd, at, n, w, hh, t, shutters, flower_box, fl, frames);
      else theme::window(bd, at, n, w, hh, wshape, t.trim_color, kGlass);
    };
    if (round) {
      // Round the wall, clear of the door.
      for (float a : {1.15f, -1.15f, 2.2f, -2.2f, 3.14159265f}) {
        const vec3 n = theme::around(a);
        win(v3(b.cx, wy, b.cz) + n * b.hx, n, ww, wh, false);
        if (two) win(v3(b.cx, storey + wy - 0.1f, b.cz) + n * b.hx, n, ww, wh, false);
      }
    } else {
      std::vector<float> front_windows;
      for (int s = -1; s <= 1; s += 2) {
        const float near = h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.25f);
        const float far = b.cx + static_cast<float>(s) * (b.hx - 0.3f);
        const float room = (far - near) * static_cast<float>(s);
        if (room < ww + 0.3f) continue;
        const int n = room > 3.4f ? static_cast<int>(room / 1.7f) : (room > 2.6f ? 2 : 1);
        for (int k = 0; k < n; ++k) {
          const float x = near + (far - near) * (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
          front_windows.push_back(x);
          win(v3(x, wy, front), Z, ww, wh, boxes);
        }
      }
      // The ends and the back: a window every 2.4 m or so.
      const int ends = b.hz > 0.8f ? maxi(1, static_cast<int>(2 * b.hz / 2.4f)) : 0, backs = b.hx > 1.2f ? maxi(1, static_cast<int>(2 * b.hx / 2.8f)) : 0;
      for (int s = -1; s <= 1; s += 2)
        for (int k = 0; k < ends; ++k) {
          const float z = b.cz - b.hz + 2 * b.hz * (static_cast<float>(k) + 0.5f) / static_cast<float>(ends);
          win(v3(b.cx + static_cast<float>(s) * b.hx, wy, z), X * static_cast<float>(s), ww, wh, false);
          if (two) win(v3(b.cx + static_cast<float>(s) * b.hx, storey + wy - 0.1f, z), X * static_cast<float>(s), ww, wh, false);
        }
      for (int k = 0; k < backs; ++k)
        win(v3(b.cx - b.hx + 2 * b.hx * (static_cast<float>(k) + 0.5f) / static_cast<float>(backs), wy, b.cz - b.hz), -Z, ww, wh, false);
      if (two) {
        front_windows.push_back(h.door_x);
        for (float x : front_windows) win(v3(x, storey + wy - 0.1f, front), Z, ww, wh, boxes);
      }
      // A gable to the street gets a window in its peak.
      if (gabled && h.along_z && h.ridge - h.wall_h > 1.3f) win(v3(b.cx, h.wall_h + 0.55f, front), Z, ww * 0.8f, wh * 0.8f, false);
    }

    // The chimney and its smoke (a stovepipe on a basic house), or the smoke hole at a round or
    // pointed roof's apex: the hearth's smoke, as a fire point (the runtime's fire system).
    if (t.smoke > 0) {
      const bool hole = th.smoke_hole && (round || form == Th::kRoofCone || form == Th::kRoofDome || form == Th::kRoofHip);
      if (hole) {
        const vec3 apex = round ? v3(b.cx, h.ridge, b.cz) : frame.at(0, h.ridge, 0);
        condition::fire(out, apex + up * 0.05f, th.hearth_fire, 2);
      } else {
        // Toward one end of the ridge, a little off it, so it rises through the roof's slope.
        float cx = h.along_z ? b.cx + b.hx * 0.4f : b.cx + b.hx - 0.45f, cz = h.along_z ? b.cz - b.hz + 0.45f : b.cz - b.hz * 0.4f;
        if (round) {
          cx = b.cx + b.hx * 0.45f;
          cz = b.cz - b.hx * 0.3f;
        }
        if (deluxe) {
          const float top = h.ridge + 0.45f;
          bd.box(v3(cx, (h.wall_h - 0.4f + top) * 0.5f, cz), v3(0.26f, (top - h.wall_h + 0.4f) * 0.5f, 0.26f),
                 tmat == Th::kWallsBrick ? t.wall_color : t.base_color, 0);
          bd.box(v3(cx, top + 0.05f, cz), v3(0.3f, 0.05f, 0.3f), t.base_color * 0.85f, 0);
          condition::fire(out, v3(cx, top + 0.15f, cz), th.hearth_fire, 2);
        } else {
          const float top = h.ridge + 0.3f;
          bd.cylinder(v3(cx, h.wall_h - 0.2f, cz), v3(cx, top, cz), 0.09f, kIron, 0);
          condition::fire(out, v3(cx, top + 0.1f, cz), th.hearth_fire, 2);
        }
      }
    }

    // Icicles along the front eaves in winter.
    if (season == house_v1_spec::kSeasonWinter && hi && !round && form == Th::kRoofGable) {
      // Along the eaves: the front on a wide house, both sides on a deep one.
      const float drop = 0.32f * tanf_(clampf(t.roof_pitch, 20, 60) * kDeg), y = h.wall_h - drop + 0.04f;
      for (int s = h.along_z ? -1 : 1; s <= 1; s += 2)
        for (float u = -frame.hu(); u < frame.hu(); u += 0.23f) {
          const float len = 0.12f + 0.2f * deco.uniform();
          const vec3 at = h.along_z ? v3(b.cx + static_cast<float>(s) * (b.hx + 0.32f), y, b.cz + u) : v3(b.cx + u, y, front + 0.32f);
          bd.cone(at, at - up * (len + 0.04f), 0.025f, v3(0.85f, 0.93f, 1.0f), 0, false);
        }
    }

    // The huts beside a round house: walls, a door's gap toward the front, and the same roof.
    for (size_t k = 0; k < huts.size(); ++k) {
      const theme::Body &hb = huts[k];
      const float hh = storey * 0.85f;
      theme::round_walls(bd, v3(hb.cx, 0, hb.cz), hb.hx, 0, hh, tmat, wall_col, t.trim_color, 0.0f, 0.75f, minf(1.6f, hh - 0.3f), seed + 31u * static_cast<uint32_t>(k + 1));
      theme::roof(bd, hb, form, hh, clampf(t.roof_pitch, 20, 60), roof_col, t.trim_color, gable_col, seed + static_cast<uint32_t>(k), false);
      if (lift == 0) solid_cylinder(out, v3(hb.cx, 0, hb.cz), hb.hx, hh);
      else solid_cylinder(out, v3(hb.cx, 0, hb.cz), hb.hx + 0.3f, hh + lift);
    }

    Yard yard(lot, b, h.door_x, front, seed);
    for (const theme::Body &hb : huts) yard.blocked.push_back({hb.cx, hb.cz, hb.hx + 0.25f, hb.hz + 0.25f});
    // ---- the sign ----
    if (t.sign != house_v1_type::kSignNone) {
      const bool hanging = deluxe && t.sign == house_v1_type::kSignHanging;
      if (hanging) {
        // A bracket out from the wall beside the door, the board swinging under it.
        const float side = h.door_x > b.cx ? -1.0f : 1.0f;
        float x = h.door_x + side * (h.door_w * 0.5f + 0.45f);
        float y = minf(storey - 0.15f, 2.45f);
        if (t.awning && !t.porch) {
          // Clear of the awning: above it on a tall wall, else out at the front corner.
          const float aw = minf(1.6f, b.hx * 0.9f), above = h.door_h + 1.3f;
          if (above < h.wall_h - 0.1f) y = above;
          else x = b.cx + side * (b.hx - 0.08f);
          if (y < above && x > h.door_x - aw - 0.35f && x < h.door_x + aw + 0.35f) x = b.cx + side * (b.hx - 0.08f);
        }
        bd.beam(v3(x, y, front), v3(x, y, front + 0.8f), 0.025f, up, kIron);
        bd.beam(v3(x, y - 0.4f, front), v3(x, y, front + 0.55f), 0.015f, up, kIron);
        const vec3 pin = v3(x, y - 0.03f, front + 0.55f);
        const uint32_t sb = bd.bone(0, pin, Z, house_v1_bone::kMotionSway, 0.8f, 0.5f, 0.7f, rng.uniform());
        const vec3 c = pin - up * 0.38f;
        bd.box(c, v3(0.03f, 0.24f, 0.32f), t.sign_color, sb, false);
        if (mid) {
          bd.beam(pin + v3(0, 0, -0.22f), c + v3(0, 0.24f, -0.22f), 0.008f, X, kIron, sb, false);
          bd.beam(pin + v3(0, 0, 0.22f), c + v3(0, 0.24f, 0.22f), 0.008f, X, kIron, sb, false);
        }
        if (in.detail < 2)
          for (int s = -1; s <= 1; s += 2)
            symbol(bd, t.symbol, c + X * (0.03f * static_cast<float>(s)), Z * static_cast<float>(-s), up, X * static_cast<float>(s), 0.36f, sb);
        yard.blocked.push_back({x, front + 0.55f, 0.4f, 0.4f});
      } else {
        const vec3 c = v3(h.door_x, h.door_h + 0.36f, front + 0.06f);
        bd.box(c, v3(minf(0.55f, h.door_w * 0.5f + 0.15f), 0.17f, 0.03f), t.sign_color, 0, false);
        if (deluxe && in.detail < 2) symbol(bd, t.symbol, c + Z * 0.03f, X, up, Z, 0.28f, 0);
      }
    }
    // ---- the porch or the awning ----
    if (deluxe && t.porch) {
      float depth = 1.0f;
      const float pw = minf(1.2f, b.hx * 0.8f);
      while (depth > 0.5f && !(lot.inside(h.door_x - pw, front + depth, 0.05f) && lot.inside(h.door_x + pw, front + depth, 0.05f))) depth -= 0.1f;
      if (depth > 0.5f) {
        const vec3 c = v3(h.door_x, 0, front + depth * 0.5f);
        bd.box(c + up * 0.1f, v3(pw, 0.1f, depth * 0.5f), kWood * 1.05f, 0);
        const float pr = h.door_h + 0.55f;
        for (int s = -1; s <= 1; s += 2)
          bd.beam(v3(h.door_x + static_cast<float>(s) * (pw - 0.08f), 0.2f, front + depth - 0.08f),
                  v3(h.door_x + static_cast<float>(s) * (pw - 0.08f), pr - 0.15f, front + depth - 0.08f), 0.05f, X, t.trim_color);
        bd.slab(v3(h.door_x - pw - 0.1f, pr + 0.2f, front), v3(h.door_x + pw + 0.1f, pr + 0.2f, front),
                v3(h.door_x + pw + 0.1f, pr - 0.15f, front + depth + 0.1f), v3(h.door_x - pw - 0.1f, pr - 0.15f, front + depth + 0.1f), 0.07f,
                t.roof_color, 0, true, up);
        yard.blocked.push_back({h.door_x, front + depth * 0.5f, pw, depth * 0.5f});
      }
    } else if (deluxe && t.awning) {
      const float aw = minf(1.6f, b.hx * 0.9f), ay = h.door_h + 0.45f;
      float depth = 0.9f;
      while (depth > 0.4f && !lot.inside(h.door_x, front + depth, 0.02f)) depth -= 0.1f;
      const int stripes = in.detail == 2 ? 1 : 6;
      for (int k = 0; k < stripes; ++k) {
        const float a = -aw + 2 * aw * static_cast<float>(k) / static_cast<float>(stripes), c = a + 2 * aw / static_cast<float>(stripes);
        bd.slab(v3(h.door_x + a, ay, front + 0.02f), v3(h.door_x + c, ay, front + 0.02f), v3(h.door_x + c, ay - 0.35f, front + depth),
                v3(h.door_x + a, ay - 0.35f, front + depth), 0.02f, k % 2 ? kCloth : t.accent, 0, true, up);
      }
      const vec3 pin = v3(h.door_x, ay - 0.35f, front + depth);
      const uint32_t val = bd.bone(0, pin, X, house_v1_bone::kMotionSway, 1.3f, 0.45f, 0.6f, rng.uniform());
      bd.box(pin - up * 0.07f, v3(aw, 0.07f, 0.012f), t.accent, val, false);
    }
    // ---- the feature ----
    // A forge's or an oven's fire: the theme's hearth fire, or a forge's coals in the heartland.
    const uint32_t forge_fire = th.hearth_fire == 1 ? 3u : th.hearth_fire;
    bool lean_to = false;
    if (deluxe) {
      if (t.feature == house_v1_type::kFeatureWeathervane) {
        const vec3 base = frame.at(-frame.hu() * 0.5f, h.ridge + 0.1f, 0), pin = base + up * 0.75f;
        bd.beam(base, pin + up * 0.08f, 0.018f, X, kIron);
        if (mid) {
          bd.beam(base + up * 0.4f - X * 0.22f, base + up * 0.4f + X * 0.22f, 0.012f, up, kIron);
          bd.beam(base + up * 0.4f - Z * 0.22f, base + up * 0.4f + Z * 0.22f, 0.012f, up, kIron);
        }
        const uint32_t vb = bd.bone(0, pin, up, house_v1_bone::kMotionSpin, 0, 0, 1, rng.uniform());
        bd.beam(pin - Z * 0.35f, pin + Z * 0.3f, 0.014f, up, kIron, vb);
        bd.box(pin - Z * 0.32f + up * 0.04f, X, up, Z, v3(0.01f, 0.1f, 0.09f), kIron, vb);
        bd.cone(pin + Z * 0.28f, pin + Z * 0.42f, 0.05f, kIron, vb);
        h.ridge += 0.85f;
      } else if (t.feature == house_v1_type::kFeatureWindmill) {
        // On a gable end, below its peak: the right end of a wide house, the back of a deep one.
        const float R = minf(2.4f + 0.5f * static_cast<float>(household), h.ridge * 0.8f);
        const vec3 E = h.along_z ? -Z : X, V = h.along_z ? X : Z;
        const vec3 hub = h.along_z ? v3(b.cx, maxf(h.ridge - 0.3f, R + 0.4f), b.cz - b.hz - 0.18f)
                                   : v3(b.cx + b.hx + 0.18f, maxf(h.ridge - 0.3f, R + 0.4f), b.cz);
        bd.cylinder(hub - E * 0.25f, hub, 0.14f, kDarkWood, 0);
        const uint32_t sb = bd.bone(0, hub, E, house_v1_bone::kMotionSpin, 0.12f, 0, 1, rng.uniform());
        bd.cylinder(hub, hub + E * 0.1f, 0.12f, kDarkWood, sb);
        for (int k = 0; k < 4; ++k) {
          const float a = kPi * 0.5f * static_cast<float>(k) + 0.3f;
          const vec3 d = up * foundry::det::cos(a) + V * foundry::det::sin(a), side = up * -foundry::det::sin(a) + V * foundry::det::cos(a);
          bd.beam(hub + E * 0.06f, hub + E * 0.06f + d * R, 0.035f, E, kWood, sb);
          bd.slab(hub + E * 0.07f + d * 0.4f, hub + E * 0.07f + d * R, hub + E * 0.07f + d * R + side * (R * 0.22f),
                  hub + E * 0.07f + d * 0.4f + side * (R * 0.22f), 0.015f, kCloth, sb, false, E);
        }
        h.ridge = maxf(h.ridge, hub.y + R);
      } else if (t.feature == house_v1_type::kFeatureWaterwheel) {
        const float R = minf(1.0f, b.hz * 0.8f);
        const vec3 hub = v3(b.cx - b.hx - 0.22f, R + 0.05f, b.cz);
        const uint32_t wb = bd.bone(0, hub, -X, house_v1_bone::kMotionSpin, 0.09f, 0, 0, rng.uniform());
        bd.cylinder(hub - X * 0.15f, hub + X * 0.15f, 0.1f, kDarkWood, wb);
        const int paddles = in.detail == 2 ? 6 : 10;
        for (int k = 0; k < paddles; ++k) {
          const float a = 2 * kPi * static_cast<float>(k) / static_cast<float>(paddles);
          const vec3 d = v3(0, foundry::det::cos(a), foundry::det::sin(a));
          bd.beam(hub, hub + d * R, 0.025f, X, kWood, wb);
          bd.box(hub + d * (R * 0.85f), X, d, cross(X, d), v3(0.14f, R * 0.15f, 0.015f), kWood * 1.1f, wb);
        }
        for (int s = -1; s <= 1; s += 2) bd.cylinder(hub + X * (0.13f * static_cast<float>(s)), hub + X * (0.15f * static_cast<float>(s)), R, kDarkWood, wb, false);
        // The sluice bringing water over the top, and the pool below.
        bd.box(v3(hub.x, 2 * R + 0.2f, b.cz - R * 0.4f - 0.6f), v3(0.14f, 0.06f, 0.7f), kWood, 0);
        bd.box(v3(hub.x, 2 * R + 0.25f, b.cz - R * 0.4f - 0.6f), v3(0.1f, 0.01f, 0.68f), kWater, 0, false);
        bd.box(v3(hub.x, 0.02f, b.cz), v3(0.3f, 0.02f, R + 0.2f), kWater, 0, false);
        solid_box(out, v3(hub.x, R, b.cz), v3(0.18f, R, R));
      } else if (t.feature == house_v1_type::kFeatureLeanTo) {
        // A shed against the right end: the forge (with its glow) or a workbench.
        float lw = 1.3f;
        const float lz = b.hz * 0.85f;
        while (lw > 0.7f && !(lot.inside(b.cx + b.hx + lw, b.cz - lz, 0.05f) && lot.inside(b.cx + b.hx + lw, b.cz + lz, 0.05f))) lw -= 0.1f;
        if (lw > 0.7f) {
          const float x0 = b.cx + b.hx, x1 = x0 + lw, top = minf(h.wall_h - 0.1f, 2.5f);
          for (int s = -1; s <= 1; s += 2) bd.beam(v3(x1 - 0.06f, 0, b.cz + static_cast<float>(s) * (lz - 0.06f)), v3(x1 - 0.06f, top - 0.6f, b.cz + static_cast<float>(s) * (lz - 0.06f)), 0.05f, Z, kWood);
          bd.slab(v3(x0, top, b.cz - lz - 0.1f), v3(x0, top, b.cz + lz + 0.1f), v3(x1 + 0.15f, top - 0.65f, b.cz + lz + 0.1f),
                  v3(x1 + 0.15f, top - 0.65f, b.cz - lz - 0.1f), 0.07f, t.roof_color * 0.9f, 0, true, up);
          const vec3 c = v3(x0 + lw * 0.5f, 0, b.cz);
          if (t.glow) {
            bd.box(c + up * 0.4f, v3(minf(0.4f, lw * 0.35f), 0.4f, 0.45f), t.base_color, 0);
            glow(bd, c + up * 0.86f, v3(0.22f, 0.08f, 0.26f), kFlame, 0.3f, rng);
            condition::fire(out, c + up * 0.9f, forge_fire, 1);
            if (mid) bd.blob(c + v3(0, 0.75f, 0.7f), v3(0.18f, 0.12f, 0.3f), 0.1f, 120u, kDarkWood, 0);  // the bellows
          } else {
            bd.box(c + up * 0.8f, v3(minf(0.45f, lw * 0.4f), 0.05f, 0.6f), kWood, 0);
            for (int s = -1; s <= 1; s += 2) bd.box(c + up * 0.4f + Z * (0.5f * static_cast<float>(s)), v3(0.05f, 0.4f, 0.05f), kDarkWood, 0, false);
            if (mid) bd.box(c + up * 0.9f, v3(0.2f, 0.05f, 0.08f), kIron, 0);
          }
          solid_box(out, c + up * 0.45f, v3(lw * 0.45f, 0.45f, lz * 0.8f));
          yard.blocked.push_back({c.x, c.z, lw * 0.5f, lz});
          lean_to = true;
        }
      }
    }
    // A glow at the door (an oven or forge with no lean-to), on a flicker bone.
    if (t.glow && !lean_to) {
      const float x = h.door_x + (h.door_x > b.cx ? -1.0f : 1.0f) * (h.door_w * 0.5f + 0.5f);
      bd.box(v3(x, 0.45f, front + 0.03f), v3(0.3f, 0.3f, 0.03f), t.base_color * 0.8f, 0, false);
      glow(bd, v3(x, 0.4f, front + 0.05f), v3(0.2f, 0.16f, 0.04f), kFlame, 0.25f, rng);
      condition::fire(out, v3(x, 0.3f, front + 0.12f), forge_fire, 1);
    }
    // ---- premium: lanterns, banners ----
    if (premium && t.lanterns)
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.25f), h.door_h, front + 0.2f);
        bd.beam(at - Z * 0.2f + up * 0.15f, at + up * 0.15f, 0.012f, up, kIron);
        bd.box(at, v3(0.08f, 0.12f, 0.08f), kIron, 0, false);
        glow(bd, at, v3(0.06f, 0.09f, 0.06f), v3(1.0f, 0.86f, 0.45f), 0.08f, rng);
        condition::fire(out, at, th.torch_fire, 1);
      }
    if (premium && t.banners)
      for (int s = -1; s <= 1; s += 2) {
        const float x = b.cx + static_cast<float>(s) * (b.hx - 0.35f);
        const vec3 pole = v3(x, h.wall_h - 0.25f, front);
        bd.beam(pole, pole + Z * 0.55f, 0.02f, up, kDarkWood);
        const vec3 pin = pole + Z * 0.45f;
        const uint32_t bb = bd.bone(0, pin, Z, house_v1_bone::kMotionSway, 0.9f, 0.6f, 0.9f, rng.uniform());
        bd.box(pin - up * 0.55f, v3(0.012f, 0.55f, 0.18f), t.accent, bb, false);
        if (mid) bd.box(pin - up * 1.05f, v3(0.014f, 0.06f, 0.18f), t.trim_color, bb, false);
      }
    // Pots by the door (deluxe and up), flowering by season.
    if (deluxe && t.flowerpots)
      for (int s = -1; s <= 1; s += 2) {
        const vec3 at = v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.32f), 0, front + 0.25f);
        bd.cylinder(at, at + up * 0.3f, 0.14f, v3(0.72f, 0.42f, 0.28f), 0);
        bd.blob(at + up * 0.38f, v3(0.16f, 0.13f, 0.16f), 0.3f, 130u + static_cast<uint32_t>(s + 1),
                fl.x >= 0 ? fl : v3(0.25f, 0.38f, 0.22f), 0);
      }

    // ---- the theme's ornaments on the house ----
    const vec3 apex = round ? v3(b.cx, roof_top, b.cz) : frame.at(0, roof_top, 0);
    if (deluxe) {
      if (th.ornament == Th::kOrnamentCarvings) {
        // Carved posts either side of the door, knobbed at the top.
        for (int s = -1; s <= 1; s += 2) {
          const vec3 at = v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.18f), 0, front + 0.12f);
          theme::frustum(bd, at, at + up * (h.door_h + 0.25f), 0.08f, 0.065f, 6, t.trim_color, 0, true);
          if (mid)
            for (float y : {0.5f, 1.1f, h.door_h})
              theme::frustum(bd, at + up * (y - 0.05f), at + up * (y + 0.05f), 0.1f, 0.1f, 6, t.trim_color * 0.8f, 0, true);
          bd.box(at + up * (h.door_h + 0.33f), v3(0.08f, 0.08f, 0.08f), t.trim_color * 1.1f, 0);
        }
      } else if (th.ornament == Th::kOrnamentDragonHeads) {
        if (gabled)
          for (int s = -1; s <= 1; s += 2)
            theme::dragon_head(bd, frame.at(static_cast<float>(s) * (frame.hu() + 0.3f), roof_top - 0.15f, 0), frame.dir(static_cast<float>(s), 0, 0), t.trim_color,
                               th.accent);
        else theme::dragon_head(bd, apex - up * 0.1f, Z, t.trim_color, th.accent);
      } else if (th.ornament == Th::kOrnamentCrystals) {
        theme::crystals(bd, apex, 0.75f, th.accent, seed);
        if (premium)
          for (int s = -1; s <= 1; s += 2)
            theme::crystals(bd, v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.35f), 0, front + 0.3f), 0.55f, th.accent, seed + 5u + static_cast<uint32_t>(s + 1));
      } else if (th.ornament == Th::kOrnamentLanterns && !(premium && t.lanterns)) {
        for (int s = -1; s <= 1; s += 2) {
          const vec3 at = theme::lantern(bd, v3(h.door_x + static_cast<float>(s) * (h.door_w * 0.5f + 0.3f), h.door_h + 0.05f, front + 0.02f), Z, kIron,
                                         v3(1.0f, 0.86f, 0.45f));
          condition::fire(out, at, th.torch_fire, 1);
        }
      }
    }
    // ---- the foundation's lift, and moss ----
    if (lift > 0) {
      theme::lift(out, v0, k0, f0, lift);
      h.ridge += lift;
      roof_top += lift;
      if (th.foundation == Th::kFoundationStilts) {
        theme::stilts(bd, b.cx, b.cz, b.hx, b.hz, lift, h.door_x, front, kWood);
        for (const theme::Body &hb : huts) theme::stilts(bd, hb.cx, hb.cz, hb.hx, hb.hz, lift, hb.cx, hb.cz + hb.hz, kWood);
      }
    }
    theme::moss(out, v0, th.moss, seed);

    // ---- the theme's ornaments in the yard ----
    if (deluxe && th.ornament == Th::kOrnamentStandingStones) {
      for (uint32_t k = 0; k < 2 + household; ++k) {
        float x, z;
        if (!yard.find(0.5f, false, x, z)) break;
        theme::standing_stone(bd, v3(x, 0, z), 1.5f + 0.9f * theme::hash01(seed + k), kStone, seed + 11u * k);
        solid_box(out, v3(x, 0.9f, z), v3(0.32f, 0.9f, 0.32f));
      }
    } else if (deluxe && th.ornament == Th::kOrnamentPrayerFlags) {
      float x, z;
      if (yard.find(0.3f, false, x, z)) {
        const vec3 top = v3(x, 2.6f + lift, z);
        bd.beam(v3(x, 0, z), top, 0.04f, X, kWood);
        const vec3 from = apex + v3(0, lift > 0 ? 0.0f : 0.0f, 0);
        const uint32_t fb = bd.bone(0, from, normalize(top - from), house_v1_bone::kMotionSway, 0.8f, 0.25f, 0.9f, theme::hash01(seed + 3u));
        theme::prayer_flags(bd, from, top, fb, seed);
      }
    }

    // ---- the yard ----
    struct Want {
      const char *what;
      float r;
      bool wall, on;
    };
    const Want wants[] = {
        {"woodpile", 0.55f, true, t.woodpile},
        {"barrels", 0.56f, true, t.barrels},
        {"crates", 0.4f, true, t.crates},
        {"sacks", 0.4f, true, t.sacks},
        {"bench", 0.6f, true, t.bench && !(deluxe && t.porch)},
        {"anvil", 0.45f, false, t.anvil && deluxe && t.feature != house_v1_type::kFeatureLeanTo},
        {"stall", 0.9f, false, t.stall && deluxe},
        {"well", 0.8f, false, t.well && deluxe},
        {"cart", 1.1f, false, t.cart && deluxe},
        {"trough", 0.65f, false, t.trough && deluxe},
        {"hay", 0.55f, false, t.hay},
        {"beehives", 0.45f, false, t.beehives && deluxe},
        {"cauldron", 0.5f, false, t.cauldron && deluxe},
    };
    // A bigger household keeps more by its walls: one more round of them for each generation.
    int basic_left = 2;
    for (uint32_t pass = 0; pass <= household; ++pass) {
      for (const Want &w : wants) {
        if (!w.on || (pass > 0 && !w.wall)) continue;
        if (stage == house_v1_spec::kStageBasic && (!w.wall || basic_left-- <= 0)) continue;
        float x, z;
        if (!yard.find(w.r, w.wall, x, z)) continue;
        // Wall things turn to lie along the nearest wall; the rest turn by the seed.
        float yaw = rng.uniform() * 6.2831853f;
        if (w.wall) {
          const float dx = maxf(b.cx - b.hx - x, x - b.cx - b.hx), dz = maxf(b.cz - b.hz - z, z - b.cz - b.hz);
          yaw = dx > dz ? kPi * 0.5f : 0.0f;
        }
        doodad(bd, out, w.what, x, z, yaw, t, season, rng);
      }
    }
    if (deluxe && t.laundry) {
      float x, z;
      if (yard.find(1.05f, false, x, z)) {
        const float yaw = rng.uniform() * kPi;
        laundry(bd, t, x, z, yaw, rng);
      }
    }
    if (deluxe && t.garden) {
      float x, z;
      if (yard.find(0.8f, false, x, z)) {
        bd.box(v3(x, 0.04f, z), v3(0.65f, 0.04f, 0.45f), kDirt, 0);
        const int per = in.detail == 2 ? 2 : 4;
        for (int row = 0; row < 3; ++row)
          for (int k = 0; k < per; ++k) {
            const vec3 at = v3(x - 0.5f + 1.0f * (static_cast<float>(k) + 0.5f) / static_cast<float>(per), 0.1f, z - 0.3f + 0.3f * static_cast<float>(row));
            const uint32_t ps = seed + static_cast<uint32_t>(row * 7 + k);
            if (season == house_v1_spec::kSeasonSpring) bd.blob(at, v3(0.06f, 0.05f, 0.06f), 0.3f, ps, v3(0.45f, 0.72f, 0.3f), 0);
            else if (season == house_v1_spec::kSeasonSummer)
              bd.blob(at + up * 0.04f, v3(0.12f, 0.1f, 0.12f), 0.3f, ps, (row + k) % 3 == 0 ? v3(0.85f, 0.25f, 0.2f) : v3(0.35f, 0.6f, 0.28f), 0);
            else if (season == house_v1_spec::kSeasonFall)
              bd.blob(at + up * 0.04f, v3(0.13f, 0.1f, 0.13f), 0.15f, ps, (row + k) % 2 ? v3(0.92f, 0.52f, 0.14f) : v3(0.55f, 0.42f, 0.25f), 0);
          }
      }
    }
    if (premium && t.fence) fence(bd, out, lot, h.door_x, front);
    // Leaves on the ground in the fall.
    if (season == house_v1_spec::kSeasonFall && mid)
      for (int k = 0; k < 5; ++k) {
        float x, z;
        if (!yard.find(0.3f, false, x, z)) break;
        bd.blob(v3(x, 0.02f, z), v3(0.28f, 0.03f, 0.22f), 0.3f, 140u + static_cast<uint32_t>(k), k % 2 ? v3(0.85f, 0.45f, 0.15f) : v3(0.7f, 0.3f, 0.12f), 0);
      }
    // The path from the door to the lot's edge.
    {
      float z1 = front + 0.45f;
      while (lot.inside(h.door_x, z1 + 0.25f, 0.05f)) z1 += 0.25f;
      if (z1 > front + 0.5f)
        bd.box(v3(h.door_x, 0.015f, (front + 0.45f + z1) * 0.5f), v3(0.38f, 0.015f, (z1 - front - 0.45f) * 0.5f),
               premium ? t.base_color : kDirt * 1.15f, 0);
    }
    out.doorway = v3(h.door_x, 0, front + 0.6f);
    out.doorway_heading = 0;

    // ---- collision: the walls and the roof ----
    {
      channels::physics_v1_shape hull;
      hull.type = channels::physics_v1_shape::kTypeConvexHull;
      hull.position = v3(0, 0, 0);
      hull.rotation = v3(0, 0, 0);
      const float eaves = h.wall_h + lift;
      if (round) {
        solid_cylinder(out, v3(b.cx, 0, b.cz), b.hx, eaves);
        for (int k = 0; k < 8; ++k) hull.points.push_back(v3(b.cx, eaves, b.cz) + theme::around(0.785398f * static_cast<float>(k)) * (b.hx + 0.3f));
        hull.points.push_back(v3(b.cx, roof_top, b.cz));
      } else {
        solid_box(out, v3(b.cx, eaves * 0.5f, b.cz), v3(b.hx, eaves * 0.5f, b.hz));
        for (int s = -1; s <= 1; s += 2) {
          const float u = static_cast<float>(s) * (frame.hu() + 0.2f);
          hull.points.push_back(frame.at(u, eaves, -frame.hv() - 0.3f));
          hull.points.push_back(frame.at(u, eaves, frame.hv() + 0.3f));
          if (gabled) hull.points.push_back(frame.at(u, roof_top, 0));
          else if (form == Th::kRoofFlat) {
            hull.points.push_back(frame.at(u, roof_top, -frame.hv()));
            hull.points.push_back(frame.at(u, roof_top, frame.hv()));
          }
        }
        if (!gabled && form != Th::kRoofFlat) hull.points.push_back(frame.at(0, roof_top, 0));
      }
      channels::physics_v1_body roof_body;
      roof_body.purpose = channels::physics_v1_body::kPurposeSolid;
      roof_body.shapes.push_back(hull);
      out.collision.push_back(roof_body);
    }
  }
  solid_box(out, v3(out.doorway.x, 0.9f, out.doorway.z - 0.15f), v3(0.6f, 0.9f, 0.4f), channels::physics_v1_body::kPurposeSensor, "doorway");

  // ---- the condition and the fire points it burns from ----
  if (ruined) condition::ruin(out, seed, th.moss);
  if (in.stage != house_v1_spec::kStageConstruction) condition::blaze_points(out, th.blaze_fire, seed ^ 0xb1a2e5u, 3 + 2 * static_cast<int>(household));

  // ---- size, footprint, and the marker ----
  float top = 0, reach = 0, ground = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    const float d = foundry::det::hypot(p.x, p.z);
    reach = maxf(reach, d);
    if (p.y < 0.4f) ground = maxf(ground, d);
  }
  // The conformance holds the collision too (the doorway's sensor reaches out past the step).
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      float r = 0;
      if (sh.type == channels::physics_v1_shape::kTypeBox) r = foundry::det::hypot(sh.half_extents.x, sh.half_extents.z);
      else if (sh.type == channels::physics_v1_shape::kTypeCylinder) r = sh.radius;
      for (const vec3 &q : sh.points) reach = maxf(reach, foundry::det::hypot(q.x, q.z));
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
    }
  out.height = top;
  out.radius = reach;
  out.footprint = fpi;
  out.footprint_cells = fp.round;
  out.footprint_radius = ground;
  out.cells.clear();
  for (int k = 0; k < fp.count; ++k) {
    out.cells.push_back(fp.cells[k][0]);
    out.cells.push_back(fp.cells[k][1]);
  }
  // The cells just outside the door: the door cell's front neighbours beyond the lot.
  out.doors.clear();
  for (int d = 0; d < 2; ++d) {
    const int q = fp.cells[door_cell][0] + (d == 0 ? -1 : 0), r = fp.cells[door_cell][1] + 1;
    if (fp.has(q, r)) continue;
    out.doors.push_back(q);
    out.doors.push_back(r);
  }
  // The lot's box: its cells' bounds (pointy-top: a cell reaches sqrt(3) / 2 across, 1 along z).
  float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
  for (int k = 0; k < fp.count; ++k) {
    const float x = cell_x(fp.cells[k][0], fp.cells[k][1]), z = cell_z(fp.cells[k][1]);
    x0 = minf(x0, x - 0.5f * kSqrt3);
    x1 = maxf(x1, x + 0.5f * kSqrt3);
    z0 = minf(z0, z - 1.0f);
    z1 = maxf(z1, z + 1.0f);
  }
  out.lot_min = v3(x0, 0, z0);
  out.lot_max = v3(x1, top, z1);
  // Every lot, for the viewer to outline and compare: this one, the same shape for the other
  // households, and the rest.
  for (uint32_t g = 0; g < kLotCount; ++g) {
    const Footprint other = footprint(g);
    for (int k = 0; k < other.count; ++k) {
      out.guide_cells.push_back(static_cast<int32_t>(g));
      out.guide_cells.push_back(other.cells[k][0]);
      out.guide_cells.push_back(other.cells[k][1]);
    }
    out.guide_tones.push_back(g == fpi ? 0u : (g % kShapes == shape ? 1u : 2u));
  }
  out.marker = v3(h.body.cx, h.ridge + 0.6f, h.body.cz);
  (void)hi;
  return true;
}

}  // namespace house_build_v1

FOUNDRY_SYSTEM(house_build_v1, channels::house_v1_spec, channels::house_v1_model)
