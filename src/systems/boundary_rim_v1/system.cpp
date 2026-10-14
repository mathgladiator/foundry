// boundary_rim_v1: a boundary's hull resolved into its rim (design/boundary.md).
//
// 1. The cells: the hull's own, or the hex ring of its radius; the gap between consecutive cells
//    filled with the hex line between them (taking the earlier cell's values), repeats dropped, and
//    the loop turned counterclockwise as seen from above.
// 2. Each cell's edge (auto cells take their direction's sector), height, and rise.
// 3. The rim: the line through the cells' centres, its corners cut twice (so a hex line's zigzag
//    reads as one line), sampled every metre or so, each sample with the cells' values where it
//    lies.
// 4. Transitions: each sample's share of each edge is the edges along the rim around it, blended
//    over `blend` metres either way, so a cliff tapers into a forest rather than stopping; heights
//    are blended over a few metres and rises over four times `blend`.
// 5. Each sample's outward direction, the rim's middle, reach, and length.
// 6. Openings (design/boundary.md, "Hulls from shapes"): where a road or river leaves the region,
//    the rim sample nearest its cell and its way out, for the terrain to carry it on.
//
// A hull may name a shape instead of cells: its ring is the cells just outside the shape, each with
// its side's edge, height, and rise, and its openings leave through it (shape.h, shared with the
// terrain builder so the two meet).
#include <vector>

#include "boundary.h"
#include "channels/boundary_v1_rim.h"
#include "channels/boundary_v1_rim_spec.h"
#include "math.h"
#include "packer/boundary_v1_rim.h"
#include "packer/boundary_v1_rim_spec.h"

#define SHAPE_NS boundary_rim_v1
#include "shape.h"

