// crop_build: a crop field (design/planner.md, "Crops"): a lot like a building's, sown in rows,
// its plants swaying in the wind as flowers do, but in bulk.
//
// 1. The lot: brick-laid hex rectangles (small rect6x4, medium rect8x6, large rect12x8), anchored at
//    the cell nearest its middle, its front (+z) toward its road; the field fills the rectangle
//    inside its cells, with a headland round it.
// 2. The soil: ridged furrows along x (snow on them in winter).
// 3. The plants, along the rows by the crop's spacing, by how they grow: grain (tufts of stalks
//    with heads), a tall stalk (maize with its cobs, a sunflower's face to the south), a leafy head
//    (cabbage, lettuce), root tops (carrot feathers, potato haulm, onion tubes, a carrot's
//    shoulder), gourds on their vines (pumpkins), pole vines (beans and tomatoes on teepees of
//    canes), trellis vines (grapes on posts and wires, in bunches), or flowering bushes (lavender,
//    flax). By growth: sown (furrows only), sprouting, growing, ripe, or harvested (stubble and
//    stooks of sheaves, or bare soil). By season: grain turns gold in the fall, snow in winter.
// 4. The wind: the crop's plants share a few wind groups (wind_groups bones of the drift motion,
//    house_animate), so a field of hundreds of plants poses with a handful of wind computations;
//    each vertex blends toward its group by its height in the plant, so each plant bends from its
//    own root.
// 5. Fire points it burns from, its lot's cells and box, every lot as guides, and collision: the
//    field a query body (walked through).
//
// Bones never depend on the level of detail; detail only drops plants' finer parts and thins the
// rows' plants (each level keeps every row).
#include <vector>

#include "boundary.h"
#include "build.h"
#include "channels/crop_model.h"
#include "channels/crop_spec.h"
#include "condition.h"
#include "lot.h"
#include "math.h"
#include "packer/crop_model.h"
#include "packer/crop_spec.h"

