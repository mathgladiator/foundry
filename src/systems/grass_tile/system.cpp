// grass_tile: one tile of grass (design/plotter.md, "Fields").
//
// A field draws one tile per cell, chosen by grass_cell, so a tile is shared by
// every cell that picks it: the whole field is a few of these.
// 1. The cell. A pointy-top hex of circumradius 1 m centred on the origin; its
//    soil is a flat hex of earth at the ground (the ground's colour: grass
//    doesn't colour what it grows on), so the gaps between blades show earth,
//    with a skirt four metres down its sides for steps between cells. Only the grass
//    takes the cell's tint (a vertex's alpha says which).
// 2. Blades. The genome's count, spread over the hex (gathered into tufts by
//    `clump`), each leaning out and bending further along its length, tapering
//    to a point; one sheet (the renderer draws both faces). Colour runs from the
//    base colour to the tip, which dries by season; some carry a seed head.
// 3. Detail. Every level covers the same cell with the same colours and height:
//    high draws every blade in three segments; medium a third of them, wider, in
//    two; low a tuft of crossed cards per few blades; mat a hex lid at the
//    grass's mean height, for cells a few pixels across.
// 4. Plates. After the grass variants: water, rock, sand, path, bare soil, flat.
// 5. Sway. Each vertex carries how far it moves in a 10 m/s wind, growing with
//    the square of its height along the blade; the renderer moves it.
// Only + - * / and foundry::det: WebAssembly and native agree to the bit.
#include <vector>

#include "boundary.h"
#include "channels/grass_model.h"
#include "channels/grass_spec.h"
#include "packer/grass_model.h"
#include "packer/grass_spec.h"

