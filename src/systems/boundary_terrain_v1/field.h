// The world a boundary invents beyond its rim (design/boundary.md): how far a point lies from the
// rim and which edges it grows from (Influence), and the land there (Land): the ground, the canopy
// over it, and the water on it. Every height is a pure function of the point, the rim, the biome,
// and the seed, so a node, its parent, its neighbours, and the dressing all agree.
#pragma once

#include <vector>

#include "channels/boundary_v1_biome.h"
#include "channels/boundary_v1_rim.h"
#include "math.h"

namespace boundary_terrain_v1 {

using namespace bmath;
using channels::boundary_v1_biome;
using channels::boundary_v1_rim;

constexpr int kEdges = 7;
enum Edge { kLedge = 0, kRidge, kMountain, kForest, kJungle, kSea, kMarsh };

// What a point grows from: its signed distance from the rim (outward positive, metres), the rim's
// height there, each edge's share, the rise behind it, and the rim's outward direction there.
struct Influence {
  float d = 0, h0 = 0, rise = 0;
  float w[kEdges] = {0, 0, 0, 1, 0, 0, 0};
  vec3 out = {1, 0, 0};
};

inline Influence mix(const Influence &a, const Influence &b, float t) {
  Influence o;
  o.d = mixf(a.d, b.d, t);
  o.h0 = mixf(a.h0, b.h0, t);
  o.rise = mixf(a.rise, b.rise, t);
  for (int e = 0; e < kEdges; ++e) o.w[e] = mixf(a.w[e], b.w[e], t);
  o.out = lerp(a.out, b.out, t);
  return o;
}

// The rim, indexed: its samples in a grid of buckets for points near it, and coarser copies (each
// level's samples twice as far apart, blended along the rim) for points far from it, so the farther
// a point lies, the more of the rim it blends: the cells seed the world, and their seeds spread.
class RimIndex {
 public:
  explicit RimIndex(const boundary_v1_rim &rim) {
    Level l0;
    const size_t n = rim.points.size();
    l0.p = rim.points;
    l0.out = rim.outward;
    l0.w = rim.weights;
    l0.rise = rim.rises;
    if (l0.w.size() < n * kEdges) l0.w.assign(n * kEdges, 0.0f);
    if (l0.rise.size() < n) l0.rise.assign(n, 0.0f);
    levels_.push_back(l0);
    while (levels_.back().p.size() >= 16) {
      const Level &f = levels_.back();
      const size_t fn = f.p.size(), cn = fn / 2;
      Level c;
      c.p.resize(cn);
      c.out.resize(cn);
      c.w.assign(cn * kEdges, 0.0f);
      c.rise.resize(cn);
      for (size_t i = 0; i < cn; ++i) {
        // A [1 2 1] filter around sample 2i.
        const size_t a = (2 * i + fn - 1) % fn, b = 2 * i, d = (2 * i + 1) % fn;
        c.p[i] = (f.p[a] + f.p[b] * 2.0f + f.p[d]) * 0.25f;
        c.out[i] = normalize(f.out[a] + f.out[b] * 2.0f + f.out[d]);
        c.rise[i] = (f.rise[a] + f.rise[b] * 2.0f + f.rise[d]) * 0.25f;
        for (int e = 0; e < kEdges; ++e)
          c.w[i * kEdges + e] = (f.w[a * kEdges + e] + f.w[b * kEdges + e] * 2.0f + f.w[d * kEdges + e]) * 0.25f;
      }
      levels_.push_back(c);
    }
    // Buckets over the rim's box, with a margin.
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (const vec3 &p : l0.p) {
      x0 = minf(x0, p.x), x1 = maxf(x1, p.x), z0 = minf(z0, p.z), z1 = maxf(z1, p.z);
    }
    gx0_ = x0 - kMargin;
    gz0_ = z0 - kMargin;
    gw_ = maxi_(1, static_cast<int>((x1 - x0 + 2 * kMargin) / kBucket) + 1);
    gh_ = maxi_(1, static_cast<int>((z1 - z0 + 2 * kMargin) / kBucket) + 1);
    start_.assign(static_cast<size_t>(gw_ * gh_) + 1, 0);
    std::vector<int> which(n);
    for (size_t i = 0; i < n; ++i) {
      which[i] = bucket(l0.p[i].x, l0.p[i].z);
      ++start_[static_cast<size_t>(which[i]) + 1];
    }
    for (size_t b = 1; b < start_.size(); ++b) start_[b] += start_[b - 1];
    items_.assign(n, 0);
    std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
    for (size_t i = 0; i < n; ++i) items_[fill[static_cast<size_t>(which[i])]++] = static_cast<uint32_t>(i);
    centre_ = rim.centre;
    extent_ = rim.extent;
    // Rim heights, for the blend (the samples' y).
  }

