// bush_grow_v1: grows a shrub from its genome, age, and season (design/systems.md, Shrubs and ground cover).
//
// A light version of the tree pipeline in one system:
// 1. Stems: several from a small patch of ground, leaning out and arching by
//    the species, with side branches sized by the outline (round, upright,
//    spreading) and twigs; or, for a fan shrub (a palmetto), bare stalks
//    ending in fan leaves.
// 2. Wind bones: a static root, two per stem, one per side branch.
// 3. Foliage: displaced icosphere clumps (broadleaf) or stretched tufts
//    (needles) along the outer wood, by season: spring leaves, summer, fall
//    colour and drop, an evergreen's winter or a deciduous shrub's last dead
//    leaves; flower clusters on a share of the clumps by season.
// 4. Berries: clusters hanging just outside the clumps (or, on a fan shrub,
//    from the stalks), unripe to ripe through the species' fruiting seasons.
//    Berries that can be picked show only while the request says the shrub
//    still carries them; seed balls and capsules always show.
// 5. Paint: flat palette regions (style.md section 5), and snow on upward
//    surfaces in winter.
#include <vector>

#include "boundary.h"
#include "channels/bush_v1_model.h"
#include "channels/bush_v1_spec.h"
#include "math.h"
#include "packer/bush_v1_model.h"
#include "packer/bush_v1_spec.h"

