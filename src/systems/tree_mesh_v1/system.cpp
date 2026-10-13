// tree_mesh_v1: wraps a tree's skeleton in geometry (design/systems.md, Body).
//
// - Bark: generalized cylinders with parallel-transported frames, root flare
//   and buttress lobes on the stump, and a bark relief by style (furrowed,
//   plated, shaggy, mottled, fluted). The stem is cut at the break height into
//   a stump and a log, each capped with end grain, so the break path can pull
//   them apart without a hole.
// - Foliage: noise-displaced icosphere clumps (broadleaf), stretched spiky
//   tufts (needles), or flattened rosettes, chosen per clump by season:
//   leaves, blossom, a deciduous tree's dead winter leaves, or bare.
// - Fruit: small spheres hanging from the clumps (or bare twigs) in the seasons
//   the species bears, unripe to ripe. Fruit that can be picked shows only
//   while the request says the tree still carries it (the game picks it off);
//   cones and propagules that can't be picked always show.
// - Roots and moss: cypress knees around the trunk, a mangrove's arching prop
//   roots, and Spanish moss hanging in strands from the limbs.
// - Stump mode: a felled tree is only its stump (and its roots), capped with
//   end grain at the cut.
// - Collision (design/spine.md, Layer 1): the trunk, or the stump, as a solid
//   cylinder from the ground; static for the build, inside the conformance.
// - Skinning: rigid per bone, blended halfway at bone starts within a piece.
// - Detail: high, medium, or low tessellation and foliage density.
#include <vector>

#include "boundary.h"
#include "channels/tree_v1_body.h"
#include "channels/tree_v1_rig.h"
#include "math.h"
#include "packer/tree_v1_body.h"
#include "packer/tree_v1_rig.h"