namespace boundary_rim_v1 {

using namespace bmath;
using channels::boundary_v1_hull;
using channels::boundary_v1_rim;
using channels::boundary_v1_rim_spec;

namespace {

constexpr int kEdges = 7;    // ledge, ridge, mountain, forest, jungle, sea, marsh (edge values 1 to 7)
constexpr uint32_t kForest = 4;

struct Cell {
  int q = 0, r = 0;
  uint32_t edge = 0;
  float height = 0, rise = -1;
};

// The hex ring of radius n around the origin, counterclockwise as seen from above.
std::vector<Cell> ring(int n) {
  static const int dq[6] = {1, 1, 0, -1, -1, 0}, dr[6] = {-1, 0, 1, 1, 0, -1};
  std::vector<Cell> out;
  int q = -n, r = n;  // start at a corner, then walk the six sides
  for (int side = 0; side < 6; ++side)
    for (int k = 0; k < n; ++k) {
      Cell c;
      c.q = q;
      c.r = r;
      out.push_back(c);
      q += dq[side];
      r += dr[side];
    }
  return out;
}

// The cells on the hex line from a to b, excluding both ends (cube coordinates, rounded).
void line_between(const Cell &a, const Cell &b, std::vector<Cell> &out) {
  const int n = hex_distance(a.q, a.r, b.q, b.r);
  for (int k = 1; k < n; ++k) {
    const float t = static_cast<float>(k) / static_cast<float>(n);
    // Nudged off the edges between cells, so a line along one rounds the same way every time.
    const float fq = mixf(static_cast<float>(a.q), static_cast<float>(b.q), t) + 1e-4f;
    const float fr = mixf(static_cast<float>(a.r), static_cast<float>(b.r), t) + 2e-4f;
    const float fs = -fq - fr;
    int rq = round_to_int(fq), rr = round_to_int(fr), rs = round_to_int(fs);
    const float eq = absf(static_cast<float>(rq) - fq), er = absf(static_cast<float>(rr) - fr),
                es = absf(static_cast<float>(rs) - fs);
    if (eq > er && eq > es) rq = -rr - rs;
    else if (er > es) rr = -rq - rs;
    Cell c = a;
    c.q = rq;
    c.r = rr;
    out.push_back(c);
  }
}

// A value along the loop at a fractional cell position t (0 to n), from the cells either side.
struct Sample {
  vec3 p;
  float t;  // position in cells along the loop
};

}  // namespace

bool run(const boundary_v1_rim_spec &in, boundary_v1_rim &out, std::string &error) {
  const boundary_v1_hull &h = in.hull;
  out = boundary_v1_rim{};
  out.seed = in.seed;

  // 1. The cells.
  std::vector<Cell> given;
  struct OpeningCell {
    int q, r;
    uint32_t kind, width;
  };
  std::vector<OpeningCell> openings;
  for (size_t i = 0; i + 1 < h.opening_cells.size(); i += 2)
    openings.push_back({h.opening_cells[i], h.opening_cells[i + 1], i / 2 < h.opening_kinds.size() ? h.opening_kinds[i / 2] : 0u,
                        i / 2 < h.opening_widths.size() ? h.opening_widths[i / 2] : 1u});
  if (h.cells.size() < 2 && !h.shape.sides.empty()) {
    shape::Resolved s;
    if (!shape::resolve(h.shape, s, error)) return false;
    std::vector<float> len(s.sides.size());
    for (size_t i = 0; i < s.sides.size(); ++i) len[i] = shape::side_length(s, static_cast<int>(i));
    for (const auto &rc : s.hull) {
      const shape::Side v = shape::side_at(s, rc.side, rc.t, len[static_cast<size_t>(rc.side)]);
      Cell c;
      c.q = rc.q;
      c.r = rc.r;
      c.edge = v.edge;
      c.height = v.height;
      c.rise = v.rise;
      given.push_back(c);
    }
    for (const shape::Opening &o : s.openings) openings.push_back({o.out_q, o.out_r, o.kind, o.width});
  } else if (h.cells.size() >= 2) {
    for (size_t i = 0; i + 1 < h.cells.size(); i += 2) {
      Cell c;
      c.q = h.cells[i];
      c.r = h.cells[i + 1];
      const size_t k = i / 2;
      c.edge = k < h.edges.size() ? h.edges[k] : 0;
      if (c.edge > kEdges) c.edge = 0;
      c.height = k < h.heights.size() ? h.heights[k] : 0.0f;
      c.rise = k < h.rises.size() ? h.rises[k] : -1.0f;
      given.push_back(c);
    }
  } else {
    const int n = maxi_(3, mini_(150, static_cast<int>(h.radius)));
    given = ring(n);
  }
  if (given.size() < 3) {
    error = "a hull needs at least three cells";
    return false;
  }
  std::vector<Cell> cells;
  for (size_t i = 0; i < given.size(); ++i) {
    const Cell &a = given[i], &b = given[(i + 1) % given.size()];
    if (!cells.empty() && cells.back().q == a.q && cells.back().r == a.r) continue;
    cells.push_back(a);
    line_between(a, b, cells);
  }
  while (cells.size() > 1 && cells.back().q == cells.front().q && cells.back().r == cells.front().r) cells.pop_back();
  // Drop spurs (a, b, a): a line that doubles back on itself encloses nothing.
  for (bool changed = true; changed && cells.size() > 3;) {
    changed = false;
    for (size_t i = 0; i < cells.size() && cells.size() > 3; ++i) {
      const size_t n = cells.size();
      const Cell &a = cells[(i + n - 1) % n], &c = cells[(i + 1) % n];
      if (a.q == c.q && a.r == c.r) {
        // Remove cells[i] and cells[i + 1] (c repeats a).
        const size_t j = (i + 1) % n;
        if (j > i) {
          cells.erase(cells.begin() + static_cast<long>(j));
          cells.erase(cells.begin() + static_cast<long>(i));
        } else {
          cells.erase(cells.begin() + static_cast<long>(i));
          cells.erase(cells.begin() + static_cast<long>(j));
        }
        changed = true;
        break;
      }
    }
  }
  if (cells.size() < 3) {
    error = "the hull encloses nothing";
    return false;
  }
  const size_t n = cells.size();
  // Counterclockwise as seen from above (+x east, +z toward the viewer's bottom): a negative area
  // in the (x, z) shoelace sum.
  float area2 = 0;
  for (size_t i = 0; i < n; ++i) {
    const Cell &a = cells[i], &b = cells[(i + 1) % n];
    area2 += hex_x(a.q, a.r) * hex_z(b.r) - hex_x(b.q, b.r) * hex_z(a.r);
  }
  if (area2 > 0) {
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
      const Cell t = cells[i];
      cells[i] = cells[j];
      cells[j] = t;
    }
  }

