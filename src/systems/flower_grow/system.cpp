// flower_grow: grows a clump of flowers from its genome and season (design/systems.md, Mushrooms and flowers).
//
// 1. Stems: spread over a disc, leaning out, bending, each a tapered tube on two
//    wind bones (lower and upper), under one root bone for the clump.
// 2. Heads, by form: ray (disc and rays), cup, trumpet, bell (a raceme of
//    hanging bells), spike (a raceme of florets), iris (falls and standards),
//    cattail, and crest (a bird of paradise). Petals are two-sided curved
//    ribbons that run from the petal colour at the base to the tip colour.
// 3. Seasons: each stem is in flower, a bud before the species' peak, or a seed
//    head after it; a dormant species dies back in winter to a few dry stalks.
// 4. Leaves: basal leaves around the clump's foot and leaves up each stem, by
//    shape (strap, lance, broad, needle, sword), green, then fall-coloured.
// Heights sit a little above wild ones and parts are thickened, so a clump reads
// at battle distance (style.md section 3).
#include <vector>

#include "boundary.h"
#include "channels/flower_model.h"
#include "channels/flower_spec.h"
#include "math.h"
#include "packer/flower_model.h"
#include "packer/flower_spec.h"

namespace flower_grow {

using namespace flower_math;
using channels::flower_genome;
using channels::flower_spec;

namespace {

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

struct Builder {
  channels::flower_model &out;
  uint32_t vertex(vec3 p, vec3 n, vec3 c, Skin s) {
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
  // A ribbon along a centreline: `across` gives half the width at each point. One sheet: the
  // renderers draw both faces and turn the normal to the side that is seen.
  void ribbon(const std::vector<vec3> &mid, const std::vector<vec3> &across, const std::vector<vec3> &colours,
              const std::vector<Skin> &skin) {
    const size_t n = mid.size();
    std::vector<uint32_t> l, r;
    for (size_t k = 0; k < n; ++k) {
      const vec3 t = k + 1 < n ? mid[k + 1] - mid[k] : mid[k] - mid[k - 1];
      vec3 nrm = cross(t, across[k]);
      nrm = length(nrm) > 1e-9f ? normalize(nrm) : v3(0, 1, 0);
      l.push_back(vertex(mid[k] - across[k], nrm, colours[k], skin[k]));
      r.push_back(vertex(mid[k] + across[k], nrm, colours[k], skin[k]));
    }
    for (size_t k = 0; k + 1 < n; ++k) {
      tri(l[k], r[k], r[k + 1]);
      tri(l[k], r[k + 1], l[k + 1]);
    }
  }
  // A tube through rings of the given radii (closed at the far end when `tip`). An open tube's
  // inside needs no faces of its own: the renderers draw both faces of each triangle.
  void tube(const std::vector<vec3> &path, const std::vector<float> &radii, const std::vector<vec3> &colours,
            const std::vector<Skin> &skin, int sides, bool tip) {
    const size_t n = path.size();
    std::vector<vec3> T(n), N(n);
    vec3 prev_n = v3(0, 0, 0);
    for (size_t k = 0; k < n; ++k) {
      vec3 t = k + 1 < n ? path[k + 1] - path[k] : path[k] - path[k - 1];
      if (k > 0 && k + 1 < n) t = normalize(path[k + 1] - path[k]) + normalize(path[k] - path[k - 1]);
      T[k] = normalize(t);
      vec3 nn = k == 0 ? perpendicular(T[k]) : prev_n - T[k] * dot(prev_n, T[k]);
      if (length(nn) < 1e-4f) nn = perpendicular(T[k]);
      N[k] = normalize(nn);
      prev_n = N[k];
    }
    std::vector<std::vector<uint32_t>> rings;
    for (size_t k = 0; k < n; ++k) {
      const vec3 B = cross(T[k], N[k]);
      std::vector<uint32_t> ring;
      for (int j = 0; j < sides; ++j) {
        const float th = 2 * kPi * static_cast<float>(j) / static_cast<float>(sides);
        const vec3 d = N[k] * foundry::det::cos(th) + B * foundry::det::sin(th);
        ring.push_back(vertex(path[k] + d * radii[k], d, colours[k], skin[k]));
      }
      rings.push_back(ring);
    }
    for (size_t k = 0; k + 1 < n; ++k)
      for (int j = 0; j < sides; ++j) {
        const size_t a = static_cast<size_t>(j), b = static_cast<size_t>((j + 1) % sides);
        tri(rings[k][a], rings[k + 1][a], rings[k + 1][b]);
        tri(rings[k][a], rings[k + 1][b], rings[k][b]);
      }
    if (tip) {
      const uint32_t t = vertex(path.back() + T.back() * (radii.back() * 0.8f), T.back(), colours.back(), skin.back());
      for (int j = 0; j < sides; ++j)
        tri(rings.back()[static_cast<size_t>(j)], t, rings.back()[static_cast<size_t>((j + 1) % sides)]);
    }
  }
  // A small closed blob (bud, floret, seed pod): an octahedron stretched along an axis.
  void blob(vec3 c, float r, vec3 axis, float stretch, vec3 colour, Skin s) {
    const vec3 a = normalize(axis), u = perpendicular(a), w = cross(a, u);
    const vec3 pts[6] = {c + a * (r * stretch), c - a * (r * stretch * 0.6f), c + u * r, c + w * r, c - u * r, c - w * r};
    const int faces[8][3] = {{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
    for (const auto &f : faces) {
      const vec3 p0 = pts[f[0]], p1 = pts[f[1]], p2 = pts[f[2]];
      vec3 n = normalize(cross(p1 - p0, p2 - p0));
      if (dot(n, (p0 + p1 + p2) * (1.0f / 3) - c) < 0) n = -n;
      const uint32_t i0 = vertex(p0, normalize(p0 - c), colour, s), i1 = vertex(p1, normalize(p1 - c), colour, s),
                     i2 = vertex(p2, normalize(p2 - c), colour, s);
      if (dot(cross(p1 - p0, p2 - p0), n) >= 0) tri(i0, i1, i2);
      else tri(i0, i2, i1);
    }
  }
};

vec3 mixv(vec3 a, vec3 b, float t) { return a + (b - a) * t; }

struct Grower {
  const flower_genome &g;
  Builder &bd;
  int segs;  // petal and leaf segments by detail
  // A petal from `base`, outward along `radial` (unit, across the head's `axis`),
  // raised by `cup` degrees, curling by `curl` radians along its length.
  void petal(vec3 base, vec3 axis, vec3 radial, float len, float width, float cup, float curl, vec3 c0, vec3 c1, Skin s,
             bool rounded) {
    const vec3 side = normalize(cross(axis, radial));
    vec3 dir = normalize(radial * foundry::det::cos(cup * kDeg) + axis * foundry::det::sin(cup * kDeg));
    std::vector<vec3> mid, across, col;
    std::vector<Skin> skin;
    vec3 p = base;
    const int n = segs + 1;
    for (int k = 0; k <= n; ++k) {
      const float u = static_cast<float>(k) / static_cast<float>(n);
      mid.push_back(p);
      float w = rounded ? foundry::det::sqrt(maxf(foundry::det::sin(kPi * (0.08f + 0.86f * u)), 0.0f))
                        : foundry::det::sin(kPi * (0.12f + 0.88f * u));
      if (k == n) w = rounded ? w * 0.6f : 0.0f;
      across.push_back(side * (0.5f * width * len * maxf(w, 0.0f) + 0.002f));
      col.push_back(mixv(c0, c1, u));
      skin.push_back(s);
      dir = normalize(rotate(dir, side, -curl / static_cast<float>(n)));
      p = p + dir * (len / static_cast<float>(n));
    }
    bd.ribbon(mid, across, col, skin);
  }
  // A dome or cone over a disc: the head's centre.
  void disc(vec3 c, vec3 axis, float r, float h, vec3 colour, Skin s) {
    const vec3 u = perpendicular(axis), w = cross(axis, u);
    const int sides = segs >= 3 ? 10 : 7;
    const vec3 top = c + axis * (r * maxf(h, 0.15f));
    const vec3 under = c - axis * (r * 0.25f);
    std::vector<uint32_t> rim_up, rim_down;
    const uint32_t t = bd.vertex(top, axis, colour, s), b = bd.vertex(under, -axis, colour * 0.8f, s);
    for (int j = 0; j < sides; ++j) {
      const float th = 2 * kPi * static_cast<float>(j) / static_cast<float>(sides);
      const vec3 d = u * foundry::det::cos(th) + w * foundry::det::sin(th);
      rim_up.push_back(bd.vertex(c + d * r, normalize(d + axis), colour, s));
      rim_down.push_back(bd.vertex(c + d * r, normalize(d - axis), colour * 0.8f, s));
    }
    for (int j = 0; j < sides; ++j) {
      const size_t a = static_cast<size_t>(j), n = static_cast<size_t>((j + 1) % sides);
      bd.tri(t, rim_up[a], rim_up[n]);
      bd.tri(b, rim_down[n], rim_down[a]);
    }
  }
};

}  // namespace

bool run(const flower_spec &in, channels::flower_model &out, std::string &error) {
  const flower_genome &g = in.genome;
  if (in.season > 3 || in.detail > 2) {
    error = "season and detail must be in range";
    return false;
  }
  if (g.bloom.size() != 4) {
    error = "genome needs 4 bloom values";
    return false;
  }
  if (g.height <= 0 || g.stems == 0) {
    error = "genome height and stems must be positive";
    return false;
  }
  const uint32_t seed = hash_str(g.name.c_str()) ^ hash_u32(in.seed * 2654435761u);
  Rng place(seed, 1u), pick(seed, 2u), leafy(seed, 3u);
  Builder bd{out};
  // Levels of detail keep every stem, leaf, petal and floret (the same clump at every level, so a
  // switch doesn't change what is there) and simplify their shapes: fewer segments along petals
  // and leaves, fewer sides around stems, and a stem of two segments at low detail.
  static const int kSegs[3] = {2, 1, 0};
  static const int kSides[3] = {4, 3, 3};
  Grower gr{g, bd, kSegs[in.detail]};
  const int sides = kSides[in.detail];
  const uint32_t season = in.season;
  const bool winter = season == flower_spec::kSeasonWinter, fall = season == flower_spec::kSeasonFall;
  const bool dead = winter && g.dormant;
  const float bloom = g.bloom[season];
  const float next_bloom = g.bloom[(season + 1) % 4];
  const bool rising = next_bloom > bloom && !fall;  // before the peak: buds, not seed heads
  vec3 leaf = g.leaf_color;
  if ((fall || winter) && !g.evergreen) leaf = g.leaf_fall;
  else if (winter) leaf = g.leaf_color * 0.85f;
  const vec3 stem_colour = dead ? g.leaf_fall * 0.75f : (fall && !g.evergreen ? mixv(g.stem_color, g.leaf_fall, 0.5f) : g.stem_color);

  out.bones.clear();
  {
    channels::flower_bone root;
    root.parent = -1;
    root.pivot = v3(0, 0, 0);
    root.axis = v3(0, 1, 0);
    root.frequency = 0;
    root.damping = 0.1f;
    root.drag = 0;
    out.bones.push_back(root);
  }
  const float chunk = 1.5f;  // cel thickening (style.md section 3)

  // ---- stems and heads ----
  int stems = static_cast<int>(g.stems);
  if (dead) stems = (stems * 2 + 4) / 5;
  std::vector<uint32_t> lower_bones;
  for (int s = 0; s < stems; ++s) {
    // Where it stands: spread over the disc, the first in the middle.
    const float rr = s == 0 ? 0.0f : g.clump_radius * foundry::det::sqrt(place.uniform());
    const float az = place.uniform() * 2 * kPi;
    const vec3 out_dir = v3(foundry::det::cos(az), 0, foundry::det::sin(az));
    const vec3 base = out_dir * rr;
    const float reach = g.clump_radius > 1e-4f ? rr / g.clump_radius : 0.0f;
    const float state_roll = pick.uniform();
    const float h_jitter = pick.uniform();
    // In flower, a bud (before the peak), or spent (after it).
    enum State { kOpen, kBud, kSpent } state = state_roll < bloom ? kOpen : (rising ? kBud : kSpent);
    float H = g.height * (0.78f + 0.3f * h_jitter);
    if (state == kBud) H *= 0.8f;
    if (dead) H *= 0.6f;
    // The stem's line: leaning out, bending.
    const float lean = g.stem_lean * (0.3f + 0.7f * reach) + 0.06f * place.signed_unit();
    // Turning up about up x out tilts it outward.
    vec3 dir = normalize(rotate(v3(0, 1, 0), normalize(cross(v3(0, 1, 0), out_dir) + v3(1e-4f, 0, 0)), lean));
    const int K = 6;
    std::vector<vec3> path = {base};
    for (int k = 1; k <= K; ++k) {
      const vec3 axis = cross(dir, out_dir);
      if (length(axis) > 1e-4f) dir = normalize(rotate(dir, normalize(axis), g.stem_curve / K));
      path.push_back(path.back() + dir * (H / K));
    }
    // Two bones: the lower and upper half.
    const float f_low = clampf(1.1f / foundry::det::sqrt(H), 0.6f, 3.5f);
    channels::flower_bone lo, up;
    lo.parent = 0;
    lo.pivot = path[0];
    lo.axis = normalize(path[1] - path[0]);
    lo.frequency = f_low;
    lo.damping = 0.12f;
    lo.drag = 0.1f * g.flex;
    out.bones.push_back(lo);
    const uint32_t A = static_cast<uint32_t>(out.bones.size() - 1);
    up.parent = static_cast<int32_t>(A);
    up.pivot = path[K / 2];
    up.axis = normalize(path[K / 2 + 1] - path[K / 2]);
    up.frequency = f_low * 1.5f;
    up.damping = 0.1f;
    up.drag = 0.16f * g.flex;
    out.bones.push_back(up);
    const uint32_t B = A + 1;
    lower_bones.push_back(A);
    std::vector<float> radii;
    std::vector<vec3> cols;
    std::vector<Skin> skin;
    const float r0 = g.stem_radius * chunk;
    for (int k = 0; k <= K; ++k) {
      const float u = static_cast<float>(k) / K;
      radii.push_back(r0 * (1.0f - 0.3f * u));
      cols.push_back(stem_colour);
      Skin sk;
      sk.a = k <= K / 2 ? A : B;
      if (k == K / 2) {
        sk.a = A;
        sk.b = B;
        sk.w = 0.5f;
      }
      skin.push_back(sk);
    }
    const uint32_t head_kind = g.head;
    const bool raceme = head_kind == flower_genome::kHeadBell || head_kind == flower_genome::kHeadSpike;
    const bool stem_tip = !raceme && head_kind != flower_genome::kHeadCattail;
    if (in.detail == 2) {  // two segments: the foot, the knee where the bones meet, the top
      const size_t at[3] = {0, static_cast<size_t>(K / 2), static_cast<size_t>(K)};
      bd.tube({path[at[0]], path[at[1]], path[at[2]]}, {radii[at[0]], radii[at[1]], radii[at[2]]},
              {cols[at[0]], cols[at[1]], cols[at[2]]}, {skin[at[0]], skin[at[1]], skin[at[2]]}, sides, stem_tip);
    } else {
      bd.tube(path, radii, cols, skin, sides, stem_tip);
    }
    Skin hs;
    hs.a = B;
    const vec3 top = path[K];
    const vec3 tip_dir = normalize(path[K] - path[K - 1]);
    // The head bows: tip direction turned down toward the horizontal and below.
    vec3 bow_axis = cross(tip_dir, v3(0, -1, 0));
    bow_axis = length(bow_axis) > 1e-3f ? normalize(bow_axis) : normalize(cross(tip_dir, out_dir + v3(0, 0, 1e-3f)));
    const vec3 head_axis = normalize(rotate(tip_dir, bow_axis, g.nod * (0.8f + 0.4f * pick.uniform())));
    const float size = g.head_size * (0.85f + 0.3f * pick.uniform());
    const vec3 radial0 = perpendicular(head_axis);
    const float spin = pick.uniform() * 2 * kPi;
    auto radial = [&](float th) {
      return normalize(rotate(radial0, head_axis, th + spin));
    };
    // Stem leaves (on every stem but a dead one's).
    if (!dead && g.stem_leaves > 0) {
      const int nl = static_cast<int>(g.stem_leaves);
      for (int j = 0; j < nl; ++j) {
        const float u = 0.12f + 0.55f * (static_cast<float>(j) + 0.5f) / static_cast<float>(nl);
        const float at = u * K;
        const int k = static_cast<int>(at);
        const vec3 p = lerp(path[static_cast<size_t>(k)], path[static_cast<size_t>(k + 1)], at - static_cast<float>(k));
        const float laz = static_cast<float>(j) * 2.4f + leafy.uniform();
        const vec3 ld = normalize(v3(foundry::det::cos(laz), 0.55f, foundry::det::sin(laz)));  // about 30 degrees up
        Skin ls;
        ls.a = u < 0.5f ? A : B;
        const float L = g.leaf_length * 0.5f * (1 - 0.4f * u);
        std::vector<vec3> mid, across, col;
        std::vector<Skin> sk;
        vec3 q = p, d = ld;
        const vec3 side = normalize(cross(d, v3(0, 1, 0)) + v3(0, 0, 1e-4f));
        const int n = gr.segs + 1;
        for (int m = 0; m <= n; ++m) {
          const float t = static_cast<float>(m) / static_cast<float>(n);
          mid.push_back(q);
          const float w = g.leaf == flower_genome::kLeafBroad ? foundry::det::sin(kPi * (0.1f + 0.85f * t))
                                                                 : foundry::det::sin(kPi * (0.05f + 0.95f * t));
          across.push_back(side * (0.5f * g.leaf_width * L * maxf(w, 0.0f) + 0.002f));
          col.push_back(leaf * (0.9f + 0.15f * t));
          sk.push_back(ls);
          d = normalize(rotate(d, side, (g.leaf_droop * 0.6f) / static_cast<float>(n)));
          q = q + d * (L / static_cast<float>(n));
        }
        bd.ribbon(mid, across, col, sk);
      }
    }
    // The head.
    if (dead) {
      // A dry seed head on a dead stalk, for the forms that keep one.
      if (head_kind == flower_genome::kHeadRay || head_kind == flower_genome::kHeadCattail ||
          head_kind == flower_genome::kHeadSpike)
        bd.blob(top, size * 0.45f, tip_dir, 1.4f, g.seed_head * 0.8f, hs);
      continue;
    }
    if (state == kBud) {
      bd.blob(top + tip_dir * (size * 0.4f), size * 0.4f, head_axis, 1.8f, g.bud, hs);
      continue;
    }
    if (state == kSpent) {
      if (head_kind == flower_genome::kHeadRay)
        gr.disc(top, head_axis, size * maxf(g.center_size, 0.3f), g.center_height + 0.4f, g.seed_head, hs);
      else if (head_kind == flower_genome::kHeadCattail)
        bd.tube({top - tip_dir * (H * g.raceme), top}, {size * 1.2f, size * 1.2f}, {g.seed_head, g.seed_head}, {hs, hs}, sides + 1,
                true);
      else
        bd.blob(top + head_axis * (size * 0.3f), size * 0.35f, head_axis, 2.0f, g.seed_head, hs);
      continue;
    }
    switch (head_kind) {
      case flower_genome::kHeadRay:
      case flower_genome::kHeadCup:
      case flower_genome::kHeadTrumpet: {
        const float rc = size * g.center_size;
        const bool cup = head_kind == flower_genome::kHeadCup;
        for (uint32_t layer = 0; layer < (g.petal_layers > 0 ? g.petal_layers : 1); ++layer) {
          const uint32_t np = g.petals;
          for (uint32_t k = 0; k < np; ++k) {
            const float th = 2 * kPi * (static_cast<float>(k) + 0.5f * static_cast<float>(layer)) / static_cast<float>(np);
            const vec3 rd = radial(th);
            const float len = (size - rc * 0.8f) * (1 - 0.18f * static_cast<float>(layer)) * (0.92f + 0.16f * pick.uniform());
            gr.petal(top + rd * (rc * 0.8f), head_axis, rd, len, g.petal_width, g.petal_cup - 9.0f * static_cast<float>(layer),
                     g.petal_curl, g.petal, g.petal_tip, hs, cup);
          }
        }
        if (head_kind == flower_genome::kHeadTrumpet) {
          std::vector<vec3> path_t;
          std::vector<float> rad;
          std::vector<vec3> cc;
          std::vector<Skin> sk;
          const float L = rc * maxf(g.center_height, 0.5f);
          for (int k = 0; k <= 3; ++k) {
            const float u = static_cast<float>(k) / 3;
            path_t.push_back(top + head_axis * (L * u));
            rad.push_back(rc * (0.75f + 0.25f * u + 0.35f * u * u * u));
            cc.push_back(g.center * (0.9f + 0.15f * u));
            sk.push_back(hs);
          }
          bd.tube(path_t, rad, cc, sk, sides + 3, false);
        } else if (rc > 0) {
          gr.disc(top, head_axis, rc, g.center_height, g.center, hs);
        }
        break;
      }
      case flower_genome::kHeadIris: {
        for (int k = 0; k < 3; ++k) {
          const vec3 rd = radial(2 * kPi * static_cast<float>(k) / 3);
          gr.petal(top, head_axis, rd, size, g.petal_width, -30.0f, -0.9f, g.center, g.petal, hs, true);  // falls
          const vec3 rs = radial(2 * kPi * (static_cast<float>(k) + 0.5f) / 3);
          gr.petal(top, head_axis, rs, size * 0.8f, g.petal_width * 0.8f, 70.0f, 0.35f, g.petal, g.petal_tip, hs, true);
        }
        break;
      }
      case flower_genome::kHeadCattail: {
        // A velvet sausage below a thin spike, standing straight up the stem's line.
        const float L = H * g.raceme;
        const vec3 a = top - tip_dir * L;
        std::vector<vec3> sp = {a, a + tip_dir * (L * 0.08f), top - tip_dir * (L * 0.08f), top};
        std::vector<float> rad = {size * 0.6f, size, size, size * 0.6f};
        bd.tube(sp, rad, {g.center, g.center, g.center, g.center}, {hs, hs, hs, hs}, sides + 2, false);
        bd.tube({top, top + tip_dir * (L * 0.5f)}, {g.stem_radius * chunk * 0.7f, g.stem_radius * chunk * 0.3f},
                {g.stem_color, g.stem_color}, {hs, hs}, sides, true);
        break;
      }
      case flower_genome::kHeadCrest: {
        // A beak held out level, with upright orange petals and a blue tongue rising from it.
        vec3 level = v3(head_axis.x, 0, head_axis.z);
        level = length(level) > 1e-3f ? normalize(level) : out_dir;
        const float L = size * 2.2f;
        bd.tube({top, top + level * (L * 0.5f), top + level * L + v3(0, size * 0.25f, 0)},
                {size * g.center_size, size * g.center_size * 0.8f, size * 0.05f}, {g.center, g.center, g.center * 1.1f},
                {hs, hs, hs}, sides + 1, true);
        const vec3 fan_base = top + level * (L * 0.35f) + v3(0, size * g.center_size * 0.6f, 0);
        const uint32_t np = g.petals > 0 ? g.petals : 3;
        for (uint32_t k = 0; k < np; ++k) {
          const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(np) - 0.5f;
          const vec3 rd = normalize(level * (0.35f + t) + cross(v3(0, 1, 0), level) * (t * 0.5f));
          const bool tongue = k == np / 2;
          gr.petal(fan_base, v3(0, 1, 0), rd, size * (tongue ? 1.1f : 1.4f), g.petal_width, g.petal_cup + 10.0f * t,
                   g.petal_curl, tongue ? g.petal_tip : g.petal, tongue ? g.petal_tip : g.petal * 1.1f, hs, false);
        }
        break;
      }
      case flower_genome::kHeadBell:
      case flower_genome::kHeadSpike: {
        const int nf = g.florets > 0 ? static_cast<int>(g.florets) : 1;
        const bool bell = head_kind == flower_genome::kHeadBell;
        const float side_az = pick.uniform() * 2 * kPi;
        for (int j = 0; j < nf; ++j) {
          const float u = static_cast<float>(j) / static_cast<float>(nf > 1 ? nf - 1 : 1);
          const float along = (1 - g.raceme) + g.raceme * u;
          const float at = along * K;
          const int k = static_cast<int>(at) < K ? static_cast<int>(at) : K - 1;
          const vec3 p = lerp(path[static_cast<size_t>(k)], path[static_cast<size_t>(k + 1)], at - static_cast<float>(k));
          const vec3 t = normalize(path[static_cast<size_t>(k + 1)] - path[static_cast<size_t>(k)]);
          const float faz = g.one_sided ? side_az + 0.5f * pick.signed_unit() : static_cast<float>(j) * 2.39996f;
          vec3 out_f = normalize(rotate(perpendicular(t), t, faz));
          const float shrink = 1 - 0.55f * u;  // the top of a raceme is still opening
          Skin fs;
          fs.a = along < 0.5f ? A : B;
          const vec3 colour = mixv(g.petal, g.petal_tip, u);
          if (bell) {
            // A short stalk, then a bell hanging down and out, open at the mouth.
            const vec3 hang = normalize(out_f * 0.55f + v3(0, -1, 0) * (0.45f + 0.5f * minf(g.nod, 1.6f) / 1.6f));
            const float bs = size * shrink;
            const vec3 b0 = p + out_f * (bs * 0.6f);
            std::vector<vec3> bp;
            std::vector<float> br;
            std::vector<vec3> bc;
            std::vector<Skin> bk;
            for (int m = 0; m <= 3; ++m) {
              const float v = static_cast<float>(m) / 3;
              bp.push_back(b0 + hang * (bs * 1.6f * v));
              br.push_back(bs * (0.35f + 0.5f * v + 0.3f * v * v * v));
              bc.push_back(mixv(colour * 0.85f, colour, v));
              bk.push_back(fs);
            }
            bd.tube(bp, br, bc, bk, sides + 2, false);
          } else {
            const vec3 c = p + out_f * (size * shrink * 0.9f) + t * (size * 0.2f);
            bd.blob(c, size * shrink, normalize(out_f + t * 0.8f), 1.3f, colour, fs);
          }
        }
        break;
      }
      default:
        break;
    }
  }

  // ---- basal leaves ----
  if (!dead) {
    const int nl = static_cast<int>(g.leaves);
    for (int j = 0; j < nl; ++j) {
      const float az = 2 * kPi * (static_cast<float>(j) + 0.5f * leafy.uniform()) / static_cast<float>(nl > 0 ? nl : 1);
      const vec3 od = v3(foundry::det::cos(az), 0, foundry::det::sin(az));
      const vec3 base = od * (g.clump_radius * 0.6f * leafy.uniform());
      const bool sword = g.leaf == flower_genome::kLeafSword;
      const float tilt = sword ? 0.12f + 0.2f * leafy.uniform() : 0.35f + 0.6f * leafy.uniform();
      vec3 d = normalize(v3(od.x * foundry::det::sin(tilt), foundry::det::cos(tilt), od.z * foundry::det::sin(tilt)));
      const vec3 side = normalize(cross(d, od) + v3(0, 1e-4f, 0));
      const float L = g.leaf_length * (0.75f + 0.4f * leafy.uniform());
      const uint32_t sway = lower_bones.empty() ? 0 : lower_bones[static_cast<size_t>(j) % lower_bones.size()];
      std::vector<vec3> mid, across, col;
      std::vector<Skin> sk;
      vec3 q = base;
      const int n = gr.segs + 2;
      for (int m = 0; m <= n; ++m) {
        const float t = static_cast<float>(m) / static_cast<float>(n);
        mid.push_back(q);
        float w;
        switch (g.leaf) {
          case flower_genome::kLeafStrap: w = t < 0.8f ? 1.0f : (1 - t) * 5; break;
          case flower_genome::kLeafBroad: w = foundry::det::sin(kPi * (0.1f + 0.85f * t)); break;
          case flower_genome::kLeafNeedle: w = 0.5f * (1 - t); break;
          case flower_genome::kLeafSword: w = 1 - t * t; break;
          default: w = foundry::det::sin(kPi * (0.05f + 0.95f * t)); break;
        }
        across.push_back(side * (0.5f * g.leaf_width * L * maxf(w, 0.0f) + 0.002f));
        col.push_back(leaf * (0.85f + 0.2f * t));
        Skin s;
        s.a = 0;
        s.b = sway;
        s.w = t * 0.7f;
        sk.push_back(s);
        d = normalize(rotate(d, side, g.leaf_droop / static_cast<float>(n)));
        q = q + d * (L / static_cast<float>(n));
        if (q.y < 0.01f) q.y = 0.01f;
      }
      bd.ribbon(mid, across, col, sk);
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
  if (out.indices.empty()) {
    error = "the clump has no geometry";
    return false;
  }

  // ---- collision ----
  // Query only: the clump collides with nothing, but rays and clicks can find it.
  out.collision.clear();
  {
    channels::physics_shape cyl;
    cyl.type = channels::physics_shape::kTypeCylinder;
    cyl.position = v3(0, top * 0.5f, 0);
    cyl.rotation = v3(0, 0, 0);
    cyl.radius = maxf(reach, 0.05f);
    cyl.half_height = maxf(top * 0.5f, 0.05f);
    channels::physics_body body;
    body.purpose = channels::physics_body::kPurposeQuery;
    body.shapes.push_back(cyl);
    out.collision.push_back(body);
  }
  return true;
}

}  // namespace flower_grow

FOUNDRY_SYSTEM(flower_grow, channels::flower_spec, channels::flower_model)
