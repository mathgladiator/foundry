#include "assets/fracture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <tuple>

#include "core/util.h"

namespace assets::fracture {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Geometry runs in double precision: cuts must meet their caps.
struct D3 {
  double x = 0, y = 0, z = 0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator-(D3 a) { return {-a.x, -a.y, -a.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) {
  const double l = len(a);
  return l > 1e-300 ? a * (1 / l) : D3{0, 0, 0};
}
D3 mul(D3 a, D3 m) { return {a.x * m.x, a.y * m.y, a.z * m.z}; }
D3 d3(const foundry::vec3 &v) { return {v.x, v.y, v.z}; }
foundry::vec3 f3(D3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
D3 perpendicular(D3 n) { return norm(std::fabs(n.x) < 0.9 ? cross(n, D3{1, 0, 0}) : cross(n, D3{0, 1, 0})); }

// Deterministic random numbers (splitmix64), so a seed always cuts the same way.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  double signed_unit() { return uniform() * 2 - 1; }
  D3 direction() {
    const double z = signed_unit();
    const double a = uniform() * 2 * kPi;
    const double r = std::sqrt(std::max(0.0, 1 - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
  }
};

// The source mesh, in double precision.
struct Source {
  std::vector<D3> pos, nrm;
  std::vector<uint32_t> col, idx;
  D3 lo, hi, centre;
  double radius = 0;
};

Source source_of(const runtime::Mesh &m) {
  Source s;
  s.pos.reserve(m.positions.size());
  for (const auto &p : m.positions) s.pos.push_back(d3(p));
  for (const auto &n : m.normals) s.nrm.push_back(d3(n));
  s.col = m.colors;
  s.idx = m.indices;
  s.lo = {1e30, 1e30, 1e30};
  s.hi = {-1e30, -1e30, -1e30};
  for (const auto &p : s.pos) {
    s.lo = {std::min(s.lo.x, p.x), std::min(s.lo.y, p.y), std::min(s.lo.z, p.z)};
    s.hi = {std::max(s.hi.x, p.x), std::max(s.hi.y, p.y), std::max(s.hi.z, p.z)};
  }
  s.centre = (s.lo + s.hi) * 0.5;
  s.radius = len(s.hi - s.lo) * 0.5;
  return s;
}

// The generalized winding number: about 1 inside, 0 outside, and still sensible where the mesh has holes.
double winding(const Source &s, D3 p) {
  double total = 0;
  for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
    const D3 a = s.pos[s.idx[t]] - p, b = s.pos[s.idx[t + 1]] - p, c = s.pos[s.idx[t + 2]] - p;
    const double la = len(a), lb = len(b), lc = len(c);
    const double num = dot(a, cross(b, c));
    const double den = la * lb * lc + dot(a, b) * lc + dot(a, c) * lb + dot(b, c) * la;
    total += 2 * std::atan2(num, den);
  }
  return total / (4 * kPi);
}
bool inside(const Source &s, D3 p) { return winding(s, p) > 0.5; }

// The nearest hit of a ray on the mesh (Moller-Trumbore); false if it misses.
bool ray_hit(const Source &s, D3 o, D3 d, D3 &hit) {
  double best = 1e30;
  for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
    const D3 a = s.pos[s.idx[t]], b = s.pos[s.idx[t + 1]], c = s.pos[s.idx[t + 2]];
    const D3 e1 = b - a, e2 = c - a, p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::fabs(det) < 1e-14) continue;
    const double inv = 1 / det;
    const D3 tv = o - a;
    const double u = dot(tv, p) * inv;
    if (u < 0 || u > 1) continue;
    const D3 q = cross(tv, e1);
    const double v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) continue;
    const double dist = dot(e2, q) * inv;
    if (dist > 1e-9 && dist < best) best = dist;
  }
  if (best >= 1e30) return false;
  hit = o + d * best;
  return true;
}

// ---- convex clipping -------------------------------------------------------

struct Plane {
  D3 n;            // unit
  double d = 0;    // keeps dot(n, x) <= d
  int other = -1;  // the neighbouring cell across it (-1: a bounding plane)
};
double side(const Plane &pl, D3 x) { return dot(pl.n, x) - pl.d; }

// A vertex with what's interpolated across a cut.
struct V {
  D3 p, n;
  double c[4] = {0, 0, 0, 0};
};

V lerp(const V &a, const V &b, double t) {
  V v;
  v.p = a.p + (b.p - a.p) * t;
  v.n = a.n + (b.n - a.n) * t;
  for (int k = 0; k < 4; ++k) v.c[k] = a.c[k] + (b.c[k] - a.c[k]) * t;
  return v;
}

bool before(D3 a, D3 b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); }

// Where edge (a, b) crosses a plane, computed the same way whichever way round the edge is
// given, so neighbouring triangles (and the cross-section) agree exactly.
V crossing(const V &a, double sa, const V &b, double sb) {
  if (before(b.p, a.p)) return crossing(b, sb, a, sa);
  return lerp(a, b, sa / (sa - sb));
}

// Clips a convex polygon to the inside of a plane (Sutherland-Hodgman).
void clip(std::vector<V> &poly, const Plane &pl) {
  if (poly.empty()) return;
  std::vector<V> out;
  out.reserve(poly.size() + 2);
  for (size_t k = 0; k < poly.size(); ++k) {
    const V &a = poly[k], &b = poly[(k + 1) % poly.size()];
    const double sa = side(pl, a.p), sb = side(pl, b.p);
    const bool ia = sa <= 0, ib = sb <= 0;
    if (ia) out.push_back(a);
    if (ia != ib) out.push_back(crossing(a, sa, b, sb));
  }
  poly.swap(out);
}

double area(const std::vector<V> &poly) {
  D3 s;
  for (size_t k = 1; k + 1 < poly.size(); ++k) s = s + cross(poly[k].p - poly[0].p, poly[k + 1].p - poly[0].p);
  return len(s) * 0.5;
}

// ---- cross-sections ------------------------------------------------------------

using Key = std::tuple<int64_t, int64_t, int64_t>;
Key key_of(D3 p) {
  return {std::llround(p.x * 1e7), std::llround(p.y * 1e7), std::llround(p.z * 1e7)};
}

