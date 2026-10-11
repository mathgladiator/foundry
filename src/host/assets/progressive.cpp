#include "assets/progressive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <queue>
#include <tuple>

namespace assets {
namespace {

using foundry::vec3;
constexpr uint32_t kNone = 0xffffffffu;

struct D3 {
  double x = 0, y = 0, z = 0;
};
D3 d3(const vec3 &v) { return {v.x, v.y, v.z}; }
D3 add(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 scale(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A symmetric 4x4 quadric: E(x) = x'Ax + 2b'x + c, stored as its 10 entries.
struct Quadric {
  double a2 = 0, ab = 0, ac = 0, ad = 0, b2 = 0, bc = 0, bd = 0, c2 = 0, cd = 0, d2 = 0;
  void add_plane(double a, double b, double c, double d, double w) {
    a2 += w * a * a; ab += w * a * b; ac += w * a * c; ad += w * a * d;
    b2 += w * b * b; bc += w * b * c; bd += w * b * d;
    c2 += w * c * c; cd += w * c * d; d2 += w * d * d;
  }
  void add(const Quadric &q) {
    a2 += q.a2; ab += q.ab; ac += q.ac; ad += q.ad; b2 += q.b2;
    bc += q.bc; bd += q.bd; c2 += q.c2; cd += q.cd; d2 += q.d2;
  }
  double eval(D3 p) const {
    return a2 * p.x * p.x + 2 * ab * p.x * p.y + 2 * ac * p.x * p.z + 2 * ad * p.x + b2 * p.y * p.y +
           2 * bc * p.y * p.z + 2 * bd * p.y + c2 * p.z * p.z + 2 * cd * p.z + d2;
  }
};

// Solves the n x n system M y = r (n <= 4) by Gaussian elimination with
// partial pivoting. Returns false when the system is too close to singular.
bool solve(double M[4][5], int n, double y[4]) {
  double scale_max = 0;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) scale_max = std::max(scale_max, std::fabs(M[i][j]));
  if (scale_max == 0) return false;
  for (int c = 0; c < n; ++c) {
    int p = c;
    for (int r = c + 1; r < n; ++r)
      if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
    if (std::fabs(M[p][c]) < 1e-12 * scale_max) return false;
    if (p != c)
      for (int j = 0; j <= n; ++j) std::swap(M[p][j], M[c][j]);
    for (int r = c + 1; r < n; ++r) {
      double f = M[r][c] / M[c][c];
      for (int j = c; j <= n; ++j) M[r][j] -= f * M[c][j];
    }
  }
  for (int r = n - 1; r >= 0; --r) {
    double s = M[r][n];
    for (int j = r + 1; j < n; ++j) s -= M[r][j] * y[j];
    y[r] = s / M[r][r];
  }
  return true;
}

struct Candidate {
  double cost;
  uint32_t a, b;  // the edge, a < b; a survives
  uint32_t ver_a, ver_b;
  uint64_t serial;  // makes the order total
  D3 x;             // where the surviving vertex goes
  bool operator>(const Candidate &o) const {
    if (cost != o.cost) return cost > o.cost;
    if (a != o.a) return a > o.a;
    if (b != o.b) return b > o.b;
    return serial > o.serial;
  }
};

uint32_t bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

struct Face {
  uint32_t v[3];
  bool alive;
  bool has(uint32_t x) const { return v[0] == x || v[1] == x || v[2] == x; }
};

// Shape quality of a triangle in [0, 1]: 1 for equilateral, 0 for degenerate.
double quality(D3 p0, D3 p1, D3 p2) {
  D3 e0 = sub(p1, p0), e1 = sub(p2, p1), e2 = sub(p0, p2);
  double l = dot(e0, e0) + dot(e1, e1) + dot(e2, e2);
  if (l <= 0) return 0;
  D3 c = cross(e0, sub(p2, p0));
  return 4 * 1.7320508075688772 * 0.5 * std::sqrt(dot(c, c)) / l;
}

}  // namespace

bool build_progressive(const runtime::Mesh &in, ProgressiveMesh &out, std::string &error,
                       const std::function<void(float)> &progress) {
  out = {};
  const uint32_t n = static_cast<uint32_t>(in.positions.size());
  if (n == 0 || in.indices.size() % 3 != 0) {
    error = "mesh needs vertices and a triangle list";
    return false;
  }
  for (uint32_t i : in.indices)
    if (i >= n) {
      error = "mesh index out of range";
      return false;
    }
  const bool has_normals = in.normals.size() == n, has_colors = in.colors.size() == n;

  // 1. Weld vertices with bit-identical positions.
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) order[i] = i;
  auto key = [&](uint32_t i) {
    const vec3 &p = in.positions[i];
    return std::make_tuple(bits(p.x), bits(p.y), bits(p.z));
  };
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    auto ka = key(a), kb = key(b);
    return ka != kb ? ka < kb : a < b;
  });
  std::vector<uint32_t> weld(n);
  std::vector<D3> pos;  // current welded positions; they move as collapses happen
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t i = order[k];
    if (k == 0 || key(i) != key(order[k - 1])) pos.push_back(d3(in.positions[i]));
    weld[i] = static_cast<uint32_t>(pos.size() - 1);
  }
  const uint32_t W = static_cast<uint32_t>(pos.size());
  std::vector<std::vector<uint32_t>> attrs(W);
  for (uint32_t i = 0; i < n; ++i) attrs[weld[i]].push_back(i);

  // 2. Welded faces, skipping ones degenerate after welding.
  std::vector<Face> faces;
  std::vector<uint32_t> source_tris;
  for (size_t t = 0; t * 3 < in.indices.size(); ++t) {
    uint32_t a = weld[in.indices[t * 3]], b = weld[in.indices[t * 3 + 1]], c = weld[in.indices[t * 3 + 2]];
    if (a == b || b == c || a == c) continue;
    faces.push_back({{a, b, c}, true});
    source_tris.push_back(static_cast<uint32_t>(t));
  }
  std::vector<std::vector<uint32_t>> vfaces(W);
  for (uint32_t f = 0; f < faces.size(); ++f)
    for (uint32_t k = 0; k < 3; ++k) vfaces[faces[f].v[k]].push_back(f);
  auto face_normal = [&](const Face &f) {  // unnormalized, length = 2 * area
    return cross(sub(pos[f.v[1]], pos[f.v[0]]), sub(pos[f.v[2]], pos[f.v[0]]));
  };

  // 3. Quadrics: area-weighted face planes, plus heavy perpendicular planes on
  // boundary edges so open borders keep their shape.
  std::vector<Quadric> q(W);
  std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t>> edges;
  for (uint32_t f = 0; f < faces.size(); ++f) {
    D3 nrm = face_normal(faces[f]);
    double len = std::sqrt(dot(nrm, nrm));
    if (len <= 0) continue;
    D3 u = scale(nrm, 1 / len);
    double area = 0.5 * len;
    D3 p0 = pos[faces[f].v[0]];
    for (uint32_t k = 0; k < 3; ++k) q[faces[f].v[k]].add_plane(u.x, u.y, u.z, -dot(u, p0), area);
    for (uint32_t k = 0; k < 3; ++k) {
      uint32_t a = faces[f].v[k], b = faces[f].v[(k + 1) % 3];
      edges.push_back({{std::min(a, b), std::max(a, b)}, f});
    }
  }
  std::sort(edges.begin(), edges.end());
  for (size_t i = 0; i < edges.size();) {
    size_t j = i;
    while (j < edges.size() && edges[j].first == edges[i].first) ++j;
    if (j - i == 1) {  // a boundary edge
      uint32_t f = edges[i].second, a = edges[i].first.first, b = edges[i].first.second;
      D3 e = sub(pos[b], pos[a]);
      D3 pn = cross(e, face_normal(faces[f]));
      double len = std::sqrt(dot(pn, pn));
      if (len > 0) {
        D3 u = scale(pn, 1 / len);
        double w = 100.0 * dot(e, e);
        q[a].add_plane(u.x, u.y, u.z, -dot(u, pos[a]), w);
        q[b].add_plane(u.x, u.y, u.z, -dot(u, pos[a]), w);
      }
    }
    i = j;
  }

  std::vector<uint32_t> ver(W, 0);
  std::vector<bool> dead(W, false);
  std::vector<uint32_t> collapsed_into(W, kNone), step(W, kNone);

  auto neighbors = [&](uint32_t v) {
    std::vector<uint32_t> out_n;
    for (uint32_t f : vfaces[v]) {
      if (!faces[f].alive) continue;
      for (uint32_t k = 0; k < 3; ++k)
        if (faces[f].v[k] != v) out_n.push_back(faces[f].v[k]);
    }
    std::sort(out_n.begin(), out_n.end());
    out_n.erase(std::unique(out_n.begin(), out_n.end()), out_n.end());
    return out_n;
  };
  auto star = [&](uint32_t a, uint32_t b) {
    std::vector<uint32_t> s;
    for (uint32_t f : vfaces[a])
      if (faces[f].alive) s.push_back(f);
    for (uint32_t f : vfaces[b])
      if (faces[f].alive) s.push_back(f);
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    return s;
  };

  // The volume-preserving optimal position for collapsing edge (a, b).
  auto place = [&](uint32_t a, uint32_t b, D3 &x) {
    Quadric s = q[a];
    s.add(q[b]);
    // Volume constraint: sum over the star of N_f . x = sum of N_f . p_f.
    D3 nsum;
    double dsum = 0;
    for (uint32_t f : star(a, b)) {
      D3 nf = face_normal(faces[f]);
      nsum = add(nsum, nf);
      dsum += dot(nf, pos[faces[f].v[0]]);
    }
    const D3 mid = scale(add(pos[a], pos[b]), 0.5);
    const double edge2 = dot(sub(pos[a], pos[b]), sub(pos[a], pos[b]));
    const double nn = dot(nsum, nsum);
    const bool constrained = nn > 1e-24;
    bool ok = false;
    double y[4];
    if (constrained) {
      double M[4][5] = {{2 * s.a2, 2 * s.ab, 2 * s.ac, nsum.x, -2 * s.ad},
                        {2 * s.ab, 2 * s.b2, 2 * s.bc, nsum.y, -2 * s.bd},
                        {2 * s.ac, 2 * s.bc, 2 * s.c2, nsum.z, -2 * s.cd},
                        {nsum.x, nsum.y, nsum.z, 0, dsum}};
      ok = solve(M, 4, y);
    } else {
      double M[4][5] = {{s.a2, s.ab, s.ac, -s.ad, 0}, {s.ab, s.b2, s.bc, -s.bd, 0}, {s.ac, s.bc, s.c2, -s.cd, 0}};
      ok = solve(M, 3, y);
    }
    if (ok) {
      x = {y[0], y[1], y[2]};
      D3 dm = sub(x, mid);  // reject far-off optima (near-singular systems); they make spikes
      if (dot(dm, dm) > 4 * edge2) ok = false;
    }
    if (!ok) {
      // Fall back to the best of the midpoint and the endpoints, each moved
      // onto the volume-constraint plane.
      D3 cands[3] = {mid, pos[a], pos[b]};
      double best = 0;
      for (int k = 0; k < 3; ++k) {
        D3 c = cands[k];
        if (constrained) c = add(c, scale(nsum, (dsum - dot(nsum, c)) / nn));
        double e = s.eval(c);
        if (k == 0 || e < best) {
          best = e;
          x = c;
        }
      }
    }
    return std::max(0.0, s.eval(x));
  };

  std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> heap;
  uint64_t serial = 0;
  auto push = [&](uint32_t u, uint32_t v) {
    Candidate c;
    c.a = std::min(u, v);
    c.b = std::max(u, v);
    c.cost = place(c.a, c.b, c.x);
    c.ver_a = ver[c.a];
    c.ver_b = ver[c.b];
    c.serial = serial++;
    heap.push(c);
  };
  for (const Face &f : faces)
    for (uint32_t k = 0; k < 3; ++k) push(f.v[k], f.v[(k + 1) % 3]);

  // 4. Greedy collapses.
  uint32_t alive_faces = static_cast<uint32_t>(faces.size());
  std::vector<uint32_t> level_faces = {alive_faces};
  uint32_t steps = 0;
  while (!heap.empty()) {
    Candidate c = heap.top();
    heap.pop();
    const uint32_t a = c.a, b = c.b;
    if (dead[a] || dead[b] || c.ver_a != ver[a] || c.ver_b != ver[b]) continue;

    std::vector<uint32_t> na = neighbors(a), nb = neighbors(b), shared;
    std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(), std::back_inserter(shared));
    uint32_t shared_faces = 0;
    for (uint32_t f : vfaces[a])
      if (faces[f].alive && faces[f].has(b)) ++shared_faces;
    if (shared_faces == 0 || shared.size() != shared_faces) continue;  // link condition

    bool ok = true;  // reject flips, folds over 60 degrees, and slivers
    for (uint32_t f : star(a, b)) {
      const Face &fc = faces[f];
      if (fc.has(a) && fc.has(b)) continue;
      D3 p[3], r[3];
      for (uint32_t k = 0; k < 3; ++k) {
        p[k] = pos[fc.v[k]];
        r[k] = (fc.v[k] == a || fc.v[k] == b) ? c.x : pos[fc.v[k]];
      }
      D3 before = cross(sub(p[1], p[0]), sub(p[2], p[0])), after = cross(sub(r[1], r[0]), sub(r[2], r[0]));
      double bb = dot(before, before), aa = dot(after, after), ba = dot(before, after);
      if (aa <= 1e-30 || ba <= 0 || ba * ba < 0.25 * bb * aa) {
        ok = false;
        break;
      }
      double q_after = quality(r[0], r[1], r[2]);
      if (q_after < 0.1 && q_after < quality(p[0], p[1], p[2])) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    for (uint32_t f : vfaces[b]) {
      Face &fc = faces[f];
      if (!fc.alive) continue;
      if (fc.has(a)) {
        fc.alive = false;
        --alive_faces;
      } else {
        for (uint32_t k = 0; k < 3; ++k)
          if (fc.v[k] == b) fc.v[k] = a;
        vfaces[a].push_back(f);
      }
    }
    vfaces[b].clear();
    dead[b] = true;
    collapsed_into[b] = a;
    step[b] = steps++;
    pos[a] = c.x;
    q[a].add(q[b]);
    out.step_welded.push_back(a);
    out.step_position.push_back({static_cast<float>(c.x.x), static_cast<float>(c.x.y), static_cast<float>(c.x.z)});
    level_faces.push_back(alive_faces);
    if (progress && (steps & 255) == 0) progress(1.0f - static_cast<float>(alive_faces) / level_faces.front());

    ++ver[a];  // a moved, so every edge whose star touches a needs a fresh cost
    std::vector<uint32_t> ring = neighbors(a);
    for (uint32_t m : ring) ++ver[m];
    for (uint32_t m : ring) {
      push(a, m);
      for (uint32_t m2 : neighbors(m))
        if (m2 != a) push(m, m2);
    }
  }

  // 5. Order vertices so each level is a prefix: never-collapsed first, then
  // by collapse step, latest first.
  std::vector<uint32_t> perm(n);
  for (uint32_t i = 0; i < n; ++i) perm[i] = i;
  auto rank = [&](uint32_t i) {
    uint32_t s = step[weld[i]];
    return std::make_tuple(s == kNone ? 0u : 1u + (steps - 1 - s), weld[i], i);
  };
  std::sort(perm.begin(), perm.end(), [&](uint32_t a, uint32_t b) { return rank(a) < rank(b); });
  std::vector<uint32_t> new_index(n);
  for (uint32_t k = 0; k < n; ++k) new_index[perm[k]] = k;

  // Each vertex collapses into the vertex of its welded target with the most
  // similar normal and color.
  auto similarity = [&](uint32_t a, uint32_t b) {
    double s = 0;
    if (has_normals) s += dot(d3(in.normals[a]), d3(in.normals[b]));
    if (has_colors) {
      uint32_t ca = in.colors[a], cb = in.colors[b];
      int diff = 0;
      for (int sh = 0; sh < 24; sh += 8)
        diff += std::abs(static_cast<int>((ca >> sh) & 255) - static_cast<int>((cb >> sh) & 255));
      s -= diff / 765.0;
    }
    return s;
  };
  out.collapse.assign(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t target = collapsed_into[weld[i]];
    if (target == kNone) {
      out.collapse[new_index[i]] = new_index[i];
      continue;
    }
    uint32_t best = attrs[target][0];
    double best_s = similarity(i, best);
    for (uint32_t b : attrs[target]) {
      double s = similarity(i, b);
      if (s > best_s || (s == best_s && new_index[b] < new_index[best])) {
        best = b;
        best_s = s;
      }
    }
    out.collapse[new_index[i]] = new_index[best];
  }

  for (uint32_t k = 0; k < n; ++k) {
    uint32_t i = perm[k];
    out.positions.push_back(in.positions[i]);
    if (has_normals) out.normals.push_back(in.normals[i]);
    if (has_colors) out.colors.push_back(in.colors[i]);
    out.welded.push_back(weld[i]);
  }
  for (uint32_t t : source_tris)
    for (uint32_t k = 0; k < 3; ++k) out.indices.push_back(new_index[in.indices[t * 3 + k]]);

  std::vector<uint32_t> removed_at(steps + 1, 0);
  for (uint32_t i = 0; i < n; ++i)
    if (step[weld[i]] != kNone) ++removed_at[step[weld[i]]];
  uint32_t keep = n;
  out.level_vertices.push_back(keep);
  for (uint32_t s = 0; s < steps; ++s) {
    keep -= removed_at[s];
    out.level_vertices.push_back(keep);
  }
  out.level_triangles = level_faces;

  char buf[200];
  std::snprintf(buf, sizeof buf, "%u vertices (%u welded), %u triangles; %u levels down to %u triangles", n, W,
                level_faces.front(), steps + 1, level_faces.back());
  out.report = buf;
  return true;
}

}  // namespace assets
