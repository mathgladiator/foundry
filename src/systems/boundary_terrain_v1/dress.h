// The rim's dressing (design/boundary.md): what stands along the rim, close enough to the player that
// a height grid and an image can't carry it. A ridge's cliff face rising in strata over boulders; a
// ledge's lip of stones and its face dropping to the valley; a forest's or jungle's wall of trunks,
// crowns, and undergrowth (broadleaf, conifer, birch, dead, palm, buttressed, mangrove); a
// mountain's scree; a marsh's reeds, cattails, snags, and lily pads; a beach's rocks, driftwood,
// and surf. Every item stands on the land the terrain draws.
#pragma once

#include <string>
#include <vector>

#include "channels/boundary_v1_figure.h"
#include "field.h"
#include "paint.h"

namespace boundary_terrain_v1 {

inline uint32_t pack_colour(vec3 c) {
  auto byte = [](float v) {
    const float s = clampf(v, 0, 1) * 255.0f + 0.5f;
    return static_cast<uint32_t>(s);
  };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}

// A vertex-coloured mesh being built.
struct Mesh {
  std::vector<vec3> &pos, &nrm;
  std::vector<uint32_t> &col, &idx;

  uint32_t vertex(vec3 p, vec3 n, vec3 c) {
    pos.push_back(p);
    nrm.push_back(n);
    col.push_back(pack_colour(c));
    return static_cast<uint32_t>(pos.size() - 1);
  }
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    idx.push_back(a);
    idx.push_back(b);
    idx.push_back(c);
  }
  // One flat triangle (two-sided in the shader).
  void face(vec3 a, vec3 b, vec3 c, vec3 col) {
    const vec3 n = normalize(cross(b - a, c - a));
    // One vertex per statement: argument order is unspecified (GCC and Clang differ).
    const uint32_t ia = vertex(a, n, col);
    const uint32_t ib = vertex(b, n, col);
    const uint32_t ic = vertex(c, n, col);
    tri(ia, ib, ic);
  }
  // A tapered tube from a to b, its sides smooth, open at both ends.
  void frustum(vec3 a, vec3 b, float r0, float r1, int sides, vec3 col, float turn = 0) {
    const vec3 ax = normalize(b - a), u = perp(ax), v = cross(ax, u);
    const uint32_t first = static_cast<uint32_t>(pos.size());
    for (int i = 0; i < sides; ++i) {
      const float t = turn + 2 * kPi * static_cast<float>(i) / static_cast<float>(sides);
      const vec3 dir = u * foundry::det::cos(t) + v * foundry::det::sin(t);
      vertex(a + dir * r0, dir, col * 0.9f);
      vertex(b + dir * r1, dir, col);
    }
    for (int i = 0; i < sides; ++i) {
      const uint32_t i0 = first + 2 * static_cast<uint32_t>(i), i1 = first + 2 * static_cast<uint32_t>((i + 1) % sides);
      tri(i0, i1, i0 + 1);
      tri(i1, i1 + 1, i0 + 1);
    }
  }
  // A cone standing on base, pointing along +y.
  void cone(vec3 base, float h, float r, int sides, vec3 col, float turn = 0) {
    const uint32_t tip = vertex(base + v3(0, h, 0), v3(0, 1, 0), col * 1.1f);
    const uint32_t first = static_cast<uint32_t>(pos.size());
    for (int i = 0; i < sides; ++i) {
      const float t = turn + 2 * kPi * static_cast<float>(i) / static_cast<float>(sides);
      const vec3 dir = v3(foundry::det::cos(t), 0, foundry::det::sin(t));
      vertex(base + dir * r, normalize(dir * h + v3(0, r, 0)), col * 0.85f);
    }
    for (int i = 0; i < sides; ++i) tri(first + static_cast<uint32_t>(i), first + static_cast<uint32_t>((i + 1) % sides), tip);
  }
  // A lumpy blob (an octahedron cut once, 32 faces): a crown, a bush, a boulder.
  void blob(vec3 c, vec3 r, vec3 col, uint32_t h, float lump, float flat_bottom = -2.0f) {
    static const vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const int faces[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    const uint32_t first = static_cast<uint32_t>(pos.size());
    // The six corners, then the twelve edge midpoints, each pushed out by its own lump.
    vec3 pts[18];
    int count = 0;
    for (int i = 0; i < 6; ++i) pts[count++] = dirs[i];
    int mid[6][6];
    for (int i = 0; i < 6; ++i)
      for (int j = 0; j < 6; ++j) mid[i][j] = -1;
    for (const auto &f : faces)
      for (int e = 0; e < 3; ++e) {
        const int a = f[e], b = f[(e + 1) % 3];
        if (mid[a][b] < 0) {
          pts[count] = normalize(dirs[a] + dirs[b]);
          mid[a][b] = mid[b][a] = count++;
        }
      }
    for (int i = 0; i < count; ++i) {
      const float k = 1 + lump * (unit(hash_u32(h + static_cast<uint32_t>(i) * 977u)) - 0.5f) * 2;
      vec3 p = v3(pts[i].x * r.x, pts[i].y * r.y, pts[i].z * r.z) * k;
      if (pts[i].y < flat_bottom) p.y = r.y * flat_bottom;
      const float shade = 0.82f + 0.3f * (pts[i].y * 0.5f + 0.5f);
      vertex(c + p, normalize(v3(pts[i].x / r.x, pts[i].y / r.y, pts[i].z / r.z)), col * shade);
    }
    for (const auto &f : faces) {
      const uint32_t a = static_cast<uint32_t>(f[0]), b = static_cast<uint32_t>(f[1]), d = static_cast<uint32_t>(f[2]);
      const uint32_t ab = static_cast<uint32_t>(mid[f[0]][f[1]]), bd = static_cast<uint32_t>(mid[f[1]][f[2]]),
                     da = static_cast<uint32_t>(mid[f[2]][f[0]]);
      tri(first + a, first + ab, first + da);
      tri(first + ab, first + b, first + bd);
      tri(first + da, first + bd, first + d);
      tri(first + ab, first + bd, first + da);
    }
  }
  // A thin blade: a reed, a frond's segment.
  void blade(vec3 base, vec3 tip, float w, vec3 col) {
    vec3 side = cross(normalize(tip - base), v3(0, 1, 0));
    if (length(side) < 1e-3f) side = v3(1, 0, 0);
    side = normalize(side) * w;
    face(base - side, base + side, tip, col);
  }

 private:
  static vec3 perp(vec3 v) {
    const vec3 ref = (v.y < 0.9f && v.y > -0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
    return normalize(cross(v, ref));
  }
};

// Inverse of smoothstep on [0, 1], by Newton's method (for faces that hug a smoothstep profile).
inline float unsmooth(float t) {
  t = clampf(t, 0, 1);
  float u = t;
  for (int i = 0; i < 6; ++i) {
    const float f = u * u * (3 - 2 * u) - t, df = 6 * u * (1 - u);
    if (df < 1e-4f) break;
    u = clampf(u - f / df, 0, 1);
  }
  return u;
}

struct Dresser {
  Mesh &m;
  const RimIndex &rim;
  const World &world;
  const Painter &paint;
  const boundary_v1_biome &b;
  uint32_t detail = 0;  // 0 high, 1 medium, 2 low
  uint32_t seed = 1;
  std::vector<vec3> fires;
  uint32_t blazes = 0;  // trees in the third row so far (every sixth holds a blaze point)
  std::vector<channels::boundary_v1_figure> figures;  // trees and bushes left for the game

  // Within the figure range a tree or bush is a figure for the game to fill, not the dressing's.
  bool figure_here(float d) const { return world.figure_range > 0 && d < world.figure_range; }
  // A biome's species list (comma-separated), one picked by a hash; empty: none.
  static std::string pick(const std::string &list, uint32_t h) {
    std::vector<std::string> names;
    std::string cur;
    for (char c : list + ",") {
      if (c == ',') {
        if (!cur.empty()) names.push_back(cur);
        cur.clear();
      } else if (c != ' ') {
        cur += c;
      }
    }
    return names.empty() ? std::string() : names[h % names.size()];
  }
  // A figure: kind 0 tree, 1 bush; ages mostly mature, some young and some ancient (a bush's 0 to 2).
  bool figure(uint32_t kind, const std::string &list, vec3 at, float height, Rng &rng) {
    const uint32_t h = rng.next();
    const std::string species = pick(list, h);
    if (species.empty()) return false;
    channels::boundary_v1_figure f;
    f.position = at;
    f.yaw = rng.uniform() * 360.0f;
    f.kind = kind;
    f.species = species;
    const float a = rng.uniform();
    f.age = kind == 0 ? (a < 0.15f ? 1u : (a < 0.8f ? 2u : 3u)) : (a < 0.25f ? 0u : (a < 0.85f ? 1u : 2u));
    f.height = height;
    f.seed = rng.next();
    figures.push_back(f);
    return true;
  }
  bool in_corridor(vec3 p, float margin) const { return world.corridor(p.x, p.z, margin) > 0.2f; }

  float density() const { return detail == 0 ? 1.0f : (detail == 1 ? 0.55f : 0.3f); }

  // The land at a point and where the ground is.
  Land land_at(vec3 p, Influence &f) const {
    f = rim.at(p.x, p.z);
    return world.land(p.x, p.z, f);
  }
  vec3 tangent(size_t k) const {
    const size_t n = rim.samples();
    vec3 t = rim.point((k + 1) % n) - rim.point((k + n - 1) % n);
    t.y = 0;
    return normalize(t);
  }

  vec3 rock_colour(float y, float x, float z) const {
    const float band = y / 6.5f + fbm(x / 30.0f, z / 30.0f, 2, seed + 127) * 0.9f;
    const float frac = band - std::floor(band);
    vec3 c = frac < 0.3f ? b.rock_dark : b.rock;
    return c * (0.88f + 0.24f * noise2(x * 0.7f, y * 0.9f + z * 0.7f, seed + 151));
  }

  // A cliff face along the rim where an edge has one: up (a ridge, in front of the rising ground) or
  // down (a ledge, beyond the falling ground). Columns every `step` samples, rows in strata.
  void faces(int e, bool up) {
    const size_t n = rim.samples();
    const size_t step = detail == 0 ? 1 : (detail == 1 ? 2 : 3);
    const float tall = up ? b.cliff_height * 1.2f : b.ledge_drop;
    const int rows = static_cast<int>(clampf(tall / (up ? 1.6f : 4.0f) / (detail == 0 ? 1.0f : 1.6f), 3, 60));
    const float W = up ? 3.6f : 4.0f + 0.04f * b.ledge_drop;  // the ground's profile's width
    std::vector<int> column(n, -1);  // the first vertex of each column, or -1
    for (size_t k = 0; k < n; k += step) {
      if (rim.weight(k, e) < 0.12f) continue;
      const vec3 p0 = rim.point(k), out = rim.outward(k);
      if (in_corridor(p0 + out * 2.0f, 3)) continue;  // a road or river carried out cuts through
      Influence f;
      const float far_d = up ? 5.2f : (0.85f + W + 2.0f);
      const vec3 q = p0 + out * far_d;
      const Land l = land_at(q, f);
      const float span = up ? l.ground - p0.y : p0.y - l.ground;
      if (span < 1.5f) continue;
      column[k] = static_cast<int>(m.pos.size());
      for (int r = 0; r <= rows; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(rows);
        const float u = unsmooth(t);
        const float jag = (noise2(static_cast<float>(k) * 0.45f, static_cast<float>(r) * 0.7f, seed + 157 + static_cast<uint32_t>(e)) - 0.5f);
        float d, y;
        if (up) {
          d = 0.15f + u * W * 0.95f + jag * 0.45f;
          y = p0.y - 0.4f + t * (span + 0.4f);
          if (r == rows) d = far_d;  // the lip meets the plateau
        } else {
          d = 0.85f + u * W + 0.4f + jag * 1.2f;
          y = p0.y - t * span;
          if (r == 0) d = 0.8f;
          if (r == rows) y -= 1.0f;  // into the valley floor
        }
        // Ledges in the strata: every few rows the face steps back.
        if (r > 0 && r < rows && (hash_u32(static_cast<uint32_t>(r) * 7919u + seed) & 3u) == 0) d += up ? 0.35f : -0.35f;
        const vec3 v = v3(p0.x + out.x * d, y, p0.z + out.z * d);
        m.vertex(v, out * -1.0f, rock_colour(y, v.x, v.z));
      }
    }
    // Stitch neighbouring columns (around the loop too).
    std::vector<size_t> ks;
    for (size_t k = 0; k < n; k += step)
      if (column[k] >= 0) ks.push_back(k);
    for (size_t i = 0; i < ks.size(); ++i) {
      const size_t ka = ks[i], kb = ks[(i + 1) % ks.size()];
      if ((kb + n - ka) % n != step || ks.size() < 2) continue;
      const uint32_t a0 = static_cast<uint32_t>(column[ka]), b0 = static_cast<uint32_t>(column[kb]);
      for (int r = 0; r < rows; ++r) {
        const uint32_t a = a0 + static_cast<uint32_t>(r), bb = b0 + static_cast<uint32_t>(r);
        if (up) {
          m.tri(a, bb, a + 1);
          m.tri(bb, bb + 1, a + 1);
        } else {
          m.tri(a, a + 1, bb);
          m.tri(bb, a + 1, bb + 1);
        }
      }
    }
    // Normals from the grid: across the columns and up the rows.
    for (size_t k = 0; k < n; k += step) {
      if (column[k] < 0) continue;
      const size_t kn = (k + step) % n, kp = (k + n - step) % n;
      for (int r = 0; r <= rows; ++r) {
        const uint32_t i = static_cast<uint32_t>(column[k] + r);
        const vec3 here = m.pos[i];
        const vec3 next = column[kn] >= 0 ? m.pos[static_cast<uint32_t>(column[kn] + r)] : here + tangent(k);
        const vec3 prevp = column[kp] >= 0 ? m.pos[static_cast<uint32_t>(column[kp] + r)] : here - tangent(k);
        const vec3 above = r < rows ? m.pos[i + 1] : here;
        const vec3 below = r > 0 ? m.pos[i - 1] : here;
        vec3 nrm = normalize(cross(above - below, next - prevp));
        // Face the inside for a ridge (the player looks at it), outward for a ledge.
        const vec3 out = rim.outward(k);
        if ((dot(nrm, out) > 0) == up) nrm = -nrm;
        m.nrm[i] = nrm;
      }
    }
  }

  // Boulders: scree under a ridge, on a mountain's slope, along a ledge's lip, on a beach.
  void boulders() {
    const size_t n = rim.samples();
    for (size_t k = 0; k < n; ++k) {
      Rng rng(seed ^ static_cast<uint32_t>(k * 2654435761u), 401);
      const float wr = rim.weight(k, kRidge), wm = rim.weight(k, kMountain), wl = rim.weight(k, kLedge), ws = rim.weight(k, kSea);
      const float chance = (wr * 0.35f + wm * 1.1f + wl * 0.45f + ws * 0.08f) * density();
      float c = rng.uniform();
      int count = 0;
      while (c < chance && count < 3) {
        ++count;
        c += 1.0f;
        const float pick = rng.uniform();
        float d, size;
        if (pick < wl / maxf(1e-3f, wr + wm + wl + ws)) {
          d = 0.55f + 0.25f * rng.uniform();
          size = 0.18f + 0.4f * rng.uniform();
        } else if (pick < (wl + wr) / maxf(1e-3f, wr + wm + wl + ws)) {
          d = 0.3f + 1.2f * rng.uniform();
          size = 0.3f + 0.9f * rng.uniform();
        } else if (pick < (wl + wr + wm) / maxf(1e-3f, wr + wm + wl + ws)) {
          const float a = rng.uniform();
          d = 0.3f + 14.0f * a * a;
          size = 0.35f + 1.2f * rng.uniform() * (1.2f - a);
        } else {
          d = 1.0f + 5.0f * rng.uniform();
          size = 0.2f + 0.6f * rng.uniform();
        }
        const float slide = rng.signed_unit() * 0.5f;
        const vec3 p = rim.point(k) + rim.outward(k) * d + tangent(k) * slide;
        if (in_corridor(p, 2)) continue;
        Influence f;
        const Land l = land_at(p, f);
        if (l.water > l.ground + 0.3f) continue;
        const vec3 at = v3(p.x, l.ground + size * 0.25f, p.z);
        const vec3 col = lerp(b.rock, b.rock_dark, rng.uniform() * 0.6f);
        const float wide = size * (0.9f + 0.4f * rng.uniform());
        const float tall = size * (0.6f + 0.3f * rng.uniform());
        const uint32_t lumps = rng.next();
        m.blob(at, v3(wide, tall, size), col, lumps, 0.35f, -0.45f);
      }
    }
  }

  // A tree of the biome's forest (or its jungle).
  void tree(vec3 base, float height, bool jungle, uint32_t h, bool trunk_seen) {
    Rng rng(h, 409);
    const uint32_t season = world.season;
    const vec3 bark = b.trunk * (0.85f + 0.3f * rng.uniform());
    const float turn = rng.uniform() * 6.28f;
    if (jungle) {
      const uint32_t form = b.jungle_form;
      const vec3 leaf = b.jungle * (0.8f + 0.4f * rng.uniform());
      if (form == channels::boundary_v1_biome::kJungleFormPalm || (form == channels::boundary_v1_biome::kJungleFormBroadleaf && rng.uniform() < 0.25f)) {
        // A palm: a curved trunk and a crown of drooping fronds.
        const float lean = 0.25f + 0.35f * rng.uniform();
        const vec3 dir = v3(foundry::det::cos(turn), 0, foundry::det::sin(turn));
        vec3 a = base;
        const int segs = 4;
        for (int s = 1; s <= segs; ++s) {
          const float t = static_cast<float>(s) / segs;
          const vec3 bpt = base + v3(0, height * t, 0) + dir * (height * lean * t * t * 0.5f);
          m.frustum(a, bpt, 0.22f - 0.07f * t + 0.05f, 0.2f - 0.07f * t, trunk_seen ? 5 : 4, bark);
          a = bpt;
        }
        const int fronds = detail == 2 ? 5 : 8;
        for (int i = 0; i < fronds; ++i) {
          const float ang = turn + 6.28f * static_cast<float>(i) / static_cast<float>(fronds) + rng.signed_unit() * 0.2f;
          const vec3 fd = v3(foundry::det::cos(ang), 0, foundry::det::sin(ang));
          const float len = height * (0.28f + 0.1f * rng.uniform());
          vec3 p = a;
          for (int sgm = 1; sgm <= 3; ++sgm) {
            const float t = static_cast<float>(sgm) / 3.0f;
            const vec3 q = a + fd * (len * t) + v3(0, len * (0.25f * t - 0.55f * t * t), 0);
            m.blade(p, q, 0.35f * (1.1f - t * 0.6f), leaf * (0.9f + 0.1f * t));
            p = q;
          }
        }
        return;
      }
      if (form == channels::boundary_v1_biome::kJungleFormMangrove) {
        // Stilt roots arching down from the trunk, and a broad crown.
        const vec3 hip = base + v3(0, 1.4f, 0);
        for (int i = 0; i < 5; ++i) {
          const float ang = turn + 1.26f * static_cast<float>(i);
          const vec3 foot = base + v3(foundry::det::cos(ang) * 1.3f, -0.2f, foundry::det::sin(ang) * 1.3f);
          m.frustum(hip, foot, 0.07f, 0.06f, 4, bark);
        }
        m.frustum(hip, base + v3(0, height * 0.7f, 0), 0.2f, 0.12f, 5, bark);
        m.blob(base + v3(0, height * 0.72f, 0), v3(height * 0.32f, height * 0.18f, height * 0.32f), leaf, rng.next(), 0.3f);
        return;
      }
      // A buttressed giant: fins at its foot, a tall bare trunk, a flat wide crown, and vines.
      const float top = height * 1.0f;
      m.frustum(base, base + v3(0, top * 0.8f, 0), 0.4f, 0.22f, trunk_seen ? 6 : 4, bark);
      if (trunk_seen) {
        for (int i = 0; i < 3; ++i) {
          const float ang = turn + 2.09f * static_cast<float>(i);
          const vec3 out = v3(foundry::det::cos(ang), 0, foundry::det::sin(ang));
          m.face(base + out * 1.4f, base + out * 0.2f + v3(0, 2.6f, 0), base + out * 0.2f, bark * 0.9f);
          m.face(base + out * 0.2f, base + out * 0.2f + v3(0, 2.6f, 0), base + out * 1.4f, bark * 0.9f);
        }
      }
      const vec3 crown_at = base + v3(0, top * 0.82f, 0);
      m.blob(crown_at, v3(top * 0.34f, top * 0.13f, top * 0.3f), leaf, rng.next(), 0.3f);
      m.blob(crown_at + v3(top * 0.15f, top * 0.06f, -top * 0.1f), v3(top * 0.2f, top * 0.1f, top * 0.22f), leaf * 1.08f, rng.next(), 0.3f);
      if (trunk_seen && detail < 2) {
        for (int i = 0; i < 3; ++i) {
          const float ang = turn + 2.0f * static_cast<float>(i) + 0.7f;
          const vec3 at = crown_at + v3(foundry::det::cos(ang) * top * 0.25f, -top * 0.05f, foundry::det::sin(ang) * top * 0.25f);
          const float drop = top * (0.25f + 0.3f * rng.uniform());
          m.blade(at, at + v3(0.1f, -drop, 0.05f), 0.08f, leaf * 0.7f);
        }
      }
      return;
    }
    uint32_t form = b.forest_form;
    if (form == channels::boundary_v1_biome::kForestFormMixed) form = rng.uniform() < 0.45f ? 1u : 0u;
    const float pick = unit(h);
    if (form == channels::boundary_v1_biome::kForestFormConifer) {
      m.frustum(base, base + v3(0, height * 0.35f, 0), 0.22f, 0.16f, trunk_seen ? 5 : 4, bark);
      vec3 col = b.foliage_dark * (0.85f + 0.3f * rng.uniform());
      if (season == Painter::kWinter) col = lerp(col, b.snow, 0.3f);
      const int tiers = detail == 2 ? 2 : 3;
      for (int t = 0; t < tiers; ++t) {
        const float y0 = height * (0.18f + 0.24f * static_cast<float>(t));
        const float r = height * (0.24f - 0.06f * static_cast<float>(t));
        m.cone(base + v3(0, y0, 0), height * 0.42f, r, 7, col * (0.9f + 0.08f * static_cast<float>(t)), turn + static_cast<float>(t));
      }
      return;
    }
    if (form == channels::boundary_v1_biome::kForestFormDead) {
      const vec3 grey = lerp(b.trunk, b.rock, 0.45f);
      m.frustum(base, base + v3(0, height * 0.8f, 0), 0.22f, 0.07f, 5, grey);
      for (int i = 0; i < 3; ++i) {
        const float ang = turn + 2.1f * static_cast<float>(i);
        const vec3 at = base + v3(0, height * (0.4f + 0.13f * static_cast<float>(i)), 0);
        m.frustum(at, at + v3(foundry::det::cos(ang) * height * 0.22f, height * 0.18f, foundry::det::sin(ang) * height * 0.22f), 0.07f, 0.02f, 4, grey);
      }
      return;
    }
    const bool birch = form == channels::boundary_v1_biome::kForestFormBirch;
    const vec3 stem = birch ? lerp(b.snow, b.rock_dark, 0.15f) : bark;
    m.frustum(base, base + v3(0, height * 0.58f, 0), birch ? 0.14f : 0.24f, birch ? 0.09f : 0.16f, trunk_seen ? 6 : 4, stem);
    const vec3 leaf = paint.crown(h, 0, 0);
    const int blobs = detail == 2 ? 1 : (birch ? 2 : 3);
    for (int i = 0; i < blobs; ++i) {
      const float ang = turn + 2.2f * static_cast<float>(i);
      const float off = i == 0 ? 0.0f : height * 0.13f;
      const vec3 c = base + v3(foundry::det::cos(ang) * off, height * (0.68f + 0.06f * static_cast<float>(i == 0) - 0.04f * static_cast<float>(i)),
                               foundry::det::sin(ang) * off);
      const float r = height * (birch ? 0.17f : 0.24f) * (i == 0 ? 1.0f : 0.8f);
      m.blob(c, v3(r, r * 0.8f, r), leaf * (0.95f + 0.1f * pick), rng.next(), 0.28f);
    }
  }

  // The forest's and jungle's wall: rows of trees from the rim outward, thinning, with undergrowth.
  void forests() {
    const size_t n = rim.samples();
    const float rows_d[] = {0.7f, 2.0f, 3.6f, 5.6f, 8.0f, 11.0f, 14.5f, 18.5f, 23.0f};
    const int rows = detail == 0 ? 9 : (detail == 1 ? 7 : 5);
    for (int j = 0; j < rows; ++j) {
      const float spacing = (1.3f + 0.35f * rows_d[j]) / (detail == 2 ? 0.7f : 1.0f);
      const float offset = unit(hash_u32(seed + static_cast<uint32_t>(j) * 31u)) * spacing;
      for (float s = offset; s < static_cast<float>(n); s += spacing) {
        const size_t k = static_cast<size_t>(s) % n;
        const float wf = rim.weight(k, kForest), wj = rim.weight(k, kJungle);
        Rng rng(seed ^ hash_u32(static_cast<uint32_t>(k) * 7u + static_cast<uint32_t>(j) * 104729u), 419);
        if (rng.uniform() > (wf + wj) * (j < 3 ? 1.0f : 0.85f)) continue;
        const bool jungle = rng.uniform() < wj / maxf(1e-3f, wf + wj);
        const float d = rows_d[j] + rng.signed_unit() * 0.45f;
        const vec3 p = rim.point(k) + rim.outward(k) * d + tangent(k) * (rng.signed_unit() * spacing * 0.3f);
        if (in_corridor(p, 3)) continue;
        Influence f;
        const Land l = land_at(p, f);
        if (l.water > l.ground - 0.05f) continue;
        const float height = b.tree_height * (0.75f + 0.4f * rng.uniform()) * (jungle ? 1.15f : 1.0f);
        if (!figure_here(d) || !figure(0, jungle ? b.figure_jungle : b.figure_forest, v3(p.x, l.ground, p.z), height, rng))
          tree(v3(p.x, l.ground - 0.1f, p.z), height, jungle, rng.next(), j < 4);
        if (j == 2 && ++blazes % 6 == 0) fires.push_back(v3(p.x, l.ground + 1.0f, p.z));
      }
    }
    // Undergrowth closing the gaps between the first trunks.
    for (size_t k = 0; k < n; k += (detail == 0 ? 1 : 2)) {
      const float wf = rim.weight(k, kForest), wj = rim.weight(k, kJungle);
      Rng rng(seed ^ hash_u32(static_cast<uint32_t>(k) * 13u + 5u), 421);
      if (rng.uniform() > (wf + wj) * 0.8f) continue;
      const bool jungle = rng.uniform() < wj / maxf(1e-3f, wf + wj);
      const float d = 0.5f + 1.6f * rng.uniform();
      const vec3 p = rim.point(k) + rim.outward(k) * d;
      if (in_corridor(p, 2)) continue;
      Influence f;
      const Land l = land_at(p, f);
      if (l.water > l.ground - 0.05f) continue;
      const vec3 base = v3(p.x, l.ground, p.z);
      // In the figure range, every third is a bush for the game; the rest stays the wall's undergrowth.
      if (figure_here(d) && k % 3 == 0 && figure(1, b.figure_bushes, base, 2.0f, rng)) continue;
      if (jungle) {
        // Broad leaves fanning from the ground.
        const vec3 leaf = b.jungle * (0.85f + 0.3f * rng.uniform());
        const int leaves = detail == 2 ? 3 : 5;
        for (int i = 0; i < leaves; ++i) {
          const float ang = rng.uniform() * 6.28f;
          const float len = 0.9f + 0.9f * rng.uniform();
          const vec3 tip = base + v3(foundry::det::cos(ang) * len * 0.7f, len, foundry::det::sin(ang) * len * 0.7f);
          m.blade(base, tip, 0.28f, leaf);
        }
      } else {
        vec3 leaf = paint.crown(rng.next(), 0, 0) * 0.85f;
        if (world.season == Painter::kWinter) leaf = lerp(b.trunk, b.snow, 0.3f);
        const float r = 0.5f + 0.5f * rng.uniform();
        m.blob(base + v3(0, r * 0.6f, 0), v3(r * 1.2f, r * 0.8f, r * 1.1f), leaf, rng.next(), 0.3f);
      }
    }
  }

  // The woods beyond the forest's wall, out to the figure range: where the land would carry a canopy
  // (its trees), rows of figures every few metres, by the edge's species (a marsh's, a mountain's,
  // a jungle's, or the forest's).
  void figure_woods() {
    if (world.figure_range <= 0) return;
    const size_t n = rim.samples();
    const float spacing = detail == 0 ? 5.5f : (detail == 1 ? 7.0f : 9.0f);
    for (float d = 26.0f; d < world.figure_range; d += spacing) {
      const float offset = unit(hash_u32(seed + static_cast<uint32_t>(d * 10.0f))) * spacing;
      for (float s = offset; s < static_cast<float>(n); s += spacing) {
        const size_t k = static_cast<size_t>(s) % n;
        Rng rng(seed ^ hash_u32(static_cast<uint32_t>(k) * 31u + static_cast<uint32_t>(d * 7.0f)), 437);
        const float jitter_d = rng.signed_unit() * spacing * 0.35f;
        const float jitter_t = rng.signed_unit() * spacing * 0.35f;
        const vec3 p = rim.point(k) + rim.outward(k) * (d + jitter_d) + tangent(k) * jitter_t;
        if (in_corridor(p, 4)) continue;
        Influence f;
        const Land l = land_at(p, f);
        if (f.d < d * 0.6f) continue;  // nearer another part of the rim, which seeds it
        if (rng.uniform() > l.trees * 0.85f || l.water > l.ground - 0.05f) continue;
        const float wm = f.w[kMountain], wj = f.w[kJungle], wr = f.w[kMarsh];
        const std::string &list = wj > 0.5f ? b.figure_jungle : (wr > 0.5f ? b.figure_marsh : (wm > 0.5f ? b.figure_mountain : b.figure_forest));
        const float height = b.tree_height * (0.7f + 0.45f * rng.uniform());
        figure(0, list.empty() ? b.figure_forest : list, v3(p.x, l.ground, p.z), height, rng);
      }
    }
  }

  // Pines climbing a mountain's lower slope, below the tree line.
  void slope_pines() {
    const size_t n = rim.samples();
    for (size_t k = 0; k < n; k += 3) {
      const float wm = rim.weight(k, kMountain);
      Rng rng(seed ^ hash_u32(static_cast<uint32_t>(k) * 17u + 9u), 431);
      if (rng.uniform() > wm * 0.35f * density()) continue;
      const float d = 6.0f + 30.0f * rng.uniform();
      const vec3 p = rim.point(k) + rim.outward(k) * d;
      if (in_corridor(p, 3)) continue;
      Influence f;
      const Land l = land_at(p, f);
      if (l.ground - f.h0 > b.tree_line * 0.8f) continue;
      const float height = b.tree_height * (0.5f + 0.4f * rng.uniform());
      if (figure_here(d) && figure(0, b.figure_mountain, v3(p.x, l.ground, p.z), height, rng)) continue;
      m.frustum(v3(p.x, l.ground - 0.1f, p.z), v3(p.x, l.ground + height * 0.3f, p.z), 0.16f, 0.1f, 4, b.trunk);
      vec3 col = b.foliage_dark * (0.85f + 0.3f * rng.uniform());
      if (world.season == Painter::kWinter) col = lerp(col, b.snow, 0.3f);
      m.cone(v3(p.x, l.ground + height * 0.2f, p.z), height * 0.8f, height * 0.22f, 6, col, rng.uniform() * 6.28f);
    }
  }

  // A marsh: reeds in clumps, cattails, snags, and lily pads on its pools.
  void marsh() {
    const size_t n = rim.samples();
    const vec3 reed = world.season == Painter::kWinter || world.season == Painter::kFall ? lerp(b.reed, b.grass_dry, 0.6f) : b.reed;
    for (size_t k = 0; k < n; ++k) {
      const float wm = rim.weight(k, kMarsh);
      Rng rng(seed ^ hash_u32(static_cast<uint32_t>(k) * 19u + 3u), 433);
      const int clumps = static_cast<int>(wm * 2.5f * density() + rng.uniform());
      for (int c = 0; c < clumps; ++c) {
        const float d0 = rng.uniform();
        const float d = 0.35f + 9.0f * d0 * rng.uniform();
        const vec3 p = rim.point(k) + rim.outward(k) * d + tangent(k) * rng.signed_unit() * 0.5f;
        if (in_corridor(p, 2)) continue;
        Influence f;
        const Land l = land_at(p, f);
        const float base_y = maxf(l.ground, l.water);
        if (l.water > l.ground + 0.6f) {
          // Open water: a lily pad.
          if (rng.uniform() < 0.5f) {
            const vec3 at = v3(p.x, l.water + 0.02f, p.z);
            const float r = 0.25f + 0.2f * rng.uniform();
            const uint32_t centre = m.vertex(at, v3(0, 1, 0), b.foliage * 0.8f);
            const uint32_t first = static_cast<uint32_t>(m.pos.size());
            for (int i = 0; i < 6; ++i) {
              const float a = 1.0472f * static_cast<float>(i);
              m.vertex(at + v3(foundry::det::cos(a) * r, 0, foundry::det::sin(a) * r), v3(0, 1, 0), b.foliage * 0.75f);
            }
            for (int i = 0; i < 5; ++i) m.tri(centre, first + static_cast<uint32_t>(i + 1), first + static_cast<uint32_t>(i));
          }
          continue;
        }
        const int blades = detail == 0 ? 8 : 5;
        for (int i = 0; i < blades; ++i) {
          const float ang = rng.uniform() * 6.28f;
          const float r = 0.25f * rng.uniform();
          const vec3 at = v3(p.x + foundry::det::cos(ang) * r, base_y - 0.05f, p.z + foundry::det::sin(ang) * r);
          const float tall = 1.1f + 1.3f * rng.uniform();
          const vec3 tip = at + v3(foundry::det::cos(ang) * tall * 0.15f, tall, foundry::det::sin(ang) * tall * 0.15f);
          m.blade(at, tip, 0.035f, reed * (0.85f + 0.3f * rng.uniform()));
          if (i == 0 && rng.uniform() < 0.4f) {
            const vec3 head = at + (tip - at) * 0.8f;
            m.frustum(head, head + (tip - at) * 0.12f, 0.04f, 0.04f, 4, b.trunk * 0.8f);
          }
        }
      }
      if (wm > 0.3f && rng.uniform() < 0.025f * density() * 3) {
        const float d = 2.0f + 6.0f * rng.uniform();
        const vec3 p = rim.point(k) + rim.outward(k) * d;
        Influence f;
        const Land l = land_at(p, f);
        const vec3 grey = lerp(b.trunk, b.rock, 0.5f);
        const float h = 3.0f + 4.0f * rng.uniform();
        const vec3 base = v3(p.x, maxf(l.ground, l.water) - 0.2f, p.z);
        m.frustum(base, base + v3(0.3f, h, 0.1f), 0.18f, 0.05f, 4, grey);
        m.frustum(base + v3(0.15f, h * 0.6f, 0.05f), base + v3(1.2f, h * 0.85f, 0.4f), 0.06f, 0.02f, 3, grey);
      }
    }
  }

  // A beach: surf along the water's edge, rocks, and driftwood.
  void beach() {
    const size_t n = rim.samples();
    const size_t step = detail == 0 ? 2 : 4;
    int prev_a = -1, prev_b = -1;
    for (size_t k = 0; k <= n; k += step) {
      const size_t kk = k % n;
      const float ws = rim.weight(kk, kSea);
      if (ws < 0.35f || k == n) {
        prev_a = prev_b = -1;
        if (k == n) break;
        continue;
      }
      // Where the ground meets the water, along the outward line.
      const vec3 p0 = rim.point(kk), out = rim.outward(kk);
      if (in_corridor(p0 + out * 4.0f, 3)) continue;
      float lo = 1.0f, hi = 30.0f;
      Influence f;
      for (int it = 0; it < 12; ++it) {
        const float mid = 0.5f * (lo + hi);
        const Land l = land_at(p0 + out * mid, f);
        if (l.water > l.ground) hi = mid;
        else lo = mid;
      }
      const vec3 at = p0 + out * hi;
      const Land l = land_at(at, f);
      const float wobble = 0.3f * noise2(static_cast<float>(k) * 0.3f, 0.5f, seed + 163);
      const vec3 a = v3(at.x - out.x * (0.5f + wobble), l.water + 0.03f, at.z - out.z * (0.5f + wobble));
      const vec3 bpt = v3(at.x + out.x * (0.9f + wobble), l.water + 0.03f, at.z + out.z * (0.9f + wobble));
      const vec3 foam = v3(0.93f, 0.95f, 0.95f);
      const int ia = static_cast<int>(m.vertex(a, v3(0, 1, 0), foam));
      const int ib = static_cast<int>(m.vertex(bpt, v3(0, 1, 0), foam * 0.92f));
      if (prev_a >= 0) {
        m.tri(static_cast<uint32_t>(prev_a), static_cast<uint32_t>(ia), static_cast<uint32_t>(prev_b));
        m.tri(static_cast<uint32_t>(ia), static_cast<uint32_t>(ib), static_cast<uint32_t>(prev_b));
      }
      prev_a = ia;
      prev_b = ib;
      // Driftwood now and then, up the beach.
      Rng rng(seed ^ hash_u32(static_cast<uint32_t>(kk) * 23u + 1u), 439);
      if (rng.uniform() < 0.05f * density()) {
        const vec3 c = p0 + out * (hi * (0.3f + 0.4f * rng.uniform()));
        Influence g;
        const Land lg = land_at(c, g);
        const vec3 along = tangent(kk) * (0.8f + 1.2f * rng.uniform());
        const vec3 base = v3(c.x, lg.ground + 0.12f, c.z);
        m.frustum(base - along, base + along, 0.13f, 0.09f, 5, lerp(b.trunk, b.sand, 0.5f));
      }
    }
  }
};

}  // namespace boundary_terrain_v1