namespace crop_build {

using namespace crop_math;
using channels::crop_model;
using channels::crop_spec;
using channels::crop_type;
using channels::house_bone;

namespace {

using B = Builder<crop_model>;
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

constexpr int kLotSizes = 3;
const int kLotW[kLotSizes] = {6, 8, 12}, kLotH[kLotSizes] = {4, 6, 8};
const vec3 kSoil = {0.40f, 0.30f, 0.21f};
const vec3 kStraw = {0.82f, 0.70f, 0.40f};
const vec3 kCane = {0.62f, 0.52f, 0.34f};

struct Ctx {
  B &b;
  const crop_type &t;
  uint32_t growth, season, seed;
  float g;  // how grown: 0.2 sprouting, 0.6 growing, 1 ripe
  bool ripe() const { return growth == crop_spec::kGrowthRipe; }
  float pick(uint32_t salt) const { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); }
  // The leaves' colour: greener young, turning toward ripe as it ripens and in the fall.
  vec3 leaf() const {
    float turn = ripe() ? 0.55f : 0.0f;
    if (season == crop_spec::kSeasonFall) turn += 0.25f;
    if (season == crop_spec::kSeasonWinter) return lerp(t.leaf_color, v3(0.55f, 0.55f, 0.5f), 0.35f);
    return lerp(t.leaf_color, t.ripe_color, clampf(turn, 0, 0.85f) * (t.kind == crop_type::kKindGrain ? 1.0f : 0.25f));
  }
};

// A flat blade from base along dir (with up as its face's tilt), w wide, two-sided.
void blade(B &b, vec3 base, vec3 tip, float w, vec3 col, uint32_t bone) {
  const vec3 d = tip - base;
  const vec3 side = normalize(cross(d, v3(0, 1, 0)) + v3(1e-4f, 0, 0)) * (w * 0.5f);
  const vec3 n = normalize(cross(side, d));
  b.triangle(base - side, base + side, tip, col, bone, false, n);
}

// A leaf: a quad from base out along dir, drooping by sag, w wide.
void leaf(B &b, vec3 base, vec3 dir, float len, float w, float sag, vec3 col, uint32_t bone) {
  const vec3 mid = base + dir * (len * 0.5f) + v3(0, sag * 0.3f, 0), tip = base + dir * len - v3(0, sag, 0);
  const vec3 side = normalize(cross(dir, v3(0, 1, 0)) + v3(1e-4f, 0, 0)) * (w * 0.5f);
  b.quad(base, mid + side, tip, mid - side, col, bone, true, v3(0, 1, 0));
}

// A ball (a cabbage, a pumpkin, a tomato): a squashed frustum pair.
void ball(B &b, vec3 c, float r, float squash, vec3 col, uint32_t bone, int sides) {
  b.frustum(c - v3(0, r * squash, 0), c, r * 0.55f, r, sides, col * 0.9f, bone, false, false);
  b.frustum(c, c + v3(0, r * squash, 0), r, r * 0.45f, sides, col, bone, false, true);
}

// A furrow's ridge along x at z: two sloped faces, half wide, h high.
void ridge(B &b, float x0, float x1, float z, float half, float h, vec3 col) {
  const vec3 a0 = v3(x0, 0.02f, z - half), a1 = v3(x1, 0.02f, z - half), t0 = v3(x0, 0.02f + h, z), t1 = v3(x1, 0.02f + h, z);
  const vec3 c0 = v3(x0, 0.02f, z + half), c1 = v3(x1, 0.02f, z + half);
  b.quad(a0, a1, t1, t0, col * 0.92f, 0, true, v3(0, 1, -1));
  b.quad(c1, c0, t0, t1, col, 0, true, v3(0, 1, 1));
}

// One plant at base, on wind group `bone`; returns how tall it stands.
float plant(Ctx &c, vec3 base, uint32_t bone, uint32_t id) {
  B &b = c.b;
  const crop_type &t = c.t;
  const float H = t.height * c.g * (0.85f + 0.3f * c.pick(id * 7u + 1u));
  const float turn = c.pick(id * 7u + 2u) * 6.2831853f;
  const vec3 lean = v3(foundry::det::cos(turn), 0, foundry::det::sin(turn));
  const vec3 lv = c.leaf();
  const bool hi = b.detail == 0, mid = b.detail <= 1;
  const bool fruit = c.ripe() || (c.g > 0.5f && t.kind == crop_type::kKindGourd);
  switch (t.kind) {
    case crop_type::kKindGrain: {
      // A tuft of stalks, each with its head; gold when ripe.
      const int n = hi ? 4 : (mid ? 3 : 2);
      const vec3 stalk = c.ripe() || c.season == crop_spec::kSeasonFall ? lerp(t.stem_color, t.ripe_color, 0.8f) : t.stem_color;
      for (int k = 0; k < n; ++k) {
        const float a = turn + 6.2831853f * static_cast<float>(k) / static_cast<float>(n);
        const vec3 d = v3(foundry::det::cos(a), 0, foundry::det::sin(a));
        const vec3 tip = base + d * (H * 0.12f) + v3(0, H, 0);
        blade(b, base + d * 0.02f, tip, 0.03f, stalk, bone);
        if (c.g > 0.5f) {
          const vec3 head = c.ripe() ? t.ripe_color : lerp(t.fruit_color, lv, 0.5f);
          b.box(tip + v3(0, t.fruit_size * 0.5f, 0), v3(t.fruit_size * 0.2f, t.fruit_size * 0.55f, t.fruit_size * 0.2f), head, bone, false);
        }
      }
      if (mid) leaf(b, base + v3(0, H * 0.25f, 0), lean, H * 0.35f, 0.04f, H * 0.12f, lv, bone);
      return H;
    }
    case crop_type::kKindTallStalk: {
      // A stalk with leaves up it; a sunflower's face toward the south (+z), a maize plant's cobs.
      b.frustum(base, base + v3(0, H, 0), 0.03f + 0.01f * H, 0.015f, 4, t.stem_color, bone, false, false);
      const int leaves = hi ? 6 : (mid ? 4 : 2);
      for (int k = 0; k < leaves; ++k) {
        const float y = H * (0.2f + 0.65f * static_cast<float>(k) / static_cast<float>(leaves));
        const float a = turn + 2.4f * static_cast<float>(k);
        leaf(b, base + v3(0, y, 0), v3(foundry::det::cos(a), 0.35f, foundry::det::sin(a)), H * 0.28f, 0.09f, H * 0.08f, lv, bone);
      }
      if (t.flower_color.x + t.flower_color.y + t.flower_color.z > 0.1f && c.g > 0.5f) {
        // A sunflower: its face (seeds) and petals, nodding south.
        const vec3 fc = base + v3(0, H + 0.02f, 0.06f), n = normalize(v3(0, 0.5f, 1));
        const float r = t.fruit_size;
        b.frustum(fc, fc + n * 0.05f, r, r * 0.9f, hi ? 10 : 6, t.fruit_color, bone, true, false);
        const int petals = hi ? 12 : (mid ? 8 : 0);
        const vec3 u = normalize(cross(n, v3(1, 0, 0))), w = cross(n, u);
        for (int k = 0; k < petals; ++k) {
          const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(petals);
          const vec3 d = u * foundry::det::cos(a) + w * foundry::det::sin(a);
          b.triangle(fc + d * (r * 0.9f) + n * 0.03f - cross(n, d) * 0.04f, fc + d * (r * 0.9f) + n * 0.03f + cross(n, d) * 0.04f,
                     fc + d * (r * 1.7f) + n * 0.03f, t.flower_color, bone, false, n);
        }
      } else if (fruit) {
        // Cobs in their husks, angled out from the stalk.
        for (int k = 0; k < 2; ++k) {
          const vec3 at = base + v3(0, H * (0.45f + 0.15f * static_cast<float>(k)), 0);
          const vec3 d = normalize(lean * (k ? -1.0f : 1.0f) + v3(0, 1.4f, 0));
          b.frustum(at, at + d * (t.fruit_size * 1.6f), t.fruit_size * 0.35f, t.fruit_size * 0.15f, 5, t.fruit_color, bone, true, true);
        }
        // The tassel.
        if (mid) blade(b, base + v3(0, H, 0), base + v3(0, H + 0.25f, 0) + lean * 0.05f, 0.05f, t.ripe_color, bone);
      }
      return H + 0.3f;
    }
    case crop_type::kKindLeafyHead: {
      // A heart (the head), its outer leaves splayed round it.
      const float r = t.fruit_size * (0.4f + 0.6f * c.g);
      ball(b, base + v3(0, r * 0.75f, 0), r, 0.75f, lerp(t.fruit_color, lv, c.ripe() ? 0.0f : 0.4f), bone, hi ? 8 : 6);
      const int n = hi ? 6 : (mid ? 4 : 0);
      for (int k = 0; k < n; ++k) {
        const float a = turn + 6.2831853f * static_cast<float>(k) / static_cast<float>(n);
        const vec3 d = v3(foundry::det::cos(a), 0.45f, foundry::det::sin(a));
        leaf(b, base + v3(0, r * 0.4f, 0) + v3(d.x, 0, d.z) * (r * 0.6f), normalize(d), r * 1.3f, r * 0.9f, r * 0.3f, lv, bone);
      }
      return r * 1.6f;
    }
    case crop_type::kKindRoot: {
      // Tops above ground: feathery fronds, a bushy haulm, or tubes; the root's shoulder when ripe.
      const int n = hi ? 5 : (mid ? 3 : 2);
      const bool tubes = t.fruit_color.x > 0.8f && t.fruit_color.y > 0.7f;  // onions: pale bulbs
      for (int k = 0; k < n; ++k) {
        const float a = turn + 6.2831853f * static_cast<float>(k) / static_cast<float>(n);
        const vec3 d = v3(foundry::det::cos(a) * 0.35f, 1, foundry::det::sin(a) * 0.35f);
        if (tubes) blade(b, base, base + normalize(d) * H, 0.025f, lv, bone);
        else blade(b, base, base + normalize(d) * H, 0.06f + 0.05f * t.fruit_size, lv, bone);
      }
      if (t.flower_color.x + t.flower_color.y + t.flower_color.z > 0.1f && c.g > 0.5f && mid)  // a potato's flowers
        b.box(base + v3(0, H, 0), v3(0.04f, 0.03f, 0.04f), t.flower_color, bone, false);
      if (fruit) ball(b, base + v3(0, 0.01f, 0), t.fruit_size * 0.5f, 0.5f, t.fruit_color, 0, 6);  // its shoulder, at the ground
      return H;
    }
    case crop_type::kKindGourd: {
      // A vine along the ground with broad leaves, and its gourds.
      const vec3 a = base, e = base + lean * (t.spacing * 0.6f);
      b.beam(a + v3(0, 0.03f, 0), e + v3(0, 0.03f, 0), 0.02f, t.stem_color, bone, false);
      const int n = hi ? 5 : (mid ? 3 : 2);
      for (int k = 0; k < n; ++k) {
        const vec3 p = a + (e - a) * (static_cast<float>(k) / static_cast<float>(n));
        const float la = turn + 1.7f * static_cast<float>(k);
        leaf(b, p + v3(0, 0.05f, 0), normalize(v3(foundry::det::cos(la), 0.9f, foundry::det::sin(la))), H * 1.4f, H * 1.2f, H * 0.25f, lv, bone);
      }
      if (fruit) {
        const vec3 at = a + (e - a) * 0.55f + v3(0, t.fruit_size * 0.5f, 0);
        ball(b, at, t.fruit_size * (0.6f + 0.4f * c.g), 0.7f, t.fruit_color, 0, hi ? 10 : 6);
        b.frustum(at + v3(0, t.fruit_size * 0.5f, 0), at + v3(0, t.fruit_size * 0.75f, 0), 0.025f, 0.02f, 4, t.stem_color, 0, false, true);
      }
      return H;
    }
    case crop_type::kKindPoleVine: {
      // A teepee of canes, the vine climbing them in leaves, its pods or fruit.
      for (int k = 0; k < 3; ++k) {
        const float a = turn + 2.094f * static_cast<float>(k);
        const vec3 foot = base + v3(foundry::det::cos(a), 0, foundry::det::sin(a)) * 0.22f;
        b.beam(foot, base + v3(0, t.height + 0.15f, 0), 0.012f, kCane, 0, true);
      }
      const int n = hi ? 10 : (mid ? 6 : 3);
      for (int k = 0; k < n; ++k) {
        const float y = H * (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
        const float a = turn + 2.4f * static_cast<float>(k);
        const vec3 at = base + v3(foundry::det::cos(a), 0, foundry::det::sin(a)) * (0.2f * (1.0f - y / (t.height + 0.15f))) + v3(0, y, 0);
        leaf(b, at, normalize(v3(foundry::det::cos(a), 0.3f, foundry::det::sin(a))), 0.26f, 0.2f, 0.06f, lv, bone);
        if (fruit && k % 2 == 1) {
          if (t.fruit_size > 0.06f) ball(b, at + v3(0, -0.06f, 0), t.fruit_size * 0.5f, 0.9f, t.fruit_color, bone, 6);
          else blade(b, at, at - v3(0, 0.16f, 0) + v3(foundry::det::cos(a), 0, foundry::det::sin(a)) * 0.04f, 0.035f, t.fruit_color, bone);
        }
      }
      return t.height + 0.15f;
    }
    case crop_type::kKindTrellisVine: {
      // A vine on the row's wires: a twisted trunk, a curtain of leaves, its bunches.
      b.frustum(base, base + v3(0, H * 0.55f, 0), 0.05f, 0.035f, 5, t.stem_color, 0, false, false);
      const int n = hi ? 8 : (mid ? 5 : 3);
      for (int k = 0; k < n; ++k) {
        const float x = (static_cast<float>(k) / static_cast<float>(n - 1) - 0.5f) * t.spacing * 0.9f;
        const float y = H * (0.55f + 0.4f * c.pick(id * 13u + static_cast<uint32_t>(k)));
        for (int s = -1; s <= 1; s += 2)
          leaf(b, base + v3(x, y, 0.02f * static_cast<float>(s)), normalize(v3(0, 0.3f, static_cast<float>(s))), 0.32f, 0.3f, 0.1f, lv, bone);
        if (fruit && k % 2 == 0) {
          const vec3 at = base + v3(x, H * 0.5f, 0.12f);
          b.frustum(at, at - v3(0, t.fruit_size * 1.6f, 0), t.fruit_size * 0.7f, 0.0f, 5, t.fruit_color, bone, true, false);
        }
      }
      return H;
    }
    default: {  // flowering bushes
      const float r = t.spacing * 0.42f * (0.5f + 0.5f * c.g);
      b.frustum(base, base + v3(0, H * 0.55f, 0), r * 0.7f, r, hi ? 6 : 5, lv, bone, false, false);
      b.frustum(base + v3(0, H * 0.55f, 0), base + v3(0, H * 0.7f, 0), r, r * 0.4f, hi ? 6 : 5, lv * 1.05f, bone, false, true);
      if (c.g > 0.5f && c.season != crop_spec::kSeasonWinter) {
        const int n = hi ? 7 : (mid ? 4 : 2);
        for (int k = 0; k < n; ++k) {
          const float a = 6.2831853f * c.pick(id * 17u + static_cast<uint32_t>(k)), rr = r * 0.85f * c.pick(id * 19u + static_cast<uint32_t>(k));
          const vec3 at = base + v3(foundry::det::cos(a) * rr, H * 0.62f, foundry::det::sin(a) * rr);
          b.frustum(at, at + v3(0, H * 0.4f, 0), 0.03f, 0.0f, 3, t.flower_color, bone, false, false);
        }
      }
      return H;
    }
  }
}

}  // namespace

bool run(const crop_spec &in, crop_model &out, std::string &error) {
  const crop_type &t = in.type;
  if (in.size > 2 || in.growth > crop_spec::kGrowthHarvested || in.season > 3 || in.detail > 2 || t.kind > crop_type::kKindFlowering) {
    error = "size, growth, season, detail, or the crop's kind out of range";
    return false;
  }
  out = crop_model();
  B b{out};
  b.winter = in.season == crop_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 8 : (in.detail == 1 ? 6 : 4);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_bone::kMotionFixed, 0, 0, 0, 0);
  const Lot lot = make_lot(kLotW[in.size], kLotH[in.size]);
  const uint32_t seed = hash_u32(in.seed * 2654435761u ^ hash_str(t.name.c_str()) ^ (in.size * 977u));
  const float growths[5] = {0.0f, 0.2f, 0.6f, 1.0f, 0.0f};
  Ctx c{b, t, in.growth, in.season, seed, growths[in.growth]};
  // The field: the lot's rectangle less a headland.
  const float x0 = lot.cx - lot.hx + 0.35f, x1 = lot.cx + lot.hx - 0.35f, z0 = lot.cz - lot.hz + 0.35f, z1 = lot.cz + lot.hz - 0.35f;
  const float rs = clampf(t.row_spacing, 0.3f, 3.0f), sp = clampf(t.spacing, 0.15f, 2.0f);
  const int rows = maxi_(1, static_cast<int>((z1 - z0) / rs));
  // The wind groups: drift bones, each its own rate and phase (bones never depend on detail).
  const uint32_t groups = in.wind_groups < 1 ? 1u : (in.wind_groups > 16 ? 16u : in.wind_groups);
  const float sway = clampf(t.sway, 0, 1);
  const bool standing = in.growth != crop_spec::kGrowthSown && in.growth != crop_spec::kGrowthHarvested;
  std::vector<uint32_t> bones;
  for (uint32_t g = 0; g < groups; ++g)
    bones.push_back(b.bone(0, v3(lot.cx, 0, lot.cz), v3(0, 1, 0), house_bone::kMotionDrift, 0.35f + 0.25f * c.pick(900 + g),
                           maxf(0.05f, t.height * 0.35f), standing ? 0.15f + sway * t.height * 0.5f : 0.0f, c.pick(950 + g)));