  size_t samples() const { return levels_[0].p.size(); }
  const vec3 &point(size_t i) const { return levels_[0].p[i]; }
  const vec3 &outward(size_t i) const { return levels_[0].out[i]; }
  float weight(size_t i, int e) const { return levels_[0].w[i * kEdges + static_cast<size_t>(e)]; }
  float rise(size_t i) const { return levels_[0].rise[i]; }

  // The influence at (x, z).
  Influence at(float x, float z) const {
    // Near the rim: the finest samples, from the buckets.
    const int bx = static_cast<int>(std::floor((x - gx0_) / kBucket)), bz = static_cast<int>(std::floor((z - gz0_) / kBucket));
    if (bx >= -1 && bz >= -1 && bx <= gw_ && bz <= gh_) {
      float best = 1e30f;
      size_t at = 0;
      bool found = false;
      const int rings = static_cast<int>(kMargin / kBucket);
      for (int r = 0; r <= rings; ++r) {
        if (found && static_cast<float>(r - 1) * kBucket > foundry::det::sqrt(best)) break;
        for (int j = -r; j <= r; ++j)
          for (int i = -r; i <= r; ++i) {
            if (absi_(i) != r && absi_(j) != r) continue;
            const int cx = bx + i, cz = bz + j;
            if (cx < 0 || cz < 0 || cx >= gw_ || cz >= gh_) continue;
            const size_t b = static_cast<size_t>(cz * gw_ + cx);
            for (uint32_t k = start_[b]; k < start_[b + 1]; ++k) {
              const vec3 &p = levels_[0].p[items_[k]];
              const float dx = p.x - x, dz = p.z - z, d2 = dx * dx + dz * dz;
              if (d2 < best) {
                best = d2;
                at = items_[k];
                found = true;
              }
            }
          }
      }
      if (found && best <= kMargin * kMargin) return near(x, z, at, foundry::det::sqrt(best));
    }
    return far(x, z);
  }

  // The rim sample nearest (x, z) and its distance (any level-0 sample; brute force).
  float distance_to_box(float x0, float z0, float x1, float z1) const {
    float best = 1e30f;
    for (const vec3 &p : levels_[0].p) {
      const float dx = p.x < x0 ? x0 - p.x : (p.x > x1 ? p.x - x1 : 0.0f);
      const float dz = p.z < z0 ? z0 - p.z : (p.z > z1 ? p.z - z1 : 0.0f);
      best = minf(best, dx * dx + dz * dz);
    }
    return foundry::det::sqrt(best);
  }

 private:
  static constexpr float kBucket = 8.0f, kMargin = 48.0f;
  struct Level {
    std::vector<vec3> p, out;
    std::vector<float> w, rise;
  };
  int bucket(float x, float z) const {
    const int bx = mini_(gw_ - 1, maxi_(0, static_cast<int>((x - gx0_) / kBucket)));
    const int bz = mini_(gh_ - 1, maxi_(0, static_cast<int>((z - gz0_) / kBucket)));
    return bz * gw_ + bx;
  }