// The mesh's cross-section with a plane, as loops of points. Chains that don't close (where the
// mesh has a gap) are closed with a straight edge and counted in open.
std::vector<std::vector<D3>> section(const Source &s, const Plane &pl, int &open) {
  std::vector<std::pair<D3, D3>> segs;
  for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
    V v[3];
    double sd[3];
    int outside = 0;
    for (int k = 0; k < 3; ++k) {
      v[k].p = s.pos[s.idx[t + k]];
      sd[k] = side(pl, v[k].p);
      outside += sd[k] > 0;
    }
    if (outside == 0 || outside == 3) continue;
    D3 pts[2];
    int n = 0;
    for (int k = 0; k < 3 && n < 2; ++k) {
      const int j = (k + 1) % 3;
      if ((sd[k] > 0) != (sd[j] > 0)) pts[n++] = crossing(v[k], sd[k], v[j], sd[j]).p;
    }
    if (n == 2 && key_of(pts[0]) != key_of(pts[1])) segs.push_back({pts[0], pts[1]});
  }
  std::map<Key, std::vector<size_t>> at;  // point -> segment ends (segment * 2 + end)
  for (size_t i = 0; i < segs.size(); ++i) {
    at[key_of(segs[i].first)].push_back(i * 2);
    at[key_of(segs[i].second)].push_back(i * 2 + 1);
  }
  std::vector<bool> used(segs.size(), false);
  auto end_point = [&](size_t e) { return e % 2 ? segs[e / 2].second : segs[e / 2].first; };
  std::vector<std::vector<D3>> loops;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (used[i]) continue;
    used[i] = true;
    std::deque<D3> chain = {segs[i].first, segs[i].second};
    bool closed = false;
    for (int dir = 0; dir < 2 && !closed; ++dir) {
      for (;;) {
        const D3 tip = dir == 0 ? chain.back() : chain.front();
        auto it = at.find(key_of(tip));
        size_t next = SIZE_MAX;
        if (it != at.end())
          for (size_t e : it->second)
            if (!used[e / 2]) {
              next = e;
              break;
            }
        if (next == SIZE_MAX) break;
        used[next / 2] = true;
        const D3 other = end_point(next ^ 1);
        const D3 far = dir == 0 ? chain.front() : chain.back();
        if (key_of(other) == key_of(far)) {
          closed = true;
          break;
        }
        if (dir == 0) chain.push_back(other);
        else chain.push_front(other);
      }
    }
    if (!closed) ++open;
    if (chain.size() >= 3) loops.emplace_back(chain.begin(), chain.end());
  }
  return loops;
}

// ---- triangulating a section -------------------------------------------------