  // ---- the soil: ridged furrows along x ----
  const vec3 soil = in.growth == crop_spec::kGrowthSown ? kSoil * 0.92f : kSoil;
  b.block(x0 - 0.2f, x1 + 0.2f, -0.1f, 0.02f, z0 - 0.2f, z1 + 0.2f, soil * 0.85f, 0, true);
  for (int r = 0; r < rows; ++r) {
    const float z = z0 + rs * (static_cast<float>(r) + 0.5f) + ((z1 - z0) - rs * static_cast<float>(rows)) * 0.5f;
    ridge(b, x0, x1, z, minf(0.22f, rs * 0.35f), 0.1f, soil * (0.95f + 0.1f * static_cast<float>(r % 2)));
  }

  // ---- the plants ----
  uint32_t plants = 0, id = 0;
  const bool trellis = t.kind == crop_type::kKindTrellisVine;
  // A level of detail thins the plants along each row (every second at medium, every fourth at low),
  // never the rows; the trellis and its posts are always whole.
  const int thin = in.detail == 0 || trellis ? 1 : (in.detail == 1 ? 2 : 4);
  for (int r = 0; r < rows; ++r) {
    const float z = z0 + rs * (static_cast<float>(r) + 0.5f) + ((z1 - z0) - rs * static_cast<float>(rows)) * 0.5f;
    if (trellis && standing) {
      // Posts at the row's ends and every few metres, two wires between.
      for (float x = x0; x <= x1 + 1e-3f; x += maxf(2.5f, (x1 - x0) / 4.0f))
        b.beam(v3(x, -0.1f, z), v3(x, t.height + 0.1f, z), 0.04f, kCane * 0.8f, 0, true);
      for (float y : {t.height * 0.5f, t.height * 0.9f}) b.beam(v3(x0, y, z), v3(x1, y, z), 0.006f, v3(0.45f, 0.45f, 0.47f), 0, false);
    }
    const int n = maxi_(1, static_cast<int>((x1 - x0) / sp));
    for (int i = 0; i < n; ++i, ++id) {
      const float jitter = (c.pick(id * 3u + 11u) - 0.5f) * sp * 0.3f;
      const vec3 base = v3(x0 + sp * (static_cast<float>(i) + 0.5f) + jitter + ((x1 - x0) - sp * static_cast<float>(n)) * 0.5f, 0.06f, z + (c.pick(id * 3u + 12u) - 0.5f) * 0.06f);
      ++plants;
      if (!standing) continue;
      // At most about 48 plants drawn a row (fewer at coarser levels): a dense crop draws its rows'
      // plants as clumps, its count still every plant.
      const int step = maxi_(thin, (n + 47) / 48 * thin);
      if (i % step != 0) continue;
      const uint32_t bone = bones[hash_u32(id * 2654435761u ^ seed) % groups];
      const size_t v0 = out.positions.size();
      const float h = plant(c, base, bone, id);
      // Each vertex blends toward its wind group by its height in the plant: the root stays.
      for (size_t v = v0; v < out.positions.size(); ++v) {
        if (out.bone_a[v] == 0 && out.bone_b[v] == 0) continue;  // fixed parts: canes, gourds, roots
        const float w = clampf((out.positions[v].y - base.y) / maxf(h, 0.05f), 0, 1);
        out.bone_a[v] = 0;
        out.bone_b[v] = bone;
        out.bone_w[v] = w * w * (3 - 2 * w);
      }
    }
  }
  // Harvested: stubble rows, and stooks of sheaves for grain; bare soil otherwise.
  if (in.growth == crop_spec::kGrowthHarvested) {
    const bool grain = t.kind == crop_type::kKindGrain;
    for (int r = 0; r < rows && grain; ++r) {
      const float z = z0 + rs * (static_cast<float>(r) + 0.5f) + ((z1 - z0) - rs * static_cast<float>(rows)) * 0.5f;
      b.block(x0, x1, 0.06f, 0.14f, z - 0.06f, z + 0.06f, kStraw * 0.85f, 0, false);
    }
    const int stooks = grain ? 3 + static_cast<int>(in.size) * 3 : 0;
    for (int k = 0; k < stooks; ++k) {
      const vec3 at = v3(x0 + 0.6f + (x1 - x0 - 1.2f) * c.pick(700 + static_cast<uint32_t>(k)), 0, z0 + 0.6f + (z1 - z0 - 1.2f) * c.pick(720 + static_cast<uint32_t>(k)));
      for (int s = 0; s < 6; ++s) {
        const float a = 1.047f * static_cast<float>(s);
        const vec3 foot = at + v3(foundry::det::cos(a), 0, foundry::det::sin(a)) * 0.25f;
        b.frustum(foot, at + v3(0, 1.0f, 0), 0.1f, 0.05f, 4, kStraw * (0.9f + 0.15f * static_cast<float>(s % 2)), 0, false, true);
      }
      b.solid_cylinder(at, 0.35f, 1.0f);
    }
  }

