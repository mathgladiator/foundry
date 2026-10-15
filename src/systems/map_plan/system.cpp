// map_plan: a map's macro plan (design/map.md, "Executing it").
//
// 1. The shape (shape.h): its outline, its cells, the ring outside (the boundary's hull), its exits.
// 2. A grid every 8 m over the map. Each node takes the parcel nearest it (its distance less the
//    parcel's reach, after warping the node's place by noise, so borders wander) and the parcels'
//    relief blended over 140 m across borders: base height, hills (rolling or ridged), and each
//    kind's own shape (peaks for mountains, basins for lakes and coasts, pools for marshes). Near the
//    map's edge the land eases to its sides' heights.
// 3. Sites' flats levelled into it, blended into the land round them.
// 4. Rivers: each the cheapest way downhill from its source to its mouth (an exit, a lake, or the
//    nearest water or edge), its water never rising, carved into a valley.
// 5. Links: each the cheapest way between its ends over the grid (grade past the kind's limit dear,
//    lakes barred, a river's crossing a bridge, roads already laid cheap), smoothed, resampled every
//    4 m, graded to the kind's limit between pinned ends and decks, and pressed into the land.
// 6. Places (each site on its flat, turned toward its road), chunks (64 x 64 axial parallelograms
//    holding any of the map's cells), the hull, and the report.
#include <cstdlib>
#include <string>
#include <vector>

#include "boundary.h"
#include "channels/map_plan.h"
#include "channels/map_spec.h"
#include "math.h"
#include "packer/map_plan.h"
#include "packer/map_spec.h"

#define SHAPE_NS map_plan
#include "shape.h"

