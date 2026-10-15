// rock_body: makes a rock and the pieces it breaks into (design/systems.md, Rocks).
//
// 1. Shape: a convex block chiselled from a cube by random planes (few for
//    blocky stone, many for worn), in a unit space where the rock's radius is
//    about 1. Every vertex then goes through one transform: worn round by the
//    material's roundness, roughened by a vector noise field, stretched and
//    flattened, turned, and scaled to the requested size. Because the transform
//    depends only on the point, faces that meet stay joined however they were cut.
// 2. Pieces: the same block cut into cells by the material's fracture: voronoi
//    chunks, slab plates along the grain, columnar prisms across it, splinter
//    shards along it, a crumble of many small pieces, impact (fine near the blow,
//    coarse away), or a split into a few halves. Each cell is the block clipped
//    by half-spaces, so every piece is closed, and the pieces fill the rock.
//    They are built a level of detail lower than the whole rock: they are seen
//    briefly and moving.
// 3. Paint: flat palette regions by pattern (speckled, banded, veined, layered,
//    jointed, glassy, pitted), fresh colour on broken faces, moss on the upper
//    faces, and snow in winter.
// 4. One mesh: the whole rock on bone 0 and piece n on bone 1 + n. The pose
//    shows one or the other, so breaking never rebuilds.
#include <vector>

#include "boundary.h"
#include "channels/rock_model.h"
#include "channels/rock_spec.h"
#include "math.h"
#include "packer/rock_model.h"
#include "packer/rock_spec.h"