  // Near: the exact distance to the rim's line through the nearest sample's two segments (signed
  // by the side), and the edges blended over the samples within a little more than it.
  Influence near(float x, float z, size_t at, float dmin) const {
    const Level &l = levels_[0];
    const size_t n = l.p.size();
    // Signed distance to the polyline near the nearest sample.
    float best = 1e30f, sign = 1;
    for (int side = -1; side <= 0; ++side) {
      const size_t a = (at + n + static_cast<size_t>(side + static_cast<int>(n))) % n, b = (a + 1) % n;
      const vec3 pa = l.p[a], pb = l.p[b];
      const float ex = pb.x - pa.x, ez = pb.z - pa.z, len2 = ex * ex + ez * ez;
      float t = len2 > 1e-9f ? ((x - pa.x) * ex + (z - pa.z) * ez) / len2 : 0.0f;
      t = clampf(t, 0, 1);
      const float qx = pa.x + ex * t, qz = pa.z + ez * t;
      const float dx = x - qx, dz = z - qz, d2 = dx * dx + dz * dz;
      if (d2 < best) {
        best = d2;
        const vec3 o = l.out[a] * (1 - t) + l.out[b] * t;
        sign = dx * o.x + dz * o.z >= 0 ? 1.0f : -1.0f;
      }
    }
    Influence f;
    f.d = sign * foundry::det::sqrt(best);
    blend(0, x, z, dmin, at, f);
    return f;
  }

  // Far: a coarser level, as coarse as the distance allows, by brute force.
  Influence far(float x, float z) const {
    const float dc = length(v3(x - centre_.x, 0, z - centre_.z));
    const float est = maxf(8.0f, dc - extent_);
    size_t k = 0;
    while (k + 1 < levels_.size() && static_cast<float>(1u << (k + 1)) < est / 12.0f) ++k;
    const Level &l = levels_[k];
    float best = 1e30f;
    size_t at = 0;
    for (size_t i = 0; i < l.p.size(); ++i) {
      const float dx = l.p[i].x - x, dz = l.p[i].z - z, d2 = dx * dx + dz * dz;
      if (d2 < best) best = d2, at = i;
    }
    Influence f;
    const float dmin = foundry::det::sqrt(best);
    const float side = (x - l.p[at].x) * l.out[at].x + (z - l.p[at].z) * l.out[at].z;
    f.d = side >= 0 ? dmin : -dmin;
    blend(k, x, z, dmin, at, f);
    return f;
  }

  // The edges, rim height, rise, and outward direction: every sample of the level within
  // dmin + tau, weighted by how little farther it is than the nearest, so a point between two parts
  // of the rim takes from both, smoothly.
  void blend(size_t k, float x, float z, float dmin, size_t at, Influence &f) const {
    const Level &l = levels_[k];
    const size_t n = l.p.size();
    const float tau = 1.5f + 0.35f * dmin;
    const float reach = dmin + tau;
    float wsum = 0, h = 0, rise = 0, w[kEdges] = {0, 0, 0, 0, 0, 0, 0};
    vec3 out = v3(0, 0, 0);
    // Walk out along the rim from the nearest sample both ways, then (for a rim that comes back
    // near itself) every sample; level 0 uses the buckets.
    auto take = [&](size_t i) {
      const float dx = l.p[i].x - x, dz = l.p[i].z - z, d = foundry::det::sqrt(dx * dx + dz * dz);
      if (d > reach) return;
      const float u = 1 - (d - dmin) / tau, wt = u * u;
      wsum += wt;
      h += l.p[i].y * wt;
      rise += l.rise[i] * wt;
      out = out + l.out[i] * wt;
      for (int e = 0; e < kEdges; ++e) w[e] += l.w[i * kEdges + static_cast<size_t>(e)] * wt;
    };
    if (k == 0) {
      // Farther points blend wider stretches of the rim; every stride-th sample is enough (the
      // samples' edges are already blended along the rim).
      const uint32_t stride = static_cast<uint32_t>(maxi_(1, static_cast<int>(tau / 3.0f)));
      const int r = static_cast<int>(std::ceil(reach / kBucket));
      const int bx = static_cast<int>(std::floor((x - gx0_) / kBucket)), bz = static_cast<int>(std::floor((z - gz0_) / kBucket));
      for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) {
          const int cx = bx + i, cz = bz + j;
          if (cx < 0 || cz < 0 || cx >= gw_ || cz >= gh_) continue;
          const size_t b = static_cast<size_t>(cz * gw_ + cx);
          for (uint32_t q = start_[b]; q < start_[b + 1]; ++q)
            if (items_[q] % stride == 0 || items_[q] == at) take(items_[q]);
        }
    } else {
      for (size_t i = 0; i < n; ++i) take(i);
    }
    if (wsum <= 0) {
      take(at);
      if (wsum <= 0) wsum = 1;
    }
    f.h0 = h / wsum;
    f.rise = rise / wsum;
    f.out = normalize(out);
    float total = 0;
    for (int e = 0; e < kEdges; ++e) total += w[e];
    for (int e = 0; e < kEdges; ++e) f.w[e] = total > 0 ? w[e] / total : 0.0f;
  }

