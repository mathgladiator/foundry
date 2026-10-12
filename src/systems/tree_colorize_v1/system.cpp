// tree_colorize_v1: colours a tree's body from its genome (design/systems.md,
// Coloring). Flat palette regions in the house style (style.md section 5):
// low-contrast bark relief, per-clump leaf variety, seasonal leaves, blossom,
// ripening fruit, moss on the up-facing bark of old trees, and snow settling on
// upward surfaces in winter. No baked light.
#include <vector>

#include "boundary.h"
#include "channels/tree_v1_body.h"
#include "channels/tree_v1_model.h"
#include "math.h"
#include "packer/tree_v1_body.h"
#include "packer/tree_v1_model.h"

namespace tree_colorize_v1 {

using namespace tree_math;
using channels::tree_v1_genome;
using channels::tree_v1_spec;

namespace {

enum Region : uint32_t { kBark = 0, kWood = 1, kLeaf = 2, kBlossom = 3, kFruit = 4, kDeadLeaf = 5, kDeadWood = 6 };

const vec3 kMoss = {0.34f, 0.43f, 0.20f};
const vec3 kSnow = {0.90f, 0.93f, 0.97f};

vec3 mixv(vec3 a, vec3 b, float t) { return a + (b - a) * t; }

uint32_t pack(vec3 c) {
  auto byte = [](float v) {
    v = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(v);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

vec3 season_leaf(const tree_v1_genome &g, uint32_t season, float shade) {
  switch (season) {
    case tree_v1_spec::kSeasonWinter: return g.leaf_winter;
    case tree_v1_spec::kSeasonSpring: return g.leaf_spring;
    case tree_v1_spec::kSeasonFall: return shade < 0.5f ? g.leaf_fall : g.leaf_fall_alt;
    default: return g.leaf_summer;
  }
}

}  // namespace

bool run(const channels::tree_v1_body &in, channels::tree_v1_model &out, std::string &error) {
  const size_t n = in.positions.size();
  if (in.normals.size() != n || in.region.size() != n || in.shade.size() != n || in.up.size() != n ||
      in.bone_a.size() != n || in.bone_b.size() != n || in.bone_w.size() != n) {
    error = "every per-vertex array must have one value per position";
    return false;
  }
  const tree_v1_spec &spec = in.spec;
  const tree_v1_genome &g = spec.genome;
  const uint32_t seed = hash_str(g.name.c_str()) ^ hash_u32(spec.seed);
  const float moss = spec.age == tree_v1_spec::kAgeAncient ? 0.65f : (spec.age == tree_v1_spec::kAgeMature ? 0.25f : 0.0f);
  const bool winter = spec.season == tree_v1_spec::kSeasonWinter;
  const float snow_from = 1.0f - 0.55f * g.snow;  // surfaces facing up more than this take snow

  out.height = in.height;
  out.stump_radius = in.stump_radius;
  out.crown_base = in.crown_base;
  out.bones = in.bones;
  out.pieces = in.pieces;
  out.positions = in.positions;
  out.normals = in.normals;
  out.indices = in.indices;
  out.bone_a = in.bone_a;
  out.bone_b = in.bone_b;
  out.bone_w = in.bone_w;
  out.colors.resize(n);
  for (size_t k = 0; k < n; ++k) {
    const float shade = in.shade[k], up = in.up[k];
    const vec3 p = in.positions[k];
    vec3 c;
    bool can_snow = true;
    switch (in.region[k]) {
      case kBark: {
        c = mixv(g.bark_dark, g.bark_light, 0.15f + 0.7f * shade);
        if (moss > 0 && up > 0.3f && noise(p * 2.5f, seed) < moss * (0.4f + up)) c = mixv(c, kMoss, 0.75f);
        break;
      }
      case kWood:
        c = g.wood * (0.82f + 0.18f * shade);
        can_snow = false;
        break;
      case kLeaf: {
        c = season_leaf(g, spec.season, shade) * (0.9f + 0.2f * shade);
        break;
      }
      case kBlossom:
        c = mixv(g.blossom_color, v3(1, 1, 1), 0.12f * shade) * (0.94f + 0.08f * shade);
        break;
      case kFruit: {
        vec3 ripe = g.fruit_ripe, unripe = g.fruit_unripe;
        if (spec.season == tree_v1_spec::kSeasonFall) c = shade < 0.2f ? mixv(unripe, ripe, 0.5f) : ripe;
        else c = shade < 0.85f ? unripe : mixv(unripe, ripe, 0.4f);
        can_snow = false;
        break;
      }
      case kDeadLeaf:
        c = g.leaf_winter * (0.9f + 0.2f * shade);
        break;
      case kDeadWood:
        c = mixv(mixv(g.bark_dark, g.bark_light, 0.2f + 0.6f * shade), v3(0.62f, 0.60f, 0.56f), 0.5f);
        break;
      default:
        c = v3(1, 0, 1);
        break;
    }
    if (winter && g.snow > 0 && can_snow && up > snow_from) c = kSnow;
    out.colors[k] = pack(c);
  }
  return true;
}

}  // namespace tree_colorize_v1

FOUNDRY_SYSTEM(tree_colorize_v1, channels::tree_v1_body, channels::tree_v1_model)