namespace map_plan {

using namespace bmath;
using channels::map_flat;
using channels::map_link;
using channels::map_parcel;
using channels::map_place;
using channels::map_segment;
using channels::map_site;
using channels::map_spec;

namespace {

constexpr float kG = 8.0f;          // the grid's spacing, metres
constexpr uint32_t kOutside = 65535;
constexpr float kDry = -1000.0f;
constexpr float kBlend = 140.0f;    // how far parcels' relief blends across a border, metres
constexpr float kWarp = 90.0f;      // how far a border wanders, metres
constexpr float kBand = 120.0f;     // the map's edge eases to its sides' heights over this, metres
enum Kind : uint32_t { kPlains, kHills, kHighlands, kMountains, kValley, kLake, kMarsh, kCoast, kKinds };

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

struct Grid {
  float gx = 0, gz = 0;
  int nx = 0, nz = 0;
  std::vector<float> h, water;
  std::vector<uint32_t> parcel;
  std::vector<uint8_t> in, road, river;
  size_t size() const { return static_cast<size_t>(nx) * static_cast<size_t>(nz); }
  float x(int i) const { return gx + static_cast<float>(i) * kG; }
  float z(int k) const { return gz + static_cast<float>(k) * kG; }
  int i_of(int id) const { return id % nx; }
  int k_of(int id) const { return id / nx; }
  vec3 pos(int id) const { return v3(x(i_of(id)), h[static_cast<size_t>(id)], z(k_of(id))); }
  // The nearest node inside the map (searching outward a few rings), or the nearest node.
  int nearest_inside(float px, float pz) const {
    const int c = nearest(px, pz);
    if (in[static_cast<size_t>(c)]) return c;
    int best = c;
    float bd = 1e30f;
    for (int ring = 1; ring <= 6 && bd > 1e29f; ++ring)
      for (int k = k_of(c) - ring; k <= k_of(c) + ring; ++k)
        for (int i = i_of(c) - ring; i <= i_of(c) + ring; ++i) {
          if (i < 0 || k < 0 || i >= nx || k >= nz || !in[static_cast<size_t>(k * nx + i)]) continue;
          const float dx = x(i) - px, dz = z(k) - pz;
          if (dx * dx + dz * dz < bd) {
            bd = dx * dx + dz * dz;
            best = k * nx + i;
          }
        }
    return best;
  }
  int nearest(float px, float pz) const {
    const int i = maxi_(0, mini_(nx - 1, round_to_int((px - gx) / kG)));
    const int k = maxi_(0, mini_(nz - 1, round_to_int((pz - gz) / kG)));
    return k * nx + i;
  }
  float sample(float px, float pz) const {  // bilinear
    const float fx = clampf((px - gx) / kG, 0, static_cast<float>(nx - 1) - 1e-3f);
    const float fz = clampf((pz - gz) / kG, 0, static_cast<float>(nz - 1) - 1e-3f);
    const int i = static_cast<int>(std::floor(fx)), k = static_cast<int>(std::floor(fz));
    const float u = fx - static_cast<float>(i), v = fz - static_cast<float>(k);
    const size_t a = static_cast<size_t>(k * nx + i);
    return mixf(mixf(h[a], h[a + 1], u), mixf(h[a + static_cast<size_t>(nx)], h[a + static_cast<size_t>(nx) + 1], u), v);
  }
};

constexpr int kNi[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kNk[8] = {0, 0, 1, -1, 1, -1, 1, -1};
const float kNlen[8] = {1, 1, 1, 1, 1.4142136f, 1.4142136f, 1.4142136f, 1.4142136f};

// The cheapest path over the grid's inside nodes from any of `from` to any goal, by step cost
// (negative: barred). Node ids, from start to goal; empty if none.
template <typename Cost>
std::vector<int> cheapest(const Grid &g, const std::vector<int> &from, const std::vector<uint8_t> &goal, Cost cost) {
  std::vector<float> best(g.size(), 1e30f);
  std::vector<int> prev(g.size(), -1);
  Heap heap;
  for (int f : from) {
    best[static_cast<size_t>(f)] = 0;
    heap.push(0, f);
  }
  int reached = -1;
  while (!heap.empty()) {
    const auto [c, at] = heap.pop();
    if (c > best[static_cast<size_t>(at)]) continue;
    if (goal[static_cast<size_t>(at)]) {
      reached = at;
      break;
    }
    const int i = g.i_of(at), k = g.k_of(at);
    for (int d = 0; d < 8; ++d) {
      const int ni = i + kNi[d], nk = k + kNk[d];
      if (ni < 0 || nk < 0 || ni >= g.nx || nk >= g.nz) continue;
      const int n = nk * g.nx + ni;
      if (!g.in[static_cast<size_t>(n)]) continue;
      const float step = cost(at, n, kNlen[d] * kG);
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
  for (size_t a = 0, b = path.size(); b > 1 && a < b - 1; ++a, --b) {
    const int t = path[a];
    path[a] = path[b - 1];
    path[b - 1] = t;
  }
  return path;
}

// A polyline's corners cut twice (endpoints kept), then resampled about every `step` metres.
std::vector<vec3> smooth_line(const std::vector<vec3> &pts, float step) {
  std::vector<vec3> line = pts;
  for (int pass = 0; pass < 2 && line.size() > 2; ++pass) {
    std::vector<vec3> cut = {line.front()};
    for (size_t i = 0; i + 1 < line.size(); ++i) {
      cut.push_back(lerp(line[i], line[i + 1], 0.25f));
      cut.push_back(lerp(line[i], line[i + 1], 0.75f));
    }
    cut.push_back(line.back());
    line.swap(cut);
  }
  std::vector<float> along(line.size(), 0.0f);
  for (size_t i = 1; i < line.size(); ++i) along[i] = along[i - 1] + length(v3(line[i].x - line[i - 1].x, 0, line[i].z - line[i - 1].z));
  const float total = along.back();
  const int n = maxi_(1, round_to_int(total / step));
  std::vector<vec3> out;
  size_t seg = 0;
  for (int k = 0; k <= n; ++k) {
    const float at = total * static_cast<float>(k) / static_cast<float>(n);
    while (seg + 2 < line.size() && along[seg + 1] < at) ++seg;
    const float len = along[seg + 1] - along[seg];
    const float f = len > 1e-6f ? clampf((at - along[seg]) / len, 0, 1) : 0.0f;
    out.push_back(lerp(line[seg], line[seg + 1], f));
  }
  return out;
}

float horizontal(vec3 a, vec3 b) { return length(v3(b.x - a.x, 0, b.z - a.z)); }

// Distance from p to segment ab in the horizontal, and where along it (0 to 1).
float to_segment(float px, float pz, vec3 a, vec3 b, float &t) {
  const float abx = b.x - a.x, abz = b.z - a.z;
  const float l2 = abx * abx + abz * abz;
  t = l2 > 1e-9f ? clampf(((px - a.x) * abx + (pz - a.z) * abz) / l2, 0, 1) : 0.0f;
  const float dx = a.x + abx * t - px, dz = a.z + abz * t - pz;
  return foundry::det::sqrt(dx * dx + dz * dz);
}

}  // namespace

bool run(const map_spec &in, channels::map_plan &out, std::string &error) {
  out = channels::map_plan{};
  const uint32_t seed = in.seed ? in.seed : 1u;
  // 1. The shape.
  shape::Resolved s;
  if (!shape::resolve(in.shape, s, error)) return false;
  for (const vec3 &p : s.outline) out.outline.push_back(p);
  const size_t sides = s.sides.size();
  std::vector<float> side_len(sides);
  for (size_t i = 0; i < sides; ++i) side_len[i] = shape::side_length(s, static_cast<int>(i));

  // 2. The grid.
  Grid g;
  float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
  for (const vec3 &p : s.outline) {
    xmin = minf(xmin, p.x);
    xmax = maxf(xmax, p.x);
    zmin = minf(zmin, p.z);
    zmax = maxf(zmax, p.z);
  }
  g.gx = std::floor(xmin / kG) * kG - 3 * kG;
  g.gz = std::floor(zmin / kG) * kG - 3 * kG;
  g.nx = static_cast<int>((xmax - g.gx) / kG) + 4;
  g.nz = static_cast<int>((zmax - g.gz) / kG) + 4;
  if (static_cast<long>(g.nx) * g.nz > 4000000) {
    error = "the map is too big";
    return false;
  }
  const size_t N = g.size();
  g.h.assign(N, 0.0f);
  g.water.assign(N, kDry);
  g.parcel.assign(N, kOutside);
  g.in.assign(N, 0);
  g.road.assign(N, 0);
  g.river.assign(N, 0);
  {  // inside: the outline's even-odd test, row by row
    std::vector<float> cross;
    const size_t m = s.outline.size();
    for (int k = 0; k < g.nz; ++k) {
      const float z = g.z(k) + 1e-3f;
      cross.clear();
      for (size_t a = 0, b = m - 1; a < m; b = a++) {
        const vec3 &pa = s.outline[a], &pb = s.outline[b];
        if ((pa.z > z) != (pb.z > z)) cross.push_back((pb.x - pa.x) * (z - pa.z) / (pb.z - pa.z) + pa.x);
      }
      for (size_t a = 1; a < cross.size(); ++a)
        for (size_t c = a; c > 0 && cross[c] < cross[c - 1]; --c) {
          const float t = cross[c];
          cross[c] = cross[c - 1];
          cross[c - 1] = t;
        }
      size_t before = 0;
      for (int i = 0; i < g.nx; ++i) {
        const float x = g.x(i) + 1e-3f;
        while (before < cross.size() && !(x < cross[before])) ++before;
        if ((cross.size() - before) % 2 == 1) g.in[static_cast<size_t>(k * g.nx + i)] = 1;
      }
    }
  }
  // Distance to the outside, metres (a 3-4 chamfer).
  std::vector<float> edge_d(N, 1e9f);
  for (size_t a = 0; a < N; ++a)
    if (!g.in[a]) edge_d[a] = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (int kk = 0; kk < g.nz; ++kk) {
      const int k = pass == 0 ? kk : g.nz - 1 - kk;
      for (int ii = 0; ii < g.nx; ++ii) {
        const int i = pass == 0 ? ii : g.nx - 1 - ii;
        float &d = edge_d[static_cast<size_t>(k * g.nx + i)];
        const int dk = pass == 0 ? -1 : 1, di = pass == 0 ? -1 : 1;
        const int pts[4][2] = {{i + di, k}, {i - di, k + dk}, {i, k + dk}, {i + di, k + dk}};
        const float w[4] = {1, 1.4142136f, 1, 1.4142136f};
        for (int c = 0; c < 4; ++c) {
          const int pi = pts[c][0], pk = pts[c][1];
          if (pi < 0 || pk < 0 || pi >= g.nx || pk >= g.nz) continue;
          d = minf(d, edge_d[static_cast<size_t>(pk * g.nx + pi)] + w[c] * kG);
        }
      }
    }

  // Parcels: each node's nearest (warped, less its reach) and the blend of their relief.
  const size_t P = in.parcels.size();
  std::vector<vec3> centre(P);
  for (size_t p = 0; p < P; ++p) centre[p] = v3(hex_x(in.parcels[p].q, in.parcels[p].r), 0, hex_z(in.parcels[p].r));
  std::vector<uint32_t> cells(P, 0);
  std::vector<float> lake_level(N, kDry);
  std::vector<float> d(P);
  for (size_t a = 0; a < N; ++a) {
    if (!g.in[a]) continue;
    const int i = static_cast<int>(a % static_cast<size_t>(g.nx)), k = static_cast<int>(a / static_cast<size_t>(g.nx));
    const float x = g.x(i), z = g.z(k);
    if (P == 0) {
      g.h[a] = 0;
      continue;
    }
    const float wx = x + kWarp * 2 * (fbm(x / 300.0f, z / 300.0f, 3, seed + 101) - 0.5f);
    const float wz = z + kWarp * 2 * (fbm(x / 300.0f + 17.3f, z / 300.0f - 9.1f, 3, seed + 103) - 0.5f);
    float dmin = 1e30f, dsec = 1e30f;
    size_t best = 0;
    for (size_t p = 0; p < P; ++p) {
      d[p] = foundry::det::sqrt((wx - centre[p].x) * (wx - centre[p].x) + (wz - centre[p].z) * (wz - centre[p].z)) - in.parcels[p].reach;
      if (d[p] < dmin) {
        dsec = dmin;
        dmin = d[p];
        best = p;
      } else if (d[p] < dsec) {
        dsec = d[p];
      }
    }
    g.parcel[a] = static_cast<uint32_t>(best);
    ++cells[best];
    float wsum = 0, base = 0, relief = 0, size = 0, ridges = 0, kinds[kKinds] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t p = 0; p < P; ++p) {
      const float t = 1 - (d[p] - dmin) / kBlend;
      if (t <= 0) continue;
      const float w = t * t;
      const map_parcel &pc = in.parcels[p];
      wsum += w;
      base += w * pc.height;
      relief += w * clampf(pc.relief, 0, 400);
      size += w * clampf(pc.hill_size, 20, 2000);
      ridges += w * clampf(pc.ridges, 0, 1);
      kinds[pc.kind < kKinds ? pc.kind : kPlains] += w;
    }
    base /= wsum;
    relief /= wsum;
    size /= wsum;
    ridges /= wsum;
    for (float &kw : kinds) kw /= wsum;
    const float roll = fbm(x / size, z / size, 4, seed + 1) - 0.5f;
    const float rid = ridged(x / (size * 1.2f), z / (size * 1.2f), 5, seed + 2) - 0.35f;
    float h = base + relief * mixf(roll * 1.6f, rid * 1.5f, ridges);
    if (kinds[kMountains] > 0) {
      float peaks = ridged(x / (size * 0.7f), z / (size * 0.7f), 5, seed + 3);
      peaks = peaks * foundry::det::sqrt(peaks);
      h += kinds[kMountains] * relief * 0.9f * peaks;
    }
    // Basins: a lake's or coast's floor falls away from its shore; a marsh lies at its level with
    // pools. The node's own parcel holds the water.
    const map_parcel &own = in.parcels[best];
    const float inner = (dsec - dmin) * 0.5f;  // about how far inside its parcel's border
    const float wet = kinds[kLake] + kinds[kCoast];
    if (wet > 0) h = mixf(h, base - 2.0f - 10.0f * (1 - exp_(-inner / 70.0f)), wet);
    if (kinds[kMarsh] > 0) h = mixf(h, base + 0.9f * (fbm(x / 35.0f, z / 35.0f, 3, seed + 5) - 0.55f), kinds[kMarsh]);
    if (own.kind == kLake || own.kind == kCoast || own.kind == kMarsh) lake_level[a] = own.height;
    // The edge eases to its sides' heights.
    if (edge_d[a] < kBand) {
      float best_d = 1e30f, side_h = h;
      const size_t m = s.outline.size();
      float run_along = 0;
      int run_side = -1;
      for (size_t c = 0; c < m; ++c) {
        if (s.outline_side[c] != run_side) {
          run_side = s.outline_side[c];
          run_along = 0;
        }
        const vec3 pa = s.outline[c], pb = s.outline[(c + 1) % m];
        float t;
        const float dd = to_segment(x, z, pa, pb, t);
        if (dd < best_d) {
          best_d = dd;
          const float at = (run_along + t * horizontal(pa, pb)) / maxf(side_len[static_cast<size_t>(run_side)], 1e-3f);
          side_h = shape::side_at(s, run_side, clampf(at, 0, 1), side_len[static_cast<size_t>(run_side)]).height;
        }
        run_along += horizontal(pa, pb);
      }
      h = mixf(side_h, h, smooth(clampf(edge_d[a] / kBand, 0, 1)));
    }
    g.h[a] = h;
  }
  out.parcels = in.parcels;
  out.parcel_cells = cells;

  // 3. Sites' flats.
  for (const map_site &st : in.sites) {
    if (st.radius == 0) continue;
    const float cx = hex_x(st.q, st.r), cz = hex_z(st.r);
    float level = st.level ? st.height : g.sample(cx, cz);
    {  // a flat in a lake or marsh is raised clear of it (landfill)
      const int c = g.nearest(cx, cz);
      if (!st.level && lake_level[static_cast<size_t>(c)] > kDry) level = maxf(level, lake_level[static_cast<size_t>(c)] + 0.8f);
    }
    const float rad = static_cast<float>(st.radius) * 1.65f;
    const float blend = maxf(30.0f, rad);
    map_flat f;
    f.position = v3(cx, level, cz);
    f.radius = rad;
    out.flats.push_back(f);
    const int span = static_cast<int>((rad + blend) / kG) + 2;
    const int ci = round_to_int((cx - g.gx) / kG), ck = round_to_int((cz - g.gz) / kG);
    for (int k = ck - span; k <= ck + span; ++k)
      for (int i = ci - span; i <= ci + span; ++i) {
        if (i < 0 || k < 0 || i >= g.nx || k >= g.nz) continue;
        const size_t a = static_cast<size_t>(k * g.nx + i);
        const float dx = g.x(i) - cx, dz = g.z(k) - cz;
        const float dd = foundry::det::sqrt(dx * dx + dz * dz);
        if (dd >= rad + blend) continue;
        g.h[a] = mixf(level, g.h[a], smooth(clampf((dd - rad) / blend, 0, 1)));
        if (dd < rad) lake_level[a] = kDry;
      }
  }
  // Lakes: water where the ground lies below its parcel's level.
  for (size_t a = 0; a < N; ++a)
    if (g.in[a] && lake_level[a] > kDry && g.h[a] < lake_level[a]) g.water[a] = lake_level[a];

  // 4. Rivers.
  for (size_t ri = 0; ri < in.rivers.size(); ++ri) {
    const auto &rv = in.rivers[ri];
    const int src = g.nearest_inside(hex_x(rv.q, rv.r), hex_z(rv.r));
    if (!g.in[static_cast<size_t>(src)]) continue;
    std::vector<uint8_t> goal(N, 0);
    bool any = false;
    if (rv.to.rfind("exit:", 0) == 0) {
      const size_t e = static_cast<size_t>(std::atoi(rv.to.c_str() + 5));
      if (e < s.openings.size()) {
        const int n = g.nearest_inside(hex_x(s.openings[e].q, s.openings[e].r), hex_z(s.openings[e].r));
        goal[static_cast<size_t>(n)] = 1;
        any = true;
      }
    } else {
      for (size_t p = 0; p < P; ++p)
        if (!rv.to.empty() && in.parcels[p].name == rv.to)
          for (size_t a = 0; a < N; ++a)
            if (g.parcel[a] == p && g.water[a] > kDry) {
              goal[a] = 1;
              any = true;
            }
    }
    if (!any)  // wherever the land leads: any water, any river, or the edge
      for (size_t a = 0; a < N; ++a)
        if (g.in[a] && (g.water[a] > kDry || g.river[a] || edge_d[a] <= kG * 1.5f) && static_cast<int>(a) != src) goal[a] = 1;
    const std::vector<int> path = cheapest(g, {src}, goal, [&](int at, int n, float len) -> float {
      const float dh = g.h[static_cast<size_t>(n)] - g.h[static_cast<size_t>(at)];
      return len * (1 + (dh > 0 ? dh * 2.5f : 0.0f));
    });
    if (path.size() < 2) continue;
    const float w = static_cast<float>(maxi_(1, static_cast<int>(rv.width))) * 1.6f;
    const float depth = 1.0f + 0.06f * w;
    std::vector<vec3> pts;
    // A river leaving a lake starts at its shore; one reaching a lake ends there.
    bool ashore = !(g.water[static_cast<size_t>(path[0])] > kDry);
    float level = 1e30f;
    for (int id : path) {
      const bool lake = g.water[static_cast<size_t>(id)] > kDry && !g.river[static_cast<size_t>(id)];
      if (!ashore) {
        if (lake) {
          level = g.water[static_cast<size_t>(id)];
          continue;
        }
        ashore = true;
      }
      const float lw = lake ? g.water[static_cast<size_t>(id)] : g.h[static_cast<size_t>(id)] - 0.6f;
      level = minf(level, lw);
      pts.push_back(v3(g.x(g.i_of(id)), level, g.z(g.k_of(id))));
      if (lake) break;  // into a lake
    }
    if (pts.size() < 2) continue;
    const std::vector<vec3> line = smooth_line(pts, 6.0f);
    for (size_t c = 0; c + 1 < line.size(); ++c) {
      map_segment sg;
      sg.a = line[c];
      sg.b = line[c + 1];
      sg.width = w;
      sg.kind = 3;
      sg.bridge = false;
      out.roads.push_back(sg);
      out.road_of.push_back(65536u + static_cast<uint32_t>(ri));
      // Carve: the bed under the water, the banks climbing gently from it.
      const float reach = w * 0.5f + 40.0f;
      const int i0 = static_cast<int>((minf(sg.a.x, sg.b.x) - reach - g.gx) / kG) - 1, i1 = static_cast<int>((maxf(sg.a.x, sg.b.x) + reach - g.gx) / kG) + 1;
      const int k0 = static_cast<int>((minf(sg.a.z, sg.b.z) - reach - g.gz) / kG) - 1, k1 = static_cast<int>((maxf(sg.a.z, sg.b.z) + reach - g.gz) / kG) + 1;
      for (int k = maxi_(0, k0); k <= mini_(g.nz - 1, k1); ++k)
        for (int i = maxi_(0, i0); i <= mini_(g.nx - 1, i1); ++i) {
          const size_t a = static_cast<size_t>(k * g.nx + i);
          if (!g.in[a]) continue;
          float t;
          const float dd = to_segment(g.x(i), g.z(k), sg.a, sg.b, t);
          if (dd > reach) continue;
          const float lv = mixf(sg.a.y, sg.b.y, t);
          if (dd < w * 0.5f + kG * 0.75f) {
            g.h[a] = minf(g.h[a], lv - depth);
            g.water[a] = maxf(g.water[a], lv);
            g.river[a] = 1;
          } else {
            g.h[a] = minf(g.h[a], lv + 0.4f + (dd - w * 0.5f) * 0.35f);
          }
        }
    }
  }

  // 5. Links.
  auto place_of = [&](const std::string &name, vec3 &at, float &level, bool &ok) {
    ok = false;
    if (name.rfind("exit:", 0) == 0) {
      const size_t e = static_cast<size_t>(std::atoi(name.c_str() + 5));
      if (e >= s.openings.size()) return;
      at = v3(hex_x(s.openings[e].q, s.openings[e].r), 0, hex_z(s.openings[e].r));
      level = g.sample(at.x, at.z);
      ok = true;
      return;
    }
    for (const map_site &st : in.sites)
      if (st.name == name) {
        at = v3(hex_x(st.q, st.r), 0, hex_z(st.r));
        level = g.sample(at.x, at.z);
        ok = true;
        return;
      }
  };
  float steepest = 0, road_m = 0;
  uint32_t bridges = 0, unrouted = 0;
  for (size_t li = 0; li < in.links.size(); ++li) {
    const map_link &lk = in.links[li];
    const uint32_t kind = lk.kind > 2 ? 2 : lk.kind;
    const float gmax = kind == 0 ? 0.08f : (kind == 1 ? 0.12f : 0.18f);
    const float width = kind == 0 ? 4.5f : (kind == 1 ? 3.0f : 1.8f);
    vec3 a, b;
    float la = 0, lb = 0;
    bool oka, okb;
    place_of(lk.from, a, la, oka);
    place_of(lk.to, b, lb, okb);
    if (!oka || !okb) {
      ++unrouted;
      continue;
    }
    // A settlement's link ends at its edge, toward the other end (its own streets carry on inside).
    auto to_edge = [&](const std::string &name, vec3 &end, float &level, vec3 toward) {
      for (const map_site &st : in.sites)
        if (st.name == name && !st.planner.empty() && st.radius > 0) {
          const vec3 d = v3(toward.x - end.x, 0, toward.z - end.z);
          const float l = length(d);
          if (l < 1) return;
          end = end + d * (minf(static_cast<float>(st.radius) * 1.65f * 0.92f, l * 0.45f) / l);
          return;
        }
      (void)level;
    };
    {
      const vec3 a0 = a, b0 = b;
      to_edge(lk.from, a, la, lk.via.size() >= 2 ? v3(hex_x(lk.via[0], lk.via[1]), 0, hex_z(lk.via[1])) : b0);
      to_edge(lk.to, b, lb, lk.via.size() >= 2 ? v3(hex_x(lk.via[lk.via.size() - 2], lk.via[lk.via.size() - 1]), 0, hex_z(lk.via[lk.via.size() - 1])) : a0);
    }
    std::vector<vec3> stops = {a};
    for (size_t v = 0; v + 1 < lk.via.size(); v += 2) stops.push_back(v3(hex_x(lk.via[v], lk.via[v + 1]), 0, hex_z(lk.via[v + 1])));
    stops.push_back(b);
    std::vector<int> nodes;
    bool failed = false;
    for (size_t sp = 0; sp + 1 < stops.size() && !failed; ++sp) {
      const int from = g.nearest_inside(stops[sp].x, stops[sp].z), to = g.nearest_inside(stops[sp + 1].x, stops[sp + 1].z);
      if (!g.in[static_cast<size_t>(from)] || !g.in[static_cast<size_t>(to)]) {
        failed = true;
        break;
      }
      std::vector<uint8_t> goal(N, 0);
      goal[static_cast<size_t>(to)] = 1;
      const std::vector<int> part = cheapest(g, {from}, goal, [&](int at, int n, float len) -> float {
        const size_t an = static_cast<size_t>(n);
        const bool marsh = g.water[an] > kDry && !g.river[an] && g.parcel[an] < P && in.parcels[g.parcel[an]].kind == kMarsh;
        if (g.water[an] > kDry && !g.river[an] && !marsh) return -1;  // lakes and the sea are barred
        const float grade = absf(g.h[an] - g.h[static_cast<size_t>(at)]) / len;
        float c = len * (1 + (grade > gmax ? (grade - gmax) * 60.0f : 0.0f));
        if (g.river[an] && !g.river[static_cast<size_t>(at)]) c += 120;  // a bridge
        if (marsh) c *= 5;                                                // a causeway
        if (g.road[an]) c *= 0.45f;
        return c;
      });
      if (part.empty()) {
        failed = true;
        break;
      }
      for (size_t c = nodes.empty() ? 0 : 1; c < part.size(); ++c) nodes.push_back(part[c]);
    }
    if (failed || nodes.size() < 2) {
      ++unrouted;
      continue;
    }
    std::vector<vec3> pts;
    pts.push_back(v3(a.x, la, a.z));
    for (size_t c = 1; c + 1 < nodes.size(); ++c) pts.push_back(g.pos(nodes[c]));
    pts.push_back(v3(b.x, lb, b.z));
    std::vector<vec3> line = smooth_line(pts, 4.0f);
    const size_t n = line.size();
    // Heights: the land's, a deck clear of any river, graded between the pinned ends and decks.
    std::vector<uint8_t> pin(n, 0), deck(n, 0);
    for (size_t c = 0; c < n; ++c) {
      const int id = g.nearest(line[c].x, line[c].z);
      line[c].y = g.sample(line[c].x, line[c].z);
      if (g.river[static_cast<size_t>(id)]) {
        line[c].y = g.water[static_cast<size_t>(id)] + 2.0f;
        deck[c] = 1;
      } else if (g.water[static_cast<size_t>(id)] > kDry) {  // a causeway over the marsh
        line[c].y = maxf(line[c].y, g.water[static_cast<size_t>(id)] + 0.6f);
        pin[c] = 1;
      }
    }
    for (size_t c = 0; c < n;) {  // a deck level over its run, with its higher bank
      if (!deck[c]) {
        ++c;
        continue;
      }
      size_t e = c;
      float top = -1e30f;
      while (e < n && deck[e]) top = maxf(top, line[e++].y);
      if (c > 0) top = maxf(top, line[c - 1].y);
      if (e < n) top = maxf(top, line[e].y);
      for (size_t j = c; j < e; ++j) {
        line[j].y = top;
        pin[j] = 1;
      }
      c = e;
    }
    pin[0] = pin[n - 1] = 1;
    line[0].y = la;
    line[n - 1].y = lb;
    for (int pass = 0; pass < 200; ++pass) {
      bool moved = false;
      for (size_t c = 1; c < n; ++c)
        if (!pin[c]) {
          const float lim = gmax * horizontal(line[c - 1], line[c]);
          const float y = clampf(line[c].y, line[c - 1].y - lim, line[c - 1].y + lim);
          moved = moved || y != line[c].y;
          line[c].y = y;
        }
      for (size_t c = n - 1; c-- > 0;)
        if (!pin[c]) {
          const float lim = gmax * horizontal(line[c], line[c + 1]);
          const float y = clampf(line[c].y, line[c + 1].y - lim, line[c + 1].y + lim);
          moved = moved || y != line[c].y;
          line[c].y = y;
        }
      if (!moved) break;
    }
    for (size_t c = 0; c + 1 < n; ++c) {
      map_segment sg;
      sg.a = line[c];
      sg.b = line[c + 1];
      sg.width = width;
      sg.kind = kind;
      sg.bridge = deck[c] && deck[c + 1];
      if (sg.bridge && (c == 0 || !deck[c - 1])) ++bridges;
      const float run = horizontal(sg.a, sg.b);
      road_m += run;
      if (run > 0.5f && !sg.bridge) steepest = maxf(steepest, absf(sg.b.y - sg.a.y) / run);
      out.roads.push_back(sg);
      out.road_of.push_back(static_cast<uint32_t>(li));
      // Press it into the land: its surface under it, the ground easing to it over 14 m.
      const float reach = width * 0.5f + 14.0f;
      const int i0 = static_cast<int>((minf(sg.a.x, sg.b.x) - reach - g.gx) / kG) - 1, i1 = static_cast<int>((maxf(sg.a.x, sg.b.x) + reach - g.gx) / kG) + 1;
      const int k0 = static_cast<int>((minf(sg.a.z, sg.b.z) - reach - g.gz) / kG) - 1, k1 = static_cast<int>((maxf(sg.a.z, sg.b.z) + reach - g.gz) / kG) + 1;
      for (int k = maxi_(0, k0); k <= mini_(g.nz - 1, k1); ++k)
        for (int i = maxi_(0, i0); i <= mini_(g.nx - 1, i1); ++i) {
          const size_t ai = static_cast<size_t>(k * g.nx + i);
          if (!g.in[ai] || g.river[ai]) continue;
          float t;
          const float dd = to_segment(g.x(i), g.z(k), sg.a, sg.b, t);
          if (dd > reach) continue;
          if (g.water[ai] > kDry) {  // a causeway: only its own width, built up out of the water
            if (dd < width * 0.5f + kG * 0.75f) {
              g.h[ai] = mixf(sg.a.y, sg.b.y, t);
              g.water[ai] = kDry;
              g.road[ai] = 1;
            }
            continue;
          }
          const float y = mixf(sg.a.y, sg.b.y, t);
          g.h[ai] = mixf(y, g.h[ai], smooth(clampf((dd - width * 0.5f) / 14.0f, 0, 1)));
          if (dd < width * 0.5f + kG * 0.5f) g.road[ai] = 1;
        }
    }
  }

  // 6. Places, chunks, the hull, and the report.
  for (size_t si = 0; si < in.sites.size(); ++si) {
    const map_site &st = in.sites[si];
    map_place pl;
    pl.name = st.name;
    pl.kind = st.kind;
    const float cx = hex_x(st.q, st.r), cz = hex_z(st.r);
    pl.position = v3(cx, g.sample(cx, cz), cz);
    for (const map_flat &f : out.flats)
      if (f.position.x == cx && f.position.z == cz) pl.position.y = f.position.y;
    pl.radius = static_cast<float>(st.radius) * 1.65f;
    pl.yaw = st.yaw;
    if (st.yaw < 0) {  // toward the nearest road beyond its flat
      float bd = 1e30f;
      vec3 toward = v3(0, 0, -1);
      for (size_t c = 0; c < out.roads.size(); ++c) {
        if (out.roads[c].kind == 3) continue;
        const vec3 q = out.roads[c].a;
        const float dd = horizontal(q, pl.position);
        if (dd > pl.radius * 0.8f && dd < bd) {
          bd = dd;
          toward = q - pl.position;
        }
      }
      pl.yaw = bd < 1e29f ? atan2_(toward.x, -toward.z) * (180.0f / kPi) : 0.0f;
      if (pl.yaw < 0) pl.yaw += 360.0f;
    }
    pl.planner = st.planner;
    pl.pipeline = st.pipeline;
    pl.params = st.params;
    pl.seed = st.seed;
    out.places.push_back(pl);
  }
  const int S = static_cast<int>(in.chunk_size >= 16 && in.chunk_size <= 128 ? in.chunk_size : 64u);
  out.chunk_size = static_cast<uint32_t>(S);
  auto floor_div = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
  for (int cr = floor_div(s.r0, S) * S; cr < s.r0 + s.h; cr += S)
    for (int cq = floor_div(s.q0, S) * S; cq < s.q0 + s.w; cq += S) {
      bool any = false;
      for (int r = cr; r < cr + S && !any; ++r)
        for (int q = cq; q < cq + S && !any; ++q) any = s.contains(q, r);
      if (!any) continue;
      out.chunk_q.push_back(cq);
      out.chunk_r.push_back(cr);
    }
  {
    channels::boundary_hull &h = out.hull;
    h.name = in.name;
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
  out.gx = g.gx;
  out.gz = g.gz;
  out.spacing = kG;
  out.nx = static_cast<uint32_t>(g.nx);
  out.nz = static_cast<uint32_t>(g.nz);
  out.heights = g.h;
  out.parcel_ids = g.parcel;
  out.water = g.water;
  out.links_unrouted = unrouted;
  out.road_m = road_m;
  out.bridges = bridges;
  out.steepest_grade = steepest;
  if (unrouted) out.note = std::to_string(unrouted) + " links found no way";
  return true;
}

}  // namespace map_plan

FOUNDRY_SYSTEM(map_plan, channels::map_spec, channels::map_plan)