  std::vector<Level> levels_;
  float gx0_ = 0, gz0_ = 0;
  int gw_ = 1, gh_ = 1;
  std::vector<uint32_t> start_, items_;
  vec3 centre_ = {0, 0, 0};
  float extent_ = 0;
};

// The land at a point: the ground, the canopy on it (0 where nothing grows tall), and the water
// level (below the ground where there is none); and what the image needs to colour it.
struct Land {
  float ground = 0, canopy = 0, water = -1e9f;
  float valley = 0;  // how much it is a farmed valley floor (patchwork)
  float woods = 0;   // how much of the canopy is distant woods rather than the rim's forest
  float jungle = 0;  // how much of the canopy is jungle
  float river = 0;   // a river on a valley floor, 0 to 1
  float road = 0;    // a road carried out from an opening, 0 to 1
  float trees = 0;   // how much would be woodland here (figures stand in the figure range instead)
  float top() const { return maxf(ground + canopy, water); }
};

// The biome's numbers and the season, with the noise seeds.
struct World {
  const boundary_v1_biome &b;
  uint32_t season = 2, seed = 1;
  // Within this many metres of the rim, trees are the game's (figures): no canopy here.
  float figure_range = 0;
  // Where a road or river leaves the region (the rim's openings): carried on outward.
  struct Opening {
    vec3 at, dir;
    uint32_t kind = 0;  // 0 road, 1 lane, 2 river in, 3 river out
    float width = 3;
  };
  std::vector<Opening> openings;
  // How much a point lies in an opening's corridor (0 to 1, with its margin), and that opening.
  float corridor(float x, float z, float margin, const Opening **which = nullptr) const {
    float best = 0;
    for (const Opening &o : openings) {
      const float vx = x - o.at.x, vz = z - o.at.z;
      const float t = vx * o.dir.x + vz * o.dir.z;
      if (t < -3) continue;
      const float lx = vx - o.dir.x * t, lz = vz - o.dir.z * t;
      const float lat = foundry::det::sqrt(lx * lx + lz * lz);
      const float k = 1 - step_between(o.width * 0.5f, o.width * 0.5f + margin, lat);
      if (k > best) {
        best = k;
        if (which) *which = &o;
      }
    }
    return best;
  }
  float rough() const { return clampf(b.roughness, 0, 1); }

  // Each edge's start and span of the far land's climb toward its peaks, metres from the rim.
  static constexpr float kClimbStart[kEdges] = {500, 60, 0, 120, 150, 900, 200};
  static constexpr float kClimbSpan[kEdges] = {1500, 700, 350, 900, 900, 1500, 1000};

