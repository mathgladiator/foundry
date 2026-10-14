// A shape resolved into cells (design/terrain.md, "The shape and its sides"): its corners from the
// kind and sizes (or given), turned in 60 degree steps and counterclockwise as seen from above; each
// side meandering by its wobble between fixed corners, chained as hex lines; the cells inside; each
// border cell's side and place along it; the ring of cells just outside (a boundary's hull), traced
// counterclockwise; and the openings' cells.
//
// The terrain builder and the boundary's rim both carry a copy (spine.md: write everything twice);
// edit them in step. The including system defines SHAPE_NS (its namespace, which holds bmath).
#pragma once

#include <vector>

#include "channels/terrain_v1_shape.h"

namespace SHAPE_NS::shape {

using namespace SHAPE_NS::bmath;

// The six neighbours, counterclockwise as seen from above, from east (north is -z).
constexpr int kDq[6] = {1, 1, 0, -1, -1, 0}, kDr[6] = {0, -1, -1, 0, 1, 1};
enum : uint32_t { kHexagon = 0, kRectangle = 1, kTriangle = 2, kL = 3, kPolygon = 4 };
enum : uint32_t { kForestEdge = 4 };

struct Side {
  uint32_t edge = kForestEdge;
  float height = 0, rise = 0.3f;
};

struct Opening {
  uint32_t kind = 0, side = 0, width = 1;
  int q = 0, r = 0;        // its cell on the edge (inside the shape)
  int out_q = 0, out_r = 0;  // the hull's cell beyond it
  vec3 inward;             // the way in (horizontal, unit)
};

struct Resolved {
  std::vector<int> corner_q, corner_r;  // counterclockwise
  std::vector<Side> sides;              // one per corner
  std::vector<vec3> outline;            // the wobbled outline's control points (x, 0, z)
  std::vector<int> outline_side;        // each control point's side (the segment to the next)
  // The grid over the shape's box: cell (q, r) at index (r - r0) * w + (q - q0).
  int q0 = 0, r0 = 0, w = 0, h = 0;
  std::vector<uint8_t> inside;   // 1 inside the shape
  std::vector<int> side_of;      // each inside cell's nearest side
  std::vector<float> along;      // its place along that side, 0 to 1
  std::vector<int> depth;        // steps to the nearest cell outside (1 on the border)
  std::vector<int> cells;        // the inside cells as q, r pairs, row by row
  struct HullCell {
    int q, r, side;
    float t;
  };
  std::vector<HullCell> hull;    // the ring outside, counterclockwise
  std::vector<Opening> openings;
  vec3 centre;                   // the cells' middle (x, 0, z)

