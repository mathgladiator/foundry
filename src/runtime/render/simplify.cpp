#include "render/simplify.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace runtime {

void simplify(DrawMesh &mesh, int cells) {
  const size_t n = mesh.positions.size();
  if (n == 0 || mesh.indices.empty() || cells < 1) return;
  foundry::vec3 lo = mesh.positions[0], hi = mesh.positions[0];
  for (const auto &p : mesh.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const float side = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-4f});
  const float inv = static_cast<float>(cells) / side;
  const bool has_n = mesh.normals.size() == n, has_c = mesh.colors.size() == n;
  const bool skinned = mesh.bone_a.size() == n && mesh.bone_b.size() == n && mesh.bone_w.size() == n;
  // A vertex's cluster: its cell, and its colour in eight steps per channel (bark and leaf stay apart).
  auto key = [&](size_t i) {
    const auto &p = mesh.positions[i];
    const uint64_t x = static_cast<uint64_t>(std::min<float>((p.x - lo.x) * inv, cells - 1.0f));
    const uint64_t y = static_cast<uint64_t>(std::min<float>((p.y - lo.y) * inv, cells - 1.0f));
    const uint64_t z = static_cast<uint64_t>(std::min<float>((p.z - lo.z) * inv, cells - 1.0f));
    const uint32_t c = has_c ? mesh.colors[i] : 0;
    const uint64_t tone = ((c >> 5) & 7) | (((c >> 13) & 7) << 3) | (((c >> 21) & 7) << 6);
    return x | (y << 12) | (z << 24) | (tone << 36);
  };
  std::unordered_map<uint64_t, uint32_t> cluster;
  cluster.reserve(n);
  std::vector<uint32_t> remap(n);
  struct Sum {
    double p[3] = {0, 0, 0}, nrm[3] = {0, 0, 0}, c[4] = {0, 0, 0, 0};
    uint32_t count = 0, first = 0;
  };
  std::vector<Sum> sums;
  for (size_t i = 0; i < n; ++i) {
    auto [it, fresh] = cluster.emplace(key(i), static_cast<uint32_t>(sums.size()));
    if (fresh) {
      sums.emplace_back();
      sums.back().first = static_cast<uint32_t>(i);
    }
    Sum &s = sums[it->second];
    remap[i] = it->second;
    const auto &p = mesh.positions[i];
    s.p[0] += p.x;
    s.p[1] += p.y;
    s.p[2] += p.z;
    if (has_n) {
      s.nrm[0] += mesh.normals[i].x;
      s.nrm[1] += mesh.normals[i].y;
      s.nrm[2] += mesh.normals[i].z;
    }
    if (has_c)
      for (int k = 0; k < 4; ++k) s.c[k] += (mesh.colors[i] >> (8 * k)) & 255;
    ++s.count;
  }
  DrawMesh out;
  out.positions.resize(sums.size());
  if (has_n) out.normals.resize(sums.size());
  if (has_c) out.colors.resize(sums.size());
  if (skinned) {
    out.bone_a.resize(sums.size());
    out.bone_b.resize(sums.size());
    out.bone_w.resize(sums.size());
  }
  for (size_t k = 0; k < sums.size(); ++k) {
    const Sum &s = sums[k];
    const double w = 1.0 / s.count;
    out.positions[k] = {static_cast<float>(s.p[0] * w), static_cast<float>(s.p[1] * w), static_cast<float>(s.p[2] * w)};
    if (has_n) {
      const double l = std::sqrt(s.nrm[0] * s.nrm[0] + s.nrm[1] * s.nrm[1] + s.nrm[2] * s.nrm[2]);
      out.normals[k] = l > 1e-9 ? foundry::vec3{static_cast<float>(s.nrm[0] / l), static_cast<float>(s.nrm[1] / l),
                                                static_cast<float>(s.nrm[2] / l)}
                                : mesh.normals[s.first];
    }
    if (has_c) {
      uint32_t c = 0;
      for (int j = 0; j < 4; ++j) c |= static_cast<uint32_t>(std::lround(s.c[j] * w)) << (8 * j);
      out.colors[k] = c;
    }
    if (skinned) {  // the first vertex's bones: the cluster moves with one of its vertices
      out.bone_a[k] = mesh.bone_a[s.first];
      out.bone_b[k] = mesh.bone_b[s.first];
      out.bone_w[k] = mesh.bone_w[s.first];
    }
  }
  // Triangles that keep three clusters, each once (either winding: both faces are drawn).
  std::unordered_set<uint64_t> seen;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    uint32_t a = remap[mesh.indices[t]], b = remap[mesh.indices[t + 1]], c = remap[mesh.indices[t + 2]];
    if (a == b || b == c || a == c) continue;
    uint32_t s[3] = {a, b, c};
    std::sort(s, s + 3);
    const uint64_t k = (uint64_t(s[0]) << 42) ^ (uint64_t(s[1]) << 21) ^ s[2];
    if (!seen.insert(k).second) continue;
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
  }
  if (out.indices.empty()) return;  // too small to cluster: keep it as it was
  mesh = std::move(out);
}

}  // namespace runtime