namespace grass_tile {

namespace {

using foundry::vec3;
using channels::grass_genome;
using channels::grass_spec;

constexpr float kPi = foundry::det::kPi;

vec3 v3(float x, float y, float z) { return vec3{x, y, z}; }
vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
vec3 normalize(vec3 a) {
  const float l = foundry::det::sqrt(dot(a, a));
  return l > 1e-12f ? a * (1.0f / l) : v3(0, 1, 0);
}
vec3 mixv(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

uint32_t hash_str(const char *s, uint32_t h = 2166136261u) {
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}

// PCG32. One stream per knob, so adding a knob never reshuffles the others.
struct Rng {
  uint64_t state = 0, inc = 1;
  Rng(uint32_t seed, uint32_t stream) {
    inc = (static_cast<uint64_t>(stream) << 1) | 1u;
    next();
    state += static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    next();
  }
  uint32_t next() {
    const uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    const uint32_t x = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    const uint32_t r = static_cast<uint32_t>(old >> 59u);
    return (x >> r) | (x << ((32u - r) & 31u));
  }
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

// Alpha says whether the cell's tint applies: 255 on grass, 0 on the ground (the ground is the
// ground's colour, whatever grass grows on it).
uint32_t pack(vec3 c, bool grass) {
  auto byte = [](float v) { return static_cast<uint32_t>(clampf(v, 0, 1) * 255.0f + 0.5f); };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | ((grass ? 255u : 0u) << 24);
}

// Earth: bare soil, and the soil under the blades. Grass doesn't colour the ground it grows on.
const vec3 kEarth = vec3{0.36f, 0.27f, 0.17f};
// How far a cell's skirt reaches down: deep enough for the steps of steep terrain.
constexpr float kSkirt = 4.0f;

// The hex's corners, pointy-top (a corner at +Z), circumradius 1.
vec3 corner(int k) {
  const float a = kPi / 6 + kPi / 3 * static_cast<float>(k % 6);
  return v3(foundry::det::cos(a), 0, foundry::det::sin(a));
}

bool inside_hex(float x, float z, float r) {
  const float ax = x < 0 ? -x : x, az = z < 0 ? -z : z;
  return ax <= 0.8660254f * r && az + 0.57735027f * ax <= r;
}

struct Builder {
  channels::grass_model &out;
  uint32_t vertex(vec3 p, vec3 n, vec3 c, float sway, bool grass = true) {
    out.positions.push_back(p);
    out.normals.push_back(n);
    out.colors.push_back(pack(c, grass));
    out.sway.push_back(sway);
    return static_cast<uint32_t>(out.positions.size() - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  }
  // A flat hex at height y: a fan of six, with a skirt four metres down its sides, so a step between
  // neighbouring cells of different heights shows ground, not a gap. grass: the lid is grass (a
  // mat), tinted with the cell; otherwise it is ground.
  void hex(float y, vec3 colour, vec3 rim, vec3 earth, vec3 bottom, bool grass = false) {
    const uint32_t c = vertex(v3(0, y, 0), v3(0, 1, 0), colour, 0, grass);
    uint32_t ring[6];
    for (int k = 0; k < 6; ++k) ring[k] = vertex(corner(k) + v3(0, y, 0), v3(0, 1, 0), rim, 0, grass);
    for (int k = 0; k < 6; ++k) tri(c, ring[(k + 1) % 6], ring[k]);
    for (int k = 0; k < 6; ++k) {
      const vec3 a = corner(k), b = corner(k + 1);
      const vec3 n = normalize((a + b) * 0.5f);
      const uint32_t a0 = vertex(a + v3(0, y, 0), n, earth, 0, grass), b0 = vertex(b + v3(0, y, 0), n, earth, 0, grass);
      const uint32_t a1 = vertex(a + v3(0, y - kSkirt, 0), n, bottom, 0, false), b1 = vertex(b + v3(0, y - kSkirt, 0), n, bottom, 0, false);
      tri(a0, b0, b1);
      tri(a0, b1, a1);
    }
  }
};

}  // namespace

bool run(const grass_spec &in, channels::grass_model &out, std::string &error) {
  const grass_genome &g = in.genome;
  if (in.season > 3 || in.detail > 3 || in.bed > 1) {
    error = "season, detail, and bed must be in range";
    return false;
  }
  if (g.dry.size() != 4 || g.season_height.size() != 4 || g.variants == 0) {
    error = "genome needs 4 dry and 4 season_height values and at least one variant";
    return false;
  }
  // Past the grass variants and the five plates, it wraps (any variant names a tile).
  const uint32_t variant = in.variant % (g.variants + 5u);
  Builder bd{out};
  out = channels::grass_model{};
  // bed none: a terrain draws the ground, so the tile is only what grows on it (no earth, no plates).
  const bool own_bed = in.bed == 0;
  if (variant >= g.variants && !own_bed) {
    // A plate under a terrain: nothing to draw.
  } else if (variant >= g.variants) {
    // A plate: water, rock, sand, path, bare soil.
    static const vec3 kPlates[5][2] = {{{0.22f, 0.45f, 0.62f}, {0.30f, 0.55f, 0.70f}},
                                       {{0.46f, 0.45f, 0.43f}, {0.40f, 0.39f, 0.37f}},
                                       {{0.86f, 0.78f, 0.58f}, {0.80f, 0.72f, 0.52f}},
                                       {{0.62f, 0.50f, 0.34f}, {0.55f, 0.44f, 0.30f}},
                                       {{0, 0, 0}, {0, 0, 0}}};
    const uint32_t k = variant - g.variants;
    const vec3 c = k == 4 ? kEarth : kPlates[k][0], rim = k == 4 ? kEarth * 0.92f : kPlates[k][1];
    const vec3 earth = k == 0 ? v3(0.36f, 0.30f, 0.22f) : rim * 0.85f;
    bd.hex(k == 0 ? -0.06f : 0.0f, c, rim, earth, earth * 0.8f);
  } else {
    const uint32_t season = in.season;
    const float dry = g.dry[season];
    const float H = g.height * g.season_height[season];
    const vec3 tip = mixv(g.tip_color, g.dry_color, dry), base = mixv(g.base_color, g.dry_color * 0.8f, dry * 0.6f);
    const bool heads = season == 2 || season == 3;
    const uint32_t seed = hash_str(g.name.c_str()) ^ (variant * 0x9E3779B9u + 0x7f4a7c15u);
    Rng place(seed, 1u), shape(seed, 2u);
    // Where the blades stand: the same places at every level (each level draws a share of them).
    const uint32_t N = g.blades;
    const uint32_t tufts = 1 + static_cast<uint32_t>(static_cast<float>(N) * (1.0f - g.clump) * 0.25f + 2);
    std::vector<vec3> centres;
    while (centres.size() < tufts) {
      const float x = (place.uniform() * 2 - 1) * 0.866f, z = place.uniform() * 2 - 1;
      if (inside_hex(x, z, 0.9f)) centres.push_back(v3(x, 0, z));
    }
    struct Blade {
      vec3 at, dir;
      float h, lean, bend;
      bool head;
    };
    std::vector<Blade> blades;
    for (uint32_t i = 0; i < N; ++i) {
      vec3 at;
      for (int tries = 0;; ++tries) {
        const vec3 c = centres[i % centres.size()];
        const float rx = place.uniform() * 2 - 1, rz = place.uniform() * 2 - 1;
        const bool clumped = place.uniform() < g.clump;
        at = clumped ? c + v3(rx, 0, rz) * 0.16f : v3(rx * 0.866f, 0, rz);
        if (inside_hex(at.x, at.z, 0.97f) || tries > 8) break;
      }
      Blade b;
      b.at = at;
      const float az = shape.uniform() * 2 * kPi;
      b.dir = v3(foundry::det::cos(az), 0, foundry::det::sin(az));
      b.h = H * (1.0f - g.height_jitter * shape.uniform());
      b.lean = g.lean * (0.3f + 0.7f * shape.uniform());
      b.bend = g.bend * (0.4f + 0.6f * shape.uniform());
      b.head = heads && shape.uniform() < g.seed_heads;
      blades.push_back(b);
    }
    const uint32_t detail = in.detail;
    // Blades: every one at high detail in three segments; at medium every 2nd, twice as wide, in
    // two; at low every 6th, five times as wide, as one tapering triangle. The same cover of the
    // cell from every level.
    auto grow = [&](Builder &into, uint32_t level) {
      const uint32_t step = level == 0 ? 1u : (level == 1 ? 2u : 6u);
      const int segs = level == 0 ? 3 : (level == 1 ? 2 : 1);
      const float widen = level == 0 ? 1.0f : (level == 1 ? 2.0f : 5.0f);
      for (uint32_t i = 0; i < blades.size(); i += step) {
        const Blade &b = blades[i];
        const vec3 side = normalize(cross(v3(0, 1, 0), b.dir));
        vec3 p = b.at;
        vec3 d = normalize(v3(0, 1, 0) * foundry::det::cos(b.lean) + b.dir * foundry::det::sin(b.lean));
        std::vector<uint32_t> left, right;
        for (int k = 0; k <= segs; ++k) {
          const float t = static_cast<float>(k) / static_cast<float>(segs);
          const float half = 0.5f * g.blade_width * widen * (1.0f - t * 0.85f);
          const vec3 n = normalize(cross(side, d) * 0.45f + v3(0, 1, 0));
          const vec3 c = mixv(base, tip, t);
          const float sway = g.sway * t * t;
          if (k < segs) {
            left.push_back(into.vertex(p - side * half, n, c, sway));
            right.push_back(into.vertex(p + side * half, n, c, sway));
          } else {
            left.push_back(into.vertex(p, n, c, sway));  // the tip
          }
          // Bend further out along the blade.
          d = normalize(d + b.dir * (b.bend / static_cast<float>(segs)));
          p = p + d * (b.h / static_cast<float>(segs));
        }
        for (int k = 0; k + 1 < segs; ++k) {
          into.tri(left[k], right[k], right[k + 1]);
          into.tri(left[k], right[k + 1], left[k + 1]);
        }
        into.tri(left[segs - 1], right[segs - 1], left[segs]);
        if (b.head && level == 0) {
          // A seed head: a small diamond at the tip.
          const vec3 top = into.out.positions[left[segs]];
          const float r = 0.012f + 0.25f * g.blade_width;
          const vec3 n = v3(0, 1, 0);
          const uint32_t a = into.vertex(top + side * r, n, g.seed_color, g.sway), c2 = into.vertex(top - side * r, n, g.seed_color, g.sway);
          const uint32_t up = into.vertex(top + d * (r * 2.5f), n, g.seed_color, g.sway), dn = into.vertex(top - d * r, n, g.seed_color * 0.85f, g.sway);
          into.tri(a, up, c2);
          into.tri(a, c2, dn);
        }
      }
    };
    if (detail == 3 && !own_bed) {
      // Under a terrain a flat mat would cut through sloping ground: the tufts stand in for it.
      grow(bd, 2);
    } else if (detail == 3) {
      // A mat: a lid at the blades' mean height, coloured as the finest blades read from above: their
      // colours weighted by the area they cover seen from above, over the soil they leave showing.
      channels::grass_model fine;
      Builder scratch{fine};
      grow(scratch, 0);
      vec3 sum = v3(0, 0, 0);
      float area = 0;
      auto unpack = [](uint32_t c) {
        return v3(static_cast<float>(c & 255u) / 255.0f, static_cast<float>((c >> 8) & 255u) / 255.0f,
                  static_cast<float>((c >> 16) & 255u) / 255.0f);
      };
      for (size_t k = 0; k + 2 < fine.indices.size(); k += 3) {
        const uint32_t i0 = fine.indices[k], i1 = fine.indices[k + 1], i2 = fine.indices[k + 2];
        const vec3 e1 = fine.positions[i1] - fine.positions[i0], e2 = fine.positions[i2] - fine.positions[i0];
        const float cy = e1.z * e2.x - e1.x * e2.z;
        const float a = 0.5f * (cy < 0 ? -cy : cy);
        sum = sum + (unpack(fine.colors[i0]) + unpack(fine.colors[i1]) + unpack(fine.colors[i2])) * (a / 3.0f);
        area += a;
      }
      const float hex_area = 2.5980762f, cover_ratio = area / hex_area;
      const float cover = cover_ratio / (1.0f + cover_ratio);  // overlapping blades cover less than their sum
      const vec3 seen = area > 0 ? mixv(kEarth, sum * (1.0f / area), cover) : kEarth;
      bd.hex(H * 0.42f, seen, seen * 0.96f, mixv(base, tip, 0.5f), kEarth * 0.8f, true);
    } else {
      if (own_bed) bd.hex(0.0f, kEarth, kEarth * 0.9f, kEarth * 0.8f, kEarth * 0.64f);
      grow(bd, detail);
    }
  }
  // ---- size ----
  float top = 0, reach = 0;
  for (const vec3 &p : out.positions) {
    top = p.y > top ? p.y : top;
    const float rr = foundry::det::sqrt(p.x * p.x + p.z * p.z);
    reach = rr > reach ? rr : reach;
  }
  out.height = top;
  out.radius = reach;
  return true;
}

}  // namespace grass_tile

FOUNDRY_SYSTEM(grass_tile, channels::grass_spec, channels::grass_model)