struct P2 {
  double x = 0, y = 0;
};
double cross2(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
double area2(const std::vector<P2> &p) {
  double a = 0;
  for (size_t k = 0; k < p.size(); ++k) {
    const P2 &u = p[k], &v = p[(k + 1) % p.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return a;
}
bool in_polygon(P2 q, const std::vector<P2> &p) {
  bool in = false;
  for (size_t k = 0, j = p.size() - 1; k < p.size(); j = k++)
    if ((p[k].y > q.y) != (p[j].y > q.y) && q.x < (p[j].x - p[k].x) * (q.y - p[k].y) / (p[j].y - p[k].y) + p[k].x)
      in = !in;
  return in;
}
bool same(P2 a, P2 b) { return std::fabs(a.x - b.x) < 1e-12 && std::fabs(a.y - b.y) < 1e-12; }
bool strictly_inside(P2 q, P2 a, P2 b, P2 c) {
  if (same(q, a) || same(q, b) || same(q, c)) return false;
  return cross2(a, b, q) > 0 && cross2(b, c, q) > 0 && cross2(c, a, q) > 0;
}
bool in_triangle(P2 q, P2 a, P2 b, P2 c) {
  return cross2(a, b, q) >= 0 && cross2(b, c, q) >= 0 && cross2(c, a, q) >= 0;
}

// Joins each hole (clockwise) into the outer polygon (counter-clockwise) with a two-way bridge to a
// vertex it can see (David Eberly, "Triangulation by Ear Clipping").
void bridge(std::vector<P2> &outer, std::vector<std::vector<P2>> holes) {
  std::sort(holes.begin(), holes.end(), [](const std::vector<P2> &a, const std::vector<P2> &b) {
    double ma = -1e300, mb = -1e300;
    for (const auto &p : a) ma = std::max(ma, p.x);
    for (const auto &p : b) mb = std::max(mb, p.x);
    return ma > mb;
  });
  for (const auto &hole : holes) {
    size_t mi = 0;
    for (size_t k = 1; k < hole.size(); ++k)
      if (hole[k].x > hole[mi].x) mi = k;
    const P2 M = hole[mi];
    double best = 1e300;
    size_t edge = SIZE_MAX;
    for (size_t k = 0; k < outer.size(); ++k) {
      const P2 &a = outer[k], &b = outer[(k + 1) % outer.size()];
      if ((a.y <= M.y && b.y > M.y) || (b.y <= M.y && a.y > M.y)) {
        const double x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= M.x && x < best) {
          best = x;
          edge = k;
        }
      }
    }
    if (edge == SIZE_MAX) continue;  // nothing to its right: not really inside
    const P2 I{best, M.y};
    size_t pick = outer[edge].x > outer[(edge + 1) % outer.size()].x ? edge : (edge + 1) % outer.size();
    double best_angle = 1e300;
    const P2 P = outer[pick];
    for (size_t k = 0; k < outer.size(); ++k) {
      if (k == pick) continue;
      const P2 &prev = outer[(k + outer.size() - 1) % outer.size()], &v = outer[k], &next = outer[(k + 1) % outer.size()];
      if (cross2(prev, v, next) >= 0 || v.x <= M.x) continue;  // only reflex vertices to its right
      const bool in = cross2(M, I, P) > 0 ? in_triangle(v, M, I, P) : in_triangle(v, M, P, I);
      if (!in) continue;
      const double angle = std::fabs(v.y - M.y) / (v.x - M.x);
      if (angle < best_angle) {
        best_angle = angle;
        pick = k;
      }
    }
    std::vector<P2> joined(outer.begin(), outer.begin() + static_cast<long>(pick) + 1);
    for (size_t k = 0; k <= hole.size(); ++k) joined.push_back(hole[(mi + k) % hole.size()]);
    joined.insert(joined.end(), outer.begin() + static_cast<long>(pick), outer.end());
    outer.swap(joined);
  }
}

// Triangulates a counter-clockwise polygon (bridged holes allowed) by clipping ears.
std::vector<std::array<P2, 3>> ears(const std::vector<P2> &poly) {
  std::vector<std::array<P2, 3>> out;
  std::vector<size_t> r(poly.size());
  for (size_t k = 0; k < r.size(); ++k) r[k] = k;
  size_t guard = 0;
  while (r.size() >= 3 && guard++ < poly.size() * poly.size() + 16) {
    const size_t n = r.size();
    bool found = false;
    for (size_t k = 0; k < n; ++k) {
      const P2 a = poly[r[(k + n - 1) % n]], b = poly[r[k]], c = poly[r[(k + 1) % n]];
      if (cross2(a, b, c) <= 1e-18) continue;
      bool ear = true;
      for (size_t j = 0; j < n && ear; ++j) {
        const size_t q = r[j];
        if (q == r[(k + n - 1) % n] || q == r[k] || q == r[(k + 1) % n]) continue;
        if (strictly_inside(poly[q], a, b, c)) ear = false;
      }
      if (!ear) continue;
      out.push_back({a, b, c});
      r.erase(r.begin() + static_cast<long>(k));
      found = true;
      break;
    }
    if (!found) {
      // What's left is degenerate (collinear or self-touching): drop its flattest corner.
      size_t flat = 0;
      double least = 1e300;
      for (size_t k = 0; k < n; ++k) {
        const double c = std::fabs(cross2(poly[r[(k + n - 1) % n]], poly[r[k]], poly[r[(k + 1) % n]]));
        if (c < least) {
          least = c;
          flat = k;
        }
      }
      r.erase(r.begin() + static_cast<long>(flat));
    }
  }
  return out;
}

// The section of the mesh on a plane, triangulated: each triangle counter-clockwise about the
// plane's normal. Loops inside an odd number of others are holes.
std::vector<std::array<D3, 3>> cap(const Source &s, const Plane &pl, int &open) {
  const std::vector<std::vector<D3>> loops3 = section(s, pl, open);
  const D3 u = perpendicular(pl.n), v = cross(pl.n, u), o = pl.n * pl.d;
  std::vector<std::vector<P2>> loops;
  for (const auto &l : loops3) {
    std::vector<P2> p;
    for (const auto &x : l) p.push_back({dot(x - o, u), dot(x - o, v)});
    if (p.size() >= 3 && std::fabs(area2(p)) > 1e-14) loops.push_back(p);
  }
  std::vector<int> depth(loops.size(), 0);
  for (size_t i = 0; i < loops.size(); ++i)
    for (size_t j = 0; j < loops.size(); ++j)
      if (i != j && in_polygon(loops[i][0], loops[j])) ++depth[i];
  std::vector<std::array<D3, 3>> out;
  for (size_t i = 0; i < loops.size(); ++i) {
    if (depth[i] % 2) continue;
    std::vector<P2> outer = loops[i];
    if (area2(outer) < 0) std::reverse(outer.begin(), outer.end());
    std::vector<std::vector<P2>> holes;
    for (size_t j = 0; j < loops.size(); ++j) {
      if (depth[j] != depth[i] + 1 || !in_polygon(loops[j][0], loops[i])) continue;
      std::vector<P2> h = loops[j];
      if (area2(h) > 0) std::reverse(h.begin(), h.end());
      holes.push_back(h);
    }
    bridge(outer, holes);
    for (const auto &t : ears(outer)) {
      std::array<D3, 3> tri;
      for (int k = 0; k < 3; ++k) tri[k] = o + u * t[k].x + v * t[k].y;
      out.push_back(tri);
    }
  }
  return out;
}

// ---- seeds -------------------------------------------------------------------------

std::vector<D3> seeds(const Source &s, const Settings &st, D3 impact, D3 dir, D3 &metric) {
  Rng rng(st.seed);
  const int n = std::clamp(st.pieces, 2, 64);
  const D3 size = s.hi - s.lo;
  const double R = s.radius;
  metric = {1, 1, 1};
  std::vector<D3> out;
  auto uniform_in_box = [&] {
    const double x = rng.uniform(), y = rng.uniform(), z = rng.uniform();
    return s.lo + mul(size, D3{x, y, z});
  };
  auto fill_inside = [&](auto make) {
    for (int tries = 0; static_cast<int>(out.size()) < n && tries < n * 60; ++tries) {
      const D3 p = make();
      if (inside(s, p)) out.push_back(p);
    }
    while (static_cast<int>(out.size()) < n) out.push_back(uniform_in_box());
  };
  if (st.algorithm == "voronoi") {
    fill_inside(uniform_in_box);
  } else if (st.algorithm == "impact") {
    fill_inside([&] {
      const double u = rng.uniform();
      return impact + rng.direction() * (R * 1.1 * std::pow(u, 1.8));
    });
  } else if (st.algorithm == "radial") {
    // Seeds on a ring around the blow's axis, all at about one depth, so the cells are wedges
    // running through the object along the blow; an inner ring makes small shards at the hit.
    const D3 a = norm(dir), bu = perpendicular(a), bv = cross(a, bu);
    const int outer = std::max(3, static_cast<int>(std::lround(n * 0.65)));
    const int inner = n - outer;
    const D3 base = impact + a * (R * 0.3);
    for (int k = 0; k < outer; ++k) {
      const double th = 2 * kPi * (k + 0.35 * rng.signed_unit()) / outer;
      const double r = R * 0.7 * (0.85 + 0.3 * rng.uniform());
      const double depth = R * 0.05 * rng.signed_unit();
      out.push_back(base + a * depth + (bu * std::cos(th) + bv * std::sin(th)) * r);
    }
    for (int k = 0; k < inner; ++k) {
      const double th = 2 * kPi * (k + 0.5 + 0.3 * rng.signed_unit()) / inner;
      const double r = R * 0.22 * (0.7 + 0.6 * rng.uniform());
      const double depth = R * 0.05 * rng.signed_unit();
      out.push_back(base + a * depth + (bu * std::cos(th) + bv * std::sin(th)) * r);
    }
  } else if (st.algorithm == "slab") {
    // Seeds stacked up the height, a little off-centre, so the cells are tilted layers.
    for (int k = 0; k < n; ++k) {
      const double y = s.lo.y + size.y * (k + 0.5 + 0.3 * rng.signed_unit()) / n;
      const double x = s.centre.x + 0.12 * R * rng.signed_unit();
      const double z = s.centre.z + 0.12 * R * rng.signed_unit();
      out.push_back({x, y, z});
    }
  } else {  // splinter: distance along the grain counts for less, so cells grow long along it
    const int grain = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
    metric = {grain == 0 ? 0.08 : 1.0, grain == 1 ? 0.08 : 1.0, grain == 2 ? 0.08 : 1.0};
    fill_inside(uniform_in_box);
  }
  return out;
}

// The plane halfway between seeds i and j under the metric, keeping i's side.
Plane bisector(const std::vector<D3> &sd, D3 m, int i, int j) {
  const D3 si = sd[static_cast<size_t>(i)], sj = sd[static_cast<size_t>(j)];
  D3 n = mul(sj - si, m);
  double d = (dot(sj, mul(sj, m)) - dot(si, mul(si, m))) * 0.5;
  const double l = len(n);
  Plane p;
  p.n = n * (1 / l);
  p.d = d / l;
  p.other = j;
  return p;
}

uint32_t pack(const double c[4]) {
  uint32_t out = 0;
  for (int k = 0; k < 4; ++k)
    out |= static_cast<uint32_t>(std::clamp(std::lround(c[k]), 0L, 255L)) << (8 * k);
  return out;
}
void unpack(uint32_t v, double c[4]) {
  for (int k = 0; k < 4; ++k) c[k] = static_cast<double>((v >> (8 * k)) & 255u);
}

// ---- rigid bodies -------------------------------------------------------------------

struct Q {
  double x = 0, y = 0, z = 0, w = 1;
};
Q qmul(Q a, Q b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q qnorm(Q q) {
  const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return {q.x / l, q.y / l, q.z / l, q.w / l};
}
D3 rotate(Q q, D3 v) {
  const D3 u{q.x, q.y, q.z};
  const D3 t = cross(u, v) * 2;
  return v + t * q.w + cross(u, t);
}

struct Body {
  D3 rest, c, v, w;
  Q q;
  double inv_m = 0, inv_i = 0, sphere = 0, still = 0;
  bool asleep = false;
  std::vector<D3> hull;  // points relative to the rest centre
};

}  // namespace

const std::vector<AlgorithmInfo> &algorithms() {
  static const std::vector<AlgorithmInfo> kAll = {
      {"voronoi", "Even chunks: Voronoi cells from seeds spread through the volume. Plaster, clay, a generic break."},
      {"impact", "Small shards at the blow and large chunks away from it: seeds packed toward the impact point. "
                 "Stone, statues, anything struck."},
      {"radial", "Wedges radiating from the blow and running through the object along it, with small shards at "
                 "the centre. Ceramic, glass, hollow things: a vase, a pot."},
      {"slab", "Tilted layers stacked up the height. Layered stone, slate, sandstone, brick."},
      {"splinter", "Long pieces along the longest axis (distance along the grain counts for less). Wood, bone."},
  };
  return kAll;
}

bool known(const std::string &algorithm) {
  for (const auto &a : algorithms())
    if (algorithm == a.name) return true;
  return false;
}

nlohmann::json Settings::to_json() const {
  nlohmann::json j = {{"algorithm", algorithm}, {"pieces", pieces}, {"seed", seed},
                      {"direction", {direction.x, direction.y, direction.z}},
                      {"strength", strength}, {"duration", duration}, {"fps", fps}};
  if (has_impact) j["impact"] = {impact.x, impact.y, impact.z};
  if (interior) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", interior & 255u, (interior >> 8) & 255u, (interior >> 16) & 255u);
    j["interior"] = buf;
  }
  return j;
}

bool Settings::from_json(const nlohmann::json &j, std::string &error) {
  try {
    algorithm = j.value("algorithm", algorithm);
    if (!known(algorithm)) {
      error = "unknown algorithm " + algorithm;
      return false;
    }
    pieces = std::clamp(j.value("pieces", pieces), 2, 64);
    seed = j.value("seed", seed);
    auto vec = [&](const char *k, foundry::vec3 &out) {
      if (!j.contains(k)) return false;
      const auto &a = j.at(k);
      out = {a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()};
      return true;
    };
    has_impact = vec("impact", impact) || has_impact;
    vec("direction", direction);
    if (len(d3(direction)) < 1e-6) {
      error = "direction must not be zero";
      return false;
    }
    strength = std::clamp(j.value("strength", strength), 0.0f, 5.0f);
    duration = std::clamp(j.value("duration", duration), 0.5f, 10.0f);
    fps = std::clamp(j.value("fps", fps), 10.0f, 60.0f);
    if (j.contains("interior")) {
      const std::string hex = j.at("interior").get<std::string>();
      unsigned r = 0, g = 0, b = 0;
      if (std::sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b) != 3) {
        error = "interior is a colour like #8a7a6a";
        return false;
      }
      interior = r | (g << 8) | (b << 16) | (255u << 24);
    }
  } catch (const std::exception &e) {
    error = std::string("bad fracture settings: ") + e.what();
    return false;
  }
  return true;
}