  Land land(float x, float z, const Influence &f) const {
    Land o;
    const float d = f.d;
    const float *w = f.w;
    const float R = clampf(f.rise, 0, 1);
    float g = 0;  // above the rim
    const float hills = fbm(x / 260.0f, z / 260.0f, 4, seed + 11) - 0.5f;
    const float detail = fbm(x / 45.0f, z / 45.0f, 3, seed + 23) - 0.5f;
    const float rough = 0.4f + 1.2f * this->rough();
    if (d <= 0.25f) {
      o.ground = f.h0;
      return o;
    }
    // Each edge's near profile.
    if (w[kLedge] > 1e-3f) {
      const float V = b.ledge_drop;
      float e = -V * step_between(0.85f, 4.85f + 0.04f * V, d);
      const float floor_t = step_between(10, 120, d);
      e += (hills * 0.35f * V * rough + detail * 3.0f) * floor_t;
      e += V * 0.55f * step_between(300, 2500, d);  // the far side climbs back toward the horizon
      // A river winding along the valley floor.
      const float rn = fbm(x / 900.0f, z / 900.0f, 2, seed + 41);
      const float rw = 0.012f + 0.01f * clampf(b.rivers, 0, 1);
      const float riv = clampf(b.rivers * 1.6f, 0, 1) * (1 - step_between(rw * 0.5f, rw, absf(rn - 0.5f))) * floor_t;
      e -= 1.5f * riv;
      o.river = maxf(o.river, riv * w[kLedge]);
      o.valley += w[kLedge] * step_between(40, 120, d);
      g += w[kLedge] * e;
    }
    if (w[kRidge] > 1e-3f) {
      const float C = b.cliff_height * (0.85f + 0.3f * fbm(x / 25.0f, z / 25.0f, 2, seed + 31));
      float e = C * step_between(1.4f, 5.0f, d);
      e += (hills * 0.45f * b.cliff_height * rough + detail * 4.0f) * step_between(8, 60, d);
      g += w[kRidge] * e;
    }
    if (w[kMountain] > 1e-3f) {
      float e = minf(d, 30.0f) * 0.9f + maxf(0.0f, d - 30.0f) * 0.45f;
      e = minf(e, b.peak_height * 0.3f + 40.0f);
      e += detail * 6.0f * step_between(2, 20, d);
      g += w[kMountain] * e;
    }
    for (int k = kForest; k <= kJungle; ++k)
      if (w[k] > 1e-3f) g += w[k] * (hills * 30.0f * rough * step_between(20, 200, d) + detail * 2.0f * step_between(1, 10, d));
    if (w[kSea] > 1e-3f) {
      const float S = b.sea_depth;
      float e = -S * step_between(0.5f, 9.0f, d) - 25.0f * step_between(9, 400, d);
      g += w[kSea] * e;
    }
    if (w[kMarsh] > 1e-3f) {
      float e = -0.35f * step_between(0.3f, 6.0f, d) + detail * 0.9f * step_between(2, 12, d);
      e += hills * 6.0f * step_between(30, 300, d);
      g += w[kMarsh] * e;
    }
    // The far land climbs toward its peaks, by the rise; a mountain edge always climbs.
    // Across a sea, the far land rises only as islands.
    const float islands = w[kSea] > 1e-3f ? step_between(0.58f, 0.68f, fbm(x / 1800.0f, z / 1800.0f, 3, seed + 83)) : 0.0f;
    float start = 0, span = 0, rise = 0;
    for (int e = 0; e < kEdges; ++e) {
      start += w[e] * kClimbStart[e];
      span += w[e] * kClimbSpan[e];
      rise += w[e] * (e == kMountain ? maxf(R, 0.55f) : (e == kSea ? R * islands : R));
    }
    const float climb = step_between(start, start + span, d);
    if (rise * climb > 1e-4f) {
      const float massif = fbm(x / 1500.0f, z / 1500.0f, 3, seed + 53);
      float peaks = ridged(x / 700.0f, z / 700.0f, 5, seed + 59);
      peaks = peaks * foundry::det::sqrt(peaks);
      g += b.peak_height * rise * climb * (0.3f * massif + 0.7f * peaks);
    }
    o.ground = f.h0 + g;

    // Openings: a road carried on outward at a gentle grade, a river in a channel at a falling (out)
    // or rising (in) level, each cutting through whatever the edge has.
    for (const Opening &op : openings) {
      const float vx = x - op.at.x, vz = z - op.at.z;
      const float t = vx * op.dir.x + vz * op.dir.z;
      if (t < -2) continue;
      const float lx = vx - op.dir.x * t, lz = vz - op.dir.z * t;
      const float lat = foundry::det::sqrt(lx * lx + lz * lz);
      const float half = op.width * 0.5f, tt = maxf(t, 0);
      if (op.kind <= 1) {
        const float target = op.at.y + clampf(o.ground - op.at.y, -0.07f * tt, 0.07f * tt);
        const float k = 1 - step_between(half + 1, half + 8 + 0.06f * tt, lat);
        o.ground = mixf(o.ground, target, k);
        o.road = maxf(o.road, 1 - step_between(half - 0.4f, half + 0.4f, lat));
      } else {
        const float level = op.at.y - 0.4f + (op.kind == 3 ? -0.025f : 0.025f) * tt;
        if (lat < half + 40) {
          const float bank = level + 0.3f + maxf(0, lat - half) * 0.55f;
          if (lat < half) o.ground = minf(o.ground, level - 1.2f);
          else o.ground = minf(o.ground, bank);
          if (lat < half + 0.5f) {
            o.water = maxf(o.water, level);
            o.river = maxf(o.river, 1.0f);
          }
        }
      }
    }

    // Water: the sea and the marsh's pools, at their levels.
    const float wet = w[kSea] + w[kMarsh];
    if (wet > 0.05f) o.water = maxf(o.water, f.h0 + (w[kSea] * (-0.85f * b.sea_depth) + w[kMarsh] * (-0.45f)) / wet);
    if (o.water > o.ground - 0.02f && d > 2.0f) {
      o.canopy = 0;
      return o;
    }

    // The canopy: the rim's forest and jungle beyond their dressing, and woods in the distance.
    const float above = g;
    const float treeline = 1 - step_between(b.tree_line * 0.8f, b.tree_line, above);
    const float patch = step_between(0.55f, 0.63f, fbm(x / 350.0f, z / 350.0f, 3, seed + 67));
    const float wooded = (w[kLedge] + w[kRidge] + w[kMountain] * 0.8f + w[kMarsh] * 0.3f + w[kSea] * 0.4f) * patch;
    const float clear = 1 - corridor(x, z, 6);  // nothing grows on a road or river carried out
    o.trees = clampf(w[kForest] + w[kJungle] + wooded * step_between(30, 60, d), 0, 1) * treeline * clear;
    // Inside the figure range the game's trees stand instead of the canopy.
    const float beyond = figure_range > 0 ? step_between(figure_range, figure_range + 20, d) : 1.0f;
    const float patches = patch * step_between(30, 60, d) * beyond * clear;
    const float forest = w[kForest] * step_between(16, 30, d) * beyond * clear;
    const float jungle = w[kJungle] * step_between(14, 26, d) * beyond * clear;
    const float woods = (w[kLedge] + w[kRidge] + w[kMountain] * 0.8f + w[kMarsh] * 0.3f + w[kSea] * 0.4f) * patches;
    const float T = b.tree_height;
    const float tall = (forest * T + jungle * 1.25f * T + woods * 0.85f * T) * treeline;
    // Crowns make the canopy lumpy.
    const float lumps = 0.85f + 0.3f * fbm(x / 7.0f, z / 7.0f, 2, seed + 71);
    o.canopy = maxf(0.0f, tall * lumps);
    const float sum = forest + jungle + woods;
    o.woods = sum > 0 ? woods / sum : 0;
    o.jungle = sum > 0 ? jungle / sum : 0;
    return o;
  }
};

}  // namespace boundary_terrain_v1
