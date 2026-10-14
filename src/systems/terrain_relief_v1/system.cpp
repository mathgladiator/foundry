// terrain_relief_v1: a terrain's ground built and planned inside its shape (design/terrain.md,
// "Building it"), as hexbound columns of spans (design/import.hexbound.world.md).
//
// 1. The shape: its cells, each border cell's side and place along it, the ring outside, and the
//    openings (shape.h).
// 2. Relief: the sides' heights carried inward (inverse-distance from the border), and hills by the
//    parameters, fading to nothing at the edge so the edge is exactly the sides'.
// 3. Flats: levelled to their height within their tolerance, blended into the land round them.
// 4. Rivers: each inlet's cheapest way to an outlet (downhill cheap, climbing dear, flats avoided),
//    carved so the bed never rises, banks eased; an inlet with no outlet ends in a lake.
// 5. Rock: steep land terraced into cliffs by `cliffs`; cave mouths in cliff faces by `caves`.
// 6. Roads: a tree of the cheapest ways joining every road opening and every flat's middle, graded
//    so no step along one is over 3/8 m, paved; bridges where they cross water, with their mounts.
// 7. Materials, then the columns in 1/8 m units.
// 8. The navigation mesh by the hexbound rules, and the report: every road opening reaches every
//    other and every flat.
// 9. The boundary's hull and the plotter's nature ground.
#include <string>
#include <vector>

#include "boundary.h"
#include "channels/terrain_v1_ground.h"
#include "channels/terrain_v1_spec.h"
#include "math.h"
#include "packer/terrain_v1_ground.h"
#include "packer/terrain_v1_spec.h"

#define SHAPE_NS terrain_relief_v1
#include "shape.h"