bool cut(const runtime::Mesh &mesh, const Settings &st, Fracture &out, std::string &error) {
  if (mesh.indices.size() < 3 || mesh.positions.empty()) {
    error = "the mesh is empty";
    return false;
  }
  if (!known(st.algorithm)) {
    error = "unknown algorithm " + st.algorithm;
    return false;
  }
  const Source s = source_of(mesh);
  const D3 dir = norm(d3(st.direction));
  D3 impact = d3(st.impact);
  if (!st.has_impact) {
    const D3 aim{s.centre.x, s.lo.y + (s.hi.y - s.lo.y) * 0.6, s.centre.z};
    if (!ray_hit(s, aim - dir * (s.radius * 3), dir, impact)) impact = aim;
  }
  D3 metric;
  const std::vector<D3> sd = seeds(s, st, impact, dir, metric);
  const int n = static_cast<int>(sd.size());

  // Neighbours: cells whose shared face, inside the mesh's bounds, isn't empty.
  std::vector<Plane> box;
  const D3 pad = (s.hi - s.lo) * 0.01 + D3{1e-4, 1e-4, 1e-4};
  for (int a = 0; a < 3; ++a) {
    D3 e;
    (a == 0 ? e.x : a == 1 ? e.y : e.z) = 1;
    box.push_back({e, dot(e, s.hi + pad), -1});
    box.push_back({-e, -dot(e, s.lo - pad), -1});
  }
  std::vector<std::vector<Plane>> cells(static_cast<size_t>(n));
  std::vector<std::vector<bool>> adjacent(static_cast<size_t>(n), std::vector<bool>(static_cast<size_t>(n), false));
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j) {
      const Plane pl = bisector(sd, metric, i, j);
      const D3 u = perpendicular(pl.n), v = cross(pl.n, u);
      const D3 c0 = s.centre - pl.n * side(pl, s.centre);
      const double h = s.radius * 3;
      std::vector<V> face(4);
      face[0].p = c0 + (u + v) * h;
      face[1].p = c0 + (-u + v) * h;
      face[2].p = c0 + (-u - v) * h;
      face[3].p = c0 + (u - v) * h;
      for (const auto &b : box) clip(face, b);
      for (int k = 0; k < n && !face.empty(); ++k)
        if (k != i && k != j) clip(face, bisector(sd, metric, i, k));
      if (face.size() >= 3 && area(face) > 1e-10) adjacent[static_cast<size_t>(i)][static_cast<size_t>(j)] = true;
    }
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      if (i != j && adjacent[static_cast<size_t>(std::min(i, j))][static_cast<size_t>(std::max(i, j))])
        cells[static_cast<size_t>(i)].push_back(bisector(sd, metric, i, j));

  // The interior colour: the surface's mean, darkened.
  double mean[4] = {0, 0, 0, 0};
  for (uint32_t c : s.col) {
    double x[4];
    unpack(c, x);
    for (int k = 0; k < 4; ++k) mean[k] += x[k];
  }
  for (double &m : mean) m = s.col.empty() ? 160 : m / static_cast<double>(s.col.size());
  double inner[4] = {mean[0] * 0.62, mean[1] * 0.62, mean[2] * 0.62, 255};
  if (st.interior) unpack(st.interior, inner);

  // Each cell's triangles: the surface clipped to it, then its caps.
  struct OutTri {
    V v[3];
  };
  std::vector<std::vector<OutTri>> tris(static_cast<size_t>(n));
  std::vector<size_t> cap_count(static_cast<size_t>(n), 0);
  auto emit = [&](std::vector<OutTri> &dst, const std::vector<V> &poly, bool reverse) {
    for (size_t k = 1; k + 1 < poly.size(); ++k) {
      OutTri t{{poly[0], poly[k], poly[k + 1]}};
      if (reverse) std::swap(t.v[1], t.v[2]);
      dst.push_back(t);
    }
  };
  for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
    V tri[3];
    for (int k = 0; k < 3; ++k) {
      const uint32_t i = s.idx[t + static_cast<size_t>(k)];
      tri[k].p = s.pos[i];
      tri[k].n = i < s.nrm.size() ? s.nrm[i] : D3{0, 1, 0};
      unpack(i < s.col.size() ? s.col[i] : 0xffa0a0a0u, tri[k].c);
    }
    for (int c = 0; c < n; ++c) {
      if (cells[static_cast<size_t>(c)].empty()) continue;  // no face inside the bounds: the cell misses the mesh
      std::vector<V> poly(tri, tri + 3);
      for (const auto &pl : cells[static_cast<size_t>(c)]) {
        int out_count = 0;
        for (const auto &x : poly) out_count += side(pl, x.p) > 0;
        if (out_count == static_cast<int>(poly.size())) {
          poly.clear();
          break;
        }
        if (out_count) clip(poly, pl);
      }
      if (poly.size() >= 3) emit(tris[static_cast<size_t>(c)], poly, false);
    }
  }
  int open = 0;
  for (int i = 0; i < n; ++i)
    for (const auto &pl : cells[static_cast<size_t>(i)]) {
      const int j = pl.other;
      if (j < i) continue;  // each shared face once, from its lower cell
      for (const auto &t : cap(s, pl, open)) {
        std::vector<V> poly(3);
        for (int k = 0; k < 3; ++k) {
          poly[static_cast<size_t>(k)].p = t[static_cast<size_t>(k)];
          poly[static_cast<size_t>(k)].n = pl.n;
          std::copy(inner, inner + 4, poly[static_cast<size_t>(k)].c);
        }
        for (const auto &o : cells[static_cast<size_t>(i)])
          if (o.other != j && !poly.empty()) clip(poly, o);
        if (poly.size() < 3 || area(poly) < 1e-12) continue;
        const size_t before_i = tris[static_cast<size_t>(i)].size(), before_j = tris[static_cast<size_t>(j)].size();
        emit(tris[static_cast<size_t>(i)], poly, false);
        for (auto &x : poly) x.n = -pl.n;
        emit(tris[static_cast<size_t>(j)], poly, true);
        cap_count[static_cast<size_t>(i)] += tris[static_cast<size_t>(i)].size() - before_i;
        cap_count[static_cast<size_t>(j)] += tris[static_cast<size_t>(j)].size() - before_j;
      }
    }

  // Pieces: cells with triangles, their volumes and centres from the closed surface.
  out = {};
  out.impact = f3(impact);
  out.fps = st.fps;
  size_t caps = 0;
  double total_volume = 0;
  std::vector<double> volumes;
  for (int c = 0; c < n; ++c) {
    const auto &ts = tris[static_cast<size_t>(c)];
    if (ts.size() < 4) continue;
    double vol = 0;
    D3 cen;
    double area_sum = 0;
    D3 area_cen;
    for (const auto &t : ts) {
      const double v6 = dot(t.v[0].p, cross(t.v[1].p, t.v[2].p));
      vol += v6 / 6;
      cen = cen + (t.v[0].p + t.v[1].p + t.v[2].p) * (v6 / 24);
      const double a = len(cross(t.v[1].p - t.v[0].p, t.v[2].p - t.v[0].p)) * 0.5;
      area_sum += a;
      area_cen = area_cen + (t.v[0].p + t.v[1].p + t.v[2].p) * (a / 3);
    }
    D3 centre = vol > 1e-12 ? cen * (1 / vol) : area_cen * (1 / std::max(area_sum, 1e-12));
    const uint32_t p = static_cast<uint32_t>(out.centres.size());
    out.centres.push_back(f3(centre));
    out.volumes.push_back(static_cast<float>(std::max(vol, 0.0)));
    volumes.push_back(std::max(vol, 0.0));
    total_volume += std::max(vol, 0.0);
    caps += cap_count[static_cast<size_t>(c)];
    // Shared vertices: one per distinct position, normal, and colour within the piece.
    std::map<std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, uint32_t>, uint32_t> welded;
    for (const auto &t : ts)
      for (const auto &x : t.v) {
        const D3 nn = norm(x.n);
        const uint32_t colour = pack(x.c);
        const auto [px, py, pz] = key_of(x.p);
        const auto key = std::make_tuple(px, py, pz, std::llround(nn.x * 1e4), std::llround(nn.y * 1e4),
                                         std::llround(nn.z * 1e4), colour);
        auto it = welded.find(key);
        if (it == welded.end()) {
          it = welded.emplace(key, static_cast<uint32_t>(out.mesh.positions.size())).first;
          out.mesh.positions.push_back(f3(x.p));
          out.mesh.normals.push_back(f3(nn));
          out.mesh.colors.push_back(colour);
          out.piece.push_back(p);
        }
        out.mesh.indices.push_back(it->second);
      }
  }
  if (out.centres.size() < 2) {
    error = "the cut made fewer than two pieces";
    return false;
  }
  std::sort(volumes.begin(), volumes.end());
  out.stats = {{"algorithm", st.algorithm},
               {"seeds", n},
               {"pieces", out.centres.size()},
               {"triangles", out.mesh.indices.size() / 3},
               {"vertices", out.mesh.positions.size()},
               {"source_triangles", s.idx.size() / 3},
               {"cap_triangles", caps},
               {"open_sections", open},
               {"impact", {out.impact.x, out.impact.y, out.impact.z}},
               {"volume_m3", total_volume},
               {"smallest_share", total_volume > 0 ? volumes.front() / total_volume : 0},
               {"largest_share", total_volume > 0 ? volumes.back() / total_volume : 0}};
  return true;
}