  // 2. Edges, heights, and rises.
  float cx = 0, cz = 0;
  for (const Cell &c : cells) {
    cx += hex_x(c.q, c.r);
    cz += hex_z(c.r);
  }
  cx /= static_cast<float>(n);
  cz /= static_cast<float>(n);
  const size_t sectors = h.sectors.size();
  for (Cell &c : cells) {
    // The angle counterclockwise from +x as seen from above (toward -z).
    float a = atan2_(-(hex_z(c.r) - cz), hex_x(c.q, c.r) - cx);
    if (a < 0) a += 2 * kPi;
    const size_t s = sectors ? mini_(static_cast<int>(sectors) - 1, static_cast<int>(a / (2 * kPi) * static_cast<float>(sectors))) : 0;
    const bool automatic = c.edge == 0;
    if (automatic) c.edge = sectors ? h.sectors[s] : kForest;
    if (c.edge == 0 || c.edge > kEdges) c.edge = kForest;
    if (c.rise < 0) c.rise = automatic && s < h.sector_rises.size() ? h.sector_rises[s] : h.rise;
    c.rise = clampf(c.rise, 0, 1);
  }
  for (const Cell &c : cells) {
    out.cells.push_back(c.q);
    out.cells.push_back(c.r);
    out.cell_edges.push_back(c.edge);
    out.cell_heights.push_back(c.height);
  }

