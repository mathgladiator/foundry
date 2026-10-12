// tree_rig_v1: grows a tree's skeleton from its genome, age, and season.
//
// 1. Branches: a stem polyline and up to three branch orders, with Weber and
//    Penn (1995) style parameters: counts, angles, divergence or whorls, a
//    crown-shape envelope, curvature, and gnarliness. Excurrent species keep a
//    stem to the top; decurrent ones steepen their upper limbs into leaders.
// 2. Radii by the pipe model (Leonardo's rule, exponent 2; Eloy 2011), scaled
//    so the trunk matches the species' slenderness, thickened 1.4x for the cel
//    look (style.md section 3).
// 3. Wind bones: a rigid stump, three stem bones, two per limb, one per twig.
//    Natural frequency from a cantilever beam, drag calibrated per branch order
//    and reduced when the tree is bare.
// 4. Break pieces: the stump, the log above the cut, and each limb.
// 5. Foliage anchors at the outer branches, sized from the genome.
// A swollen base (swell) widens the bottom quarter of the stem.
#include <vector>

#include "boundary.h"
#include "channels/tree_v1_rig.h"
#include "channels/tree_v1_spec.h"
#include "math.h"
#include "packer/tree_v1_rig.h"
#include "packer/tree_v1_spec.h"

namespace tree_rig_v1 {

using namespace tree_math;
using channels::tree_v1_genome;
using channels::tree_v1_spec;

namespace {

constexpr int kMaxBones = 128;
constexpr int kMaxFoliage = 260;
constexpr float kChunk = 1.4f;  // cel thickening (style.md section 3)

struct Branch {
  int parent = -1, at = 0, level = 0;
  bool dead = false;
  std::vector<vec3> p;
  std::vector<float> r, s;
  std::vector<int> bone;
  std::vector<int> bone_starts;  // point index where each of this branch's bones starts
  std::vector<int> bone_ids;
  int children = 0;
};

struct Grower {
  const tree_v1_genome &g;
  uint32_t seed;
  Rng trunk, place, len, ang, wob, leaf, dead;
  std::vector<Branch> b;
  Grower(const tree_v1_genome &genome, uint32_t s)
      : g(genome),
        seed(s),
        trunk(s, hash_str("trunk", hash_str(genome.name.c_str()))),
        place(s, hash_str("place", hash_str(genome.name.c_str()))),
        len(s, hash_str("length", hash_str(genome.name.c_str()))),
        ang(s, hash_str("angle", hash_str(genome.name.c_str()))),
        wob(s, hash_str("wobble", hash_str(genome.name.c_str()))),
        leaf(s, hash_str("leaf", hash_str(genome.name.c_str()))),
        dead(s, hash_str("dead", hash_str(genome.name.c_str()))) {}
};

float crown_shape(uint32_t shape, float ratio) {
  switch (shape) {
    case tree_v1_genome::kCrownShapeConical: return 0.06f + 0.94f * ratio;  // to a point
    case tree_v1_genome::kCrownShapeSpherical: return 0.2f + 0.8f * foundry::det::sin(kPi * ratio);
    case tree_v1_genome::kCrownShapeHemispherical: return 0.2f + 0.8f * foundry::det::sin(0.5f * kPi * ratio);
    case tree_v1_genome::kCrownShapeCylindrical: return 1.0f;
    case tree_v1_genome::kCrownShapeTapered: return 0.5f + 0.5f * ratio;
    case tree_v1_genome::kCrownShapeFlame: return ratio <= 0.7f ? ratio / 0.7f : (1 - ratio) / 0.3f;
    case tree_v1_genome::kCrownShapeInverseConical: return 1 - 0.8f * ratio;
    default: return ratio <= 0.7f ? 0.5f + 0.5f * ratio / 0.7f : 0.5f + 0.5f * (1 - ratio) / 0.3f;
  }
}

void arclength(Branch &b) {
  b.s.assign(b.p.size(), 0.0f);
  for (size_t i = 1; i < b.p.size(); ++i) b.s[i] = b.s[i - 1] + length(b.p[i] - b.p[i - 1]);
}

float total(const Branch &b) { return b.s.empty() ? 0 : b.s.back(); }

// A point at fraction f of a branch's length; returns the segment's start index.
int along(const Branch &b, float f, vec3 &pos) {
  float want = f * total(b);
  for (size_t i = 0; i + 1 < b.p.size(); ++i) {
    if (b.s[i + 1] >= want) {
      float seg = b.s[i + 1] - b.s[i];
      float k = seg > 1e-6f ? (want - b.s[i]) / seg : 0;
      pos = lerp(b.p[i], b.p[i + 1], k);
      return static_cast<int>(i);
    }
  }
  pos = b.p.back();
  return static_cast<int>(b.p.size()) - 2;
}

vec3 child_direction(vec3 t, float azimuth, float down) {
  vec3 a = rotate(perpendicular(t), t, azimuth);
  return normalize(rotate(t, a, down));
}

// Grows a polyline bending toward +Y by `curve` (negative droops), with an
// extra `tip_up` near the tip and random wobble. Stops before the ground.
void grow(Grower &gr, Branch &b, vec3 base, vec3 dir, float len, int n, float curve, float tip_up, float wobble,
          float floor_y) {
  b.p.clear();
  b.p.push_back(base);
  vec3 d = normalize(dir);
  float step = len / static_cast<float>(n - 1);
  const vec3 up = v3(0, 1, 0);
  for (int i = 1; i < n; ++i) {
    float u = static_cast<float>(i) / static_cast<float>(n - 1);
    float bend = curve / static_cast<float>(n - 1) + tip_up * 3 * u * u / static_cast<float>(n - 1);
    vec3 ax = cross(d, up);
    if (length(ax) > 1e-3f) {
      ax = normalize(ax);
      float dd = dot(d, up);
      if ((bend > 0 && dd < 0.97f) || (bend < 0 && dd > -0.97f)) d = normalize(rotate(d, ax, bend));
    }
    if (wobble > 0) {
      vec3 wa = rotate(perpendicular(d), d, gr.wob.uniform() * 2 * kPi);
      d = normalize(rotate(d, wa, gr.wob.signed_unit() * wobble));
    }
    vec3 next = b.p.back() + d * step;
    if (b.level > 0 && next.y < floor_y) {  // don't dig into the ground: end the branch
      next.y = floor_y;
      b.p.push_back(next);
      break;
    }
    b.p.push_back(next);
  }
  if (b.p.size() < 2) b.p.push_back(base + d * step);
  arclength(b);
}

}  // namespace

bool run(const tree_v1_spec &in, channels::tree_v1_rig &out, std::string &error) {
  const tree_v1_genome &g = in.genome;
  if (in.age > 3 || in.season > 3) {
    error = "age and season must be in range";
    return false;
  }
  if (g.age_height.size() != 4 || g.age_girth.size() != 4) {
    error = "genome needs 4 age_height and 4 age_girth values";
    return false;
  }
  if (g.height <= 0 || g.slender <= 0) {
    error = "genome height and slender must be positive";
    return false;
  }
  const bool ancient = in.age == tree_v1_spec::kAgeAncient;
  static const float kMaturity[4] = {0.12f, 0.45f, 1.0f, 1.0f};
  const float m = kMaturity[in.age];
  const float H = g.height * g.age_height[in.age];
  const float twist = g.twist * (ancient ? 1.7f : 1.0f) * (0.6f + 0.4f * m);
  const float wobble = g.wobble * (ancient ? 1.4f : 1.0f);
  const float lean_max = g.lean * (ancient ? 1.5f : 1.0f);

  Grower gr(g, in.seed);
  std::vector<Branch> &br = gr.b;
  br.reserve(512);

  // ---- the stem --------------------------------------------------------------
  const float cut = clampf(0.06f * H, 0.25f, 0.6f);
  float stem_len = g.excurrent ? H : H * (1 - g.crown_ratio * 0.45f);
  {
    Branch stem;
    stem.level = 0;
    float lean = gr.trunk.uniform() * lean_max;
    float lean_az = gr.trunk.uniform() * 2 * kPi;
    vec3 d = rotate(v3(0, 1, 0), v3(foundry::det::cos(lean_az), 0, foundry::det::sin(lean_az)), lean);
    const int n = 14;
    stem.p.push_back(v3(0, 0, 0));
    stem.p.push_back(v3(0, cut, 0));
    float step = (stem_len - cut) / static_cast<float>(n - 2);
    for (int i = 2; i < n; ++i) {
      vec3 wa = rotate(perpendicular(d), d, gr.wob.uniform() * 2 * kPi);
      d = normalize(rotate(d, wa, gr.wob.signed_unit() * twist));
      if (d.y < 0.75f) d = normalize(v3(d.x * 0.8f, d.y + 0.2f, d.z * 0.8f));
      stem.p.push_back(stem.p.back() + d * step);
    }
    arclength(stem);
    br.push_back(stem);
  }

  // ---- first-order limbs -------------------------------------------------------
  int n1 = static_cast<int>(static_cast<float>(g.branches) * (0.3f + 0.7f * m) + 0.5f);
  if (n1 < 2) n1 = 2;
  float t0 = clampf(H * (1 - g.crown_ratio) / maxf(stem_len, 0.1f), 0.05f, 0.9f);
  float t1 = g.excurrent ? 0.96f : 0.995f;
  t0 = t0 + (t1 - t0) * g.cluster;  // clustered species branch only near the top
  const int whorl = g.whorl > 0 ? static_cast<int>(g.whorl) : 1;
  const int groups = (n1 + whorl - 1) / whorl;
  float az = gr.place.uniform() * 2 * kPi;
  float max_l1 = 0;
  const float floor_y = 0.25f * clampf(H / 8, 0.3f, 1.0f);
  for (int grp = 0; grp < groups; ++grp) {
    float tf = t0 + (t1 - t0) * (static_cast<float>(grp) + 0.5f + 0.35f * gr.place.signed_unit()) / static_cast<float>(groups);
    tf = clampf(tf, t0, t1);
    float rel = (tf - t0) / maxf(t1 - t0, 1e-3f);
    for (int k = 0; k < whorl && static_cast<int>(br.size()) - 1 < n1; ++k) {
      float azk = az + 2 * kPi * static_cast<float>(k) / static_cast<float>(whorl) + 0.25f * gr.place.signed_unit();
      vec3 base;
      int at = along(br[0], tf, base);
      vec3 t = normalize(br[0].p[at + 1] - br[0].p[at]);
      float a = g.branch_angle + g.branch_angle_var * gr.ang.signed_unit();
      if (!g.excurrent) a *= 1 - 0.55f * rel;  // decurrent: upper limbs become leaders
      float L = g.branch_length * stem_len * crown_shape(g.crown_shape, 1 - rel) *
                (0.82f + 0.36f * gr.len.uniform()) * (0.6f + 0.4f * m) * (ancient ? 1.1f : 1.0f);
      Branch b;
      b.parent = 0;
      b.at = at;
      b.level = 1;
      grow(gr, b, base, child_direction(t, azk, a * kDeg), L, 8, g.branch_curve, g.tip_up, wobble, floor_y);
      max_l1 = maxf(max_l1, L);
      br.push_back(b);
    }
    az += g.divergence * kDeg * (g.whorl > 0 ? 1.0f : static_cast<float>(whorl)) + 0.2f * gr.place.signed_unit();
  }
  const size_t last1 = br.size();

  // Dead limbs on ancient trees: broken short and bare.
  if (ancient && g.dead_limbs > 0) {
    for (size_t i = 1; i < last1; ++i) {
      if (gr.dead.uniform() < g.dead_limbs) {
        Branch &b = br[i];
        b.dead = true;
        size_t keep = 3 + static_cast<size_t>(gr.dead.uniform() * 3);
        if (keep < b.p.size()) b.p.resize(keep);
        arclength(b);
      }
    }
  }

  // ---- twigs and sprigs ----------------------------------------------------------
  auto place_children = [&](size_t first, size_t last, int level, int count_base, float angle, float len_ratio,
                            float curve) {
    for (size_t i = first; i < last; ++i) {
      const float Lp = total(br[i]);
      int n = count_base;
      if (level == 2) n = static_cast<int>(static_cast<float>(count_base) * Lp / maxf(max_l1, 1e-3f) * (0.3f + 0.7f * m) + 0.5f);
      if (n < 1) n = 1;
      if (br[i].dead) n = n / 3;
      float az2 = gr.place.uniform() * 2 * kPi;
      for (int k = 0; k < n; ++k) {
        float u = 0.25f + 0.7f * (static_cast<float>(k) + 0.5f + 0.3f * gr.place.signed_unit()) / static_cast<float>(n);
        u = u + (1 - u) * g.cluster * 0.85f;
        vec3 base;
        int at = along(br[i], clampf(u, 0.15f, 0.97f), base);
        vec3 t = normalize(br[i].p[at + 1] - br[i].p[at]);
        float a = angle + 10 * gr.ang.signed_unit();
        float L = len_ratio * Lp * (1 - 0.5f * u * (1 - g.cluster)) * (0.8f + 0.4f * gr.len.uniform());
        az2 += g.divergence * kDeg + 0.3f * gr.place.signed_unit();
        Branch b;
        b.parent = static_cast<int>(i);
        b.at = at;
        b.level = level;
        b.dead = br[i].dead;
        grow(gr, b, base, child_direction(t, az2, a * kDeg), L, level == 2 ? 6 : 3, curve, 0, wobble, floor_y);
        br.push_back(b);
      }
    }
  };
  if (g.twigs > 0) place_children(1, last1, 2, static_cast<int>(g.twigs), g.twig_angle, g.twig_length, g.twig_curve);
  const size_t last2 = br.size();
  if (g.sprigs > 0 && m >= 0.4f)
    place_children(last1, last2, 3, static_cast<int>(g.sprigs), 40.0f, g.sprig_length, 0.1f);
  for (size_t i = 1; i < br.size(); ++i) br[static_cast<size_t>(br[i].parent)].children++;

  // ---- scale so the crown top lands on the height ------------------------------
  const float clump_r = maxf(g.clump * H, 0.1f);
  {
    float max_y = 0;
    for (const auto &b : br)
      for (const auto &p : b.p) max_y = maxf(max_y, p.y);
    float top = H - clump_r * 0.6f;
    float sc = top / maxf(max_y, 0.1f);
    for (auto &b : br) {
      for (size_t k = 0; k < b.p.size(); ++k) {
        if (b.level == 0 && k <= 1) continue;  // the stump keeps its cut height
        b.p[k] = b.p[k] * sc;
        if (b.level > 0 && b.p[k].y < floor_y) b.p[k].y = floor_y;
      }
      arclength(b);
    }
    stem_len *= sc;
  }

  // ---- radii: the pipe model, tips down ----------------------------------------
  {
    const float twig_r = maxf(0.008f, 0.0011f * H);
    for (size_t ii = br.size(); ii-- > 0;) {
      Branch &b = br[ii];
      b.r.assign(b.p.size(), 0.0f);
      float A = twig_r * twig_r;
      b.r.back() = twig_r;
      for (size_t k = b.p.size() - 1; k-- > 0;) {
        for (size_t c = ii + 1; c < br.size(); ++c)
          if (br[c].parent == static_cast<int>(ii) && br[c].at == static_cast<int>(k)) A += br[c].r[0] * br[c].r[0];
        A *= 1 + 0.10f * (b.s[k + 1] - b.s[k]);  // retired pipes: a gentle taper between branchings
        b.r[k] = foundry::det::sqrt(A);
      }
      std::vector<float> tmp(b.r.size());
      for (size_t k = 0; k < b.r.size(); ++k) {
        float sum = b.r[k] * 2, w = 2;
        if (k > 0) { sum += b.r[k - 1]; w += 1; }
        if (k + 1 < b.r.size()) { sum += b.r[k + 1]; w += 1; }
        tmp[k] = sum / w;
      }
      b.r = tmp;
    }
    // Toy-like proportions (style.md section 1): trunks 1.6x stouter again than the cel thickening alone.
    const float target = H / g.slender * kChunk * 1.6f * 0.5f * g.age_girth[in.age];
    const float f = target / maxf(br[0].r[1], 1e-5f);
    const float size = clampf(H / 8, 0.5f, 1.3f);
    static const float kMinR[4] = {0.03f, 0.02f, 0.013f, 0.009f};
    for (auto &b : br) {
      for (auto &r : b.r) r = maxf(r * f, kMinR[b.level] * size);
      if (b.parent >= 0) {
        float cap = br[static_cast<size_t>(b.parent)].r[static_cast<size_t>(b.at)] * 0.78f;
        for (auto &r : b.r) r = minf(r, cap);
      }
    }
    br[0].r[0] = br[0].r[1] * 1.12f;  // the base of the stump, before the flare
    // A swollen base (water tupelo, bald cypress): the bottom quarter of the stem bulges, tapering up.
    if (g.swell > 0) {
      const float reach = 0.25f * H;
      for (size_t k = 0; k < br[0].p.size(); ++k) {
        const float u = 1 - clampf(br[0].p[k].y / reach, 0, 1);
        br[0].r[k] *= 1 + g.swell * u * u * (0.4f + 0.6f * m);
      }
    }
  }

  // ---- bones ---------------------------------------------------------------------
  out.bones.clear();
  out.pieces.clear();
  const bool leafless = g.deciduous && (in.season == tree_v1_spec::kSeasonWinter);
  const float leaf_drag = leafless ? 0.45f + 0.55f * g.winter_keep : 1.0f;
  const float E = g.stiffness * 1e9f, rho = g.density * 1000.0f * 1.8f;
  const float sound = foundry::det::sqrt(E / rho);
  auto add_bone = [&](int parent, const Branch &b, int start, int level, uint32_t piece) {
    channels::tree_v1_bone bone;
    bone.parent = parent;
    bone.pivot = b.p[static_cast<size_t>(start)];
    size_t next = static_cast<size_t>(start) + 1 < b.p.size() ? static_cast<size_t>(start) + 1 : b.p.size() - 1;
    bone.axis = normalize(b.p[next] - b.p[static_cast<size_t>(start)]);
    bone.length = maxf(total(b) - b.s[static_cast<size_t>(start)], 0.05f);
    float r = b.r[static_cast<size_t>(start)];
    float cf = level == 0 ? 0.3f : 0.35f;
    bone.frequency = clampf(0.2798f * r / maxf(bone.length * bone.length, 1e-4f) * sound * cf, 0.2f, 5.0f);
    bone.damping = level == 0 ? 0.08f : 0.12f;
    static const float kDrag[3] = {0.012f, 0.045f, 0.07f};
    bone.drag = kDrag[level < 3 ? level : 2] * g.flex * leaf_drag * (b.dead ? 0.4f : 1.0f);
    bone.level = static_cast<uint32_t>(level);
    bone.piece = piece;
    out.bones.push_back(bone);
    return static_cast<int>(out.bones.size()) - 1;
  };
  // The bone that owns a point of a branch, or -1 when the branch has no bone there.
  auto owner_of = [&](size_t bi, int idx) {
    const Branch &b = br[bi];
    int id = -1;
    for (size_t k = 0; k < b.bone_starts.size(); ++k)
      if (b.bone_starts[k] <= idx) id = b.bone_ids[k];
    return id;
  };
  {
    Branch &stem = br[0];
    // Bone 0: the rigid stump (piece 0). Then three stem bones on the log (piece 1).
    stem.bone_starts = {0, 1, 5, 9};
    stem.bone_ids.clear();
    stem.bone_ids.push_back(add_bone(-1, stem, 0, 0, 0));
    out.bones[0].drag = 0;
    out.bones[0].frequency = 0;
    stem.bone_ids.push_back(add_bone(0, stem, 1, 0, 1));
    stem.bone_ids.push_back(add_bone(stem.bone_ids[1], stem, 5, 0, 1));
    stem.bone_ids.push_back(add_bone(stem.bone_ids[2], stem, 9, 0, 1));
  }
  int n_limb = 0, n_twig = 0;
  for (size_t i = 1; i < br.size(); ++i) {
    if (br[i].level == 1) n_limb++;
    if (br[i].level == 2) n_twig++;
  }
  int per_limb = 2;
  bool twig_bones = true;
  if (4 + n_limb * per_limb + n_twig > kMaxBones) twig_bones = false;
  if (4 + n_limb * per_limb > kMaxBones) per_limb = 1;
  // Pieces: 0 the stump, 1 the log, then one per limb.
  std::vector<uint32_t> piece_of(br.size(), 1);
  for (size_t i = 1; i < br.size(); ++i) {
    Branch &b = br[i];
    int parent_bone = owner_of(static_cast<size_t>(b.parent), b.at);
    uint32_t piece = piece_of[static_cast<size_t>(b.parent)];
    if (b.level == 1) piece = 2 + static_cast<uint32_t>(i - 1);
    piece_of[i] = piece;
    int nb = b.level == 1 ? per_limb : (b.level == 2 && twig_bones ? 1 : 0);
    if (static_cast<int>(out.bones.size()) + nb > kMaxBones) nb = 0;
    for (int k = 0; k < nb; ++k) {
      int start = (k * (static_cast<int>(b.p.size()) - 1)) / nb;
      int prev = k > 0 ? b.bone_ids.back() : parent_bone;
      b.bone_starts.push_back(start);
      b.bone_ids.push_back(add_bone(prev, b, start, b.level, piece));
    }
  }
  // Point owners.
  for (size_t i = 0; i < br.size(); ++i) {
    Branch &b = br[i];
    b.bone.assign(b.p.size(), 0);
    for (size_t k = 0; k < b.p.size(); ++k) {
      int id = owner_of(i, static_cast<int>(k));
      if (id < 0) id = owner_of(static_cast<size_t>(b.parent), b.at);
      if (id < 0) {
        // Walk further up for sprigs on boneless twigs.
        size_t up = static_cast<size_t>(b.parent);
        while (id < 0 && br[up].parent >= 0) {
          id = owner_of(static_cast<size_t>(br[up].parent), br[up].at);
          up = static_cast<size_t>(br[up].parent);
        }
        if (id < 0) id = 1;
      }
      b.bone[k] = id;
    }
  }

  // ---- break pieces --------------------------------------------------------------
  {
    // Collect mass and centre per piece.
    // Limbs are branches 1 .. last1 - 1, so limb i is piece i + 1.
    const uint32_t n_pieces = static_cast<uint32_t>(last1) + 1;
    std::vector<float> mass(n_pieces, 0.0f);
    std::vector<vec3> moment(n_pieces, v3(0, 0, 0));
    const float wood_rho = g.density * 1000.0f * 1.6f;
    for (size_t i = 0; i < br.size(); ++i) {
      const Branch &b = br[i];
      for (size_t k = 0; k + 1 < b.p.size(); ++k) {
        float r = 0.5f * (b.r[k] + b.r[k + 1]) / kChunk;
        float vol = kPi * r * r * (b.s[k + 1] - b.s[k]);
        uint32_t piece = (i == 0 && k == 0) ? 0 : piece_of[i];
        mass[piece] += vol * wood_rho;
        moment[piece] = moment[piece] + (b.p[k] + b.p[k + 1]) * (0.5f * vol * wood_rho);
      }
    }
    for (uint32_t p = 0; p < n_pieces; ++p) {
      channels::tree_v1_piece piece;
      if (p == 0) {
        piece.kind = channels::tree_v1_piece::kKindStump;
        piece.bone = 0;
        piece.pivot = v3(0, 0, 0);
        piece.length = cut;
      } else if (p == 1) {
        piece.kind = channels::tree_v1_piece::kKindLog;
        piece.bone = 1;
        piece.pivot = br[0].p[1];
        piece.length = total(br[0]) - cut;
      } else {
        size_t bi = p - 1;  // limb branch index
        piece.kind = channels::tree_v1_piece::kKindLimb;
        piece.bone = br[bi].bone_ids.empty() ? static_cast<uint32_t>(br[bi].bone[0]) : static_cast<uint32_t>(br[bi].bone_ids[0]);
        piece.pivot = br[bi].p[0];
        piece.length = total(br[bi]);
      }
      piece.mass = mass[p];
      piece.center = mass[p] > 0 ? moment[p] * (1.0f / mass[p]) : piece.pivot;
      out.pieces.push_back(piece);
    }
  }

  // ---- foliage anchors -------------------------------------------------------------
  out.foliage.clear();
  {
    int deepest = 1;
    for (const auto &b : br) deepest = b.level > deepest ? b.level : deepest;
    std::vector<channels::tree_v1_foliage> cand;
    for (size_t i = 1; i < br.size(); ++i) {
      const Branch &b = br[i];
      if (b.dead) continue;
      bool outer = b.children == 0 || b.level >= deepest - 1;
      if (!outer) continue;
      float L = total(b);
      float spacing = clump_r * 2.0f / maxf(g.leafiness, 0.2f);
      int n = 1 + static_cast<int>(L * 0.55f / spacing);
      if (b.children > 0) n = 1 + n / 3;
      for (int k = 0; k < n; ++k) {
        float u = 1.0f - 0.55f * static_cast<float>(k) / static_cast<float>(n > 1 ? n : 1);
        if (n > 1) u = 1.0f - 0.55f * static_cast<float>(k) / static_cast<float>(n - 1);
        vec3 pos;
        int at = along(b, clampf(u, 0.0f, 1.0f), pos);
        channels::tree_v1_foliage f;
        // One draw per statement: argument order is unspecified, and GCC and Clang differ.
        float jx = gr.leaf.signed_unit();
        float jy = gr.leaf.signed_unit() * 0.5f;
        float jz = gr.leaf.signed_unit();
        f.position = pos + v3(jx, jy, jz) * (clump_r * 0.35f);
        f.radius = clump_r * (0.75f + 0.5f * gr.leaf.uniform()) * (b.level >= 3 ? 0.85f : 1.0f);
        f.bone = static_cast<uint32_t>(b.bone[static_cast<size_t>(at + 1 < static_cast<int>(b.p.size()) ? at + 1 : at)]);
        f.random = gr.leaf.uniform();
        cand.push_back(f);
      }
    }
    // Saplings: a few clumps right on the stem's top when it has little else.
    if (cand.size() < 3) {
      for (int k = 0; k < 3; ++k) {
        channels::tree_v1_foliage f;
        vec3 pos;
        along(br[0], 0.75f + 0.12f * static_cast<float>(k), pos);
        f.position = pos;
        f.radius = clump_r;
        f.bone = 3;
        f.random = gr.leaf.uniform();
        cand.push_back(f);
      }
    }
    // Cap the count, growing the kept clumps to cover.
    float grow_by = 1.0f;
    size_t stride_num = cand.size(), stride_den = kMaxFoliage;
    if (cand.size() > static_cast<size_t>(kMaxFoliage)) grow_by = foundry::det::sqrt(static_cast<float>(cand.size()) / kMaxFoliage);
    vec3 centre = v3(0, 0, 0);
    size_t kept = 0;
    for (size_t k = 0; k < cand.size(); ++k) {
      if (cand.size() > static_cast<size_t>(kMaxFoliage) && (k * stride_den) / stride_num == ((k + 1) * stride_den) / stride_num)
        continue;
      cand[k].radius *= grow_by;
      out.foliage.push_back(cand[k]);
      centre = centre + cand[k].position;
      kept++;
    }
    centre = kept ? centre * (1.0f / static_cast<float>(kept)) : v3(0, H * 0.7f, 0);
    for (auto &f : out.foliage) {
      f.normal = normalize(f.position - centre + v3(0, f.radius * 0.5f, 0));
      if (f.position.y - f.radius * 0.6f < 0.15f) f.position.y = 0.15f + f.radius * 0.6f;  // foliage stays off the ground
    }
  }

  // ---- out ---------------------------------------------------------------------------
  out.spec = in;
  float top = 0;
  for (const auto &b : br)
    for (const auto &p : b.p) top = maxf(top, p.y);
  for (const auto &f : out.foliage) top = maxf(top, f.position.y + f.radius);
  out.height = top;
  out.stump_radius = br[0].r[1] / kChunk * 1.25f;
  out.cut = cut;
  {
    // The colliding stump reaches up to where the lowest limb leaves the stem.
    float crown_base = H;
    for (size_t i = 1; i < br.size(); ++i)
      if (br[i].level == 1) crown_base = minf(crown_base, br[i].p[0].y);
    out.crown_base = clampf(crown_base, cut, H);
  }
  out.branches.clear();
  for (const auto &b : br) {
    channels::tree_v1_branch cb;
    cb.parent = b.parent;
    cb.at = static_cast<uint32_t>(b.at);
    cb.level = static_cast<uint32_t>(b.level);
    cb.dead = b.dead;
    cb.points = b.p;
    cb.radii = b.r;
    for (int id : b.bone) cb.bones.push_back(static_cast<uint32_t>(id));
    out.branches.push_back(cb);
  }
  return true;
}

}  // namespace tree_rig_v1

FOUNDRY_SYSTEM(tree_rig_v1, channels::tree_v1_spec, channels::tree_v1_rig)