void bake(Fracture &f, const Settings &st) {
  const size_t np = f.pieces();
  const D3 dir = norm(d3(st.direction)), impact = d3(f.impact);
  // Bounds, for the blow's reach.
  D3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
  for (const auto &p : f.mesh.positions) {
    lo = {std::min(lo.x, static_cast<double>(p.x)), std::min(lo.y, static_cast<double>(p.y)), std::min(lo.z, static_cast<double>(p.z))};
    hi = {std::max(hi.x, static_cast<double>(p.x)), std::max(hi.y, static_cast<double>(p.y)), std::max(hi.z, static_cast<double>(p.z))};
  }
  const double R = std::max(len(hi - lo) * 0.5, 0.05);
  const double scale = std::clamp(R / 0.4, 0.4, 1.0);  // small things scatter less; big ones no farther
  std::vector<Body> bodies(np);
  std::vector<std::vector<D3>> pts(np);
  for (size_t i = 0; i < f.mesh.positions.size(); ++i) pts[f.piece[i]].push_back(d3(f.mesh.positions[i]));
  Rng rng(st.seed ^ 0x5bd1e995u);
  for (size_t p = 0; p < np; ++p) {
    Body &b = bodies[p];
    b.rest = b.c = d3(f.centres[p]);
    // The hull for ground contact: every distinct point of a small piece, else the farthest
    // points along 162 directions spread over the sphere.
    double reach = 0;
    std::map<Key, D3> distinct;
    for (const auto &x : pts[p]) {
      reach = std::max(reach, len(x - b.rest));
      distinct.emplace(key_of(x), x - b.rest);
    }
    if (distinct.size() <= 256) {
      for (const auto &kv : distinct) b.hull.push_back(kv.second);
    } else {
      std::map<Key, D3> picked;
      const int dirs = 162;
      for (int k = 0; k < dirs; ++k) {
        const double z = 1 - 2 * (k + 0.5) / dirs, r = std::sqrt(1 - z * z), a = k * 2.399963229728653;
        const D3 d{r * std::cos(a), z, r * std::sin(a)};
        double best = -1e30;
        D3 far;
        for (const auto &kv : distinct)
          if (dot(kv.second, d) > best) {
            best = dot(kv.second, d);
            far = kv.second;
          }
        picked.emplace(key_of(far), far);
      }
      for (const auto &kv : picked) b.hull.push_back(kv.second);
    }
    const double vol = std::max(static_cast<double>(f.volumes[p]), 1e-7);
    const double m = vol * 1000;  // density cancels in everything but the spheres' push; water's will do
    b.inv_m = 1 / m;
    b.inv_i = 1 / (0.4 * m * std::max(reach * reach * 0.5, 1e-6));
    b.sphere = std::cbrt(3 * vol / (4 * kPi)) * 0.9;
    // The blow: out from the impact, along the blow, and up, falling off with distance.
    const D3 rel = b.rest - impact;
    const double dist = len(rel);
    const double fall = 1 / (1 + (dist / (0.35 * R)) * (dist / (0.35 * R)));
    const D3 out_dir = dist > 1e-6 ? rel * (1 / dist) : dir;
    b.v = (out_dir * (1.6 * fall) + dir * (1.0 * fall) + D3{0, 0.8 * fall, 0}) * (st.strength * scale);
    const D3 spin = rng.direction();
    b.w = spin * ((4.0 * fall + 0.5) * st.strength);
  }

  // Spheres never push closer pieces apart than they sat at rest: packed pieces overlap as spheres.
  std::vector<double> reach(np * np, 0);
  for (size_t i = 0; i < np; ++i)
    for (size_t j = 0; j < np; ++j)
      reach[i * np + j] = std::min(bodies[i].sphere + bodies[j].sphere, len(bodies[i].rest - bodies[j].rest) * 0.98);

  const double dt = 1.0 / 240;
  const int sub = std::max(1, static_cast<int>(std::lround(240 / st.fps)));
  f.fps = static_cast<float>(240.0 / sub);
  f.frames = static_cast<uint32_t>(std::lround(st.duration * f.fps)) + 1;
  f.track.assign(static_cast<size_t>(f.frames) * np * 7, 0.0f);
  double settled_at = -1;
  auto record = [&](uint32_t frame) {
    for (size_t p = 0; p < np; ++p) {
      float *o = &f.track[(static_cast<size_t>(frame) * np + p) * 7];
      const Body &b = bodies[p];
      o[0] = static_cast<float>(b.c.x);
      o[1] = static_cast<float>(b.c.y);
      o[2] = static_cast<float>(b.c.z);
      o[3] = static_cast<float>(b.q.x);
      o[4] = static_cast<float>(b.q.y);
      o[5] = static_cast<float>(b.q.z);
      o[6] = static_cast<float>(b.q.w);
    }
  };
  record(0);
  for (uint32_t frame = 1; frame < f.frames; ++frame) {
    for (int step = 0; step < sub; ++step) {
      for (auto &b : bodies) {
        if (b.asleep) continue;
        b.v.y -= 9.81 * dt;
        b.v = b.v * (1 - 0.05 * dt);
        b.w = b.w * (1 - 0.4 * dt);
        b.c = b.c + b.v * dt;
        const Q dq{b.w.x * dt * 0.5, b.w.y * dt * 0.5, b.w.z * dt * 0.5, 0};
        const Q add = qmul(dq, b.q);
        b.q = qnorm({b.q.x + add.x, b.q.y + add.y, b.q.z + add.z, b.q.w + add.w});
      }
      // The ground: impulses at the hull points below it, with friction, then push out.
      for (auto &b : bodies) {
        if (b.asleep) continue;
        double deepest = 0;
        bool touching = false;
        for (const auto &h : b.hull) {
          const D3 r = rotate(b.q, h);
          const D3 x = b.c + r;
          if (x.y >= 0) continue;
          touching = true;
          deepest = std::min(deepest, x.y);
          const D3 vp = b.v + cross(b.w, r);
          if (vp.y >= 0) continue;
          const D3 nrm{0, 1, 0};
          const D3 rn = cross(r, nrm);
          const double k = b.inv_m + b.inv_i * dot(rn, rn);
          const double e = vp.y < -0.6 ? 0.25 : 0.0;
          const double jn = -(1 + e) * vp.y / k;
          b.v = b.v + nrm * (jn * b.inv_m);
          b.w = b.w + cross(r, nrm * jn) * b.inv_i;
          const D3 vp2 = b.v + cross(b.w, r);
          D3 vt = vp2 - nrm * vp2.y;
          const double lt = len(vt);
          if (lt > 1e-6) {
            const D3 t = vt * (1 / lt);
            const D3 rt = cross(r, t);
            const double kt = b.inv_m + b.inv_i * dot(rt, rt);
            const double jt = std::min(lt / kt, 0.6 * jn);
            b.v = b.v - t * (jt * b.inv_m);
            b.w = b.w - cross(r, t * jt) * b.inv_i;
          }
        }
        b.c.y -= deepest * 0.8;
        if (touching) {
          // Rolling resistance: rough pieces on rough ground stop rolling; the hull's few points can't.
          // A slow piece rocking on a few hull points settles instead of rocking forever.
          b.w = b.w * (1 - (len(b.v) < 0.15 ? 12.0 : 3.0) * dt);
          b.v.x *= 1 - 1.5 * dt;
          b.v.z *= 1 - 1.5 * dt;
        }
        // Rest: slow for a moment (under gravity, only something holding it up keeps it slow that long).
        // On the ground, spin doesn't count: sequential impulses on a few hull points keep it rocking in place.
        if (len(b.v) < 0.1 && (touching || len(b.w) < 0.5)) b.still += dt;
        else b.still = 0;
        if (b.still > 0.4) {
          b.asleep = true;
          b.v = b.w = D3{};
        }
      }
      // Pieces push each other apart as spheres of their volume.
      for (size_t i = 0; i < np; ++i)
        for (size_t j = i + 1; j < np; ++j) {
          Body &a = bodies[i], &b = bodies[j];
          if (a.asleep && b.asleep) continue;
          const D3 d = b.c - a.c;
          const double dist = len(d), r = reach[i * np + j];
          if (dist >= r || dist < 1e-9) continue;
          const D3 nrm = d * (1 / dist);
          const double wa = a.asleep ? 0 : a.inv_m, wb = b.asleep ? 0 : b.inv_m;
          if (wa + wb <= 0) continue;
          const double push = (r - dist) * 0.5;
          a.c = a.c - nrm * (push * wa / (wa + wb));
          b.c = b.c + nrm * (push * wb / (wa + wb));
          const double vn = dot(b.v - a.v, nrm);
          if (vn < 0) {
            const double jn = -1.1 * vn / (wa + wb);
            a.v = a.v - nrm * (jn * wa);
            b.v = b.v + nrm * (jn * wb);
          }
        }
    }
    record(frame);
    bool all = true;
    for (const auto &b : bodies) all = all && b.asleep;
    if (all && settled_at < 0) settled_at = frame / static_cast<double>(f.fps);
  }
  double travel = 0;
  for (size_t p = 0; p < np; ++p) travel = std::max(travel, len(bodies[p].c - bodies[p].rest));
  f.stats["seconds"] = f.seconds();
  f.stats["fps"] = f.fps;
  f.stats["settled_s"] = settled_at < 0 ? nlohmann::json(nullptr) : nlohmann::json(settled_at);
  f.stats["farthest_m"] = travel;
  int moving = 0;
  double fastest = 0;
  for (const auto &b : bodies) {
    moving += !b.asleep;
    fastest = std::max(fastest, len(b.v));
  }
  f.stats["moving_at_end"] = moving;
  f.stats["fastest_at_end_m_s"] = fastest;
}

