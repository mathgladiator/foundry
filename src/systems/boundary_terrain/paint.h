// The images a boundary's terrain nodes carry (design/boundary.md): every texel's colour from the
// land under it (interpolated from the node's grid) and patterns finer than the grid, painted per
// texel: crowns in the canopy, strata in the rock, a valley's patchwork of fields and hedges, its
// river, beaches, surf, snow by height and season, and relief from a fine bump.
#pragma once

#include "field.h"

namespace boundary_terrain {

struct Painter {
  const World &world;
  const boundary_biome &b;

  enum Season { kWinter = 0, kSpring, kSummer, kFall };

  // A crown's colour by the biome's forest form and the season; jungle crowns their own.
  vec3 crown(uint32_t h, float jungle, float woods) const {
    const uint32_t season = world.season;
    const float pick = unit(h);
    vec3 c = b.foliage;
    uint32_t form = b.forest_form;
    if (woods > 0.5f && form == channels::boundary_biome::kForestFormDead) form = channels::boundary_biome::kForestFormMixed;
    bool conifer = form == channels::boundary_biome::kForestFormConifer ||
                   (form == channels::boundary_biome::kForestFormMixed && pick < 0.45f);
    if (conifer) {
      c = b.foliage_dark * (0.85f + 0.3f * unit(hash_u32(h)));
      if (season == kWinter) c = lerp(c, b.snow, 0.35f);
    } else if (form == channels::boundary_biome::kForestFormDead) {
      c = lerp(b.trunk, b.rock, 0.4f) * (0.8f + 0.3f * pick);
    } else {
      if (form == channels::boundary_biome::kForestFormBirch) c = lerp(b.foliage, v3(0.75f, 0.8f, 0.35f), 0.25f);
      c = c * (0.85f + 0.3f * unit(hash_u32(h ^ 0x51u)));
      if (season == kSpring) c = lerp(c, v3(0.62f, 0.78f, 0.32f), 0.3f);
      if (season == kFall) c = lerp(c, b.autumn * (0.8f + 0.4f * unit(hash_u32(h ^ 0x77u))), 0.35f + 0.6f * pick);
      if (season == kWinter) c = lerp(lerp(b.trunk, b.rock, 0.45f), b.snow, 0.25f);
    }
    if (jungle > 0.5f) c = b.jungle * (0.8f + 0.4f * unit(hash_u32(h ^ 0x33u)));
    return c;
  }