  // ---- fire points: a standing or harvested field burns from points across it ----
  if (in.growth != crop_spec::kGrowthSown) {
    const int n = 4 + 3 * static_cast<int>(in.size);
    for (int k = 0; k < n; ++k) {
      const vec3 at = v3(x0 + (x1 - x0) * (0.1f + 0.8f * c.pick(800 + static_cast<uint32_t>(k))), 0.1f,
                         z0 + (z1 - z0) * (0.1f + 0.8f * c.pick(820 + static_cast<uint32_t>(k))));
      condition::fire(out, at, 4u, 0u);
    }
  }
  // ---- collision: the field, walked through ----
  // As tall as what grows on it (the conformance holds it).
  float grown = 0.1f;
  for (const vec3 &p : out.positions) grown = maxf(grown, p.y);
  b.solid(v3(lot.cx, grown * 0.5f, lot.cz), v3((x1 - x0) * 0.5f, grown * 0.5f, (z1 - z0) * 0.5f), 0, channels::physics_body::kPurposeQuery, "field");

  // ---- size, the lot, guides ----
  float top = 0, reach = 0, ground = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    const float d = foundry::det::hypot(p.x, p.z);
    reach = maxf(reach, d);
    if (p.y < 0.4f) ground = maxf(ground, d);
  }
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      const float r = sh.type == channels::physics_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
    }
  out.height = top;
  out.radius = reach;
  out.footprint = in.size;
  out.footprint_cells = lot.round;
  out.footprint_radius = ground;
  out.cells = std::vector<int32_t>(lot.cells.begin(), lot.cells.end());
  lot_box(lot, top, out.lot_min, out.lot_max);
  for (uint32_t s = 0; s < kLotSizes; ++s) {
    const Lot other = make_lot(kLotW[s], kLotH[s]);
    for (size_t k = 0; k + 1 < other.cells.size(); k += 2) {
      out.guide_cells.push_back(static_cast<int32_t>(s));
      out.guide_cells.push_back(other.cells[k]);
      out.guide_cells.push_back(other.cells[k + 1]);
    }
    out.guide_tones.push_back(s == in.size ? 0u : 2u);
  }
  out.plants = plants;
  return true;
}

}  // namespace crop_build

FOUNDRY_SYSTEM(crop_build, channels::crop_spec, channels::crop_model)