runtime::Mesh pose(const Fracture &f, float t) {
  runtime::Mesh m = f.mesh;
  const size_t np = f.pieces();
  if (f.frames == 0 || f.track.size() < static_cast<size_t>(f.frames) * np * 7) return m;
  const float ft = std::clamp(t * f.fps, 0.0f, static_cast<float>(f.frames - 1));
  const uint32_t a = static_cast<uint32_t>(ft), b = std::min(a + 1, f.frames - 1);
  const float w = ft - static_cast<float>(a);
  std::vector<D3> pos(np);
  std::vector<Q> rot(np);
  for (size_t p = 0; p < np; ++p) {
    const float *x = &f.track[(static_cast<size_t>(a) * np + p) * 7], *y = &f.track[(static_cast<size_t>(b) * np + p) * 7];
    pos[p] = {x[0] + (y[0] - x[0]) * w, x[1] + (y[1] - x[1]) * w, x[2] + (y[2] - x[2]) * w};
    Q qa{x[3], x[4], x[5], x[6]}, qb{y[3], y[4], y[5], y[6]};
    if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) qb = {-qb.x, -qb.y, -qb.z, -qb.w};
    rot[p] = qnorm({qa.x + (qb.x - qa.x) * w, qa.y + (qb.y - qa.y) * w, qa.z + (qb.z - qa.z) * w, qa.w + (qb.w - qa.w) * w});
  }
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const uint32_t p = f.piece[i];
    m.positions[i] = f3(pos[p] + rotate(rot[p], d3(f.mesh.positions[i]) - d3(f.centres[p])));
    if (i < m.normals.size()) m.normals[i] = f3(rotate(rot[p], d3(f.mesh.normals[i])));
  }
  return m;
}