namespace terrain_relief_v1 {

using namespace bmath;
using channels::boundary_v1_hull;
using channels::plot_v1_region;
using channels::terrain_v1_bridge;
using channels::terrain_v1_ground;
using channels::terrain_v1_opening;
using channels::terrain_v1_spec;
using shape::kDq;
using shape::kDr;

namespace {

constexpr float kUnit = 0.125f;     // 1/8 m: the height unit
constexpr float kRun = kSqrt3;      // metres between neighbouring cells' centres
constexpr float kRoadStep = 0.375f; // the most a road climbs from one cell to the next (3 units)
constexpr float kWalkStep = 0.5f;   // a walker's step (hexbound: walk up to 0.5 m without fusing)
constexpr float kHeadroom = 2.0f;
constexpr float kFloorDepth = 16.0f;  // the world floor below the lowest ground
constexpr int kBank = 10;             // how far a river's valley reaches from its water, cells

enum Material : uint32_t { kGrass = 0, kDirt = 1, kSand = 2, kSnow = 3, kCobble = 4, kRock = 5 };
const float kSmooth[6] = {1.5f, 1.5f, 1.5f, 1.25f, 1.0f, 0.6f};  // hexbound §4

enum Flag : uint32_t {
  kEdgeCell = 1, kRoad = 2, kLane = 4, kRiver = 8, kLake = 16, kBridge = 32, kFlat = 64, kOpening = 128, kCave = 256,
  kTerrace = 1u << 16,  // internal: terraced rock (not in the channel)
};

// A min-heap of (cost, id) with a total order, so every build pops the same way.
struct Heap {
  std::vector<std::pair<float, int>> a;
  static bool less(const std::pair<float, int> &x, const std::pair<float, int> &y) {
    return x.first < y.first || (x.first == y.first && x.second < y.second);
  }
  bool empty() const { return a.empty(); }
  void push(float c, int id) {
    a.push_back({c, id});
    size_t i = a.size() - 1;
    while (i > 0) {
      const size_t p = (i - 1) / 2;
      if (!less(a[i], a[p])) break;
      const auto t = a[i];
      a[i] = a[p];
      a[p] = t;
      i = p;
    }
  }
  std::pair<float, int> pop() {
    const auto top = a[0];
    a[0] = a.back();
    a.pop_back();
    size_t i = 0;
    for (;;) {
      const size_t l = 2 * i + 1, r = l + 1;
      size_t m = i;
      if (l < a.size() && less(a[l], a[m])) m = l;
      if (r < a.size() && less(a[r], a[m])) m = r;
      if (m == i) break;
      const auto t = a[i];
      a[i] = a[m];
      a[m] = t;
      i = m;
    }
    return top;
  }
};

struct Ground {
  const shape::Resolved &s;
  size_t N;
  std::vector<float> H;         // the top's height, metres
  std::vector<uint32_t> flags;
  std::vector<float> water;     // water level, metres (very low: dry)
  std::vector<float> ceiling;   // a cave's ceiling over the floor, metres (0: none)
  std::vector<uint8_t> material;
  explicit Ground(const shape::Resolved &r)
      : s(r), N(static_cast<size_t>(r.w) * static_cast<size_t>(r.h)), H(N, 0.0f), flags(N, 0), water(N, -1e9f),
        ceiling(N, 0.0f), material(N, kGrass) {}
  bool in(int i) const { return i >= 0 && static_cast<size_t>(i) < N && s.inside[static_cast<size_t>(i)]; }
  int q_of(int i) const { return s.q0 + i % s.w; }
  int r_of(int i) const { return s.r0 + i / s.w; }
  vec3 centre(int i) const { return v3(hex_x(q_of(i), r_of(i)), H[static_cast<size_t>(i)], hex_z(r_of(i))); }
  int neighbour(int i, int d) const {
    const int q = q_of(i) + kDq[d], r = r_of(i) + kDr[d];
    return s.contains(q, r) ? s.index(q, r) : -1;
  }
  bool wet(int i) const { return water[static_cast<size_t>(i)] > -1e8f; }
};

// Steps from a set of cells over the shape's cells (breadth first).
std::vector<int> steps_from(const Ground &g, const std::vector<int> &from, int cap) {
  std::vector<int> d(g.N, 1 << 20);
  std::vector<int> queue;
  for (int i : from)
    if (g.in(i) && d[static_cast<size_t>(i)] != 0) {
      d[static_cast<size_t>(i)] = 0;
      queue.push_back(i);
    }
  for (size_t k = 0; k < queue.size(); ++k) {
    const int at = queue[k];
    if (d[static_cast<size_t>(at)] >= cap) continue;
    for (int dd = 0; dd < 6; ++dd) {
      const int n = g.neighbour(at, dd);
      if (n < 0 || d[static_cast<size_t>(n)] <= d[static_cast<size_t>(at)] + 1) continue;
      d[static_cast<size_t>(n)] = d[static_cast<size_t>(at)] + 1;
      queue.push_back(n);
    }
  }
  return d;
}

// The cheapest way from any of `from` to any of `to`, by a step's cost (negative: barred).
template <typename Cost>
std::vector<int> cheapest(const Ground &g, const std::vector<int> &from, const std::vector<uint8_t> &to, Cost cost) {
  std::vector<float> best(g.N, 1e30f);
  std::vector<int> prev(g.N, -1);
  Heap heap;
  for (int i : from)
    if (g.in(i)) {
      best[static_cast<size_t>(i)] = 0;
      heap.push(0, i);
    }
  int reached = -1;
  while (!heap.empty()) {
    const auto [c, at] = heap.pop();
    if (c > best[static_cast<size_t>(at)]) continue;
    if (to[static_cast<size_t>(at)]) {
      reached = at;
      break;
    }
    for (int d = 0; d < 6; ++d) {
      const int n = g.neighbour(at, d);
      if (n < 0) continue;
      const float step = cost(at, n);
      if (step < 0) continue;
      const float nc = c + step;
      if (nc < best[static_cast<size_t>(n)]) {
        best[static_cast<size_t>(n)] = nc;
        prev[static_cast<size_t>(n)] = at;
        heap.push(nc, n);
      }
    }
  }
  std::vector<int> path;
  for (int at = reached; at >= 0; at = prev[static_cast<size_t>(at)]) path.push_back(at);
  for (size_t i = 0, j = path.size(); j > 1 && i < j - 1; ++i, --j) {
    const int t = path[i];
    path[i] = path[j - 1];
    path[j - 1] = t;
  }
  return path;
}

int to_units(float m) { return round_to_int(m / kUnit); }

}  // namespace

bool run(const terrain_v1_spec &in, terrain_v1_ground &out, std::string &error) {
  out = terrain_v1_ground{};
  // 1. The shape.
  shape::Resolved s;
  if (!shape::resolve(in.shape, s, error)) return false;
  Ground g(s);
  const size_t N = g.N;
  const uint32_t seed = in.seed ? in.seed : 1u;
  const size_t sides = s.sides.size();
  std::vector<float> side_len(sides);
  for (size_t i = 0; i < sides; ++i) side_len[i] = shape::side_length(s, static_cast<int>(i));
  auto edge_value = [&](int i) {
    return shape::side_at(s, s.side_of[static_cast<size_t>(i)], s.along[static_cast<size_t>(i)],
                          side_len[static_cast<size_t>(s.side_of[static_cast<size_t>(i)])]);
  };
  std::vector<int> inside;
  for (size_t i = 0; i + 1 < s.cells.size(); i += 2) inside.push_back(s.index(s.cells[i], s.cells[i + 1]));
  std::vector<int> border;
  for (int i : inside)
    if (s.depth[static_cast<size_t>(i)] == 1) {
      border.push_back(i);
      g.flags[static_cast<size_t>(i)] |= kEdgeCell;
    }

  // 2. Relief. The sides' heights carried inward by inverse distance from border samples, then hills
  // fading in over the first `band` metres.
  {
    std::vector<int> samples;
    const size_t stride = border.size() > 240 ? border.size() / 240 : 1;
    for (size_t k = 0; k < border.size(); k += stride) samples.push_back(border[k]);
    std::vector<float> sample_h;
    for (int b : samples) sample_h.push_back(edge_value(b).height);
    const float relief = clampf(in.relief, 0, 60), hill = clampf(in.hill_size, 8, 200);
    const float rough = clampf(in.roughness, 0, 1), ridges = clampf(in.ridges, 0, 1);
    const float band = 6 + 0.5f * hill;
    for (int i : inside) {
      float base;
      if (s.depth[static_cast<size_t>(i)] == 1) {
        base = edge_value(i).height;
      } else {
        const vec3 p = g.centre(i);
        float wsum = 0, hsum = 0;
        for (size_t k = 0; k < samples.size(); ++k) {
          const vec3 d = g.centre(samples[k]) - p;
          const float d2 = d.x * d.x + d.z * d.z + 1;
          const float w = 1.0f / (d2 * d2);
          wsum += w;
          hsum += w * sample_h[k];
        }
        base = hsum / wsum;
      }
      const vec3 p = g.centre(i);
      const float x = p.x / hill, z = p.z / hill;
      const float roll = fbm(x, z, 4, seed * 7 + 1) - 0.5f;
      const float ridge = ridged(x * 0.8f, z * 0.8f, 4, seed * 7 + 2) - 0.35f;
      float hills = relief * (mixf(roll * 1.6f, ridge * 1.4f, ridges));
      hills += rough * 1.2f * (fbm(p.x / 6.0f, p.z / 6.0f, 3, seed * 7 + 3) - 0.5f);
      const float fade = step_between(0, band, static_cast<float>(s.depth[static_cast<size_t>(i)] - 1) * 1.5f);
      g.H[static_cast<size_t>(i)] = base + hills * fade;
    }
  }

  // 3. Flats.
  std::vector<std::vector<int>> flats;
  std::vector<float> flat_level;
  std::vector<float> flat_tol;
  {
    std::vector<uint8_t> is_flat(N, 0);
    for (const auto &f : in.flats) {
      std::vector<int> cells;
      auto add = [&](int q, int r) {
        if (!s.contains(q, r)) return;
        const int i = s.index(q, r);
        if (s.depth[static_cast<size_t>(i)] <= 1) return;  // the edge is the sides'
        if (is_flat[static_cast<size_t>(i)]) return;         // an earlier flat has it (overlapping flats)
        for (int c : cells)
          if (c == i) return;
        cells.push_back(i);
      };
      const int rad = mini_(60, static_cast<int>(f.radius));
      if (rad == 0 && f.cells.empty()) continue;  // no flat ("none")
      if (rad > 0)
        for (int dq = -rad; dq <= rad; ++dq)
          for (int dr = maxi_(-rad, -dq - rad); dr <= mini_(rad, -dq + rad); ++dr) add(f.q + dq, f.r + dr);
      for (size_t k = 0; k + 1 < f.cells.size(); k += 2) add(f.cells[k], f.cells[k + 1]);
      if (cells.empty()) continue;
      float mean = 0;
      for (int c : cells) mean += g.H[static_cast<size_t>(c)];
      mean /= static_cast<float>(cells.size());
      const float level = f.level ? clampf(f.height, -40, 80) : mean;
      const float tol = clampf(f.tolerance, 0, 3);
      float worst = 0;
      for (int c : cells) worst = maxf(worst, absf(g.H[static_cast<size_t>(c)] - level));
      const float k = worst > tol ? tol / worst : 1.0f;
      for (int c : cells) {
        g.H[static_cast<size_t>(c)] = level + (g.H[static_cast<size_t>(c)] - level) * k * 0.5f;
        g.flags[static_cast<size_t>(c)] |= kFlat;
        is_flat[static_cast<size_t>(c)] = 1;
      }
      // Blended into the land round it, as wide as it takes for no ring to step more than 0.9 m
      // (so the blend rounds into a slope, never a cliff ringing the flat): 5 to 30 cells.
      const std::vector<int> d = steps_from(g, cells, 31);
      float apart = 0;
      for (int i : inside)
        if (d[static_cast<size_t>(i)] >= 1 && d[static_cast<size_t>(i)] <= 30) apart = maxf(apart, absf(g.H[static_cast<size_t>(i)] - level));
      const int K = maxi_(5, mini_(30, static_cast<int>(apart / 0.9f) + 1));
      for (int i : inside) {
        const int k2 = d[static_cast<size_t>(i)];
        if (k2 < 1 || k2 > K || is_flat[static_cast<size_t>(i)] || s.depth[static_cast<size_t>(i)] <= 1) continue;
        g.H[static_cast<size_t>(i)] = mixf(level, g.H[static_cast<size_t>(i)], static_cast<float>(k2) / static_cast<float>(K + 1));
      }
      flats.push_back(cells);
      flat_level.push_back(level);
      flat_tol.push_back(tol);
    }
  }

  // The openings' cells sit at their side's height, as the edge does.
  std::vector<shape::Opening> roads, inlets, outlets;
  for (const shape::Opening &o : s.openings) {
    const int i = s.index(o.q, o.r);
    g.flags[static_cast<size_t>(i)] |= kOpening;
    out.opening_cells.push_back(o.q);
    out.opening_cells.push_back(o.r);
    if (o.kind == terrain_v1_opening::kKindRoad || o.kind == terrain_v1_opening::kKindLane) roads.push_back(o);
    else if (o.kind == terrain_v1_opening::kKindRiverIn) inlets.push_back(o);
    else outlets.push_back(o);
  }

  // 4. Rivers.
  const float depth = clampf(in.river_depth, 0.5f, 6);
  std::vector<int> river_path_cells;  // every river cell, for the bank pass
  std::vector<float> river_width;     // per river cell, half width in cells
  {
    std::vector<uint8_t> on_river(N, 0);
    auto river_cost = [&](int a, int b) -> float {
      if (s.depth[static_cast<size_t>(b)] <= 1 && !(g.flags[static_cast<size_t>(b)] & kOpening)) return -1;  // not along the edge
      const float dh = g.H[static_cast<size_t>(b)] - g.H[static_cast<size_t>(a)];
      float c = 1 + (dh > 0 ? dh * 6 : -dh * 0.5f);
      if (g.flags[static_cast<size_t>(b)] & kFlat) c += 40;
      if (on_river[static_cast<size_t>(b)]) c *= 0.3f;
      return c;
    };
    // Sources: the inlets, then a spring for every outlet no inlet reaches.
    struct Run {
      int from;
      float width;
      bool spring;
    };
    std::vector<Run> runs;
    for (const auto &o : inlets) runs.push_back({s.index(o.q, o.r), static_cast<float>(o.width), false});
    std::vector<uint8_t> outlet_cells(N, 0);
    for (const auto &o : outlets) outlet_cells[static_cast<size_t>(s.index(o.q, o.r))] = 1;
    if (inlets.empty())
      for (const auto &o : outlets) {
        // A spring: the highest cell well inside, toward the far side from the outlet.
        const int oi = s.index(o.q, o.r);
        int best = -1;
        float score = -1e30f;
        for (int i : inside) {
          if (s.depth[static_cast<size_t>(i)] < 6 || (g.flags[static_cast<size_t>(i)] & kFlat)) continue;
          const vec3 d = g.centre(i) - g.centre(oi);
          const float sc = g.H[static_cast<size_t>(i)] * 2 + foundry::det::sqrt(d.x * d.x + d.z * d.z) * 0.15f;
          if (sc > score) {
            score = sc;
            best = i;
          }
        }
        if (best >= 0) runs.push_back({best, static_cast<float>(o.width), true});
      }
    uint32_t river_cells_so_far = 0;
    for (const Run &run : runs) {
      std::vector<uint8_t> goal = outlet_cells;
      bool lake = outlets.empty();
      if (lake) {  // no outlet: the lowest cell well inside is the lake's middle
        int low = -1;
        for (int i : inside)
          if (s.depth[static_cast<size_t>(i)] >= 5 && (low < 0 || g.H[static_cast<size_t>(i)] < g.H[static_cast<size_t>(low)])) low = i;
        if (low < 0) continue;
        goal.assign(N, 0);
        goal[static_cast<size_t>(low)] = 1;
      }
      // Joining a river already laid ends the run there.
      for (int i : river_path_cells) goal[static_cast<size_t>(i)] = 1;
      const std::vector<int> path = cheapest(g, {run.from}, goal, river_cost);
      if (path.size() < 2) continue;
      // Carve: the bed never rises from source to mouth.
      std::vector<float> beds(path.size());
      float b = g.H[static_cast<size_t>(path[0])] - depth;
      for (size_t k = 0; k < path.size(); ++k) {
        b = minf(b, g.H[static_cast<size_t>(path[k])] - depth);
        beds[k] = b;
      }
      for (size_t k = 0; k < path.size(); ++k) {
        const int i = path[k];
        if (on_river[static_cast<size_t>(i)]) break;  // the river it joins carries on
        on_river[static_cast<size_t>(i)] = 1;
        g.flags[static_cast<size_t>(i)] |= kRiver;
        g.H[static_cast<size_t>(i)] = beds[k];
        g.water[static_cast<size_t>(i)] = beds[k] + depth * 0.75f;
        river_path_cells.push_back(i);
        river_width.push_back(maxf(0, (run.width - 1) * 0.5f));
        out.river_cells.push_back(g.q_of(i));
        out.river_cells.push_back(g.r_of(i));
      }
      out.river_counts.push_back(static_cast<uint32_t>(out.river_cells.size() / 2) - river_cells_so_far);
      river_cells_so_far = static_cast<uint32_t>(out.river_cells.size() / 2);
      if (lake) {  // a lake round the run's end, at its water level
        const int end = path.back();
        const float level = g.water[static_cast<size_t>(end)];
        const std::vector<int> d = steps_from(g, {end}, 7);
        for (int i : inside) {
          const int k = d[static_cast<size_t>(i)];
          if (k > 6 || s.depth[static_cast<size_t>(i)] <= 2) continue;
          const float shore = 4 + 2 * (fbm(g.centre(i).x / 9, g.centre(i).z / 9, 2, seed + 41) - 0.5f) * 3;
          if (static_cast<float>(k) > shore) continue;
          g.H[static_cast<size_t>(i)] = minf(g.H[static_cast<size_t>(i)], level - depth * (1 - static_cast<float>(k) / 7.0f));
          g.water[static_cast<size_t>(i)] = level;
          g.flags[static_cast<size_t>(i)] |= kLake;
        }
      }
    }
    // Width and banks: cells within a river's half width are its bed; the banks ease down to it.
    std::vector<int> bank_d(N, 1 << 20);
    std::vector<float> bank_level(N, 0.0f);
    for (size_t k = 0; k < river_path_cells.size(); ++k) {
      const int c = river_path_cells[k];
      const int half = round_to_int(river_width[k]);
      const int reach = half + kBank;
      const std::vector<int> d = steps_from(g, {c}, reach);
      // Only the neighbourhood is touched: walk it from the cell's own ring.
      for (int dq = -reach; dq <= reach; ++dq)
        for (int dr = maxi_(-reach, -dq - reach); dr <= mini_(reach, -dq + reach); ++dr) {
          const int q = g.q_of(c) + dq, r = g.r_of(c) + dr;
          if (!s.contains(q, r)) continue;
          const int i = s.index(q, r);
          const int k2 = d[static_cast<size_t>(i)];
          if (k2 > reach) continue;
          if (k2 <= half) {
            if (!(g.flags[static_cast<size_t>(i)] & kRiver) && s.depth[static_cast<size_t>(i)] > 1) {
              g.flags[static_cast<size_t>(i)] |= kRiver;
              g.H[static_cast<size_t>(i)] = minf(g.H[static_cast<size_t>(i)], g.H[static_cast<size_t>(c)] + 0.25f * static_cast<float>(k2));
              g.water[static_cast<size_t>(i)] = g.water[static_cast<size_t>(c)];
            }
          } else if (k2 - half < bank_d[static_cast<size_t>(i)]) {
            bank_d[static_cast<size_t>(i)] = k2 - half;
            bank_level[static_cast<size_t>(i)] = g.water[static_cast<size_t>(c)];
          }
        }
    }
    // Water a flat couldn't avoid leaves it: the flat is its dry ground.
    for (auto &f : flats) {
      std::vector<int> dry;
      for (int c : f)
        if (g.wet(c)) g.flags[static_cast<size_t>(c)] &= ~kFlat;
        else dry.push_back(c);
      f.swap(dry);
    }
    for (int i : inside) {
      const int k = bank_d[static_cast<size_t>(i)];
      if (k > kBank || g.wet(i) || s.depth[static_cast<size_t>(i)] <= 1 || (g.flags[static_cast<size_t>(i)] & kFlat)) continue;
      // A valley the river has cut: banks climbing about 0.9 m a cell, which still rounds into a slope.
      const float allowed = bank_level[static_cast<size_t>(i)] + 0.3f + 0.9f * static_cast<float>(k);
      g.H[static_cast<size_t>(i)] = minf(g.H[static_cast<size_t>(i)], allowed);
    }
  }

  // 5. Rock: steep land terraced into cliffs; cave mouths in the faces.
  {
    const float cliffs = clampf(in.cliffs, 0, 1);
    std::vector<float> slope(N, 0.0f);
    for (int i : inside)
      for (int d = 0; d < 6; ++d) {
        const int n = g.neighbour(i, d);
        if (n >= 0) slope[static_cast<size_t>(i)] = maxf(slope[static_cast<size_t>(i)], absf(g.H[static_cast<size_t>(n)] - g.H[static_cast<size_t>(i)]) / kRun);
      }
    for (int i : inside) {
      const uint32_t f = g.flags[static_cast<size_t>(i)];
      if (f & (kFlat | kRiver | kLake | kOpening) || s.depth[static_cast<size_t>(i)] <= 2) continue;
      const vec3 p = g.centre(i);
      // Outcrops, not contours: where the mask is strong the ground snaps to a ledge of the step
      // (its offset wandering), and fades back to the slope where it weakens.
      const float mask = fbm(p.x / 30, p.z / 30, 3, seed * 7 + 9);
      const float strength = step_between(1 - cliffs * 0.75f, 1.05f - cliffs * 0.65f, mask) * step_between(0.2f, 0.45f, slope[static_cast<size_t>(i)]);
      if (strength > 0.05f) {
        const float step = 5.5f + 5.0f * fbm(p.x / 25, p.z / 25, 2, seed * 7 + 10);  // a face or two a hill, not a stair
        const float offset = 4.0f * fbm(p.x / 45, p.z / 45, 2, seed * 7 + 11);
        const float h = g.H[static_cast<size_t>(i)];
        const float ledge = std::floor((h + offset) / step + 0.5f) * step - offset;
        g.H[static_cast<size_t>(i)] = mixf(h, ledge, strength);
        if (strength > 0.5f) g.flags[static_cast<size_t>(i)] |= kTerrace;
      }
    }
    const float caves = clampf(in.caves, 0, 1);
    if (caves > 0)
      for (int i : inside) {
        if (s.depth[static_cast<size_t>(i)] <= 3 || (g.flags[static_cast<size_t>(i)] & (kFlat | kRiver | kLake))) continue;
        if (unit(hash2(g.q_of(i), g.r_of(i), seed * 13 + 5)) > caves * 0.25f) continue;
        // A face: a lower neighbour at least 3.5 m down, open and dry.
        for (int d = 0; d < 6; ++d) {
          const int n = g.neighbour(i, d);
          if (n < 0 || g.wet(n) || (g.flags[static_cast<size_t>(n)] & kCave)) continue;
          const float floor = g.H[static_cast<size_t>(n)];
          if (g.H[static_cast<size_t>(i)] - floor < 3.5f) continue;
          // The mouth, and up to two cells on into the rock while it stays high enough.
          int at = i;
          for (int k = 0; k < 3 && at >= 0; ++k) {
            const size_t a = static_cast<size_t>(at);
            if (g.H[a] - floor < 3.25f || (g.flags[a] & (kRiver | kLake | kFlat | kCave)) || s.depth[a] <= 2) break;
            g.ceiling[a] = floor + 2.25f + 0.25f * static_cast<float>(k == 0);
            g.flags[a] |= kCave;
            at = g.neighbour(at, (d + 3) % 6);
          }
          break;
        }
      }
  }

  // Each cell's flat, if any (the first that claimed it, dry).
  std::vector<int> flat_of(N, -1);
  for (size_t f = 0; f < flats.size(); ++f)
    for (int c : flats[f]) flat_of[static_cast<size_t>(c)] = static_cast<int>(f);

  // 6. Roads: from the first road opening, each other opening and flat joined by its cheapest way to
  // the roads so far.
  std::vector<std::vector<int>> road_paths;
  std::vector<uint8_t> road_kind;  // 0 road, 1 lane
  {
    std::vector<uint8_t> on_road(N, 0);
    auto road_cost = [&](int a, int b) -> float {
      const uint32_t fb = g.flags[static_cast<size_t>(b)];
      if (s.depth[static_cast<size_t>(b)] <= 1 && !(fb & kOpening)) return -1;  // never along the edge
      if (fb & kCave) return -1;
      const float dh = absf(g.H[static_cast<size_t>(b)] - g.H[static_cast<size_t>(a)]);
      if (dh > 4.5f && !g.wet(b) && !g.wet(a)) return -1;  // too steep to cut
      const float grade = dh / kRun;
      float c = 1 + (grade > 0.15f ? (grade - 0.15f) * 40 : 0);
      if (g.wet(b)) c += g.wet(a) ? 3 : 18;  // a bridge: dear to start, cheap to carry on
      if (fb & kTerrace) c += 3;
      if (fb & kFlat) c *= 0.8f;
      if (on_road[static_cast<size_t>(b)]) c *= 0.3f;
      return c;
    };
    std::vector<int> nodes;
    std::vector<uint8_t> kinds;
    for (const auto &o : roads) {
      nodes.push_back(s.index(o.q, o.r));
      kinds.push_back(o.kind == terrain_v1_opening::kKindLane ? 1 : 0);
    }
    for (const auto &f : flats) {
      // The flat's middle: its cell nearest its centroid.
      float cx = 0, cz = 0;
      for (int c : f) {
        cx += g.centre(c).x;
        cz += g.centre(c).z;
      }
      cx /= static_cast<float>(f.size());
      cz /= static_cast<float>(f.size());
      int best = f[0];
      float bd = 1e30f;
      for (int c : f) {
        const float dx = g.centre(c).x - cx, dz = g.centre(c).z - cz;
        if (dx * dx + dz * dz < bd) {
          bd = dx * dx + dz * dz;
          best = c;
        }
      }
      nodes.push_back(best);
      kinds.push_back(0);
    }
    if (nodes.size() >= 2) {
      std::vector<int> network = {nodes[0]};
      on_road[static_cast<size_t>(nodes[0])] = 1;
      std::vector<uint8_t> joined(nodes.size(), 0);
      joined[0] = 1;
      for (size_t round = 1; round < nodes.size(); ++round) {
        // The nearest node not yet joined (by straight distance from the network), then its way in.
        size_t pick = nodes.size();
        float pd = 1e30f;
        for (size_t k = 0; k < nodes.size(); ++k) {
          if (joined[k]) continue;
          for (int c : network) {
            const vec3 d = g.centre(nodes[k]) - g.centre(c);
            const float d2 = d.x * d.x + d.z * d.z;
            if (d2 < pd) {
              pd = d2;
              pick = k;
            }
          }
        }
        if (pick == nodes.size()) break;
        joined[pick] = 1;
        std::vector<uint8_t> goal(N, 0);
        for (int c : network) goal[static_cast<size_t>(c)] = 1;
        std::vector<int> path = cheapest(g, {nodes[pick]}, goal, road_cost);
        if (path.size() < 2) continue;
        for (int c : path)
          if (!on_road[static_cast<size_t>(c)]) {
            on_road[static_cast<size_t>(c)] = 1;
            network.push_back(c);
          }
        road_paths.push_back(path);
        road_kind.push_back(kinds[pick]);
      }
    }
    // Grading each road, in the order laid: pinned at its ends (an opening at its side's height, or
    // where it meets a road already graded) and at bridge decks; no step between cells along it
    // over kRoadStep.
    std::vector<uint8_t> graded(N, 0);
    for (size_t p = 0; p < road_paths.size(); ++p) {
      const std::vector<int> &path = road_paths[p];
      const size_t n = path.size();
      std::vector<float> h(n);
      // Each cell's allowed band: exact where pinned (an opening at its side's height, a road already
      // graded, a bridge's deck), within its tolerance of the level on a flat, free elsewhere.
      std::vector<float> a(n, -1e30f), b(n, 1e30f);
      std::vector<uint8_t> pin(n, 0);
      for (size_t k = 0; k < n; ++k) {
        // The ground smoothed along the road.
        float sum = 0, wsum = 0;
        for (int o = -3; o <= 3; ++o) {
          const long j = static_cast<long>(k) + o;
          if (j < 0 || j >= static_cast<long>(n)) continue;
          const int c = path[static_cast<size_t>(j)];
          const float w = 4.0f - static_cast<float>(absi_(o));
          sum += w * (g.wet(c) ? g.water[static_cast<size_t>(c)] + 1.5f : g.H[static_cast<size_t>(c)]);
          wsum += w;
        }
        h[k] = sum / wsum;
        const int c = path[k];
        const int fl = flat_of[static_cast<size_t>(c)];
        if (graded[static_cast<size_t>(c)] || (g.flags[static_cast<size_t>(c)] & kOpening)) {
          h[k] = a[k] = b[k] = g.H[static_cast<size_t>(c)];
          pin[k] = 1;
        } else if (fl >= 0) {
          a[k] = flat_level[static_cast<size_t>(fl)] - flat_tol[static_cast<size_t>(fl)];
          b[k] = flat_level[static_cast<size_t>(fl)] + flat_tol[static_cast<size_t>(fl)];
          h[k] = clampf(g.H[static_cast<size_t>(c)], a[k], b[k]);
          pin[k] = 1;
        }
      }
      // Bridges: a deck over each run of water, level with its higher bank and clear of the water.
      for (size_t k = 0; k < n;) {
        if (!g.wet(path[k])) {
          ++k;
          continue;
        }
        size_t e = k;
        float deck = -1e30f;
        while (e < n && g.wet(path[e])) deck = maxf(deck, g.water[static_cast<size_t>(path[e++])] + 1.5f);
        if (k > 0) deck = maxf(deck, h[k - 1]);
        if (e < n) deck = maxf(deck, h[e]);
        for (size_t j = k; j < e; ++j) {
          h[j] = a[j] = b[j] = deck;
          pin[j] = 1;
        }
        k = e;
      }
      // The feasible band from the constrained cells, then relaxed passes within it.
      std::vector<float> lo(n, -1e30f), hi(n, 1e30f);
      for (size_t k = 0; k < n; ++k)
        if (pin[k])
          for (size_t j = 0; j < n; ++j) {
            const float span = kRoadStep * static_cast<float>(j > k ? j - k : k - j);
            lo[j] = maxf(lo[j], a[k] - span);
            hi[j] = minf(hi[j], b[k] + span);
          }
      for (size_t k = 0; k < n; ++k) {
        const float l = maxf(lo[k], a[k]), u = minf(hi[k], b[k]);
        h[k] = clampf(h[k], minf(l, u), maxf(l, u));
      }
      auto settle = [&](size_t k, float prev) {
        const float l = maxf(a[k], prev - kRoadStep), u = minf(b[k], prev + kRoadStep);
        const float c = clampf(h[k], minf(l, u), maxf(l, u));
        const bool moved = c != h[k];
        h[k] = c;
        return moved;
      };
      for (int pass = 0; pass < 64; ++pass) {
        bool moved = false;
        for (size_t k = 1; k < n; ++k) moved = settle(k, h[k - 1]) || moved;
        for (size_t k = n - 1; k-- > 0;) moved = settle(k, h[k + 1]) || moved;
        if (!moved) break;
      }
      // Write it: quantized to the height unit, the cells under the road and its width.
      const uint32_t flag = road_kind[p] ? kLane : kRoad;
      for (size_t k = 0; k < n; ++k) {
        const int c = path[k];
        const float hk = static_cast<float>(to_units(h[k])) * kUnit;
        g.flags[static_cast<size_t>(c)] |= flag;
        graded[static_cast<size_t>(c)] = 1;
        g.ceiling[static_cast<size_t>(c)] = 0;
        g.flags[static_cast<size_t>(c)] &= ~(kCave | kTerrace);
        if (g.wet(c)) {
          g.flags[static_cast<size_t>(c)] |= kBridge;
          continue;  // the river keeps its bed; the deck is the bridge's
        }
        g.H[static_cast<size_t>(c)] = hk;
      }
      out.road_counts.push_back(static_cast<uint32_t>(n));
      for (int c : path) {
        out.road_cells.push_back(g.q_of(c));
        out.road_cells.push_back(g.r_of(c));
      }
      // A road is two cells wide: the cell beside it on its left, at the road's height (a lane is one).
      if (!road_kind[p])
        for (size_t k = 0; k + 1 < n; ++k) {
          const int a = path[k], b = path[k + 1];
          int dir = 0;
          for (int d = 0; d < 6; ++d)
            if (g.neighbour(a, d) == b) dir = d;
          const int side = g.neighbour(a, (dir + 1) % 6);
          if (side < 0 || graded[static_cast<size_t>(side)] || g.wet(side) || s.depth[static_cast<size_t>(side)] <= 1 ||
              (g.flags[static_cast<size_t>(side)] & kFlat))  // a flat keeps its level
            continue;
          g.H[static_cast<size_t>(side)] = g.H[static_cast<size_t>(a)];
          g.flags[static_cast<size_t>(side)] |= kRoad;
          g.flags[static_cast<size_t>(side)] &= ~(kCave | kTerrace);
          g.ceiling[static_cast<size_t>(side)] = 0;
          graded[static_cast<size_t>(side)] = 1;
        }
      // Bridges: each run of water along the road, its mounts on the banks either side.
      for (size_t k = 0; k < n;) {
        if (!g.wet(path[k])) {
          ++k;
          continue;
        }
        size_t e = k;
        while (e < n && g.wet(path[e])) ++e;
        terrain_v1_bridge br;
        const int a = k > 0 ? path[k - 1] : path[k], b = e < n ? path[e] : path[e - 1];
        const float deck = static_cast<float>(to_units(h[k])) * kUnit;
        const vec3 pa = g.centre(a), pb = g.centre(b);
        br.a = v3(pa.x, deck, pa.z);
        br.b = v3(pb.x, deck, pb.z);
        br.width = road_kind[p] ? 2.0f : 3.5f;
        for (size_t j = k; j < e; ++j) {
          br.cells.push_back(g.q_of(path[j]));
          br.cells.push_back(g.r_of(path[j]));
        }
        out.bridges.push_back(br);
        k = e;
      }
    }
    // Shoulders: the land beside a road eases toward it so roads don't run in trenches or on dykes
    // where the ground is near.
    const std::vector<int> d = [&] {
      std::vector<int> on;
      for (int i : inside)
        if (g.flags[static_cast<size_t>(i)] & (kRoad | kLane)) on.push_back(i);
      return steps_from(g, on, 3);
    }();
    for (int i : inside) {
      const int k = d[static_cast<size_t>(i)];
      if (k < 1 || k > 2 || (g.flags[static_cast<size_t>(i)] & (kRoad | kLane | kFlat)) || g.wet(i) || s.depth[static_cast<size_t>(i)] <= 1) continue;
      float near = 0;
      int count = 0;
      for (int dd = 0; dd < 6; ++dd) {
        const int n = g.neighbour(i, dd);
        if (n >= 0 && (g.flags[static_cast<size_t>(n)] & (kRoad | kLane)) && !g.wet(n)) {
          near += g.H[static_cast<size_t>(n)];
          ++count;
        }
      }
      if (!count) continue;
      near /= static_cast<float>(count);
      const float dh = g.H[static_cast<size_t>(i)] - near;
      if (absf(dh) < 3) g.H[static_cast<size_t>(i)] = near + dh * (k == 1 ? 0.35f : 0.7f);
    }
  }

  // 7. Materials.
  for (int i : inside) {
    const size_t a = static_cast<size_t>(i);
    const uint32_t f = g.flags[a];
    float steep = 0;
    for (int d = 0; d < 6; ++d) {
      const int n = g.neighbour(i, d);
      if (n >= 0) steep = maxf(steep, absf(g.H[static_cast<size_t>(n)] - g.H[a]) / kRun);
    }
    const vec3 p = g.centre(i);
    uint8_t m = kGrass;
    if (f & kRoad) m = kCobble;
    else if (f & kLane) m = kDirt;
    else if (g.wet(i)) m = kSand;
    else if (f & (kTerrace | kCave) || steep > 0.85f) m = kRock;
    else if (fbm(p.x / 14, p.z / 14, 3, seed * 7 + 20) > 0.68f) m = kDirt;
    g.material[a] = m;
  }
  for (int i : inside) {  // sand on the banks just above the water
    if (g.wet(i) || g.material[static_cast<size_t>(i)] != kGrass) continue;
    for (int d = 0; d < 6; ++d) {
      const int n = g.neighbour(i, d);
      if (n >= 0 && g.wet(n) && g.H[static_cast<size_t>(i)] < g.water[static_cast<size_t>(n)] + 1.0f) g.material[static_cast<size_t>(i)] = kSand;
    }
  }

  // The columns, in 1/8 m units: a span from the world floor to the top, or for a cave two spans
  // with the air between.
  float lowest = 1e30f;
  for (int i : inside) lowest = minf(lowest, g.H[static_cast<size_t>(i)]);
  const int floor_u = to_units(lowest - kFloorDepth);
  std::vector<int> top_u(N, 0), cave_lo_u(N, 0), cave_hi_u(N, 0);
  for (int i : inside) {
    const size_t a = static_cast<size_t>(i);
    out.cells.push_back(g.q_of(i));
    out.cells.push_back(g.r_of(i));
    out.span_first.push_back(static_cast<uint32_t>(out.span_lo.size()));
    top_u[a] = mini_(32000, maxi_(floor_u + 1, to_units(g.H[a])));
    if (g.flags[a] & kCave) {
      // The cave's floor 2 m and more under its ceiling; the rock above from the ceiling to the old top.
      const int ceil_u = to_units(g.ceiling[a]);
      const int floor_top = mini_(ceil_u - to_units(kHeadroom), ceil_u - 16);
      cave_lo_u[a] = floor_top;
      cave_hi_u[a] = ceil_u;
      out.span_lo.push_back(floor_u);
      out.span_hi.push_back(floor_top);
      out.span_material.push_back(kRock);
      out.span_lo.push_back(ceil_u);
      out.span_hi.push_back(maxi_(ceil_u + 4, top_u[a]));
      out.span_material.push_back(kRock);
    } else {
      out.span_lo.push_back(floor_u);
      out.span_hi.push_back(top_u[a]);
      out.span_material.push_back(g.material[a]);
    }
    out.water.push_back(g.wet(i) ? to_units(g.water[a]) : -32768);
    out.flags.push_back(g.flags[a] & 0xFFFFu);
  }
  out.span_first.push_back(static_cast<uint32_t>(out.span_lo.size()));

  // 8. The navigation mesh: a hexagon on each walkable top (a cave's floor, and the rock above it
  // when it is open to the sky), linked across edges by the hexbound rules; bridges' decks over the
  // water, linked along their road.
  std::vector<int> poly_of(N, -1), poly_cave(N, -1), poly_deck(N, -1);
  std::vector<float> poly_h;
  static const float kC[6] = {0.8660254f, 0, -0.8660254f, -0.8660254f, 0, 0.8660254f};
  static const float kS[6] = {0.5f, 1, 0.5f, -0.5f, -1, -0.5f};
  std::vector<int> cell_index(N, -1);
  for (size_t k = 0; k < inside.size(); ++k) cell_index[static_cast<size_t>(inside[k])] = static_cast<int>(k);
  auto add_poly = [&](int i, float y) {
    const int id = static_cast<int>(out.nav_cells.size());
    const vec3 c = g.centre(i);
    for (int j = 0; j < 6; ++j) {
      out.nav_polys.push_back(static_cast<uint32_t>(out.nav_vertices.size()));
      out.nav_vertices.push_back(v3(c.x + kC[j], y, c.z - kS[j]));
    }
    out.nav_cells.push_back(static_cast<uint32_t>(cell_index[static_cast<size_t>(i)]));
    poly_h.push_back(y);
    return id;
  };
  for (int i : inside) {
    const size_t a = static_cast<size_t>(i);
    if (g.wet(i) && !(g.flags[a] & kBridge)) continue;  // water isn't walked
    if (g.flags[a] & kBridge) {
      poly_deck[a] = -2;  // added below, along its road
      continue;
    }
    if (g.flags[a] & kCave) {
      poly_cave[a] = add_poly(i, static_cast<float>(cave_lo_u[a]) * kUnit);
      poly_of[a] = add_poly(i, static_cast<float>(maxi_(cave_hi_u[a] + 4, top_u[a])) * kUnit);
    } else {
      poly_of[a] = add_poly(i, static_cast<float>(top_u[a]) * kUnit);
    }
  }
  auto link = [&](int a, int b) {
    if (a < 0 || b < 0 || a == b) return;
    out.nav_links.push_back(static_cast<uint32_t>(a));
    out.nav_links.push_back(static_cast<uint32_t>(b));
  };
  auto mat_of = [&](int i) { return g.material[static_cast<size_t>(i)]; };
  for (int i : inside) {
    for (int d = 0; d < 3; ++d) {  // each edge once
      const int n = g.neighbour(i, d);
      if (n < 0) continue;
      const float limit = maxf(kWalkStep, minf(kSmooth[mat_of(i)], kSmooth[mat_of(n)]));
      // Tops pair with their nearest across the edge: the open tops, and cave floors with cave floors
      // or the open ground at their mouth.
      const int a_top = poly_of[static_cast<size_t>(i)], b_top = poly_of[static_cast<size_t>(n)];
      if (a_top >= 0 && b_top >= 0 && absf(poly_h[static_cast<size_t>(a_top)] - poly_h[static_cast<size_t>(b_top)]) <= limit) link(a_top, b_top);
      const int a_cave = poly_cave[static_cast<size_t>(i)], b_cave = poly_cave[static_cast<size_t>(n)];
      const int a_low = a_cave >= 0 ? a_cave : a_top, b_low = b_cave >= 0 ? b_cave : b_top;
      if ((a_cave >= 0 || b_cave >= 0) && a_low >= 0 && b_low >= 0 &&
          absf(poly_h[static_cast<size_t>(a_low)] - poly_h[static_cast<size_t>(b_low)]) <= limit)
        link(a_low, b_low);
    }
  }
  // Decks along each road: a polygon per bridge cell at the deck's height, linked along the road and
  // to the land at its ends.
  {
    size_t at = 0;
    for (size_t p = 0; p < out.road_counts.size(); ++p) {
      const size_t n = out.road_counts[p];
      int prev_poly = -1;
      for (size_t k = 0; k < n; ++k) {
        const int q = out.road_cells[2 * (at + k)], r = out.road_cells[2 * (at + k) + 1];
        const int i = s.index(q, r);
        int poly;
        if (g.flags[static_cast<size_t>(i)] & kBridge) {
          if (poly_deck[static_cast<size_t>(i)] < 0) {
            float deck = 0;
            for (const auto &b : out.bridges)
              for (size_t j = 0; j + 1 < b.cells.size(); j += 2)
                if (b.cells[j] == q && b.cells[j + 1] == r) deck = b.a.y;
            poly_deck[static_cast<size_t>(i)] = add_poly(i, deck);
          }
          poly = poly_deck[static_cast<size_t>(i)];
          link(prev_poly, poly);
        } else {
          poly = poly_of[static_cast<size_t>(i)];
          // Off a deck onto its bank: the bridge's own end.
          if (k > 0 && (g.flags[static_cast<size_t>(s.index(out.road_cells[2 * (at + k - 1)], out.road_cells[2 * (at + k - 1) + 1]))] & kBridge))
            link(prev_poly, poly);
        }
        prev_poly = poly;
      }
      at += n;
    }
  }
  // Reachability over the links.
  const size_t P = out.nav_cells.size();
  std::vector<std::vector<int>> adj(P);
  for (size_t k = 0; k + 1 < out.nav_links.size(); k += 2) {
    adj[out.nav_links[k]].push_back(static_cast<int>(out.nav_links[k + 1]));
    adj[out.nav_links[k + 1]].push_back(static_cast<int>(out.nav_links[k]));
  }
  auto walk = [&](int from, std::vector<float> &dist) {
    dist.assign(P, 1e30f);
    if (from < 0) return;
    Heap heap;
    dist[static_cast<size_t>(from)] = 0;
    heap.push(0, from);
    while (!heap.empty()) {
      const auto [c, at] = heap.pop();
      if (c > dist[static_cast<size_t>(at)]) continue;
      for (int n : adj[static_cast<size_t>(at)]) {
        const float nc = c + kRun;
        if (nc < dist[static_cast<size_t>(n)]) {
          dist[static_cast<size_t>(n)] = nc;
          heap.push(nc, n);
        }
      }
    }
  };
  std::vector<int> road_polys;
  for (const auto &o : roads) road_polys.push_back(poly_of[static_cast<size_t>(s.index(o.q, o.r))]);
  float longest = 0;
  uint32_t unreached = 0, flats_unreached = 0;
  std::vector<uint8_t> opening_ok(road_polys.size(), 1);
  std::vector<uint8_t> flat_ok(flats.size(), road_polys.empty() ? 1 : 0);
  for (size_t a = 0; a < road_polys.size(); ++a) {
    std::vector<float> dist;
    walk(road_polys[a], dist);
    for (size_t b = 0; b < road_polys.size(); ++b) {
      if (b == a) continue;
      const float d = road_polys[b] >= 0 ? dist[static_cast<size_t>(road_polys[b])] : 1e30f;
      if (d >= 1e29f) opening_ok[b] = 0;
      else longest = maxf(longest, d);
    }
    for (size_t f = 0; f < flats.size(); ++f)
      for (int c : flats[f]) {
        const int poly = poly_of[static_cast<size_t>(c)];
        if (poly >= 0 && dist[static_cast<size_t>(poly)] < 1e29f) flat_ok[f] = 1;
      }
  }
  for (uint8_t ok : opening_ok) unreached += ok ? 0 : 1;
  for (uint8_t ok : flat_ok) flats_unreached += ok ? 0 : 1;
  out.openings_unreached = unreached;
  out.flats_unreached = flats_unreached;
  out.longest_route_m = longest;
  {
    float steepest = 0;
    size_t at = 0;
    for (uint32_t n : out.road_counts) {
      for (size_t k = 1; k < n; ++k) {
        const int a = s.index(out.road_cells[2 * (at + k - 1)], out.road_cells[2 * (at + k - 1) + 1]);
        const int b = s.index(out.road_cells[2 * (at + k)], out.road_cells[2 * (at + k) + 1]);
        if (g.wet(a) || g.wet(b)) continue;
        steepest = maxf(steepest, absf(static_cast<float>(top_u[static_cast<size_t>(a)] - top_u[static_cast<size_t>(b)])) * kUnit / kRun);
      }
      at += n;
    }
    out.steepest_grade = steepest;
  }
  {
    float worst = 0;
    for (size_t f = 0; f < flats.size(); ++f)
      for (int c : flats[f]) worst = maxf(worst, absf(static_cast<float>(top_u[static_cast<size_t>(c)]) * kUnit - flat_level[f]));
    out.flat_error_m = worst;
  }
  {
    float cliff = 0;
    uint32_t caves = 0;
    for (int i : inside) {
      if (g.flags[static_cast<size_t>(i)] & kCave) ++caves;
      for (int d = 0; d < 3; ++d) {
        const int n = g.neighbour(i, d);
        if (n < 0) continue;
        const float dy = absf(static_cast<float>(top_u[static_cast<size_t>(i)] - top_u[static_cast<size_t>(n)])) * kUnit;
        if (dy > minf(kSmooth[mat_of(i)], kSmooth[mat_of(n)])) cliff += 1.0f;  // an edge is a metre long
      }
    }
    out.cliff_m = cliff;
    out.caves = caves;
  }
  out.bridge_count = static_cast<uint32_t>(out.bridges.size());
  if (unreached) out.note = "some road openings can't reach each other";
  else if (flats_unreached) out.note = "some flats aren't reached from a road opening";
  else if (roads.size() == 1 && !flats.empty() && out.road_counts.empty()) out.note = "no road was laid";
  else if (out.steepest_grade > kRoadStep / kRun + 0.01f)
    out.note = "a road climbs steeper than its grade where it is held (between flats at different levels, or onto a bridge)";

  // The shape for the viewer: its corners at their sides' heights, and the outline's segments.
  for (size_t i = 0; i < s.corner_q.size(); ++i) {
    const shape::Side c = shape::side_at(s, static_cast<int>(i), 0, side_len[i]);
    out.corners.push_back(v3(hex_x(s.corner_q[i], s.corner_r[i]), c.height, hex_z(s.corner_r[i])));
    out.side_edges.push_back(s.sides[i].edge);
  }
  for (size_t i = 0; i < s.outline.size(); ++i) {
    const vec3 a = s.outline[i], b = s.outline[(i + 1) % s.outline.size()];
    int qa, ra, qb, rb;
    shape::to_axial(a.x, a.z, qa, ra);
    shape::to_axial(b.x, b.z, qb, rb);
    const float ha = s.contains(qa, ra) ? g.H[static_cast<size_t>(s.index(qa, ra))] : 0;
    const float hb = s.contains(qb, rb) ? g.H[static_cast<size_t>(s.index(qb, rb))] : 0;
    out.side_lines.push_back(v3(a.x, ha + 0.3f, a.z));
    out.side_lines.push_back(v3(b.x, hb + 0.3f, b.z));
    out.side_of_line.push_back(static_cast<uint32_t>(s.outline_side[i]));
  }

  // 9. The boundary's hull: the ring outside, each cell with its side's edge, height, and rise, and
  // the openings leaving through it.
  {
    boundary_v1_hull &h = out.hull;
    h.name = in.shape.name;
    h.radius = 3;
    h.rise = 0.3f;
    for (const auto &c : s.hull) {
      const shape::Side v = shape::side_at(s, c.side, c.t, side_len[static_cast<size_t>(c.side)]);
      h.cells.push_back(c.q);
      h.cells.push_back(c.r);
      h.edges.push_back(v.edge);
      h.heights.push_back(v.height);
      h.rises.push_back(v.rise);
    }
    for (const shape::Opening &o : s.openings) {
      h.opening_cells.push_back(o.out_q);
      h.opening_cells.push_back(o.out_r);
      h.opening_kinds.push_back(o.kind);
      h.opening_widths.push_back(o.width);
    }
  }

  // The plotter's nature ground: open land available (wetter near water), water as water, roads and
  // bridges a path no plant takes, rock as rock; grouped by height (5 cm) and moisture.
  {
    std::vector<int> wet;
    for (int i : inside)
      if (g.wet(i)) wet.push_back(i);
    const std::vector<int> dw = steps_from(g, wet, 20);
    std::vector<int> road;
    for (int i : inside)
      if (g.flags[static_cast<size_t>(i)] & (kRoad | kLane)) road.push_back(i);
    const std::vector<int> dr = steps_from(g, road, 2);
    struct Key {
      uint32_t kind;
      int h, m;
    };
    std::vector<Key> keys;
    std::vector<std::vector<int32_t>> groups;
    std::vector<uint32_t> order;  // keys sorted, for finding a group by binary search
    auto key_less = [](const Key &a, const Key &b) {
      return a.kind != b.kind ? a.kind < b.kind : (a.h != b.h ? a.h < b.h : a.m < b.m);
    };
    for (int i : inside) {
      const size_t a = static_cast<size_t>(i);
      uint32_t kind = 0;  // 0 open, 1 water, 2 path, 3 rock
      if (g.wet(i)) kind = 1;
      else if (g.flags[a] & (kRoad | kLane | kBridge)) kind = 2;
      else if (g.material[a] == kRock || (g.flags[a] & kCave)) kind = 3;
      const int k = dw[a];
      const float moist = kind == 1 ? 1.0f : clampf(0.85f - 0.06f * static_cast<float>(k < 20 ? k : 20), 0.3f, 0.85f);
      Key key{kind, round_to_int(static_cast<float>(top_u[a]) * kUnit / 0.05f), round_to_int(moist / 0.05f)};
      // Kept clear of the roads: a cell beside one is open but takes nothing with a footprint.
      if (kind == 0 && dr[a] == 1) key.kind = 4;
      size_t lo = 0, hi = order.size();
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (key_less(keys[order[mid]], key)) lo = mid + 1;
        else hi = mid;
      }
      size_t gi;
      if (lo < order.size() && !key_less(key, keys[order[lo]]) && !key_less(keys[order[lo]], key)) {
        gi = order[lo];
      } else {
        gi = keys.size();
        keys.push_back(key);
        groups.emplace_back();
        order.insert(order.begin() + static_cast<long>(lo), static_cast<uint32_t>(gi));
      }
      groups[gi].push_back(g.q_of(i));
      groups[gi].push_back(g.r_of(i));
    }
    for (size_t k = 0; k < keys.size(); ++k) {
      plot_v1_region r;
      r.shape = plot_v1_region::kShapeCells;
      r.cells = groups[k];
      r.priority = 10;
      r.cut = false;
      r.set = 1 | 2 | 8 | 16;  // ground, available, height, moisture
      const uint32_t kind = keys[k].kind;
      r.ground = kind == 1 ? plot_v1_region::kGroundWater
                           : (kind == 2 ? plot_v1_region::kGroundPath : (kind == 3 ? plot_v1_region::kGroundRock : plot_v1_region::kGroundSoil));
      r.available = kind == 0;
      r.keep_clear_m = 0;
      r.height = static_cast<float>(keys[k].h) * 0.05f;
      r.moisture = static_cast<float>(keys[k].m) * 0.05f;
      out.nature.regions.push_back(r);
    }
  }
  return true;
}

}  // namespace terrain_relief_v1

FOUNDRY_SYSTEM(terrain_relief_v1, channels::terrain_v1_spec, channels::terrain_v1_ground)
