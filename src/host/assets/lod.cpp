#include "assets/lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include <meshoptimizer.h>

namespace assets {

size_t level_for(const ProgressiveMesh &pm, uint32_t triangles) {
  size_t level = 0;
  const auto &lt = pm.level_triangles;
  while (level + 1 < lt.size() && lt[level] > triangles) ++level;
  return level;
}

runtime::Mesh extract_level(const ProgressiveMesh &pm, size_t level) {
  runtime::Mesh out;
  const uint32_t n = static_cast<uint32_t>(pm.positions.size());
  if (n == 0 || pm.level_vertices.empty()) return out;
  level = std::min(level, pm.level_vertices.size() - 1);
  const uint32_t keep = pm.level_vertices[level];

  // Welded positions after `level` collapse steps.
  uint32_t wmax = 0;
  for (uint32_t w : pm.welded) wmax = std::max(wmax, w);
  std::vector<foundry::vec3> wpos(wmax + 1);
  for (uint32_t i = 0; i < n; ++i) wpos[pm.welded[i]] = pm.positions[i];
  for (size_t s = 0; s < level && s < pm.step_welded.size(); ++s) wpos[pm.step_welded[s]] = pm.step_position[s];

  std::vector<uint32_t> resolved(n, 0xffffffffu), remap(n, 0xffffffffu);
  auto resolve = [&](uint32_t i) {
    if (resolved[i] != 0xffffffffu) return resolved[i];
    uint32_t r = i;
    while (r >= keep && pm.collapse[r] != r) r = pm.collapse[r];
    return resolved[i] = r;
  };
  const bool has_n = pm.normals.size() == n, has_c = pm.colors.size() == n;
  std::vector<uint32_t> source;  // output vertex -> progressive vertex
  auto use = [&](uint32_t i) {
    if (remap[i] == 0xffffffffu) {
      remap[i] = static_cast<uint32_t>(out.positions.size());
      out.positions.push_back(wpos[pm.welded[i]]);
      if (has_c) out.colors.push_back(pm.colors[i]);
      source.push_back(i);
    }
    return remap[i];
  };
  std::vector<uint32_t> face_welded;
  for (size_t t = 0; t + 2 < pm.indices.size(); t += 3) {
    uint32_t a = pm.indices[t], b = pm.indices[t + 1], c = pm.indices[t + 2];
    if (a >= n || b >= n || c >= n) continue;
    a = resolve(a);
    b = resolve(b);
    c = resolve(c);
    uint32_t wa = pm.welded[a], wb = pm.welded[b], wc = pm.welded[c];
    if (wa == wb || wb == wc || wa == wc) continue;
    out.indices.push_back(use(a));
    out.indices.push_back(use(b));
    out.indices.push_back(use(c));
    face_welded.insert(face_welded.end(), {wa, wb, wc});
  }

  // Normals: area-weighted face normals around each welded position, keeping
  // only faces within 60 degrees of the vertex's original normal (hard edges).
  const size_t tris = out.indices.size() / 3;
  std::vector<foundry::vec3> fn(tris);
  std::vector<std::vector<uint32_t>> wfaces(wmax + 1);
  for (size_t t = 0; t < tris; ++t) {
    const auto &p0 = out.positions[out.indices[t * 3]], &p1 = out.positions[out.indices[t * 3 + 1]],
               &p2 = out.positions[out.indices[t * 3 + 2]];
    float ux = p1.x - p0.x, uy = p1.y - p0.y, uz = p1.z - p0.z, vx = p2.x - p0.x, vy = p2.y - p0.y, vz = p2.z - p0.z;
    fn[t] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
    for (int k = 0; k < 3; ++k) wfaces[face_welded[t * 3 + k]].push_back(static_cast<uint32_t>(t));
  }
  out.normals.resize(out.positions.size());
  for (size_t v = 0; v < out.positions.size(); ++v) {
    uint32_t i = source[v];
    foundry::vec3 ref = has_n ? pm.normals[i] : foundry::vec3{0, 0, 0};
    float rl = std::sqrt(ref.x * ref.x + ref.y * ref.y + ref.z * ref.z);
    foundry::vec3 sum{0, 0, 0}, all{0, 0, 0};
    for (uint32_t t : wfaces[pm.welded[i]]) {
      const auto &f = fn[t];
      all = {all.x + f.x, all.y + f.y, all.z + f.z};
      float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
      if (rl > 0 && fl > 0 && (f.x * ref.x + f.y * ref.y + f.z * ref.z) < 0.5f * fl * rl) continue;
      sum = {sum.x + f.x, sum.y + f.y, sum.z + f.z};
    }
    if (sum.x == 0 && sum.y == 0 && sum.z == 0) sum = all;
    float l = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
    out.normals[v] = l > 0 ? foundry::vec3{sum.x / l, sum.y / l, sum.z / l} : foundry::vec3{0, 1, 0};
  }
  return out;
}

runtime::Mesh extract_lod(const ProgressiveMesh &pm, uint32_t triangles) {
  return extract_level(pm, level_for(pm, triangles));
}

runtime::Mesh optimize_list(const runtime::Mesh &in) {
  runtime::Mesh m = in;
  const size_t n = m.positions.size();
  meshopt_optimizeVertexCache(m.indices.data(), m.indices.data(), m.indices.size(), n);
  std::vector<unsigned int> remap(n);
  size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), m.indices.data(), m.indices.size(), n);
  meshopt_remapIndexBuffer(m.indices.data(), m.indices.data(), m.indices.size(), remap.data());
  auto remap_stream = [&](auto &v) {
    if (v.size() != n) return;
    auto copy = v;
    meshopt_remapVertexBuffer(v.data(), copy.data(), n, sizeof(v[0]), remap.data());
    v.resize(unique);
  };
  remap_stream(m.positions);
  remap_stream(m.normals);
  remap_stream(m.colors);
  return m;
}