runtime::Mesh exploded(const Fracture &f, float amount, bool tint) {
  runtime::Mesh m = f.mesh;
  D3 centre;
  for (const auto &c : f.centres) centre = centre + d3(c);
  centre = centre * (1.0 / static_cast<double>(std::max<size_t>(f.pieces(), 1)));
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const uint32_t p = f.piece[i];
    m.positions[i] = f3(d3(m.positions[i]) + (d3(f.centres[p]) - centre) * (amount * 2.0));
    if (tint) {
      // A distinct hue per piece (golden-ratio steps), blended over the surface colour.
      const double h = std::fmod(p * 0.61803398875, 1.0) * 6;
      const double x = 1 - std::fabs(std::fmod(h, 2.0) - 1);
      double rgb[3] = {0, 0, 0};
      const int seg = static_cast<int>(h);
      const double table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
      for (int k = 0; k < 3; ++k) rgb[k] = table[seg % 6][k];
      double c[4];
      unpack(m.colors[i], c);
      for (int k = 0; k < 3; ++k) c[k] = c[k] * 0.45 + (60 + 180 * rgb[k]) * 0.55;
      m.colors[i] = pack(c);
    }
  }
  return m;
}

nlohmann::json recommend(const runtime::Mesh &mesh, const std::string &prompt) {
  std::string p = prompt;
  std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  struct Rule {
    const char *algorithm;
    std::vector<const char *> words;
    const char *why;
  };
  static const std::vector<Rule> kRules = {
      {"radial", {"vase", "pot ", "pottery", "ceramic", "porcelain", "glass", "clay", "jar", "urn", "bottle", "terracotta", "plate", "bowl"},
       "brittle and often hollow: shards radiate from the blow"},
      {"splinter", {"wood", "log", "plank", "timber", "branch", "stump", "crate", "barrel", "bone"}, "it has a grain: it splits along it"},
      {"slab", {"slate", "shale", "brick", "layered", "sandstone", "strata"}, "layered: it parts along its layers"},
      {"impact", {"stone", "rock", "marble", "statue", "granite", "concrete", "boulder", "resin", "plaster"},
       "solid: small shards where it's struck, big chunks elsewhere"},
  };
  // The shape: how much of its bounds it fills, and how thin its walls are.
  const Source s = source_of(mesh);
  double vol = 0, surf = 0;
  for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
    const D3 a = s.pos[s.idx[t]], b = s.pos[s.idx[t + 1]], c = s.pos[s.idx[t + 2]];
    vol += dot(a, cross(b, c)) / 6;
    surf += len(cross(b - a, c - a)) * 0.5;
  }
  vol = std::fabs(vol);
  const D3 size = s.hi - s.lo;
  const double fill = vol / std::max(size.x * size.y * size.z, 1e-12);
  const double wall = vol / std::max(surf, 1e-12);  // a solid ball of radius r gives r/3
  const double longest = std::max({size.x, size.y, size.z}), shortest = std::min({size.x, size.y, size.z});
  const int pieces = std::clamp(static_cast<int>(std::lround(12 + 20 * s.radius)), 8, 32);
  nlohmann::json shape = {{"fill", fill}, {"wall_m", wall}, {"aspect", longest / std::max(shortest, 1e-9)}};
  for (const auto &r : kRules)
    for (const char *w : r.words)
      if (p.find(w) != std::string::npos)
        return {{"algorithm", r.algorithm}, {"pieces", pieces}, {"shape", shape},
                {"reason", std::string("the prompt says \"") + w + "\": " + r.why}};
  if (wall < s.radius * 0.04)
    return {{"algorithm", "radial"}, {"pieces", pieces}, {"shape", shape}, {"reason", "thin-walled for its size: it shatters like a shell"}};
  if (longest / std::max(shortest, 1e-9) > 3)
    return {{"algorithm", "splinter"}, {"pieces", pieces}, {"shape", shape}, {"reason", "long and thin: it splits along its length"}};
  return {{"algorithm", "impact"}, {"pieces", pieces}, {"shape", shape}, {"reason", "a solid shape: shards where it's struck, chunks elsewhere"}};
}