namespace rock_body {

using namespace rock_math;
using channels::rock_material;
using channels::rock_spec;

namespace {

constexpr int kMaxPieces = 40;
const vec3 kSnow = {0.90f, 0.93f, 0.97f};

// ---- convex polytopes ------------------------------------------------------------

struct Face {
  std::vector<vec3> p;  // counter-clockwise seen from outside
  bool cut = false;     // a broken face (fresh colour), not the rock's skin
};
using Poly = std::vector<Face>;

struct Plane {
  vec3 n;
  float d;  // keeps dot(n, x) <= d
};

Poly cube(float h) {
  const vec3 c[8] = {v3(-h, -h, -h), v3(h, -h, -h), v3(h, h, -h), v3(-h, h, -h),
                     v3(-h, -h, h),  v3(h, -h, h),  v3(h, h, h),  v3(-h, h, h)};
  const int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
  Poly out;
  for (const auto &q : f) {
    Face face;
    for (int k : q) face.p.push_back(c[k]);
    out.push_back(face);
  }
  return out;
}

// Orders points on a plane counter-clockwise about its normal, without trigonometry.
void order_ccw(std::vector<vec3> &pts, vec3 n) {
  vec3 c = v3(0, 0, 0);
  for (const vec3 &p : pts) c = c + p;
  c = c * (1.0f / static_cast<float>(pts.size()));
  const vec3 u = perpendicular(n), w = cross(n, u);
  struct Key {
    float x, y;
    vec3 p;
  };
  std::vector<Key> keys;
  for (const vec3 &p : pts) keys.push_back({dot(p - c, u), dot(p - c, w), p});
  auto half = [](const Key &k) { return k.y > 0 || (k.y == 0 && k.x > 0) ? 0 : 1; };
  // Insertion sort by angle: half-plane first, then the sign of the cross product.
  for (size_t i = 1; i < keys.size(); ++i) {
    Key k = keys[i];
    size_t j = i;
    while (j > 0) {
      const Key &a = keys[j - 1];
      const int ha = half(a), hk = half(k);
      const bool after = ha < hk || (ha == hk && a.x * k.y - a.y * k.x > 0);
      if (after) break;
      keys[j] = keys[j - 1];
      --j;
    }
    keys[j] = k;
  }
  for (size_t i = 0; i < keys.size(); ++i) pts[i] = keys[i].p;
}

// Keeps the part of a polytope where dot(n, x) <= d, closing it with a cap face.
Poly clip(const Poly &in, const Plane &pl, bool cut) {
  Poly out;
  std::vector<vec3> cap;
  const float eps = 1e-6f;
  auto add_cap = [&](vec3 p) {
    for (const vec3 &q : cap)
      if (length(q - p) < 1e-5f) return;
    cap.push_back(p);
  };
  for (const Face &f : in) {
    Face g;
    g.cut = f.cut;
    const size_t n = f.p.size();
    for (size_t i = 0; i < n; ++i) {
      const vec3 a = f.p[i], b = f.p[(i + 1) % n];
      const float da = dot(pl.n, a) - pl.d, db = dot(pl.n, b) - pl.d;
      if (da <= eps) g.p.push_back(a);
      if (da > -eps && da <= eps) add_cap(a);
      if ((da < -eps && db > eps) || (da > eps && db < -eps)) {
        const vec3 x = a + (b - a) * (da / (da - db));
        g.p.push_back(x);
        add_cap(x);
      }
    }
    if (g.p.size() >= 3) out.push_back(g);
  }
  if (cap.size() >= 3) {
    order_ccw(cap, pl.n);
    Face c;
    c.p = cap;
    c.cut = cut;
    out.push_back(c);
  }
  return out;
}

bool inside(const std::vector<Plane> &planes, vec3 p) {
  for (const Plane &pl : planes)
    if (dot(pl.n, p) > pl.d) return false;
  return true;
}

// ---- the point transform ---------------------------------------------------------

struct Shape {
  float roundness, roughness, relief;
  vec3 scale;  // stretch and size, metres per unit
  float yaw_c, yaw_s;
  float lift = 0;
  uint32_t seed;
  vec3 world(vec3 q) const {
    // Worn round: points near the skin move toward the unit sphere; the inside stays put.
    const float r = length(q);
    if (r > 1e-4f && roundness > 0) {
      const float near = smooth(clampf((r - 0.55f) / 0.45f, 0, 1));
      q = q * (1 + roundness * 0.75f * near * (1 / r - 1));
    }
    // Roughened by a vector noise field.
    if (roughness > 0) {
      const vec3 s = q * (1.0f / relief);
      const float nx = noise(s, seed + 101) - 0.5f;
      const float ny = noise(s, seed + 202) - 0.5f;
      const float nz = noise(s, seed + 303) - 0.5f;
      q = q + v3(nx, ny, nz) * (2 * roughness);
    }
    const vec3 p = mul(q, scale);
    return v3(p.x * yaw_c - p.z * yaw_s, p.y + lift, p.x * yaw_s + p.z * yaw_c);
  }
  // The outward normal of the worn ellipsoid at unit-space point q, in world space.
  vec3 round_normal(vec3 q) const {
    const vec3 e = normalize(v3(q.x / scale.x, q.y / scale.y, q.z / scale.z));
    return v3(e.x * yaw_c - e.z * yaw_s, e.y, e.x * yaw_s + e.z * yaw_c);
  }
};

// ---- painting ------------------------------------------------------------------

uint32_t pack(vec3 c) {
  auto byte = [](float v) {
    v = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(v);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

struct Painter {
  const rock_material &m;
  const rock_spec &spec;
  uint32_t seed;
  vec3 grain;  // world space
  float moss;
  vec3 colour(vec3 p, vec3 n, bool cut) const {
    const float sc = maxf(m.pattern_scale, 0.01f);
    const float base = noise(p * (1.0f / (sc * 3.0f)), seed + 7);
    float shade = clampf(0.35f + 0.5f * base + 0.25f * n.y, 0, 1);
    vec3 light = cut ? m.fresh : m.color_light;
    vec3 dark = cut ? m.fresh * 0.72f : m.color_dark;
    vec3 c = dark + (light - dark) * shade;
    const vec3 accent = cut ? m.accent * 0.8f + m.fresh * 0.2f : m.accent;
    switch (m.pattern) {
      case rock_material::kPatternSpeckled: {
        const float s = noise(p * (1.0f / sc), seed + 11);
        if (s > 0.7f) c = accent;
        else if (s < 0.24f) c = dark * 0.8f;
        break;
      }
      case rock_material::kPatternBanded: {
        const float t = dot(p, grain) / sc + 1.4f * noise(p * (0.6f / sc), seed + 13);
        const float band = t - std::floor(t);
        if (band < 0.32f) c = accent * (0.92f + 0.16f * shade);
        else if (band > 0.86f) c = dark;
        break;
      }
      case rock_material::kPatternVeined: {
        const float v = noise(p * (1.0f / sc), seed + 17) + 0.5f * noise(p * (2.3f / sc), seed + 19);
        const float d = v - 0.75f;
        if (d > -0.035f && d < 0.035f) c = accent;
        break;
      }
      case rock_material::kPatternLayered: {
        const float t = dot(p, grain) / sc + 0.4f * noise(p * (1.0f / sc), seed + 23);
        const float layer = t - std::floor(t);
        if (layer < 0.14f) c = dark * 0.75f;
        else if (layer > 0.62f && layer < 0.7f) c = accent;
        break;
      }
      case rock_material::kPatternJointed: {
        const float t = dot(p, grain) / sc + 0.25f * noise(p * (1.0f / sc), seed + 29);
        const float j = t - std::floor(t);
        if (j < 0.07f) c = dark * 0.6f;
        if (noise(p * (0.7f / sc), seed + 31) > 0.72f) c = c + (accent - c) * 0.6f;  // rusty weathering
        break;
      }
      case rock_material::kPatternGlassy: {
        // A sheen where the face turns toward a fixed sky direction, in streaks.
        const float facing = dot(n, normalize(v3(0.5f, 0.8f, 0.3f)));
        const float streak = noise(p * (1.0f / sc), seed + 37);
        if (facing > 0.78f && streak > 0.35f) c = accent;
        else c = dark + (light - dark) * (0.2f * shade);
        break;
      }
      case rock_material::kPatternPitted: {
        const float s = noise(p * (1.0f / sc), seed + 41);
        if (s < 0.2f) c = dark * 0.78f;
        else if (s > 0.8f) c = accent;
        break;
      }
      default:
        break;
    }
    if (!cut) {
      if (moss > 0 && n.y > 0.3f) {
        const float g = noise(p * 2.2f, seed + 43);
        if (g < moss * (0.35f + 0.9f * n.y)) c = m.moss_color * (0.85f + 0.3f * noise(p * 6.0f, seed + 47));
      }
      if (spec.season == rock_spec::kSeasonWinter && m.snow > 0 && n.y > 1.0f - 0.55f * m.snow) c = kSnow;
    }
    return c;
  }
};

// ---- meshing -------------------------------------------------------------------

struct Mesh {
  channels::rock_model &out;
  const Shape &shape;
  const Painter &paint;
  // Emits a polytope: each face fanned, each triangle split `levels` times, every
  // vertex through the shape's transform, flat-shaded in the cel look.
  void emit(const Poly &poly, int levels, uint32_t bone) {
    for (const Face &f : poly) {
      for (size_t k = 1; k + 1 < f.p.size(); ++k) tri(f.p[0], f.p[k], f.p[k + 1], levels, bone, f.cut);
    }
  }
  void tri(vec3 a, vec3 b, vec3 c, int levels, uint32_t bone, bool cut) {
    if (levels > 0) {
      const vec3 ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, ca = (c + a) * 0.5f;
      tri(a, ab, ca, levels - 1, bone, cut);
      tri(b, bc, ab, levels - 1, bone, cut);
      tri(c, ca, bc, levels - 1, bone, cut);
      tri(ab, bc, ca, levels - 1, bone, cut);
      return;
    }
    const vec3 q[3] = {a, b, c};
    vec3 p[3];
    for (int i = 0; i < 3; ++i) p[i] = shape.world(q[i]);
    vec3 fn = cross(p[1] - p[0], p[2] - p[0]);
    if (length(fn) < 1e-12f) return;  // worn flat to nothing
    fn = normalize(fn);
    const uint32_t first = static_cast<uint32_t>(out.positions.size());
    // One colour per triangle, from its centre: crisp palette facets (style.md section 5).
    const uint32_t colour = pack(paint.colour((p[0] + p[1] + p[2]) * (1.0f / 3), fn, cut));
    for (int i = 0; i < 3; ++i) {
      vec3 n = fn;
      if (!cut && shape.roundness > 0) n = normalize(fn + (shape.round_normal(q[i]) - fn) * (shape.roundness * 0.85f));
      out.positions.push_back(p[i]);
      out.normals.push_back(n);
      out.bone_a.push_back(bone);
      out.bone_b.push_back(bone);
      out.bone_w.push_back(0);
      out.colors.push_back(colour);
    }
    out.indices.push_back(first);
    out.indices.push_back(first + 1);
    out.indices.push_back(first + 2);
  }
};

// Volume and centre of mass of the triangles from `first_index` on (a closed mesh).
void mass_properties(const channels::rock_model &m, size_t first_index, float &volume, vec3 &centre) {
  float v = 0;
  vec3 c = v3(0, 0, 0);
  for (size_t k = first_index; k + 2 < m.indices.size(); k += 3) {
    const vec3 a = m.positions[m.indices[k]], b = m.positions[m.indices[k + 1]], d = m.positions[m.indices[k + 2]];
    const float t = dot(a, cross(b, d)) / 6.0f;
    v += t;
    c = c + (a + b + d) * (t / 4.0f);
  }
  volume = v;
  centre = v > 1e-9f ? c * (1.0f / v) : v3(0, 0, 0);
}

// The cells of a fracture, as half-space lists in unit space.
struct Fracturer {
  const rock_material &m;
  const std::vector<Plane> &shape;
  Rng &rng;
  vec3 grain;
  vec3 random_inside() {
    for (int tries = 0; tries < 200; ++tries) {
      const float x = rng.signed_unit();
      const float y = rng.signed_unit();
      const float z = rng.signed_unit();
      const vec3 p = v3(x, y, z) * 0.95f;
      if (inside(shape, p)) return p;
    }
    return v3(0, 0, 0);
  }
  // Voronoi cells of the sites under the metric I + a g g^T (a < 0 stretches cells along g).
  std::vector<std::vector<Plane>> voronoi(const std::vector<vec3> &sites, float a) {
    auto M = [&](vec3 v) { return v + grain * (a * dot(grain, v)); };
    std::vector<std::vector<Plane>> cells(sites.size());
    for (size_t i = 0; i < sites.size(); ++i)
      for (size_t j = 0; j < sites.size(); ++j) {
        if (i == j) continue;
        const vec3 si = sites[i], sj = sites[j];
        const vec3 n = M(sj - si);
        if (length(n) < 1e-6f) continue;
        cells[i].push_back({n, 0.5f * (dot(sj, M(sj)) - dot(si, M(si)))});
      }
    return cells;
  }
  // Layers across the grain, each split by a voronoi of `per` sites that ignores the grain.
  std::vector<std::vector<Plane>> layered(int layers, int per, float lo, float hi) {
    std::vector<std::vector<Plane>> cells;
    std::vector<float> cuts = {lo - 1};
    for (int k = 1; k < layers; ++k) {
      const float t = (static_cast<float>(k) + 0.3f * rng.signed_unit()) / static_cast<float>(layers);
      cuts.push_back(lo + (hi - lo) * t);
    }
    cuts.push_back(hi + 1);
    for (int k = 0; k < layers; ++k) {
      std::vector<vec3> sites;
      for (int s = 0; s < per; ++s) {
        vec3 p = random_inside();
        const float mid = clampf(0.5f * (cuts[static_cast<size_t>(k)] + cuts[static_cast<size_t>(k) + 1]), lo, hi);
        p = p + grain * (mid - dot(p, grain));
        sites.push_back(p);
      }
      auto vc = voronoi(sites, -0.98f);
      for (auto &c : vc) {
        c.push_back({grain, cuts[static_cast<size_t>(k) + 1]});
        c.push_back({-grain, -cuts[static_cast<size_t>(k)]});
        cells.push_back(c);
      }
    }
    return cells;
  }
  std::vector<std::vector<Plane>> cells(int n, float lo, float hi) {
    std::vector<vec3> sites;
    switch (m.fracture) {
      case rock_material::kFractureSlab: {
        const int layers = n / 2 > 2 ? n / 2 : 2;
        return layered(layers, 2, lo, hi);
      }
      case rock_material::kFractureColumnar: {
        const int per = n / 2 > 2 ? n / 2 : 2;
        return layered(2, per, lo, hi);
      }
      case rock_material::kFractureSplinter:
        for (int k = 0; k < n; ++k) sites.push_back(random_inside());
        return voronoi(sites, -0.85f * maxf(m.grain_strength, 0.3f));
      case rock_material::kFractureCrumble:
        for (int k = 0; k < n * 2 + 2; ++k) sites.push_back(random_inside());
        return voronoi(sites, 0);
      case rock_material::kFractureImpact: {
        // Dense near the front face, where a blow lands, sparse behind.
        const vec3 hit = v3(0, 0.15f, -0.85f);
        for (int k = 0; k < n; ++k) {
          vec3 p = random_inside();
          const float u = rng.uniform();
          p = hit + (p - hit) * (0.25f + 0.75f * u * u);
          sites.push_back(p);
        }
        return voronoi(sites, 0);
      }
      case rock_material::kFractureSplit: {
        // One or two cuts near the middle: halves or quarters.
        std::vector<std::vector<Plane>> out = {{}};
        const int planes = n > 3 ? 2 : 1;
        for (int k = 0; k < planes; ++k) {
          const float az = rng.uniform() * 2 * kPi;
          const float tilt = 0.35f * rng.signed_unit();
          const vec3 nn = normalize(v3(foundry::det::cos(az), tilt, foundry::det::sin(az)));
          const float d = 0.15f * rng.signed_unit();
          std::vector<std::vector<Plane>> next;
          for (const auto &c : out) {
            auto a = c, b = c;
            a.push_back({nn, d});
            b.push_back({-nn, -d});
            next.push_back(a);
            next.push_back(b);
          }
          out = next;
        }
        return out;
      }
      default: {
        for (int k = 0; k < n; ++k) sites.push_back(random_inside());
        return voronoi(sites, 2.0f * m.grain_strength);  // a little flatter across the grain
      }
    }
  }
};

}  // namespace

bool run(const rock_spec &in, channels::rock_model &out, std::string &error) {
  const rock_material &m = in.material;
  if (in.size > 3 || in.moss > 2 || in.season > 3 || in.detail > 2) {
    error = "size, moss, season, and detail must be in range";
    return false;
  }
  if (m.pattern_scale <= 0 || m.relief_scale <= 0 || m.density <= 0) {
    error = "material pattern_scale, relief_scale, and density must be positive";
    return false;
  }
  const uint32_t seed = hash_str(m.name.c_str()) ^ hash_u32(in.seed * 2654435761u + in.size);
  Rng rng(seed, 3u);

  // ---- the shape, in unit space ----
  static const float kRadius[4] = {0.3f, 0.6f, 1.1f, 2.0f};
  const float R = kRadius[in.size];
  std::vector<Plane> planes;
  const int count = static_cast<int>(m.facets) + static_cast<int>(m.roundness * 14.0f);
  planes.push_back({v3(0, -1, 0), 0.55f + 0.1f * rng.uniform()});  // a flatter base to sit on
  for (int k = 0; k < count; ++k) {
    vec3 n;
    do {
      const float x = rng.signed_unit();
      const float y = rng.signed_unit();
      const float z = rng.signed_unit();
      n = v3(x, y, z);
    } while (dot(n, n) > 1 || dot(n, n) < 0.05f);
    n = normalize(n);
    const float spread = 0.28f * (1 - m.roundness);
    planes.push_back({n, 1 - spread * rng.uniform()});
  }
  Poly rock = cube(1.5f);
  for (const Plane &pl : planes) rock = clip(rock, pl, false);

  Shape shape;
  shape.roundness = m.roundness;
  shape.roughness = m.roughness;
  shape.relief = m.relief_scale;
  shape.seed = seed;
  const float stretch = 0.85f + 0.3f * rng.uniform();
  shape.scale = v3(m.elongation * stretch, m.flatness, 1) * (R / foundry::det::sqrt(m.elongation));
  const float yaw = rng.uniform() * 2 * kPi;
  shape.yaw_c = foundry::det::cos(yaw);
  shape.yaw_s = foundry::det::sin(yaw);
  // The grain, turned a little at random per rock, in unit space and in world space.
  vec3 grain = length(m.grain) > 1e-4f ? normalize(m.grain) : v3(0, 1, 0);
  {
    const float tilt = 0.25f * rng.signed_unit();
    grain = normalize(rotate(grain, perpendicular(grain), tilt));
  }
  // Sink the rock a little into the ground: find its lowest point first.
  {
    float lo = 1e9f, hi = -1e9f;
    for (const Face &f : rock)
      for (const vec3 &q : f.p) {
        const float y = shape.world(q).y;
        lo = minf(lo, y);
        hi = maxf(hi, y);
      }
    shape.lift = -lo - 0.1f * (hi - lo);
  }
  const vec3 grain_world = normalize(v3(grain.x * shape.scale.x, grain.y * shape.scale.y, grain.z * shape.scale.z));
  static const float kMoss[3] = {0.0f, 0.3f, 0.65f};
  const Painter paint{m, in, seed, v3(grain_world.x * shape.yaw_c - grain_world.z * shape.yaw_s, grain_world.y,
                                     grain_world.x * shape.yaw_s + grain_world.z * shape.yaw_c),
                      kMoss[in.moss] * m.lichen};
  Mesh mesh{out, shape, paint};

  // ---- the whole rock, bone 0 ----
  static const int kWhole[3] = {2, 1, 1};
  static const int kPiece[3] = {1, 0, 0};
  const int whole_levels = kWhole[in.detail] + (in.detail == 0 && m.roundness > 0.5f ? 1 : 0);
  mesh.emit(rock, whole_levels, 0);

  // ---- the pieces, bones 1.. ----
  static const float kCount[4] = {0.5f, 1.0f, 1.6f, 2.3f};
  int n = static_cast<int>(static_cast<float>(m.pieces) * kCount[in.size] + 0.5f);
  n = n < 2 ? 2 : (n > kMaxPieces ? kMaxPieces : n);
  float lo = 1e9f, hi = -1e9f;
  for (const Face &f : rock)
    for (const vec3 &q : f.p) {
      lo = minf(lo, dot(q, grain));
      hi = maxf(hi, dot(q, grain));
    }
  Fracturer fr{m, planes, rng, grain};
  const auto cells = fr.cells(n, lo, hi);
  float whole_volume = 0;
  vec3 whole_centre;
  mass_properties(out, 0, whole_volume, whole_centre);
  out.pieces.clear();
  for (const auto &cell : cells) {
    if (static_cast<int>(out.pieces.size()) >= kMaxPieces) break;
    Poly piece = rock;
    for (const Plane &pl : cell) {
      piece = clip(piece, pl, true);
      if (piece.size() < 4) break;
    }
    if (piece.size() < 4) continue;
    const size_t first_vertex = out.positions.size(), first_index = out.indices.size();
    const uint32_t bone = static_cast<uint32_t>(out.pieces.size()) + 1;
    mesh.emit(piece, kPiece[in.detail], bone);
    float volume;
    vec3 centre;
    mass_properties(out, first_index, volume, centre);
    if (volume < whole_volume * 0.002f) {  // a sliver: too small to see
      out.positions.resize(first_vertex);
      out.normals.resize(first_vertex);
      out.bone_a.resize(first_vertex);
      out.bone_b.resize(first_vertex);
      out.bone_w.resize(first_vertex);
      out.colors.resize(first_vertex);
      out.indices.resize(first_index);
      continue;
    }
    channels::rock_piece p;
    p.bone = bone;
    p.center = centre;
    p.mass = volume * m.density * 1000.0f;
    p.radius = 0;
    // Support points: the piece's extremes in 14 directions.
    static const float kDirs[14][3] = {{1, 0, 0},  {-1, 0, 0},  {0, 1, 0},  {0, -1, 0}, {0, 0, 1},
                                       {0, 0, -1}, {1, 1, 1},   {1, 1, -1}, {1, -1, 1}, {1, -1, -1},
                                       {-1, 1, 1}, {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1}};
    for (const auto &d : kDirs) {
      const vec3 dir = v3(d[0], d[1], d[2]);
      float best = -1e9f;
      vec3 at = v3(0, 0, 0);
      for (size_t k = first_vertex; k < out.positions.size(); ++k) {
        const float s = dot(out.positions[k] - centre, dir);
        if (s > best) {
          best = s;
          at = out.positions[k] - centre;
        }
      }
      p.support.push_back(at);
    }
    for (size_t k = first_vertex; k < out.positions.size(); ++k) p.radius = maxf(p.radius, length(out.positions[k] - centre));
    out.pieces.push_back(p);
  }

  // ---- size for the conformance ----
  float top = 0, reach = 0;
  for (size_t k = 0; k < out.positions.size(); ++k) {
    if (out.bone_a[k] != 0) break;  // the whole rock comes first
    const vec3 p = out.positions[k];
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
  }
  out.height = top;
  out.radius = reach;

  // ---- collision ----
  // Exact: a convex hull of the whole rock, from its support points in 26 directions (the
  // cube's faces, edges, and corners), which hold the rock and keep the hull small.
  out.collision.clear();
  {
    channels::physics_shape hull;
    hull.type = channels::physics_shape::kTypeConvexHull;
    hull.position = v3(0, 0, 0);
    hull.rotation = v3(0, 0, 0);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          if (dx == 0 && dy == 0 && dz == 0) continue;
          const vec3 d = v3(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
          float best = -1e30f;
          vec3 at = v3(0, 0, 0);
          for (size_t k = 0; k < out.positions.size(); ++k) {
            if (out.bone_a[k] != 0) break;
            const float s = dot(out.positions[k], d);
            if (s > best) {
              best = s;
              at = out.positions[k];
            }
          }
          bool seen = false;
          for (const vec3 &p : hull.points) seen = seen || (p.x == at.x && p.y == at.y && p.z == at.z);
          if (!seen) hull.points.push_back(at);
        }
    channels::physics_body body;
    body.purpose = channels::physics_body::kPurposeSolid;
    body.shapes.push_back(hull);
    out.collision.push_back(body);
  }
  return true;
}

}  // namespace rock_body

FOUNDRY_SYSTEM(rock_body, channels::rock_spec, channels::rock_model)