namespace bush_grow_v1 {

using namespace bush_math;
using channels::bush_v1_genome;
using channels::bush_v1_spec;

namespace {

constexpr int kMaxBones = 128;
constexpr int kMaxFoliage = 220;
constexpr float kChunk = 1.5f;  // cel thickening (style.md section 3)
const vec3 kSnow = {0.90f, 0.93f, 0.97f};

vec3 mixv(vec3 a, vec3 b, float t) { return a + (b - a) * t; }

uint32_t pack(vec3 c) {
  auto byte = [](float v) {
    v = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(v);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

struct Skin {
  uint32_t a = 0, b = 0;
  float w = 0;
};

struct Wood {
  std::vector<vec3> p;
  std::vector<float> r;
  std::vector<Skin> skin;
  int level = 0;  // 0 stem, 1 branch, 2 twig
  int children = 0;
};

struct Clump {
  vec3 position, normal;
  float radius;
  uint32_t bone;
  float r1, r2;  // stable randoms for per-clump choices
};

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
    std::vector<std::pair<uint64_t, uint32_t>> mids;
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

struct Builder {
  channels::bush_v1_model &out;
  bool winter;
  float snow_from;  // up-facing surfaces beyond this take snow (above 1: none)
  uint32_t vertex(vec3 p, vec3 n, vec3 c, Skin s, bool can_snow = true) {
    if (winter && can_snow && n.y > snow_from) c = kSnow;
    out.positions.push_back(p);
    out.normals.push_back(n);
    out.colors.push_back(pack(c));
    out.bone_a.push_back(s.a);
    out.bone_b.push_back(s.w > 0 ? s.b : s.a);
    out.bone_w.push_back(s.w);
    return static_cast<uint32_t>(out.positions.size() - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  }
  // A tapered tube along a branch, closed at the tip.
  void tube(const Wood &w, int sides, vec3 light, vec3 dark, uint32_t seed) {
    const size_t n = w.p.size();
    vec3 prev_n = v3(0, 0, 0);
    std::vector<std::vector<uint32_t>> rings;
    vec3 last_t = v3(0, 1, 0);
    for (size_t k = 0; k < n; ++k) {
      vec3 t = k + 1 < n ? w.p[k + 1] - w.p[k] : w.p[k] - w.p[k - 1];
      if (k > 0 && k + 1 < n) t = normalize(w.p[k + 1] - w.p[k]) + normalize(w.p[k] - w.p[k - 1]);
      t = normalize(t);
      last_t = t;
      vec3 nn = k == 0 ? perpendicular(t) : prev_n - t * dot(prev_n, t);
      if (length(nn) < 1e-4f) nn = perpendicular(t);
      nn = normalize(nn);
      prev_n = nn;
      const vec3 b = cross(t, nn);
      // The base sinks into its parent so the joint has no gap.
      const vec3 c = k == 0 && w.level > 0 ? w.p[0] - t * (w.r[0] * 0.8f) : w.p[k];
      std::vector<uint32_t> ring;
      for (int j = 0; j < sides; ++j) {
        const float th = 2 * kPi * static_cast<float>(j) / static_cast<float>(sides);
        const vec3 d = nn * foundry::det::cos(th) + b * foundry::det::sin(th);
        const float shade = noise(v3(foundry::det::cos(th) * 1.3f, static_cast<float>(k) * 0.7f, foundry::det::sin(th) * 1.3f), seed);
        ring.push_back(vertex(c + d * w.r[k], d, mixv(dark, light, 0.2f + 0.6f * shade), w.skin[k]));
      }
      rings.push_back(ring);
    }
    for (size_t k = 0; k + 1 < n; ++k)
      for (int j = 0; j < sides; ++j) {
        const size_t a = static_cast<size_t>(j), bb = static_cast<size_t>((j + 1) % sides);
        tri(rings[k][a], rings[k + 1][a], rings[k + 1][bb]);
        tri(rings[k][a], rings[k + 1][bb], rings[k][bb]);
      }
    const uint32_t tip = vertex(w.p.back() + last_t * (w.r.back() * 1.5f), last_t, dark, w.skin.back());
    for (int j = 0; j < sides; ++j)
      tri(rings.back()[static_cast<size_t>(j)], tip, rings.back()[static_cast<size_t>((j + 1) % sides)]);
  }
  // A berry: an octahedron (8 triangles), smooth-normalled so it shades round.
  void berry(vec3 c, float r, vec3 colour, uint32_t bone) {
    const vec3 pts[6] = {c + v3(0, r, 0), c - v3(0, r, 0), c + v3(r, 0, 0), c + v3(0, 0, r), c - v3(r, 0, 0), c - v3(0, 0, r)};
    const int faces[8][3] = {{0, 3, 2}, {0, 4, 3}, {0, 5, 4}, {0, 2, 5}, {1, 2, 3}, {1, 3, 4}, {1, 4, 5}, {1, 5, 2}};
    Skin s;
    s.a = bone;
    for (const auto &f : faces) {
      const uint32_t a = vertex(pts[f[0]], normalize(pts[f[0]] - c), colour, s, false);
      const uint32_t b = vertex(pts[f[1]], normalize(pts[f[1]] - c), colour, s, false);
      const uint32_t d = vertex(pts[f[2]], normalize(pts[f[2]] - c), colour, s, false);
      tri(a, b, d);
    }
  }
  // A displaced sphere, normals from its faces.
  void blob(const Icosphere &ico, vec3 centre, float radius, vec3 fwd, vec3 scale, float bump, float freq, uint32_t seed,
            vec3 colour, uint32_t bone, bool can_snow = true) {
    const vec3 f = normalize(fwd), side = perpendicular(f), up = cross(f, side);
    const uint32_t first = static_cast<uint32_t>(out.positions.size());
    std::vector<vec3> pos(ico.v.size());
    for (size_t k = 0; k < ico.v.size(); ++k) {
      const vec3 d = ico.v[k];
      const float n = noise(d * freq + v3(static_cast<float>(seed % 97), static_cast<float>(seed % 89), 0), seed);
      const float r = radius * (1 + bump * (n - 0.5f) * 2);
      const vec3 local = mul(d, scale) * r;
      pos[k] = centre + side * local.x + up * local.y + f * local.z;
    }
    std::vector<vec3> nrm(ico.v.size(), v3(0, 0, 0));
    for (size_t k = 0; k < ico.i.size(); k += 3) {
      const vec3 a = pos[ico.i[k]], b = pos[ico.i[k + 1]], c = pos[ico.i[k + 2]];
      const vec3 fn = cross(b - a, c - a);
      for (int j = 0; j < 3; ++j) nrm[ico.i[k + j]] = nrm[ico.i[k + j]] + fn;
    }
    Skin s;
    s.a = bone;
    for (size_t k = 0; k < ico.v.size(); ++k) {
      vec3 n = normalize(nrm[k]);
      if (dot(n, pos[k] - centre) < 0) n = -n;
      vertex(pos[k], n, colour, s, can_snow);
    }
    for (size_t k = 0; k < ico.i.size(); k += 3) {
      const vec3 a = pos[ico.i[k]], b = pos[ico.i[k + 1]], c = pos[ico.i[k + 2]];
      if (dot(cross(b - a, c - a), (a + b + c) * (1.0f / 3) - centre) >= 0)
        tri(first + ico.i[k], first + ico.i[k + 1], first + ico.i[k + 2]);
      else
        tri(first + ico.i[k], first + ico.i[k + 2], first + ico.i[k + 1]);
    }
  }
};

float outline(uint32_t shape, float t) {
  switch (shape) {
    case bush_v1_genome::kShapeUpright: return 0.45f + 0.15f * foundry::det::sin(kPi * t);
    case bush_v1_genome::kShapeSpreading: return 1.0f - 0.55f * t;
    default: return 0.3f + 0.7f * foundry::det::sin(kPi * (0.15f + 0.85f * t));  // round
  }
}

int along(const Wood &w, float f, vec3 &pos) {
  float total = 0;
  for (size_t i = 0; i + 1 < w.p.size(); ++i) total += length(w.p[i + 1] - w.p[i]);
  float want = f * total, s = 0;
  for (size_t i = 0; i + 1 < w.p.size(); ++i) {
    const float seg = length(w.p[i + 1] - w.p[i]);
    if (s + seg >= want) {
      pos = lerp(w.p[i], w.p[i + 1], seg > 1e-6f ? (want - s) / seg : 0);
      return static_cast<int>(i);
    }
    s += seg;
  }
  pos = w.p.back();
  return static_cast<int>(w.p.size()) - 2;
}

}  // namespace

bool run(const bush_v1_spec &in, channels::bush_v1_model &out, std::string &error) {
  const bush_v1_genome &g = in.genome;
  if (in.age > 2 || in.season > 3 || in.detail > 2) {
    error = "age, season, and detail must be in range";
    return false;
  }
  if (g.age_height.size() != 3 || g.bloom.size() != 4 || g.fruit_seasons.size() != 4) {
    error = "genome needs 3 age_height, 4 bloom, and 4 fruit_seasons values";
    return false;
  }
  if (g.height <= 0 || g.stems == 0) {
    error = "genome height and stems must be positive";
    return false;
  }
  const uint32_t seed = hash_str(g.name.c_str()) ^ hash_u32(in.seed * 2654435761u + in.age);
  Rng place(seed, 1u), len(seed, 2u), leaf(seed, 3u), wob(seed, 4u);
  static const float kMaturity[3] = {0.5f, 1.0f, 1.0f};
  const float m = kMaturity[in.age];
  const bool old = in.age == bush_v1_spec::kAgeOld;
  const float H = g.height * g.age_height[in.age];
  const float spread = g.width * H;
  const float wobble = g.wobble * (old ? 1.4f : 1.0f);
  const bool winter = in.season == bush_v1_spec::kSeasonWinter;
  const bool fan = g.shape == bush_v1_genome::kShapeFan;

  // ---- stems ----
  std::vector<Wood> wood;
  int n_stems = static_cast<int>(static_cast<float>(g.stems) * (in.age == 0 ? 0.6f : (old ? 1.25f : 1.0f)) + 0.5f);
  n_stems = n_stems < 1 ? 1 : (n_stems > 24 ? 24 : n_stems);
  const float foot = minf(0.12f * spread, 0.35f);
  for (int s = 0; s < n_stems; ++s) {
    const float az = 2 * kPi * (static_cast<float>(s) + 0.6f * place.uniform()) / static_cast<float>(n_stems);
    const float rr = s == 0 ? 0.0f : foot * foundry::det::sqrt(place.uniform());
    const vec3 out_dir = v3(foundry::det::cos(az), 0, foundry::det::sin(az));
    const float lean = g.stem_spread * (s == 0 ? 0.2f : 0.35f + 0.65f * place.uniform());
    Wood w;
    w.level = 0;
    vec3 d = normalize(v3(out_dir.x * foundry::det::sin(lean), foundry::det::cos(lean), out_dir.z * foundry::det::sin(lean)));
    const int n = 7;
    const float L = H * (0.85f + 0.2f * len.uniform()) / maxf(foundry::det::cos(lean), 0.45f);
    w.p.push_back(out_dir * rr);
    for (int k = 1; k < n; ++k) {
      vec3 ax = cross(d, out_dir);
      if (length(ax) > 1e-4f) d = normalize(rotate(d, normalize(ax), g.stem_curve / static_cast<float>(n - 1)));
      if (wobble > 0) {
        const float wa = wob.uniform() * 2 * kPi;
        const float wt = wob.signed_unit() * wobble;
        d = normalize(rotate(d, rotate(perpendicular(d), d, wa), wt));
      }
      vec3 next = w.p.back() + d * (L / static_cast<float>(n - 1));
      if (next.y < 0.05f) next.y = 0.05f;
      w.p.push_back(next);
    }
    wood.push_back(w);
  }
  const size_t last_stem = wood.size();

  // ---- side branches and twigs ----
  if (!fan && g.branches > 0) {
    for (size_t s = 0; s < last_stem; ++s) {
      int nb = static_cast<int>(static_cast<float>(g.branches) * (0.4f + 0.6f * m) + 0.5f);
      float az = place.uniform() * 2 * kPi;
      for (int k = 0; k < nb; ++k) {
        const float t = 0.25f + 0.7f * (static_cast<float>(k) + 0.5f + 0.3f * place.signed_unit()) / static_cast<float>(nb);
        vec3 base;
        const int at = along(wood[s], clampf(t, 0.15f, 0.95f), base);
        const vec3 tan = normalize(wood[s].p[static_cast<size_t>(at) + 1] - wood[s].p[static_cast<size_t>(at)]);
        az += 2.39996f + 0.3f * place.signed_unit();
        const float ang = (g.branch_angle + 10 * place.signed_unit()) * kDeg;
        vec3 dir = normalize(rotate(tan, rotate(perpendicular(tan), tan, az), ang));
        float total = 0;
        for (size_t i = 0; i + 1 < wood[s].p.size(); ++i) total += length(wood[s].p[i + 1] - wood[s].p[i]);
        const float L = g.branch_length * total * outline(g.shape, t) * (0.8f + 0.4f * len.uniform());
        Wood b;
        b.level = 1;
        b.p.push_back(base);
        for (int j = 1; j < 4; ++j) {
          if (wobble > 0) {
            const float wa = wob.uniform() * 2 * kPi;
            const float wt = wob.signed_unit() * wobble;
            dir = normalize(rotate(dir, rotate(perpendicular(dir), dir, wa), wt));
          }
          vec3 next = b.p.back() + dir * (L / 3);
          if (next.y < 0.05f) next.y = 0.05f;
          b.p.push_back(next);
        }
        b.skin.assign(1, Skin{static_cast<uint32_t>(s), 0, 0});  // the parent stem index, fixed below
        wood.push_back(b);
        wood[s].children++;
      }
    }
  }
  const size_t last_branch = wood.size();
  if (!fan && g.twigs > 0 && m >= 0.6f) {
    for (size_t bi = last_stem; bi < last_branch; ++bi) {
      for (uint32_t k = 0; k < g.twigs; ++k) {
        const float t = 0.35f + 0.6f * (static_cast<float>(k) + 0.5f) / static_cast<float>(g.twigs);
        vec3 base;
        const int at = along(wood[bi], t, base);
        const vec3 tan = normalize(wood[bi].p[static_cast<size_t>(at) + 1] - wood[bi].p[static_cast<size_t>(at)]);
        const float az = place.uniform() * 2 * kPi;
        const vec3 dir = normalize(rotate(tan, rotate(perpendicular(tan), tan, az), 40 * kDeg));
        float total = 0;
        for (size_t i = 0; i + 1 < wood[bi].p.size(); ++i) total += length(wood[bi].p[i + 1] - wood[bi].p[i]);
        Wood tw;
        tw.level = 2;
        tw.p = {base, base + dir * (total * 0.22f), base + dir * (total * 0.45f)};
        tw.skin.assign(1, Skin{static_cast<uint32_t>(bi), 0, 0});
        wood.push_back(tw);
        wood[bi].children++;
      }
    }
  }

  // ---- fit the height ----
  {
    float top = 0;
    for (const auto &w : wood)
      for (const vec3 &p : w.p) top = maxf(top, p.y);
    const float clump_r = maxf(g.clump * H, 0.05f);
    // Foliage rises above the wood: half a clump, or most of a fan leaf.
    const float sc = (H - (fan ? clump_r * 1.9f : clump_r * 0.5f)) / maxf(top, 0.05f);
    for (auto &w : wood)
      for (vec3 &p : w.p) p = v3(p.x * sc, maxf(p.y * sc, 0.03f), p.z * sc);
  }

  // ---- radii ----
  const float r_stem = maxf(0.012f, 0.012f * H) * kChunk * (old ? 1.3f : 1.0f);
  for (auto &w : wood) {
    const float r0 = w.level == 0 ? r_stem : (w.level == 1 ? r_stem * 0.6f : r_stem * 0.35f);
    w.r.clear();
    for (size_t k = 0; k < w.p.size(); ++k) {
      const float u = static_cast<float>(k) / static_cast<float>(w.p.size() - 1);
      w.r.push_back(maxf(r0 * (1 - 0.5f * u), 0.006f));
    }
  }

  // ---- bones ----
  out.bones.clear();
  {
    channels::bush_v1_bone root;
    root.parent = -1;
    root.pivot = v3(0, 0, 0);
    root.axis = v3(0, 1, 0);
    root.frequency = 0;
    root.damping = 0.1f;
    root.drag = 0;
    out.bones.push_back(root);
  }
  const bool bare = g.deciduous && winter;
  const float leaf_drag = bare ? 0.5f + 0.5f * g.winter_keep : 1.0f;
  const float f0 = clampf(1.4f / foundry::det::sqrt(maxf(H, 0.2f)), 0.6f, 3.5f);
  auto add_bone = [&](int parent, vec3 pivot, vec3 axis, float freq, float drag) {
    channels::bush_v1_bone b;
    b.parent = parent;
    b.pivot = pivot;
    b.axis = normalize(axis);
    b.frequency = freq;
    b.damping = 0.12f;
    b.drag = drag * g.flex * leaf_drag;
    out.bones.push_back(b);
    return static_cast<uint32_t>(out.bones.size() - 1);
  };
  const size_t branch_count = last_branch - last_stem;
  const bool branch_bones = 1 + 2 * last_stem + branch_count <= static_cast<size_t>(kMaxBones);
  std::vector<uint32_t> lower(last_stem), upper(last_stem);
  for (size_t s = 0; s < last_stem; ++s) {
    Wood &w = wood[s];
    const size_t mid = w.p.size() / 2;
    lower[s] = add_bone(0, w.p[0], w.p[1] - w.p[0], f0, 0.05f);
    upper[s] = add_bone(static_cast<int>(lower[s]), w.p[mid], w.p[mid + 1] - w.p[mid], f0 * 1.4f, 0.09f);
    w.skin.clear();
    for (size_t k = 0; k < w.p.size(); ++k) {
      Skin sk;
      sk.a = k < mid ? lower[s] : upper[s];
      if (k == mid) {
        sk.a = lower[s];
        sk.b = upper[s];
        sk.w = 0.5f;
      }
      w.skin.push_back(sk);
    }
  }
  std::vector<uint32_t> owner(wood.size(), 0);
  for (size_t bi = last_stem; bi < wood.size(); ++bi) {
    Wood &w = wood[bi];
    const size_t parent = w.skin[0].a;
    uint32_t parent_bone;
    if (wood[parent].level == 0) {
      // Which half of the stem it leaves from.
      vec3 base = w.p[0];
      const Wood &st = wood[parent];
      const size_t mid = st.p.size() / 2;
      parent_bone = base.y < st.p[mid].y ? lower[parent] : upper[parent];
    } else {
      parent_bone = owner[parent];
    }
    uint32_t bone = parent_bone;
    if (w.level == 1 && branch_bones) bone = add_bone(static_cast<int>(parent_bone), w.p[0], w.p[1] - w.p[0], f0 * 1.8f, 0.12f);
    owner[bi] = bone;
    w.skin.assign(w.p.size(), Skin{bone, 0, 0});
  }
  for (size_t s = 0; s < last_stem; ++s) owner[s] = upper[s];

  // ---- foliage anchors ----
  const float clump_r = maxf(g.clump * H, 0.05f) * (in.age == 0 ? 0.8f : 1.0f);
  std::vector<Clump> clumps;
  if (!fan) {
    for (size_t i = 0; i < wood.size(); ++i) {
      const Wood &w = wood[i];
      const bool outer = w.children == 0 || w.level >= 1;
      if (!outer) continue;
      float total = 0;
      for (size_t k = 0; k + 1 < w.p.size(); ++k) total += length(w.p[k + 1] - w.p[k]);
      const float spacing = clump_r * 1.8f / maxf(g.leafiness, 0.2f);
      int n = 1 + static_cast<int>(total * (w.level == 0 ? 0.45f : 0.7f) / spacing);
      if (w.children > 0) n = 1 + n / 2;
      for (int k = 0; k < n; ++k) {
        const float u = n > 1 ? 1.0f - 0.6f * static_cast<float>(k) / static_cast<float>(n - 1) : 1.0f;
        vec3 pos;
        along(w, clampf(u, 0, 1), pos);
        const float jx = leaf.signed_unit();
        const float jy = leaf.signed_unit() * 0.5f;
        const float jz = leaf.signed_unit();
        Clump c;
        c.position = pos + v3(jx, jy, jz) * (clump_r * 0.35f);
        c.radius = clump_r * (0.75f + 0.5f * leaf.uniform());
        c.bone = owner[i];
        c.r1 = leaf.uniform();
        c.r2 = leaf.uniform();
        clumps.push_back(c);
      }
    }
    // Cap the count, growing the kept clumps to cover.
    if (clumps.size() > static_cast<size_t>(kMaxFoliage)) {
      const float grow = foundry::det::sqrt(static_cast<float>(clumps.size()) / kMaxFoliage);
      std::vector<Clump> kept;
      for (size_t k = 0; k < clumps.size(); ++k)
        if ((k * kMaxFoliage) / clumps.size() != ((k + 1) * kMaxFoliage) / clumps.size()) {
          Clump c = clumps[k];
          c.radius *= grow;
          kept.push_back(c);
        }
      clumps.swap(kept);
    }
    vec3 centre = v3(0, H * 0.5f, 0);
    for (auto &c : clumps) {
      c.normal = normalize(c.position - centre + v3(0, c.radius * 0.5f, 0));
      if (c.position.y - c.radius * 0.6f < 0.05f) c.position.y = 0.05f + c.radius * 0.6f;
    }
  }

  // ---- mesh ----
  Builder bd{out, winter, g.snow > 0 ? 1.0f - 0.55f * g.snow : 2.0f};
  static const int kSides[3][3] = {{6, 4, 3}, {5, 3, 3}, {4, 3, 3}};
  const uint32_t detail = in.detail;
  for (const auto &w : wood) {
    if (detail == 2 && w.level == 2) continue;  // twigs vanish into the foliage at low detail
    bd.tube(w, kSides[detail][w.level], g.bark_light, g.bark_dark, seed + static_cast<uint32_t>(w.level));
  }
  const Icosphere ico = icosphere(detail == 0 ? 1 : (detail == 1 ? 0 : -1));
  const size_t keep_den = 1;
  const size_t keep_num = 1;
  const float lod_grow = detail == 2 ? 1.15f : 1.0f;
  const uint32_t season = in.season;
  const float bloom = g.bloom[season];
  const float ripeness = g.fruit_seasons[season];
  const bool fruiting = (!g.pickable || in.fruit) && g.fruit > 0 && ripeness > 0 && in.age != bush_v1_spec::kAgeYoung;
  // One cluster of berries hanging from `at` (on bone `bone`), down and out along d.
  auto cluster = [&](vec3 at, vec3 d, Rng &rng, uint32_t bone) {
    const float fr = maxf(g.fruit_size, 0.015f);
    // Fewer, larger berries per cluster at lower detail.
    const uint32_t per = detail == 0 ? g.fruit_cluster : (g.fruit_cluster + 1) / 2;
    const float grow = detail == 0 ? 1.0f : 1.2f;
    const float spread = fr * 1.3f * foundry::det::sqrt(static_cast<float>(g.fruit_cluster)) * 0.5f;
    // The cluster's centre sits clear of the foliage, so it reads from outside.
    const vec3 centre = at + d * (spread + fr);
    for (uint32_t b = 0; b < per; ++b) {
      const float bx = rng.signed_unit();
      const float by = rng.signed_unit();
      const float bz = rng.signed_unit();
      const float shade = rng.uniform();
      const vec3 bc = centre + v3(bx, by * 0.8f - 0.3f, bz) * spread;
      const float ripe = clampf(ripeness + 0.25f * (shade - 0.5f), 0, 1);
      bd.berry(bc, fr * grow, mixv(g.fruit_unripe, g.fruit_ripe, ripe), bone);
    }
  };

  if (fan) {
    // Fan leaves: a half-disc of blades at each stalk's tip, facing out and up.
    const int blades = detail == 0 ? 14 : (detail == 1 ? 10 : 7);
    for (size_t s = 0; s < last_stem; ++s) {
      const Wood &w = wood[s];
      const vec3 tip = w.p.back();
      const vec3 t = normalize(w.p.back() - w.p[w.p.size() - 2]);
      vec3 side = cross(t, v3(0, 1, 0));
      side = length(side) > 1e-3f ? normalize(side) : perpendicular(t);
      const vec3 face_up = normalize(cross(side, t));
      const float L = clump_r * 2.6f;
      const float r = leaf.uniform();
      vec3 colour = g.leaf_summer;
      if (season == bush_v1_spec::kSeasonSpring) colour = g.leaf_spring;
      else if (season == bush_v1_spec::kSeasonFall) colour = r < 0.5f ? g.leaf_fall : g.leaf_fall_alt;
      else if (winter) colour = g.leaf_winter;
      for (int b = 0; b < blades; ++b) {
        const float a = kPi * (static_cast<float>(b) + 0.5f) / static_cast<float>(blades) - 0.5f * kPi;
        const vec3 dir = normalize(t * foundry::det::cos(a) + side * foundry::det::sin(a) + face_up * 0.15f);
        const vec3 across = normalize(cross(dir, face_up)) * (L * 0.07f);
        const vec3 n0 = face_up;
        for (int sd = 0; sd < 2; ++sd) {
          const vec3 nn = sd == 0 ? n0 : -n0;
          const vec3 c = colour * (0.9f + 0.15f * static_cast<float>(b % 2));
          const uint32_t i0 = bd.vertex(tip, nn, c, w.skin.back());
          const uint32_t i1 = bd.vertex(tip + dir * (L * 0.5f) + across, nn, c, w.skin.back());
          const uint32_t i2 = bd.vertex(tip + dir * L, nn, c, w.skin.back());
          const uint32_t i3 = bd.vertex(tip + dir * (L * 0.5f) - across, nn, c, w.skin.back());
          if (sd == 0) {
            bd.tri(i0, i1, i2);
            bd.tri(i0, i2, i3);
          } else {
            bd.tri(i0, i2, i1);
            bd.tri(i0, i3, i2);
          }
        }
      }
    }
    // Fruit on a fan shrub: clusters hanging from partway up a share of the stalks (`fruit` per stalk).
    if (fruiting) {
      for (size_t s = 0; s < last_stem; ++s) {
        Rng rng(seed ^ hash_u32(static_cast<uint32_t>(s) + 91u), 5u);
        if (rng.uniform() >= g.fruit * 0.5f) continue;
        const Wood &w = wood[s];
        const size_t at = w.p.size() / 2;
        vec3 out = v3(w.p[at].x, 0, w.p[at].z);
        out = length(out) > 1e-3f ? normalize(out) : v3(1, 0, 0);
        cluster(w.p[at], normalize(out + v3(0, -0.6f, 0)), rng, w.skin[at].a);
      }
    }
  }
  for (size_t k = 0; k < clumps.size(); ++k) {
    if (keep_den > 1 && (k * keep_num) / keep_den == ((k + 1) * keep_num) / keep_den) continue;
    const Clump &c = clumps[k];
    bool show = true;
    float size = 1.0f;
    vec3 colour = g.leaf_summer;
    if (season == bush_v1_spec::kSeasonSpring) {
      colour = g.leaf_spring;
      size = g.deciduous ? 0.85f : 1.0f;
    } else if (season == bush_v1_spec::kSeasonFall) {
      show = !g.deciduous || c.r1 >= g.fall_drop;
      if (g.deciduous) colour = c.r2 < 0.5f ? g.leaf_fall : g.leaf_fall_alt;
    } else if (winter) {
      colour = g.leaf_winter;
      if (g.deciduous) {
        show = c.r1 < g.winter_keep;
        size = 0.7f;
      }
    }
    const float radius = c.radius * size * lod_grow;
    const uint32_t cs = seed ^ hash_u32(static_cast<uint32_t>(k) + 17u);
    if (show) {
      colour = colour * (0.9f + 0.2f * c.r2);
      if (g.foliage == bush_v1_genome::kFoliageNeedles)
        bd.blob(ico, c.position, radius, c.normal, v3(1.0f, 0.62f, 1.35f), 0.32f, 3.6f, cs, colour, c.bone);
      else
        bd.blob(ico, c.position, radius, c.normal, v3(1, 0.85f, 1), 0.24f, 2.2f, cs, colour, c.bone);
      // Flowers: clusters on the clump's outer face.
      if (c.r2 < bloom) {
        const vec3 fc = c.position + c.normal * (radius * 0.75f);
        bd.blob(ico, fc, radius * g.blossom_size * 0.55f, c.normal, v3(1, 0.8f, 1), 0.2f, 3.0f, cs + 5u,
                g.blossom_color * (0.94f + 0.1f * c.r1), c.bone);
      }
    }
    // Berries, hanging in clusters on the clump's outer lower side (even on bare winter twigs).
    if (fruiting && (show || winter)) {
      const float want = g.fruit;
      int clusters = static_cast<int>(want);
      if (c.r1 < want - static_cast<float>(clusters)) clusters++;
      Rng rng(cs, 7u);
      // Whether a point is buried in another shown clump (dense crowns hide most of their insides).
      auto buried = [&](vec3 q) {
        for (size_t o = 0; o < clumps.size(); ++o) {
          if (o == k) continue;
          const float rr = clumps[o].radius * lod_grow * 0.85f;
          const vec3 dq = q - clumps[o].position;
          if (dot(dq, dq) < rr * rr) return true;
        }
        return false;
      };
      for (int j = 0; j < clusters; ++j) {
        // Out from the shrub's middle and a little down, so the cluster hangs on the outside of the
        // crown, in view, rather than inside a neighbouring clump.
        const float dx = rng.signed_unit();
        const float dy = -0.3f - 0.4f * rng.uniform();
        const float dz = rng.signed_unit();
        vec3 out = v3(c.position.x, 0, c.position.z);
        out = length(out) > 1e-3f ? normalize(out) : c.normal;
        // Up to three tries for a spot in view; a cluster with none is left off.
        for (int t = 0; t < 3; ++t) {
          const vec3 d = normalize(out * 2.0f + c.normal * 0.5f + v3(dx, dy, dz) * (0.7f + 0.6f * static_cast<float>(t)));
          // A bare winter twig carries its berries close in.
          // Past the clump's bumps (up to a quarter of its radius), so the cluster sits on the surface.
          const vec3 at = c.position + d * (show ? radius * 1.12f : radius * 0.3f);
          if (show && buried(at + d * (radius * 0.15f))) continue;
          cluster(at, d, rng, c.bone);
          break;
        }
      }
    }
  }

  // ---- size ----
  float top = 0, reach = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
  }
  out.height = top;
  out.radius = reach;

  // ---- collision ----
  // Soft: a cylinder about half the shrub's spread, so a player pushes partway in before it stops them
  // (design/plotter.md, Footprints and conformance). Static for the build, inside the conformance.
  out.collision.clear();
  {
    channels::physics_v1_shape cyl;
    cyl.type = channels::physics_v1_shape::kTypeCylinder;
    const float h = top * 0.8f;
    cyl.position = v3(0, h * 0.5f, 0);
    cyl.rotation = v3(0, 0, 0);
    cyl.radius = maxf(reach * 0.5f, 0.05f);
    cyl.half_height = h * 0.5f;
    channels::physics_v1_body body;
    body.purpose = channels::physics_v1_body::kPurposeSoft;
    body.shapes.push_back(cyl);
    out.collision.push_back(body);
  }
  return true;
}

}  // namespace bush_grow_v1

FOUNDRY_SYSTEM(bush_grow_v1, channels::bush_v1_spec, channels::bush_v1_model)