namespace tree_mesh_v1 {

using namespace tree_math;
using channels::tree_v1_genome;
using channels::tree_v1_spec;

namespace {

enum Region : uint32_t { kBark = 0, kWood = 1, kLeaf = 2, kBlossom = 3, kFruit = 4, kDeadLeaf = 5, kDeadWood = 6, kHangingMoss = 7 };

struct Builder {
  channels::tree_v1_body &out;
  const std::vector<channels::tree_v1_bone> &bones;
  explicit Builder(channels::tree_v1_body &o, const std::vector<channels::tree_v1_bone> &b) : out(o), bones(b) {}
  uint32_t vertex(vec3 p, vec3 n, uint32_t region, float shade, float up, uint32_t a, uint32_t b = 0, float w = 0) {
    out.positions.push_back(p);
    out.normals.push_back(n);
    out.region.push_back(region);
    out.shade.push_back(clampf(shade, 0, 1));
    out.up.push_back(clampf(up, 0, 1));
    out.bone_a.push_back(a);
    out.bone_b.push_back(w > 0 ? b : a);
    out.bone_w.push_back(w);
    return static_cast<uint32_t>(out.positions.size() - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  }
};

// ---- bark ------------------------------------------------------------------------

struct Bark {
  uint32_t style;
  uint32_t seed;
  // Relief (fraction of the radius, signed) and shade in [0, 1] at angle theta,
  // distance s along the branch.
  void at(float theta, float s, int level, float &relief, float &shade) const {
    const float c = foundry::det::cos(theta), sn = foundry::det::sin(theta);
    vec3 q = v3(c * 1.3f, s, sn * 1.3f);
    float n = noise(q * 1.7f, seed);
    relief = 0;
    shade = 0.45f + 0.2f * (n - 0.5f);
    const float strength = level == 0 ? 1.0f : (level == 1 ? 0.6f : 0.0f);
    switch (style) {
      case tree_v1_genome::kBarkFurrowed: {
        float ridge = 0.5f + 0.5f * foundry::det::cos(theta * 7 + 3 * noise(v3(0, s * 1.2f, 0), seed + 1));
        shade = 0.25f + 0.65f * smooth(ridge) + 0.1f * (n - 0.5f);
        relief = 0.07f * (ridge - 0.5f) * strength;
        break;
      }
      case tree_v1_genome::kBarkPlated: {
        float plate = noise(v3(c * 2.2f, s * 2.4f, sn * 2.2f), seed + 2);
        shade = plate > 0.5f ? 0.78f : 0.32f;
        relief = (plate > 0.5f ? 0.04f : -0.03f) * strength;
        break;
      }
      case tree_v1_genome::kBarkShaggy: {
        float strip = noise(v3(c * 3.5f, s * 0.45f, sn * 3.5f), seed + 3);
        shade = strip > 0.55f ? 0.85f : (strip > 0.4f ? 0.5f : 0.2f);
        relief = 0.09f * (strip - 0.5f) * strength;
        break;
      }
      case tree_v1_genome::kBarkMottled: {
        float patch = noise(v3(c * 1.6f, s * 0.9f, sn * 1.6f), seed + 4);
        shade = patch > 0.58f ? 0.85f : (patch < 0.38f ? 0.25f : 0.55f);
        break;
      }
      case tree_v1_genome::kBarkRinged: {
        // A palm's leaf scars: a dark ring every 0.22 m up the trunk.
        float ring = foundry::det::cos(s * (2 * kPi / 0.22f));
        shade = ring > 0.55f ? 0.18f : 0.62f + 0.12f * (n - 0.5f);
        relief = (ring > 0.55f ? -0.025f : 0.015f) * strength;
        break;
      }
      case tree_v1_genome::kBarkPapery: {
        // A birch: pale, with dark lenticel dashes running around the trunk and darker patches low down.
        float dash = noise(v3(c * 2.5f, s * 7.0f, sn * 2.5f), seed + 6);
        float patch = noise(v3(c * 1.2f, s * 0.8f, sn * 1.2f), seed + 7);
        shade = dash > 0.68f ? 0.05f : 0.95f;
        if (patch > 0.72f - 0.04f * (s < 1.5f ? 1.5f - s : 0.0f)) shade = 0.08f;
        break;
      }
      case tree_v1_genome::kBarkFluted: {
        float lobe = foundry::det::cos(theta * 5 + s * 0.7f + 2 * (noise(v3(0, s * 0.5f, 0), seed + 5) - 0.5f));
        shade = 0.35f + 0.4f * (0.5f + 0.5f * lobe) + 0.15f * (n - 0.5f);
        relief = 0.13f * lobe * strength;
        break;
      }
      default:
        shade = 0.5f + 0.25f * (n - 0.5f);
        break;
    }
  }
};

struct Ring {
  std::vector<uint32_t> v;
};

// Joins two rings of the same size with quads (outward winding).
void join(Builder &bd, const Ring &a, const Ring &b) {
  const size_t n = a.v.size();
  for (size_t j = 0; j < n; ++j) {
    size_t k = (j + 1) % n;
    bd.tri(a.v[j], b.v[j], b.v[k]);
    bd.tri(a.v[j], b.v[k], a.v[k]);
  }
}

// A flat cap over a ring's positions: a fan around a centre, its own vertices.
void cap(Builder &bd, const Ring &ring, vec3 centre, vec3 normal, uint32_t bone, uint32_t region, float shade) {
  const auto &P = bd.out.positions;
  uint32_t c = bd.vertex(centre, normal, region, shade, normal.y, bone);
  std::vector<uint32_t> rim;
  for (uint32_t id : ring.v) {
    vec3 p = P[id];
    float d = length(p - centre);
    rim.push_back(bd.vertex(p, normal, region, clampf(0.35f + d * 2.0f, 0, 1), normal.y, bone));
  }
  const size_t n = rim.size();
  for (size_t j = 0; j < n; ++j) {
    size_t k = (j + 1) % n;
    if (normal.y >= 0) bd.tri(c, rim[k], rim[j]);
    else bd.tri(c, rim[j], rim[k]);
  }
}

struct Frame {
  vec3 t, n, b;
};

std::vector<Frame> frames(const std::vector<vec3> &p) {
  std::vector<Frame> f(p.size());
  vec3 prev_n = v3(0, 0, 0);
  for (size_t k = 0; k < p.size(); ++k) {
    vec3 t;
    if (k == 0) t = p[1] - p[0];
    else if (k + 1 == p.size()) t = p[k] - p[k - 1];
    else t = normalize(p[k + 1] - p[k]) + normalize(p[k] - p[k - 1]);
    t = normalize(t);
    vec3 n = k == 0 ? perpendicular(t) : prev_n - t * dot(prev_n, t);
    if (length(n) < 1e-4f) n = perpendicular(t);
    n = normalize(n);
    f[k] = {t, n, cross(t, n)};
    prev_n = n;
  }
  return f;
}

// ---- foliage ---------------------------------------------------------------------

struct Icosphere {
  std::vector<vec3> v;
  std::vector<uint32_t> i;
};

// An icosahedron subdivided this many times; -1 gives an octahedron (the fewest faces that still
// read as a lump, for the lowest level of detail).
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
    std::vector<std::pair<uint64_t, uint32_t>> mids;  // edge key -> vertex, searched linearly (small)
    auto mid = [&](uint32_t a, uint32_t b) {
      uint64_t key = a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
      for (const auto &m : mids)
        if (m.first == key) return m.second;
      s.v.push_back(normalize(s.v[a] + s.v[b]));
      uint32_t id = static_cast<uint32_t>(s.v.size() - 1);
      mids.push_back({key, id});
      return id;
    };
    for (size_t k = 0; k < s.i.size(); k += 3) {
      uint32_t a = s.i[k], b = s.i[k + 1], c = s.i[k + 2];
      uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
      uint32_t tris[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
      next.insert(next.end(), tris, tris + 12);
    }
    s.i = next;
  }
  return s;
}

// Adds a displaced sphere, with normals from its faces. `shape` maps a unit
// direction to a local offset (stretch and squash), in the clump's frame.
void blob(Builder &bd, const Icosphere &ico, vec3 centre, float radius, vec3 fwd, vec3 scale, float bump, float freq,
          uint32_t seed, uint32_t region, float shade, uint32_t bone) {
  const vec3 f = normalize(fwd);
  const vec3 side = perpendicular(f);
  const vec3 up = cross(f, side);
  const uint32_t first = static_cast<uint32_t>(bd.out.positions.size());
  std::vector<vec3> pos(ico.v.size());
  for (size_t k = 0; k < ico.v.size(); ++k) {
    vec3 d = ico.v[k];
    float n = noise(d * freq + v3(static_cast<float>(seed % 97), static_cast<float>(seed % 89), 0), seed);
    float r = radius * (1 + bump * (n - 0.5f) * 2);
    vec3 local = mul(d, scale) * r;
    pos[k] = centre + side * local.x + up * local.y + f * local.z;
  }
  // Face normals summed per vertex.
  std::vector<vec3> nrm(ico.v.size(), v3(0, 0, 0));
  for (size_t k = 0; k < ico.i.size(); k += 3) {
    vec3 a = pos[ico.i[k]], b = pos[ico.i[k + 1]], c = pos[ico.i[k + 2]];
    vec3 fn = cross(b - a, c - a);
    for (int j = 0; j < 3; ++j) nrm[ico.i[k + j]] = nrm[ico.i[k + j]] + fn;
  }
  for (size_t k = 0; k < ico.v.size(); ++k) {
    vec3 n = normalize(nrm[k]);
    if (dot(n, pos[k] - centre) < 0) n = -n;
    float exposure = clampf(n.y * 1.2f, 0, 1);
    bd.vertex(pos[k], n, region, shade, exposure, bone);
  }
  for (size_t k = 0; k < ico.i.size(); k += 3) {
    vec3 a = pos[ico.i[k]], b = pos[ico.i[k + 1]], c = pos[ico.i[k + 2]];
    // Keep winding outward after displacement.
    if (dot(cross(b - a, c - a), (a + b + c) * (1.0f / 3) - centre) >= 0)
      bd.tri(first + ico.i[k], first + ico.i[k + 1], first + ico.i[k + 2]);
    else
      bd.tri(first + ico.i[k], first + ico.i[k + 2], first + ico.i[k + 1]);
  }
}

}  // namespace

bool run(const channels::tree_v1_rig &in, channels::tree_v1_body &out, std::string &error) {
  const tree_v1_spec &spec = in.spec;
  const tree_v1_genome &g = spec.genome;
  if (in.branches.empty() || in.bones.empty()) {
    error = "the rig has no branches or bones";
    return false;
  }
  for (const auto &b : in.branches) {
    if (b.points.size() < 2 || b.radii.size() != b.points.size() || b.bones.size() != b.points.size()) {
      error = "a branch needs at least 2 points with a radius and bone each";
      return false;
    }
    for (uint32_t id : b.bones)
      if (id >= in.bones.size()) {
        error = "a branch point names a bone that doesn't exist";
        return false;
      }
  }
  out.spec = spec;
  out.height = in.height;
  out.stump_radius = in.stump_radius;
  out.footprint_radius = in.footprint_radius;
  out.footprint_cells = in.footprint_cells;
  out.crown_base = in.crown_base;
  out.bones = in.bones;
  out.pieces = in.pieces;
  const bool stump = spec.stump;
  Builder bd(out, in.bones);

  const uint32_t detail = spec.detail > 2 ? 2 : spec.detail;
  static const int kSides[3][4] = {{12, 8, 5, 4}, {9, 6, 4, 3}, {7, 4, 3, 3}};
  const uint32_t name_seed = hash_str(g.name.c_str()) ^ hash_u32(spec.seed);
  const Bark bark{g.bark, name_seed};
  auto piece_of = [&](uint32_t bone) { return in.bones[bone].piece; };

  // ---- bark ----
  for (size_t bi = 0; bi < in.branches.size(); ++bi) {
    const auto &br = in.branches[bi];
    const int level = static_cast<int>(br.level > 3 ? 3 : br.level);
    if (detail == 2 && level == 3) continue;  // sprigs vanish into the crown at low detail
    const int sides = kSides[detail][level];
    const uint32_t region = br.dead ? kDeadWood : kBark;
    std::vector<vec3> P = br.points;
    std::vector<float> R = br.radii;
    std::vector<uint32_t> B = br.bones;
    // Fine bark patterns (rings, lenticels) need rings closer than the rig's points: split
    // each segment of the stem and limbs (never the stump's first, which holds the cut).
    const bool fine = (g.bark == tree_v1_genome::kBarkRinged || g.bark == tree_v1_genome::kBarkPapery) && level <= 1 &&
                      detail < 2;
    if (fine) {
      const float want = g.bark == tree_v1_genome::kBarkRinged ? 0.11f : 0.25f;
      std::vector<vec3> P2;
      std::vector<float> R2;
      std::vector<uint32_t> B2;
      for (size_t k = 0; k + 1 < P.size(); ++k) {
        P2.push_back(P[k]);
        R2.push_back(R[k]);
        B2.push_back(B[k]);
        if (bi == 0 && k == 0) continue;
        const int split = static_cast<int>(length(P[k + 1] - P[k]) / want);
        const int n = split < 1 ? 1 : (split > 12 ? 12 : split);
        for (int j = 1; j < n; ++j) {
          const float u = static_cast<float>(j) / static_cast<float>(n);
          P2.push_back(lerp(P[k], P[k + 1], u));
          R2.push_back(mixf(R[k], R[k + 1], u));
          B2.push_back(B[k + 1]);
        }
      }
      P2.push_back(P.back());
      R2.push_back(R.back());
      B2.push_back(B.back());
      P.swap(P2);
      R.swap(R2);
      B.swap(B2);
    }
    std::vector<float> S(P.size(), 0.0f);
    for (size_t k = 1; k < P.size(); ++k) S[k] = S[k - 1] + length(P[k] - P[k - 1]);
    std::vector<Frame> F = frames(P);

    auto make_ring = [&](size_t k, vec3 centre, float radius, float flare, uint32_t bone_a, uint32_t bone_b,
                         float w) {
      Ring ring;
      const Frame &fr = F[k];
      for (int j = 0; j < sides; ++j) {
        float theta = 2 * kPi * static_cast<float>(j) / static_cast<float>(sides);
        vec3 dir = fr.n * foundry::det::cos(theta) + fr.b * foundry::det::sin(theta);
        float relief, shade;
        bark.at(theta, level == 0 ? centre.y : S[k], level, relief, shade);
        float lobes = 0;
        if (flare > 0) {
          float c5 = foundry::det::cos(theta * 5 + static_cast<float>(name_seed % 7));
          lobes = c5 > 0 ? c5 * c5 * c5 : 0;
        }
        float r = radius * (1 + relief) * (1 + flare * (0.6f + 0.6f * lobes));
        vec3 p = centre + dir * r;
        ring.v.push_back(bd.vertex(p, dir, region, shade, dir.y, bone_a, bone_b, w));
      }
      return ring;
    };

    if (bi == 0) {
      // The stump: sunk a little below the ground, flared, up to the cut.
      const float cut = in.cut;
      const vec3 base = P[0], top = P[1];
      const float fl = g.flare * (spec.age == tree_v1_spec::kAgeSapling ? 0.3f : 1.0f);
      const float ys[4] = {-0.05f, 0.12f, 0.4f, 1.0f};
      Ring prev;
      for (int k = 0; k < 4; ++k) {
        float y = ys[k] * cut;
        vec3 c = lerp(base, top, clampf(y / cut, 0, 1));
        c.y = y;
        float t = clampf(y / cut, 0, 1);
        float falloff = (1 - t) * (1 - t);
        Ring ring = make_ring(k < 2 ? 0 : 1, c, mixf(R[0], R[1], t), fl * falloff, 0, 0, 0);
        if (k > 0) join(bd, prev, ring);
        prev = ring;
      }
      cap(bd, prev, top, v3(0, 1, 0), 0, kWood, 0.5f);
      if (stump) continue;  // a felled tree keeps only its stump
      // The log: from the cut up, with its own bottom cap.
      Ring first = make_ring(1, P[1], R[1], 0, B[1], B[1], 0);
      cap(bd, first, P[1], v3(0, -1, 0), B[1], kWood, 0.5f);
      prev = first;
      for (size_t k = 2; k < P.size(); ++k) {
        bool start = B[k] != B[k - 1] && piece_of(B[k]) == piece_of(B[k - 1]);
        Ring ring = make_ring(k, P[k], R[k], 0, B[k], B[k - 1], start ? 0.5f : 0);
        join(bd, prev, ring);
        prev = ring;
      }
      uint32_t tip = bd.vertex(P.back() + F.back().t * R.back(), F.back().t, region, 0.5f, F.back().t.y, B.back());
      for (size_t j = 0; j < prev.v.size(); ++j) bd.tri(prev.v[j], tip, prev.v[(j + 1) % prev.v.size()]);
      continue;
    }

    if (stump) continue;  // limbs went with the log
    Ring prev;
    for (size_t k = 0; k < P.size(); ++k) {
      bool start = k > 0 && B[k] != B[k - 1] && piece_of(B[k]) == piece_of(B[k - 1]);
      // The branch base sinks into its parent so the joint has no gap.
      vec3 c = P[k];
      if (k == 0) c = P[0] - F[0].t * (R[0] * 0.8f);
      Ring ring = make_ring(k, c, R[k], 0, B[k], k > 0 ? B[k - 1] : B[k], start ? 0.5f : 0);
      if (k > 0) join(bd, prev, ring);
      prev = ring;
    }
    if (br.dead) {
      cap(bd, prev, P.back(), F.back().t, B.back(), kWood, 0.6f);  // a broken end shows wood
    } else {
      uint32_t tip = bd.vertex(P.back() + F.back().t * (R.back() * 1.5f), F.back().t, region, 0.5f, F.back().t.y, B.back());
      for (size_t j = 0; j < prev.v.size(); ++j) bd.tri(prev.v[j], tip, prev.v[(j + 1) % prev.v.size()]);
    }
  }

  // ---- foliage, by season (none on a stump) ----
  if (!stump) {
  const bool deciduous = g.deciduous;
  const uint32_t season = spec.season;
  const Icosphere ico = icosphere(detail == 0 ? 1 : (detail == 1 ? 0 : -1));
  const Icosphere fruit_ico = icosphere(detail == 0 ? 1 : 0);
  const size_t keep_den = 1;
  const size_t keep_num = 1;
  const float lod_grow = detail == 2 ? 1.15f : 1.0f;
  float fruit_scale = spec.age == tree_v1_spec::kAgeSapling ? 0.0f : (spec.age == tree_v1_spec::kAgeYoung ? 0.5f : 1.0f);
  if (g.fruit_seasons.size() != 4) {
    error = "genome needs 4 fruit_seasons values";
    return false;
  }
  const float ripeness = g.fruit_seasons[season];
  // Fruit hangs in the seasons it bears; pickable fruit only while the request says it's still on.
  const bool fruiting = g.fruit > 0 && fruit_scale > 0 && ripeness > 0 && (!g.pickable || spec.fruit);
  const float fruit_grow = 0.8f + 0.2f * ripeness;  // unripe fruit is a little smaller
  const bool fronds = g.foliage == tree_v1_genome::kFoliageFronds;
  if (fronds) {
    // Pinnate fronds: leaflet pairs along each first-order limb, drooping to both sides.
    const int stations = detail == 0 ? 22 : (detail == 1 ? 14 : 8);
    for (size_t bi = 1; bi < in.branches.size(); ++bi) {
      const auto &br = in.branches[bi];
      if (br.level != 1 || br.dead) continue;
      const std::vector<vec3> &P = br.points;
      std::vector<float> S(P.size(), 0.0f);
      for (size_t k = 1; k < P.size(); ++k) S[k] = S[k - 1] + length(P[k] - P[k - 1]);
      const float L = S.back();
      if (L <= 1e-3f) continue;
      const float shade = static_cast<float>(hash_u32(name_seed ^ static_cast<uint32_t>(bi) * 2246822519u) >> 8) / 16777216.0f;
      uint32_t region = kLeaf;
      if (deciduous && season == tree_v1_spec::kSeasonWinter) {
        // Deciduous fronds (a willow's weeping strands) drop in winter; winter_keep of them hang on, dead.
        if (shade >= g.winter_keep) continue;
        region = kDeadLeaf;
      }
      for (int st = 0; st < stations; ++st) {
        // The first quarter is a bare stalk (the petiole), where the fruit hangs in view.
        const float u = 0.24f + 0.74f * (static_cast<float>(st) + 0.5f) / static_cast<float>(stations);
        const float want = u * L;
        size_t k = 0;
        while (k + 2 < P.size() && S[k + 1] < want) ++k;
        const float seg = S[k + 1] - S[k];
        const float f = seg > 1e-6f ? (want - S[k]) / seg : 0;
        const vec3 p = lerp(P[k], P[k + 1], clampf(f, 0, 1));
        const vec3 t = normalize(P[k + 1] - P[k]);
        vec3 side = cross(t, v3(0, 1, 0));
        side = length(side) > 1e-3f ? normalize(side) : perpendicular(t);
        const uint32_t bone = br.bones[k + 1];
        const float len = L * 0.3f * foundry::det::sqrt(foundry::det::sin(kPi * (u - 0.24f) / 0.76f)) + 0.05f;
        const float width = len * 0.16f;
        for (int sd = -1; sd <= 1; sd += 2) {
          // Out to the side, drooping, swept a little toward the tip.
          const vec3 dir = normalize(side * (0.82f * static_cast<float>(sd)) + v3(0, -0.5f, 0) + t * 0.45f);
          vec3 nrm = normalize(cross(dir, t));
          if (nrm.y < 0) nrm = -nrm;
          const vec3 tip = p + dir * len;
          const vec3 mid = p + dir * (len * 0.4f);
          const vec3 across = normalize(cross(nrm, dir)) * (width * 0.5f);
          const float up = clampf(nrm.y * 1.2f, 0, 1);
          uint32_t a = bd.vertex(p, nrm, region, shade, up, bone);
          uint32_t b = bd.vertex(mid + across, nrm, region, shade, up, bone);
          uint32_t c = bd.vertex(tip, nrm, region, shade, up, bone);
          uint32_t d = bd.vertex(mid - across, nrm, region, shade, up, bone);
          bd.tri(a, b, c);
          bd.tri(a, c, d);
        }
      }
    }
    // Fruit: a cluster under the crown, where the fronds meet the trunk.
    if (fruiting) {
      const auto &stem = in.branches[0];
      float lowest = stem.points.back().y;
      for (size_t bi = 1; bi < in.branches.size(); ++bi)
        if (in.branches[bi].level == 1) lowest = minf(lowest, in.branches[bi].points[0].y);
      size_t at = stem.points.size() - 1;
      while (at > 1 && stem.points[at - 1].y > lowest) --at;
      const vec3 top = stem.points[at];
      const float trunk_r = stem.radii[at];
      // The mean frond length: the cluster hangs below the frond bases by a share of it, under the crown.
      float frond = 0;
      int fronds_n = 0;
      for (size_t bi = 1; bi < in.branches.size(); ++bi) {
        const auto &br = in.branches[bi];
        if (br.level != 1) continue;
        for (size_t k = 1; k < br.points.size(); ++k) frond += length(br.points[k] - br.points[k - 1]);
        ++fronds_n;
      }
      frond = fronds_n > 0 ? frond / static_cast<float>(fronds_n) : 1.0f;
      const int count = static_cast<int>(g.fruit * 4 * fruit_scale + 0.5f);
      Rng rng(name_seed, 11u);
      for (int j = 0; j < count; ++j) {
        const float az = 2 * kPi * (static_cast<float>(j) + 0.4f * rng.uniform()) / static_cast<float>(count > 0 ? count : 1);
        // Against the trunk, below the leaflets that hang from the frond bases, so it shows from the side.
        const float drop = frond * (0.22f + 0.1f * rng.uniform());
        const float fr = g.fruit_size * (0.85f + 0.3f * rng.uniform()) * fruit_grow;
        const float out_r = trunk_r + fr * 1.1f;
        const vec3 c = top + v3(foundry::det::cos(az) * out_r, -drop - fr, foundry::det::sin(az) * out_r);
        blob(bd, fruit_ico, c, fr, v3(0, 0, 1), v3(1, 1.1f, 1), 0.04f, 1.5f, name_seed + static_cast<uint32_t>(j), kFruit,
             rng.uniform(), stem.bones[at]);
      }
    }
  }
  for (size_t k = 0; k < in.foliage.size() && !fronds; ++k) {
    if (keep_den > 1 && (k * keep_num) / keep_den == ((k + 1) * keep_num) / keep_den) continue;
    const auto &f = in.foliage[k];
    if (f.bone >= in.bones.size()) {
      error = "a foliage anchor names a bone that doesn't exist";
      return false;
    }
    const float r1 = f.random;
    const float r2 = static_cast<float>(hash_u32(name_seed ^ static_cast<uint32_t>(k) * 2654435761u) >> 8) / 16777216.0f;
    uint32_t region = kLeaf;
    float size = 1.0f;
    bool show = true;
    if (!deciduous) {
      if (season == tree_v1_spec::kSeasonSpring && r1 < g.blossom) region = kBlossom;
    } else if (season == tree_v1_spec::kSeasonWinter) {
      show = r1 < g.winter_keep;
      region = kDeadLeaf;
      size = 0.7f;
    } else if (season == tree_v1_spec::kSeasonSpring) {
      if (r1 < g.blossom) {
        region = kBlossom;
        size = 0.9f;
      } else if (g.blossom > 0) {
        show = r2 < g.blossom_leaf;
        size = 0.8f;
      } else {
        size = 0.85f;
      }
    } else if (season == tree_v1_spec::kSeasonFall) {
      show = r1 >= g.fall_drop;
    }
    if (!show && !fruiting) continue;
    const float radius = f.radius * size * lod_grow;
    const uint32_t seed = name_seed ^ hash_u32(static_cast<uint32_t>(k) + 17u);
    // The colour variety per clump rides on shade.
    const float shade = r2;
    switch (show ? g.foliage : ~0u) {
      case ~0u:  // bare: only fruit hangs here
        break;
      case tree_v1_genome::kFoliageNeedles:
        blob(bd, ico, f.position, radius, f.normal, v3(1.0f, 0.62f, 1.35f), 0.32f, 3.6f, seed, region, shade, f.bone);
        break;
      case tree_v1_genome::kFoliageRosettes:
        blob(bd, ico, f.position, radius * 1.1f, v3(0, 0, 1) + f.normal * 0.3f, v3(1.15f, 0.48f, 1.15f), 0.28f, 3.0f,
             seed, region, shade, f.bone);
        break;
      default:
        blob(bd, ico, f.position, radius, f.normal, v3(1, 0.9f, 1), 0.24f, 2.2f, seed, region, shade, f.bone);
        break;
    }
    // Fruit on the clumps, or on the bare twig where the clump has dropped (a baobab's pods in the dry season).
    if (fruiting && region != kBlossom) {
      float want = g.fruit * fruit_scale;
      int count = static_cast<int>(want);
      if (r2 < want - static_cast<float>(count)) count++;
      Rng rng(seed, 7u);
      for (int j = 0; j < count; ++j) {
        // On the clump's outer lower side (out from the trunk), so it hangs where it can be seen.
        // One draw per statement: argument order is unspecified, and GCC and Clang differ.
        float dx = rng.signed_unit();
        float dy = -0.4f - 0.5f * rng.uniform();
        float dz = rng.signed_unit();
        vec3 out = v3(f.position.x, 0, f.position.z);
        out = length(out) > 1e-3f ? normalize(out) : f.normal;
        vec3 d = normalize(out * 1.2f + f.normal + v3(dx, dy, dz));
        float fr = g.fruit_size * (0.85f + 0.3f * rng.uniform()) * fruit_grow;
        // Past the clump's bumps (up to a quarter of its radius), so the fruit sits on its surface;
        // a bare twig carries it close in.
        vec3 c = f.position + d * (show ? radius * 1.1f + fr * 0.5f : radius * 0.3f);
        blob(bd, fruit_ico, c, fr, v3(0, 0, 1), v3(1, 0.95f, 1), 0.04f, 1.5f, seed + static_cast<uint32_t>(j), kFruit,
             rng.uniform(), f.bone);
      }
    }
  }

  }  // foliage

  // ---- roots and moss ----
  static const float kGrown[4] = {0.0f, 0.35f, 1.0f, 1.35f};  // how far along knees and roots are, by age
  const auto &stem = in.branches[0];
  const float H = in.height;
  const float trunk_r = stem.radii[1];
  auto stem_at = [&](float y, float &r) {
    // The stem's centre and radius at height y.
    for (size_t k = 0; k + 1 < stem.points.size(); ++k) {
      if (stem.points[k + 1].y >= y || k + 2 == stem.points.size()) {
        const float span = stem.points[k + 1].y - stem.points[k].y;
        const float u = span > 1e-5f ? clampf((y - stem.points[k].y) / span, 0, 1) : 0;
        r = mixf(stem.radii[k], stem.radii[k + 1], u);
        return lerp(stem.points[k], stem.points[k + 1], u);
      }
    }
    r = stem.radii.back();
    return stem.points.back();
  };
  // A tapered tube along a path, rigid on one bone, with a rounded tip.
  auto tube = [&](const std::vector<vec3> &path, float r0, float r1, int sides, uint32_t bone) {
    const std::vector<Frame> F = frames(path);
    Ring prev;
    float s = 0;
    for (size_t k = 0; k < path.size(); ++k) {
      if (k > 0) s += length(path[k] - path[k - 1]);
      const float u = static_cast<float>(k) / static_cast<float>(path.size() - 1);
      Ring ring;
      for (int j = 0; j < sides; ++j) {
        const float theta = 2 * kPi * static_cast<float>(j) / static_cast<float>(sides);
        const vec3 dir = F[k].n * foundry::det::cos(theta) + F[k].b * foundry::det::sin(theta);
        float relief, shade;
        bark.at(theta, s, 1, relief, shade);
        ring.v.push_back(bd.vertex(path[k] + dir * (mixf(r0, r1, u) * (1 + relief)), dir, kBark, shade, dir.y, bone));
      }
      if (k > 0) join(bd, prev, ring);
      prev = ring;
    }
    const uint32_t tip = bd.vertex(path.back() + F.back().t * r1, F.back().t, kBark, 0.5f, F.back().t.y, bone);
    for (size_t j = 0; j < prev.v.size(); ++j) bd.tri(prev.v[j], tip, prev.v[(j + 1) % prev.v.size()]);
  };
  const float grown = kGrown[spec.age > 3 ? 3 : spec.age];
  const int root_sides = detail == 0 ? 6 : (detail == 1 ? 5 : 4);
  // Cypress knees: blunt woody cones standing up from the roots around the trunk.
  const int knees = static_cast<int>(static_cast<float>(g.knees) * grown + 0.5f);
  if (knees > 0) {
    Rng rng(name_seed, 23u);
    const float inner = trunk_r * 1.8f, spread = 0.16f * H;
    for (int j = 0; j < knees; ++j) {
      const float az = 2 * kPi * (static_cast<float>(j) + 0.8f * rng.uniform()) / static_cast<float>(knees);
      const float d = inner + spread * rng.uniform();
      const float h = (0.3f + 0.5f * rng.uniform()) * clampf(H / 10, 0.5f, 1.4f) * (0.6f + 0.4f * grown);
      const vec3 base = v3(foundry::det::cos(az) * d, -0.05f, foundry::det::sin(az) * d);
      const float lean_x = rng.signed_unit() * 0.12f;
      const float lean_z = rng.signed_unit() * 0.12f;
      const vec3 top = base + v3(lean_x * h, h + 0.05f, lean_z * h);
      tube({base, lerp(base, top, 0.55f), top}, h * 0.38f, h * 0.16f, root_sides, 0);
    }
  }
  // Prop roots: arching from the lower trunk out and down into the ground.
  const int roots = static_cast<int>(static_cast<float>(g.prop_roots) * minf(grown, 1.0f) + 0.5f);
  if (roots > 0) {
    Rng rng(name_seed, 29u);
    const int segs = detail == 0 ? 8 : 6;
    for (int j = 0; j < roots; ++j) {
      const float az = 2 * kPi * (static_cast<float>(j) + 0.6f * rng.uniform()) / static_cast<float>(roots);
      const float y0 = H * (0.06f + 0.2f * rng.uniform());
      float r_at = trunk_r;
      const vec3 c = stem_at(y0, r_at);
      const vec3 out_dir = v3(foundry::det::cos(az), 0, foundry::det::sin(az));
      const vec3 a = c + out_dir * (r_at * 0.6f);
      const float reach = y0 * (1.1f + 0.8f * rng.uniform()) + r_at;
      const vec3 b = c + out_dir * reach;
      const vec3 end = v3(b.x, -0.08f, b.z);
      const vec3 ctrl = a + out_dir * (reach * 0.55f) + v3(0, y0 * 0.25f, 0);
      std::vector<vec3> path;
      for (int k = 0; k <= segs; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(segs);
        path.push_back(a * ((1 - u) * (1 - u)) + ctrl * (2 * u * (1 - u)) + end * (u * u));
      }
      const float rr = maxf(r_at * 0.3f, 0.04f);  // chunky enough to read (style.md section 3)
      tube(path, rr, rr * 0.7f, root_sides, 0);
    }
  }
  // Spanish moss: grey strands hanging from the limbs and twigs, in every season.
  if (g.hanging_moss > 0 && spec.age != tree_v1_spec::kAgeSapling && !stump) {
    Rng rng(name_seed, 31u);
    const float spacing = detail == 0 ? 0.45f : (detail == 1 ? 0.65f : 0.9f);
    const float len_scale = clampf(H / 10, 0.5f, 1.3f) * (0.6f + 0.6f * g.hanging_moss);
    for (size_t bi = 1; bi < in.branches.size(); ++bi) {
      const auto &br = in.branches[bi];
      if (br.level > 2) continue;
      const std::vector<vec3> &P = br.points;
      float carry = rng.uniform() * spacing;
      for (size_t k = 0; k + 1 < P.size(); ++k) {
        const vec3 d = P[k + 1] - P[k];
        const float seg = length(d);
        if (seg < 1e-4f) continue;
        const vec3 t = d * (1.0f / seg);
        float at = carry;
        for (; at < seg; at += spacing) {
          const float pick = rng.uniform();
          const float lr = rng.uniform();
          const float wr = rng.uniform();
          if (pick >= g.hanging_moss || t.y > 0.75f) continue;  // hangs from the near-level wood
          const vec3 top = P[k] + t * at - v3(0, br.radii[k] * 0.8f, 0);
          const float L = (0.35f + 0.75f * lr) * len_scale;
          const float w = L * (0.22f + 0.1f * wr);
          const uint32_t bone = br.bones[k + 1];
          const float shade = rng.uniform();
          // Two crossed ribbons that narrow to a point, swaying a little sideways as they fall.
          for (int side = 0; side < 2; ++side) {
            const vec3 across = side == 0 ? v3(w * 0.5f, 0, 0) : v3(0, 0, w * 0.5f);
            const vec3 nrm = side == 0 ? v3(0, 0, 1) : v3(1, 0, 0);
            const vec3 mid = top + v3(across.z * 0.6f, -L * 0.55f, across.x * 0.6f);
            const vec3 tip = top + v3(-across.z * 0.4f, -L, -across.x * 0.4f);
            const uint32_t a = bd.vertex(top - across, nrm, kHangingMoss, shade, 0.2f, bone);
            const uint32_t b = bd.vertex(top + across, nrm, kHangingMoss, shade, 0.2f, bone);
            const uint32_t c = bd.vertex(mid + across * 0.7f, nrm, kHangingMoss, shade, 0.1f, bone);
            const uint32_t e = bd.vertex(mid - across * 0.7f, nrm, kHangingMoss, shade, 0.1f, bone);
            const uint32_t f = bd.vertex(tip, nrm, kHangingMoss, shade, 0.0f, bone);
            bd.tri(a, b, c);
            bd.tri(a, c, e);
            bd.tri(e, c, f);
          }
        }
        carry = at - seg;  // the distance still to go to the next strand, on the next segment
      }
    }
  }

  // ---- collision ----
  // A solid vertical cylinder from the ground: the trunk up to the crown, or the stump up to the cut.
  out.collision.clear();
  {
    const float top_y = stump ? in.cut : maxf(in.crown_base, in.cut);
    channels::physics_v1_shape cyl;
    cyl.type = channels::physics_v1_shape::kTypeCylinder;
    cyl.position = v3(0, top_y * 0.5f, 0);
    cyl.rotation = v3(0, 0, 0);
    cyl.radius = in.stump_radius;
    cyl.half_height = top_y * 0.5f;
    channels::physics_v1_body body;
    body.purpose = channels::physics_v1_body::kPurposeSolid;
    body.shapes.push_back(cyl);
    out.collision.push_back(body);
  }
  return true;
}

}  // namespace tree_mesh_v1

FOUNDRY_SYSTEM(tree_mesh_v1, channels::tree_v1_rig, channels::tree_v1_body)