namespace {
constexpr uint32_t kFractureVersion = 1;
}

bool save(const std::string &path, const Fracture &f) {
  std::vector<uint8_t> out;
  foundry::Writer w(out);
  w.u32(foundry::kDiskMagic);
  w.u32(kFractureVersion);
  w.str("foundry_fracture");
  auto vecs = [&](const std::vector<foundry::vec3> &v) { w.array(v, [](foundry::Writer &w2, const foundry::vec3 &x) { w2.vec(x); }); };
  auto u32s = [&](const std::vector<uint32_t> &v) { w.array(v, [](foundry::Writer &w2, uint32_t x) { w2.u32(x); }); };
  auto f32s = [&](const std::vector<float> &v) { w.array(v, [](foundry::Writer &w2, float x) { w2.f32(x); }); };
  vecs(f.mesh.positions);
  vecs(f.mesh.normals);
  u32s(f.mesh.colors);
  u32s(f.mesh.indices);
  u32s(f.piece);
  vecs(f.centres);
  f32s(f.volumes);
  w.vec(f.impact);
  w.f32(f.fps);
  w.u32(f.frames);
  f32s(f.track);
  w.str(f.stats.dump());
  return core::write_file(path, out.data(), out.size());
}

bool load(const std::string &path, Fracture &f) {
  std::vector<uint8_t> in;
  if (!core::read_file(path, in)) return false;
  foundry::Reader r(in.data(), in.size());
  if (r.u32() != foundry::kDiskMagic || r.u32() != kFractureVersion || r.str() != "foundry_fracture" || !r.ok()) return false;
  f = {};
  auto vecs = [&](std::vector<foundry::vec3> &v) { r.array(v, 12, [](foundry::Reader &r2, foundry::vec3 &x) { x = r2.vec(); }); };
  auto u32s = [&](std::vector<uint32_t> &v) { r.array(v, 4, [](foundry::Reader &r2, uint32_t &x) { x = r2.u32(); }); };
  auto f32s = [&](std::vector<float> &v) { r.array(v, 4, [](foundry::Reader &r2, float &x) { x = r2.f32(); }); };
  vecs(f.mesh.positions);
  vecs(f.mesh.normals);
  u32s(f.mesh.colors);
  u32s(f.mesh.indices);
  u32s(f.piece);
  vecs(f.centres);
  f32s(f.volumes);
  f.impact = r.vec();
  f.fps = r.f32();
  f.frames = r.u32();
  f32s(f.track);
  f.stats = nlohmann::json::parse(r.str(), nullptr, false);
  return r.ok() && f.piece.size() == f.mesh.positions.size();
}

}  // namespace assets::fracture