  bool in_box(int q, int r) const { return q >= q0 && r >= r0 && q < q0 + w && r < r0 + h; }
  int index(int q, int r) const { return (r - r0) * w + (q - q0); }
  bool contains(int q, int r) const { return in_box(q, r) && inside[static_cast<size_t>(index(q, r))]; }
};

inline void to_axial(float x, float z, int &q, int &r) {
  // Cube rounding of the fractional axial coordinates.
  const float fr = z / 1.5f, fq = x / kSqrt3 - fr * 0.5f, fs = -fq - fr;
  int rq = round_to_int(fq), rr = round_to_int(fr);
  const int rs = round_to_int(fs);
  const float eq = absf(static_cast<float>(rq) - fq), er = absf(static_cast<float>(rr) - fr), es = absf(static_cast<float>(rs) - fs);
  if (eq > er && eq > es) rq = -rr - rs;
  else if (er > es) rr = -rq - rs;
  q = rq;
  r = rr;
}

// A side's value at a place along it: its own, blending into its neighbours' round each corner
// (half each at the corner itself) over `blend_m` metres.
inline Side side_at(const Resolved &s, int side, float t, float length_m, float blend_m = 10.0f) {
  const size_t n = s.sides.size();
  const Side &me = s.sides[static_cast<size_t>(side)];
  const Side &prev = s.sides[(static_cast<size_t>(side) + n - 1) % n], &next = s.sides[(static_cast<size_t>(side) + 1) % n];
  const float b = minf(blend_m, length_m * 0.5f);
  const float d0 = t * length_m, d1 = (1 - t) * length_m;
  Side out = me;
  if (b > 0 && d0 < b) {
    const float k = 0.5f * (1 - smooth(d0 / b));
    out.height = mixf(me.height, prev.height, k);
    out.rise = mixf(me.rise, prev.rise, k);
  } else if (b > 0 && d1 < b) {
    const float k = 0.5f * (1 - smooth(d1 / b));
    out.height = mixf(me.height, next.height, k);
    out.rise = mixf(me.rise, next.rise, k);
  }
  return out;
}

// The length of a side's outline, metres.
inline float side_length(const Resolved &s, int side) {
  const size_t n = s.corner_q.size();
  const size_t a = static_cast<size_t>(side), b = (a + 1) % n;
  const vec3 pa = v3(hex_x(s.corner_q[a], s.corner_r[a]), 0, hex_z(s.corner_r[a]));
  const vec3 pb = v3(hex_x(s.corner_q[b], s.corner_r[b]), 0, hex_z(s.corner_r[b]));
  return length(pb - pa);
}

inline bool resolve(const channels::terrain_v1_shape &in, Resolved &s, std::string &error) {
  s = Resolved{};
  // 1. Corners, in x and z.
  std::vector<vec3> pts;
  auto axial = [&](int q, int r) { pts.push_back(v3(hex_x(q, r), 0, hex_z(r))); };
  const int R = maxi_(4, mini_(200, static_cast<int>(in.radius)));
  const float W = static_cast<float>(maxi_(4, mini_(300, static_cast<int>(in.width)))) * kSqrt3 * 0.5f;
  const float D = static_cast<float>(maxi_(4, mini_(300, static_cast<int>(in.depth)))) * 0.75f;
  switch (in.kind) {
    case kRectangle:
      pts = {v3(-W, 0, -D), v3(W, 0, -D), v3(W, 0, D), v3(-W, 0, D)};
      break;
    case kTriangle:
      for (int k = 0; k < 6; k += 2) axial(kDq[k] * R, kDr[k] * R);
      break;
    case kL: {
      const float n = clampf(static_cast<float>(in.notch), 10, 80) * 0.01f;
      const float nx = W - 2 * W * n, nz = D - 2 * D * n;
      pts = {v3(-W, 0, -D), v3(W, 0, -D), v3(W, 0, nz), v3(nx, 0, nz), v3(nx, 0, D), v3(-W, 0, D)};
      break;
    }
    case kPolygon:
      for (size_t i = 0; i + 1 < in.corners.size(); i += 2) axial(in.corners[i], in.corners[i + 1]);
      break;
    default:
      for (int k = 0; k < 6; ++k) axial(kDq[k] * R, kDr[k] * R);
  }
  if (pts.size() < 3) {
    error = "a shape needs at least three corners";
    return false;
  }
  // Snapped to cells, turned in 60 degree steps: (q, r) -> (-r, q + r) turns a sixth.
  for (const vec3 &p : pts) {
    int q, r;
    to_axial(p.x, p.z, q, r);
    for (uint32_t k = 0; k < in.rotation % 6; ++k) {
      const int nq = -r, nr = q + r;
      q = nq;
      r = nr;
    }
    if (!s.corner_q.empty() && s.corner_q.back() == q && s.corner_r.back() == r) continue;
    s.corner_q.push_back(q);
    s.corner_r.push_back(r);
  }
  while (s.corner_q.size() > 1 && s.corner_q.back() == s.corner_q.front() && s.corner_r.back() == s.corner_r.front()) {
    s.corner_q.pop_back();
    s.corner_r.pop_back();
  }
  const size_t n = s.corner_q.size();
  if (n < 3) {
    error = "a shape needs at least three distinct corners";
    return false;
  }
  // Counterclockwise as seen from above: a negative shoelace sum in (x, z).
  float area2 = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    area2 += hex_x(s.corner_q[i], s.corner_r[i]) * hex_z(s.corner_r[j]) - hex_x(s.corner_q[j], s.corner_r[j]) * hex_z(s.corner_r[i]);
  }
  if (area2 > 0)
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
      const int tq = s.corner_q[i], tr = s.corner_r[i];
      s.corner_q[i] = s.corner_q[j];
      s.corner_r[i] = s.corner_r[j];
      s.corner_q[j] = tq;
      s.corner_r[j] = tr;
    }
  if (area2 == 0) {
    error = "the shape encloses nothing";
    return false;
  }
  // The sides' aspects: missing sides repeat the last.
  for (size_t i = 0; i < n; ++i) {
    Side sd;
    if (!in.sides.empty()) {
      const auto &g = in.sides[i < in.sides.size() ? i : in.sides.size() - 1];
      sd.edge = g.edge >= 1 && g.edge <= 7 ? g.edge : (g.edge == 0 ? 0u : kForestEdge);
      sd.height = clampf(g.height, -40, 80);
      sd.rise = clampf(g.rise, 0, 1);
    }
    s.sides.push_back(sd);
  }

  // 2. The outline: each side's control points every 4 m or so, pushed sideways by its wobble
  // (zero at the corners).
  const float wobble = clampf(in.wobble, 0, 12) * kSqrt3;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    const vec3 a = v3(hex_x(s.corner_q[i], s.corner_r[i]), 0, hex_z(s.corner_r[i]));
    const vec3 b = v3(hex_x(s.corner_q[j], s.corner_r[j]), 0, hex_z(s.corner_r[j]));
    const float len = length(b - a);
    const int k = maxi_(1, round_to_int(len / 4.0f));
    const vec3 along = (b - a) * (1.0f / maxf(len, 1e-3f)), across = v3(along.z, 0, -along.x);
    const uint32_t hs = hash2(static_cast<int>(i), static_cast<int>(in.seed), 0x5A17u);
    const float ph1 = unit(hs) * 2 * kPi, ph2 = unit(hash_u32(hs)) * 2 * kPi;
    const float f1 = 1 + static_cast<float>(round_to_int(len / 60.0f)), f2 = f1 * 2 + 1;
    for (int m = 0; m < k; ++m) {
      const float t = static_cast<float>(m) / static_cast<float>(k);
      const float wave = 0.65f * foundry::det::sin(2 * kPi * f1 * t * 0.5f + ph1) + 0.35f * foundry::det::sin(2 * kPi * f2 * t * 0.5f + ph2);
      const float env = foundry::det::sin(kPi * t);
      s.outline.push_back(lerp(a, b, t) + across * (wobble * wave * env));
      s.outline_side.push_back(static_cast<int>(i));
    }
  }

  // 3. The cells inside: the box round the outline, each cell's centre tested against it (even-odd),
  // and every cell the outline passes through.
  float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
  for (const vec3 &p : s.outline) {
    xmin = minf(xmin, p.x);
    xmax = maxf(xmax, p.x);
    zmin = minf(zmin, p.z);
    zmax = maxf(zmax, p.z);
  }
  s.r0 = round_to_int(zmin / 1.5f) - 3;
  s.h = round_to_int(zmax / 1.5f) + 3 - s.r0 + 1;
  s.q0 = round_to_int(xmin / kSqrt3 - static_cast<float>(s.r0 + s.h) * 0.5f) - 3;
  s.w = round_to_int(xmax / kSqrt3 - static_cast<float>(s.r0) * 0.5f) + 3 - s.q0 + 1;
  if (static_cast<long>(s.w) * s.h > 4000000) {
    error = "the shape is too big";
    return false;
  }
  const size_t cells = static_cast<size_t>(s.w) * static_cast<size_t>(s.h);
  s.inside.assign(cells, 0);
  const size_t m = s.outline.size();
  for (int r = s.r0; r < s.r0 + s.h; ++r)
    for (int q = s.q0; q < s.q0 + s.w; ++q) {
      const float x = hex_x(q, r) + 1e-3f, z = hex_z(r) + 2e-3f;  // nudged off the outline's vertices
      bool in_poly = false;
      for (size_t i = 0, j = m - 1; i < m; j = i++) {
        const vec3 &pi = s.outline[i], &pj = s.outline[j];
        if ((pi.z > z) != (pj.z > z) && x < (pj.x - pi.x) * (z - pi.z) / (pj.z - pi.z) + pi.x) in_poly = !in_poly;
      }
      if (in_poly) s.inside[static_cast<size_t>(s.index(q, r))] = 1;
    }
  for (size_t i = 0; i < m; ++i) {
    const vec3 &a = s.outline[i], &b = s.outline[(i + 1) % m];
    const int steps = maxi_(1, round_to_int(length(b - a) * 2));
    for (int k = 0; k <= steps; ++k) {
      const vec3 p = lerp(a, b, static_cast<float>(k) / static_cast<float>(steps));
      int q, r;
      to_axial(p.x, p.z, q, r);
      if (s.in_box(q, r)) s.inside[static_cast<size_t>(s.index(q, r))] = 1;
    }
  }

  // 4. Each inside cell's nearest side and place along it (the nearest outline segment), and its
  // depth: steps to the nearest cell outside.
  s.side_of.assign(cells, -1);
  s.along.assign(cells, 0.0f);
  s.depth.assign(cells, 0);
  std::vector<float> seg_start(m + 1, 0.0f);  // each control point's distance along its side
  for (size_t i = 0; i < m; ++i) {
    const bool first = i == 0 || s.outline_side[i] != s.outline_side[i - 1];
    seg_start[i] = first ? 0.0f : seg_start[i - 1] + length(s.outline[i] - s.outline[i - 1]);
  }
  std::vector<float> side_len(n, 0.0f);
  for (size_t i = 0; i < m; ++i) {
    const size_t side = static_cast<size_t>(s.outline_side[i]);
    side_len[side] = maxf(side_len[side], seg_start[i] + length(s.outline[(i + 1) % m] - s.outline[i]));
  }
  std::vector<int> frontier;
  for (int r = s.r0; r < s.r0 + s.h; ++r)
    for (int q = s.q0; q < s.q0 + s.w; ++q) {
      const size_t at = static_cast<size_t>(s.index(q, r));
      if (!s.inside[at]) continue;
      s.cells.push_back(q);
      s.cells.push_back(r);
      bool border = false;
      for (int d = 0; d < 6; ++d) border = border || !s.contains(q + kDq[d], r + kDr[d]);
      if (border) {
        s.depth[at] = 1;
        frontier.push_back(static_cast<int>(at));
      }
      const vec3 p = v3(hex_x(q, r), 0, hex_z(r));
      float best = 1e30f;
      for (size_t i = 0; i < m; ++i) {
        const vec3 &a = s.outline[i], &b = s.outline[(i + 1) % m];
        const vec3 ab = b - a;
        const float l2 = maxf(dot(ab, ab), 1e-6f);
        const float t = clampf(dot(p - a, ab) / l2, 0, 1);
        const vec3 c = a + ab * t - p;
        const float d2 = dot(c, c);
        if (d2 < best) {
          best = d2;
          const size_t side = static_cast<size_t>(s.outline_side[i]);
          s.side_of[at] = static_cast<int>(side);
          s.along[at] = clampf((seg_start[i] + t * foundry::det::sqrt(l2)) / maxf(side_len[side], 1e-3f), 0, 1);
        }
      }
    }
  if (s.cells.size() < 6) {
    error = "the shape holds too few cells";
    return false;
  }
  for (size_t f = 0; f < frontier.size(); ++f) {  // breadth first, in order: the same every time
    const int at = frontier[f];
    const int q = s.q0 + at % s.w, r = s.r0 + at / s.w;
    for (int d = 0; d < 6; ++d) {
      const int nq = q + kDq[d], nr = r + kDr[d];
      if (!s.contains(nq, nr)) continue;
      const size_t ni = static_cast<size_t>(s.index(nq, nr));
      if (s.depth[ni]) continue;
      s.depth[ni] = s.depth[static_cast<size_t>(at)] + 1;
      frontier.push_back(static_cast<int>(ni));
    }
  }
  float cx = 0, cz = 0;
  for (size_t i = 0; i + 1 < s.cells.size(); i += 2) {
    cx += hex_x(s.cells[i], s.cells[i + 1]);
    cz += hex_z(s.cells[i + 1]);
  }
  const float count = static_cast<float>(s.cells.size() / 2);
  s.centre = v3(cx / count, 0, cz / count);

  // 5. The ring outside: every edge between an inside cell and one outside, running with the shape
  // on its left as seen from above, chained corner to corner; the longest loop is the outer one.
  struct Edge {
    long fx, fz, tx, tz;
    int q, r, side;
    float t;
    bool used;
  };
  static const float kC[6] = {0.8660254f, 0, -0.8660254f, -0.8660254f, 0, 0.8660254f};
  static const float kS[6] = {0.5f, 1, 0.5f, -0.5f, -1, -0.5f};
  auto corner_key = [](int q, int r, int j, long &x, long &z) {
    x = static_cast<long>(round_to_int((hex_x(q, r) + kC[j]) * 1000.0f));
    z = static_cast<long>(round_to_int((hex_z(r) - kS[j]) * 1000.0f));
  };
  std::vector<Edge> edges;
  for (size_t i = 0; i + 1 < s.cells.size(); i += 2) {
    const int q = s.cells[i], r = s.cells[i + 1];
    const size_t at = static_cast<size_t>(s.index(q, r));
    for (int d = 0; d < 6; ++d) {
      const int nq = q + kDq[d], nr = r + kDr[d];
      if (s.contains(nq, nr)) continue;
      Edge e{};
      corner_key(q, r, (d + 5) % 6, e.fx, e.fz);
      corner_key(q, r, d, e.tx, e.tz);
      e.q = nq;
      e.r = nr;
      e.side = s.side_of[at];
      e.t = s.along[at];
      edges.push_back(e);
    }
  }
  // Chain them: sorted by their first corner, each edge's next is one starting where it ends.
  std::vector<size_t> by_from(edges.size());
  for (size_t i = 0; i < edges.size(); ++i) by_from[i] = i;
  auto less = [&](size_t a, size_t b) {
    return edges[a].fx != edges[b].fx ? edges[a].fx < edges[b].fx : (edges[a].fz != edges[b].fz ? edges[a].fz < edges[b].fz : a < b);
  };
  for (size_t i = 1; i < by_from.size(); ++i)  // insertion sort: deterministic, and the list is short
    for (size_t j = i; j > 0 && less(by_from[j], by_from[j - 1]); --j) {
      const size_t t = by_from[j];
      by_from[j] = by_from[j - 1];
      by_from[j - 1] = t;
    }
  auto starting_at = [&](long x, long z) {
    size_t lo = 0, hi = by_from.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      const Edge &e = edges[by_from[mid]];
      if (e.fx < x || (e.fx == x && e.fz < z)) lo = mid + 1;
      else hi = mid;
    }
    for (size_t k = lo; k < by_from.size() && edges[by_from[k]].fx == x && edges[by_from[k]].fz == z; ++k)
      if (!edges[by_from[k]].used) return by_from[k];
    return edges.size();
  };
  for (size_t start = 0; start < edges.size(); ++start) {
    if (edges[start].used) continue;
    std::vector<Resolved::HullCell> loop;
    for (size_t at = start; at < edges.size() && !edges[at].used; at = starting_at(edges[at].tx, edges[at].tz)) {
      Edge &e = edges[at];
      e.used = true;
      if (!loop.empty() && loop.back().q == e.q && loop.back().r == e.r) continue;
      loop.push_back({e.q, e.r, e.side, e.t});
    }
    while (loop.size() > 1 && loop.back().q == loop.front().q && loop.back().r == loop.front().r) loop.pop_back();
    if (loop.size() > s.hull.size()) s.hull.swap(loop);
  }

  // 6. The openings: the border cell of its side nearest its place along the side, the hull's cell
  // beyond it, and the way in.
  for (const auto &o : in.openings) {
    Opening op;
    op.kind = o.kind;
    op.side = o.side % static_cast<uint32_t>(n);
    op.width = static_cast<uint32_t>(maxi_(1, mini_(8, static_cast<int>(o.width))));
    float best = 1e30f;
    for (size_t i = 0; i + 1 < s.cells.size(); i += 2) {
      const size_t at = static_cast<size_t>(s.index(s.cells[i], s.cells[i + 1]));
      if (s.depth[at] != 1 || s.side_of[at] != static_cast<int>(op.side)) continue;
      const float d = absf(s.along[at] - clampf(o.at, 0, 1));
      if (d < best) {
        best = d;
        op.q = s.cells[i];
        op.r = s.cells[i + 1];
      }
    }
    if (best > 1e29f) continue;  // a side with no border cells of its own
    const size_t a = op.side, b = (a + 1) % n;
    const vec3 along = normalize(v3(hex_x(s.corner_q[b], s.corner_r[b]) - hex_x(s.corner_q[a], s.corner_r[a]), 0,
                                    hex_z(s.corner_r[b]) - hex_z(s.corner_r[a])));
    op.inward = v3(-along.z, 0, along.x);  // counterclockwise: the inside is on the left
    if (dot(op.inward, s.centre - v3(hex_x(op.q, op.r), 0, hex_z(op.r))) < 0) op.inward = -op.inward;
    float far = -1e30f;
    op.out_q = op.q;
    op.out_r = op.r;
    for (int d = 0; d < 6; ++d) {
      const int nq = op.q + kDq[d], nr = op.r + kDr[d];
      if (s.contains(nq, nr)) continue;
      const float lead = -dot(op.inward, v3(hex_x(nq, nr) - hex_x(op.q, op.r), 0, hex_z(nr) - hex_z(op.r)));
      if (lead > far) {
        far = lead;
        op.out_q = nq;
        op.out_r = nr;
      }
    }
    s.openings.push_back(op);
  }
  return true;
}

}  // namespace SHAPE_NS::shape