nlohmann::json mesh_stats(const runtime::Mesh &m) {
  const size_t n = m.positions.size(), tris = m.indices.size() / 3;
  nlohmann::json s;
  s["triangles"] = tris;
  s["vertices"] = n;
  if (n == 0) return s;

  auto key = [&](uint32_t i) {
    uint32_t b[3];
    std::memcpy(&b[0], &m.positions[i].x, 4);
    std::memcpy(&b[1], &m.positions[i].y, 4);
    std::memcpy(&b[2], &m.positions[i].z, 4);
    return std::make_tuple(b[0], b[1], b[2]);
  };
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> ids;
  std::vector<uint32_t> weld(n);
  for (uint32_t i = 0; i < n; ++i) weld[i] = ids.emplace(key(i), static_cast<uint32_t>(ids.size())).first->second;
  s["welded_vertices"] = ids.size();

  foundry::vec3 lo = m.positions[0], hi = m.positions[0];
  for (const auto &p : m.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  s["bounds_min"] = {lo.x, lo.y, lo.z};
  s["bounds_max"] = {hi.x, hi.y, hi.z};
  s["size"] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};

  double area = 0, volume = 0;
  size_t degenerate = 0;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> edges;
  for (size_t t = 0; t < tris; ++t) {
    uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
    if (i0 >= n || i1 >= n || i2 >= n) continue;
    const auto &a = m.positions[i0], &b = m.positions[i1], &c = m.positions[i2];
    double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    double len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (len <= 1e-12) ++degenerate;
    area += 0.5 * len;
    // Signed volume of the tetrahedron with the origin (divergence theorem).
    volume += (static_cast<double>(a.x) * (static_cast<double>(b.y) * c.z - static_cast<double>(b.z) * c.y) -
               static_cast<double>(a.y) * (static_cast<double>(b.x) * c.z - static_cast<double>(b.z) * c.x) +
               static_cast<double>(a.z) * (static_cast<double>(b.x) * c.y - static_cast<double>(b.y) * c.x)) / 6.0;
    uint32_t w[3] = {weld[i0], weld[i1], weld[i2]};
    for (int k = 0; k < 3; ++k) {
      uint32_t x = w[k], y = w[(k + 1) % 3];
      if (x != y) ++edges[{std::min(x, y), std::max(x, y)}];
    }
  }
  size_t boundary = 0, nonmanifold = 0;
  for (const auto &[e, count] : edges) {
    if (count == 1) ++boundary;
    else if (count > 2) ++nonmanifold;
  }
  s["surface_area_m2"] = area;
  s["volume_m3"] = std::fabs(volume);
  s["degenerate_triangles"] = degenerate;
  s["boundary_edges"] = boundary;
  s["non_manifold_edges"] = nonmanifold;
  s["watertight"] = boundary == 0 && nonmanifold == 0;
  s["index_count"] = m.indices.size();
  s["gpu_bytes"] = n * 28 + m.indices.size() * 4;
  return s;
}

}  // namespace assets