  // The colour of the texel at (x, z): the influence and land there, the fine surface's upward
  // normal component, and the texel's size in metres.
  vec3 colour(float x, float z, const Influence &f, const Land &l, float ny, float texel) const {
    const uint32_t seed = world.seed;
    const uint32_t season = world.season;
    const float slope = 1 - clampf(ny, 0, 1);
    const float above = l.ground - f.h0;
    // Ground cover: grass, drier in patches.
    const float var = fbm(x / 80.0f, z / 80.0f, 3, seed + 101);
    vec3 c = lerp(b.grass, b.grass_dry, step_between(0.55f, 0.75f, var) * 0.7f);
    if (season == kSpring) c = lerp(c, v3(0.5f, 0.72f, 0.3f), 0.2f);
    if (season == kFall) c = lerp(c, b.grass_dry, 0.35f);
    c = c * (0.9f + 0.2f * fbm(x / 11.0f, z / 11.0f, 2, seed + 109));
    // A farmed valley floor: fields in the cells of a jittered grid, hedges along their borders.
    const float farmed = l.valley * clampf(b.fields, 0, 1) * (1 - step_between(0.2f, 0.4f, slope));
    if (farmed > 0.01f && texel < 60.0f) {
      const Cell2 cc = cells2(x / 110.0f, z / 110.0f, seed + 103);
      const float pick = unit(cc.id);
      vec3 field = pick < 0.25f ? b.grass * 0.92f
                   : pick < 0.45f ? b.grass_dry
                   : pick < 0.6f ? b.soil * 1.05f
                   : pick < 0.8f ? lerp(b.grass_dry, b.sand, 0.35f)
                                 : lerp(b.grass, b.foliage, 0.5f);
      if (season == kWinter) field = lerp(field, b.soil, 0.5f);
      if (season == kSpring && pick >= 0.45f && pick < 0.6f) field = lerp(b.soil, b.grass, 0.4f);
      // Furrows across each field, its own way.
      const float turn = unit(hash_u32(cc.id)) * kPi;
      const float along = x * foundry::det::cos(turn) + z * foundry::det::sin(turn);
      const float furrow = texel < 4.0f ? 0.94f + 0.06f * foundry::det::sin(along * 2.2f) : 1.0f;
      field = field * furrow;
      const float hedge = (1 - step_between(0.03f, 0.08f, cc.d2 - cc.d1)) * (1 - step_between(20, 60, texel));
      field = lerp(field, b.foliage_dark * 0.8f, hedge * 0.8f);
      c = lerp(c, field, farmed);
    }
    if (l.river > 0.01f) c = lerp(c, b.water_shallow * 0.9f, clampf(l.river, 0, 1));
    if (l.road > 0.01f) c = lerp(c, lerp(b.soil, b.grass_dry, 0.25f), clampf(l.road, 0, 1));  // a road carried out
    // Beaches: sand from the water's edge up a metre or two.
    if (l.water > -1e8f) {
      const float shore = 1 - step_between(0.6f, 2.0f, l.ground - l.water);
      c = lerp(c, f.w[kMarsh] > f.w[kSea] ? lerp(b.soil, b.reed, 0.5f) : b.sand, shore);
    }
    if (f.w[kMarsh] > 0.2f) {
      const float reeds = step_between(0.45f, 0.6f, fbm(x / 9.0f, z / 9.0f, 2, seed + 113)) * f.w[kMarsh];
      c = lerp(c, b.reed, reeds * 0.8f);
    }
    // Rock on steep ground and above the trees, in strata.
    float rock = step_between(0.22f, 0.42f, slope);
    rock = maxf(rock, step_between(b.tree_line * 0.9f, b.tree_line * 1.3f, above) * 0.8f);
    if (rock > 0.01f) {
      const float band = l.ground / 6.5f + fbm(x / 30.0f, z / 30.0f, 2, seed + 127) * 0.9f;
      const float frac = band - std::floor(band);
      vec3 rc = frac < 0.3f ? b.rock_dark : b.rock;
      rc = rc * (0.88f + 0.24f * fbm(x / 9.0f, z / 9.0f, 2, seed + 131));
      c = lerp(c, rc, rock);
    }
    // Snow: above the snow line on ground not too steep to hold it, and everywhere in winter.
    float snow = 0;
    if (b.snow_line > 0) {
      const float jitter = (fbm(x / 120.0f, z / 120.0f, 2, seed + 137) - 0.5f) * 80.0f;
      snow = step_between(b.snow_line - 30 + jitter, b.snow_line + 30 + jitter, above);
    }
    if (season == kWinter) snow = maxf(snow, 0.85f);
    snow *= 1 - step_between(0.45f, 0.7f, slope);
    c = lerp(c, b.snow, snow);
    // The canopy: crowns, with dark gaps between them where they show.
    if (l.canopy > 0.5f) {
      const float size = l.jungle > 0.5f ? 6.0f : 4.5f;
      const float show = 1 - step_between(size * 0.5f, size * 1.6f, texel);
      const Cell2 cc = cells2(x / size, z / size, seed + 107);
      const vec3 cr = crown(cc.id, l.jungle, l.woods);
      const vec3 lit = cr * clampf(0.72f + 0.45f * (1 - cc.d1), 0.6f, 1.15f);
      const float gap = step_between(0.5f, 0.75f, cc.d1);
      const vec3 pattern = lerp(lit, b.foliage_dark * 0.45f, gap * 0.8f);
      // Far away, the crowns' average (a forest's mean colour, with its shade).
      const Cell2 big = cells2(x / 40.0f, z / 40.0f, seed + 139);
      const vec3 avg = crown(big.id, l.jungle, l.woods) * 0.82f;
      const vec3 cc_final = lerp(avg, pattern, show);
      c = lerp(c, cc_final, step_between(0.5f, 3.0f, l.canopy));
    }
    // Water, by depth, with surf at its edge.
    if (l.water > l.ground) {
      const float depth = l.water - l.ground;
      vec3 wc = lerp(b.water_shallow, b.water_deep, step_between(0, 20, depth));
      wc = wc * (0.95f + 0.1f * fbm(x / 60.0f, z / 60.0f, 2, seed + 167));  // swells
      wc = lerp(wc, b.reed * 0.5f, f.w[kMarsh] * 0.35f);
      if (season == kWinter && f.w[kMarsh] > 0.3f) wc = lerp(wc, b.snow * 0.85f, 0.6f);
      const float surf = (1 - step_between(0.05f, 0.35f, depth)) * (1 - step_between(4, 16, texel));
      wc = lerp(wc, v3(0.93f, 0.95f, 0.95f), surf * 0.7f);
      return wc;
    }
    // Relief: the fine surface's shading, so a far slope keeps its folds.
    c = c * (0.78f + 0.22f * ny * ny);
    return c;
  }
};

}  // namespace boundary_terrain