  // 3. The rim: the centres' loop with its corners cut twice, then resampled about every metre.
  std::vector<Sample> loop;
  for (size_t i = 0; i < n; ++i) loop.push_back({v3(hex_x(cells[i].q, cells[i].r), 0, hex_z(cells[i].r)), static_cast<float>(i)});
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<Sample> cut;
    const size_t m = loop.size();
    for (size_t i = 0; i < m; ++i) {
      const Sample &a = loop[i], &b = loop[(i + 1) % m];
      const float tb = (i + 1 == m) ? b.t + static_cast<float>(n) : b.t;  // unwrapped
      cut.push_back({lerp(a.p, b.p, 0.25f), mixf(a.t, tb, 0.25f)});
      cut.push_back({lerp(a.p, b.p, 0.75f), mixf(a.t, tb, 0.75f)});
    }
    for (Sample &s : cut)
      if (s.t >= static_cast<float>(n)) s.t -= static_cast<float>(n);
    loop.swap(cut);
  }
  const size_t m = loop.size();
  std::vector<float> along(m + 1, 0.0f);
  for (size_t i = 0; i < m; ++i) along[i + 1] = along[i] + length(loop[(i + 1) % m].p - loop[i].p);
  const float total = along[m];
  const int count = maxi_(8, round_to_int(total));
  out.length = total;
  std::vector<vec3> pts(static_cast<size_t>(count));
  std::vector<float> ts(static_cast<size_t>(count));
  size_t seg = 0;
  for (int k = 0; k < count; ++k) {
    const float at = total * static_cast<float>(k) / static_cast<float>(count);
    while (seg + 1 < m && along[seg + 1] <= at) ++seg;
    const float len = along[seg + 1] - along[seg];
    const float f = len > 1e-6f ? (at - along[seg]) / len : 0.0f;
    const Sample &a = loop[seg], &b = loop[(seg + 1) % m];
    float tb = b.t;
    if (tb < a.t) tb += static_cast<float>(n);
    float t = mixf(a.t, tb, f);
    if (t >= static_cast<float>(n)) t -= static_cast<float>(n);
    pts[static_cast<size_t>(k)] = lerp(a.p, b.p, f);
    ts[static_cast<size_t>(k)] = t;
  }

  // The cells' values at each sample, before blending.
  const size_t N = static_cast<size_t>(count);
  std::vector<float> raw_w(N * kEdges, 0.0f), raw_h(N), raw_r(N);
  for (size_t k = 0; k < N; ++k) {
    const float t = ts[k];
    const size_t i0 = static_cast<size_t>(std::floor(t)) % n, i1 = (i0 + 1) % n;
    const float f = t - std::floor(t);
    raw_w[k * kEdges + (cells[i0].edge - 1)] += 1 - f;
    raw_w[k * kEdges + (cells[i1].edge - 1)] += f;
    raw_h[k] = mixf(cells[i0].height, cells[i1].height, f);
    raw_r[k] = mixf(cells[i0].rise, cells[i1].rise, f);
  }
  // 4. Blends along the loop: a triangle filter `width` samples either way (samples are about a
  // metre apart).
  auto blend = [&](const std::vector<float> &src, size_t stride, float width, std::vector<float> &dst) {
    const int w = maxi_(1, round_to_int(width));
    dst.assign(src.size(), 0.0f);
    for (size_t k = 0; k < N; ++k) {
      float wsum = 0;
      for (int o = -w; o <= w; ++o) {
        const float wt = static_cast<float>(w + 1 - absi_(o));
        const size_t j = (k + N + static_cast<size_t>(o + static_cast<int>(N) * 4)) % N;
        for (size_t c = 0; c < stride; ++c) dst[k * stride + c] += src[j * stride + c] * wt;
        wsum += wt;
      }
      for (size_t c = 0; c < stride; ++c) dst[k * stride + c] /= wsum;
    }
  };
  const float width = clampf(in.blend, 1, 20) * static_cast<float>(N) / maxf(total, 1.0f);
  std::vector<float> wts, hts, rss;
  blend(raw_w, kEdges, width, wts);
  blend(raw_h, 1, 3.0f * static_cast<float>(N) / maxf(total, 1.0f), hts);
  blend(raw_r, 1, 4.0f * width, rss);

  // 5. Outward directions: perpendicular to the rim, away from the inside.
  out.points.resize(N);
  out.outward.resize(N);
  out.rises.resize(N);
  out.weights.resize(N * kEdges);
  float sx = 0, sz = 0;
  for (size_t k = 0; k < N; ++k) {
    const vec3 prev = pts[(k + N - 1) % N], next = pts[(k + 1) % N];
    vec3 t = next - prev;
    t.y = 0;
    t = normalize(t);
    out.outward[k] = v3(-t.z, 0, t.x);  // one perpendicular; turned outward below if need be
    out.points[k] = v3(pts[k].x, hts[k], pts[k].z);
    out.rises[k] = clampf(rss[k], 0, 1);
    float sum = 0;
    for (int e = 0; e < kEdges; ++e) sum += wts[k * kEdges + e];
    for (int e = 0; e < kEdges; ++e) out.weights[k * kEdges + e] = sum > 0 ? wts[k * kEdges + e] / sum : (e == 3 ? 1.0f : 0.0f);
    sx += pts[k].x;
    sz += pts[k].z;
  }
  out.centre = v3(sx / static_cast<float>(N), 0, sz / static_cast<float>(N));
  // Make sure the outward directions point away from the inside: on the whole they lead away from
  // the middle.
  float lead = 0;
  for (size_t k = 0; k < N; ++k) lead += dot(out.outward[k], out.points[k] - out.centre);
  if (lead < 0)
    for (vec3 &o : out.outward) o = -o;
  float hsum = 0;
  for (size_t k = 0; k < N; ++k) hsum += out.points[k].y;
  out.centre.y = hsum / static_cast<float>(N);
  float reach = 0;
  for (const vec3 &p : out.points) reach = maxf(reach, length(v3(p.x - out.centre.x, 0, p.z - out.centre.z)));
  out.extent = reach;

  // 6. Openings: the nearest sample to each opening's cell, its height there, and its way out.
  for (const OpeningCell &o : openings) {
    const vec3 c = v3(hex_x(o.q, o.r), 0, hex_z(o.r));
    size_t best = 0;
    float bd = 1e30f;
    for (size_t k = 0; k < N; ++k) {
      const float dx = out.points[k].x - c.x, dz = out.points[k].z - c.z;
      if (dx * dx + dz * dz < bd) {
        bd = dx * dx + dz * dz;
        best = k;
      }
    }
    out.opening_points.push_back(out.points[best]);
    out.opening_dirs.push_back(out.outward[best]);
    out.opening_kinds.push_back(o.kind);
    out.opening_widths.push_back(static_cast<float>(maxi_(1, mini_(8, static_cast<int>(o.width)))) * kSqrt3 + 1.0f);
  }
  return true;
}

}  // namespace boundary_rim_v1

FOUNDRY_SYSTEM(boundary_rim_v1, channels::boundary_v1_rim_spec, channels::boundary_v1_rim)
