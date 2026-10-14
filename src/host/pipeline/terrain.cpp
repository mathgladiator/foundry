#include "pipeline/terrain.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pipeline {

namespace {

// The runtime's refine distance at high quality, in node spacings (runtime/render/terrain_renderer.h).
constexpr float kRefine = 48.0f;
constexpr int kTexels = 32;  // the most texels a side a node is retessellated at

std::vector<float> floats(const nlohmann::json &m, const std::string &f) {
  std::vector<float> out;
  if (!m.contains(f) || !m[f].is_array()) return out;
  out.reserve(m[f].size());
  for (const auto &x : m[f]) out.push_back(x.get<float>());
  return out;
}
std::vector<uint32_t> uints(const nlohmann::json &m, const std::string &f) {
  std::vector<uint32_t> out;
  if (!m.contains(f) || !m[f].is_array()) return out;
  out.reserve(m[f].size());
  for (const auto &x : m[f]) out.push_back(x.get<uint32_t>());
  return out;
}
std::vector<foundry::vec3> vec3s(const nlohmann::json &m, const std::string &f) {
  std::vector<foundry::vec3> out;
  if (!m.contains(f) || !m[f].is_array()) return out;
  out.reserve(m[f].size());
  for (const auto &x : m[f]) out.push_back({x[0].get<float>(), x[1].get<float>(), x[2].get<float>()});
  return out;
}

}  // namespace

void flatten_terrain(const nlohmann::json &m, const nlohmann::json &names, float ground_y, runtime::Mesh &out,
                     nlohmann::json &facts) {
  const float eye_y = ground_y + 1.7f;
  auto name = [&](const char *k) { return names.value(k, std::string(k)); };
  const auto mins = vec3s(m, name("node_min")), maxs = vec3s(m, name("node_max"));
  const auto children = uints(m, name("children")), first = uints(m, name("first")), count = uints(m, name("count"));
  const auto spacing = floats(m, name("spacing"));
  const auto pos = vec3s(m, name("positions")), nrm = vec3s(m, name("normals"));
  const auto idx = uints(m, name("indices"));
  const auto images = uints(m, name("images"));
  const uint32_t S = m.value(name("image_size"), 0u);
  const size_t nodes = mins.size();
  if (nodes == 0 || maxs.size() != nodes || children.size() != nodes * 4 || first.size() != nodes || count.size() != nodes ||
      spacing.size() != nodes || S < 2 || images.size() < nodes * S * S || pos.size() % nodes != 0)
    return;
  // Every node has the same vertices: its G x G grid, row by row, then four skirts of G.
  const size_t per = pos.size() / nodes;
  const int G = static_cast<int>(std::lround(std::sqrt(4.0 + static_cast<double>(per)) - 2.0));
  if (G < 2 || static_cast<size_t>(G * G + 4 * G) != per) return;
  const int quads = G - 1;

  // The stand-in floor's quads (x0, z0 to x1, z1, in one colour), appended after everything else so
  // a view with its own ground (a plotter's) can leave them off: the last terrain_floor_triangles.
  struct Floor {
    float x0, z0, x1, z1;
    uint32_t colour;
  };
  std::vector<Floor> floors;
  auto floor_quad = [&](float x0, float z0, float x1, float z1, uint32_t colour) { floors.push_back({x0, z0, x1, z1, colour}); };
  // The cut, from the middle of the root at the eye's height.
  const float ex = 0.5f * (mins[0].x + maxs[0].x), ez = 0.5f * (mins[0].z + maxs[0].z);
  std::vector<uint32_t> cut;
  std::vector<uint32_t> stack = {0};
  while (!stack.empty()) {
    const uint32_t n = stack.back();
    stack.pop_back();
    const float dx = std::max({mins[n].x - ex, 0.0f, ex - maxs[n].x});
    const float dy = std::max({mins[n].y - eye_y, 0.0f, eye_y - maxs[n].y});
    const float dz = std::max({mins[n].z - ez, 0.0f, ez - maxs[n].z});
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool any = false;
    for (int q = 0; q < 4; ++q) any = any || children[n * 4 + q] != 0xffffffffu;
    if (any && d < kRefine * spacing[n]) {
      for (int q = 0; q < 4; ++q) {
        if (children[n * 4 + q] != 0xffffffffu && children[n * 4 + q] < nodes) {
          stack.push_back(children[n * 4 + q]);
          continue;
        }
        // A child left out lies wholly inside the region: stand the floor in, in the colour of the
        // parent's image there.
        const float hx = 0.5f * (maxs[n].x - mins[n].x), hz = 0.5f * (maxs[n].z - mins[n].z);
        const float x0 = mins[n].x + ((q & 1) ? hx : 0.0f), z0 = mins[n].z + ((q & 2) ? hz : 0.0f);
        const size_t a = (q & 1) ? 3 * S / 4 : S / 4, b = (q & 2) ? 3 * S / 4 : S / 4;
        floor_quad(x0, z0, x0 + hx, z0 + hz, images[static_cast<size_t>(n) * S * S + b * S + a]);
      }
    } else {
      cut.push_back(n);
    }
  }

  uint64_t drawn = 0;
  const int step = std::max(1, static_cast<int>(S) / kTexels);
  for (uint32_t n : cut) {
    const size_t base = n * per;
    // Which of the grid's quads the node keeps (inside the region it keeps none).
    std::vector<uint8_t> keep(static_cast<size_t>(quads * quads), 0);
    for (uint32_t t = first[n]; t + 2 < first[n] + count[n] && t + 2 < idx.size(); t += 3) {
      const uint32_t lo = std::min({idx[t], idx[t + 1], idx[t + 2]});
      const uint32_t hi = std::max({idx[t], idx[t + 1], idx[t + 2]});
      if (lo < base || hi >= base + static_cast<size_t>(G * G)) continue;  // a skirt
      const int i = static_cast<int>((lo - base) % G), j = static_cast<int>((lo - base) / G);
      if (i < quads && j < quads) keep[static_cast<size_t>(j * quads + i)] = 1;
    }
    // Texel columns and rows to place vertices at.
    std::vector<int> at;
    for (int a = 0; a < static_cast<int>(S) - 1; a += step) at.push_back(a);
    at.push_back(static_cast<int>(S) - 1);
    const int A = static_cast<int>(at.size());
    const uint32_t first_vertex = static_cast<uint32_t>(out.positions.size());
    const uint32_t *img = images.data() + static_cast<size_t>(n) * S * S;
    for (int b = 0; b < A; ++b)
      for (int a = 0; a < A; ++a) {
        const float gx = static_cast<float>(at[static_cast<size_t>(a)]) / static_cast<float>(S - 1) * static_cast<float>(quads);
        const float gz = static_cast<float>(at[static_cast<size_t>(b)]) / static_cast<float>(S - 1) * static_cast<float>(quads);
        const int i = std::min(quads - 1, static_cast<int>(gx)), j = std::min(quads - 1, static_cast<int>(gz));
        const float fx = gx - static_cast<float>(i), fz = gz - static_cast<float>(j);
        auto P = [&](int ii, int jj) { return pos[base + static_cast<size_t>(jj * G + ii)]; };
        auto N = [&](int ii, int jj) { return nrm.size() == pos.size() ? nrm[base + static_cast<size_t>(jj * G + ii)] : foundry::vec3{0, 1, 0}; };
        auto bil = [&](foundry::vec3 p00, foundry::vec3 p10, foundry::vec3 p01, foundry::vec3 p11) {
          auto l = [](foundry::vec3 a0, foundry::vec3 a1, float t) {
            return foundry::vec3{a0.x + (a1.x - a0.x) * t, a0.y + (a1.y - a0.y) * t, a0.z + (a1.z - a0.z) * t};
          };
          return l(l(p00, p10, fx), l(p01, p11, fx), fz);
        };
        out.positions.push_back(bil(P(i, j), P(i + 1, j), P(i, j + 1), P(i + 1, j + 1)));
        out.normals.push_back(bil(N(i, j), N(i + 1, j), N(i, j + 1), N(i + 1, j + 1)));
        out.colors.push_back(img[static_cast<size_t>(at[static_cast<size_t>(b)]) * S + static_cast<size_t>(at[static_cast<size_t>(a)])] |
                             0xff000000u);
      }
    auto quad_kept = [&](int a, int b) {
      const float gx = (static_cast<float>(at[static_cast<size_t>(a)]) + 0.5f * step) / static_cast<float>(S - 1) * quads;
      const float gz = (static_cast<float>(at[static_cast<size_t>(b)]) + 0.5f * step) / static_cast<float>(S - 1) * quads;
      const int i = std::min(quads - 1, static_cast<int>(gx)), j = std::min(quads - 1, static_cast<int>(gz));
      return keep[static_cast<size_t>(j * quads + i)] != 0;
    };
    for (int b = 0; b + 1 < A; ++b)
      for (int a = 0; a + 1 < A; ++a) {
        if (!quad_kept(a, b)) {
          // Inside the region: the stand-in floor.
          const foundry::vec3 p0 = out.positions[first_vertex + static_cast<uint32_t>(b * A + a)];
          const foundry::vec3 p1 = out.positions[first_vertex + static_cast<uint32_t>((b + 1) * A + a + 1)];
          floor_quad(p0.x, p0.z, p1.x, p1.z, out.colors[first_vertex + static_cast<uint32_t>(b * A + a)]);
          continue;
        }
        const uint32_t v00 = first_vertex + static_cast<uint32_t>(b * A + a), v10 = v00 + 1, v01 = v00 + static_cast<uint32_t>(A),
                       v11 = v01 + 1;
        out.indices.insert(out.indices.end(), {v00, v01, v11, v00, v11, v10});
        drawn += 2;
      }
    // Skirts down to the node's floor, where its edge is kept.
    const float floor_y = mins[n].y;
    for (int side = 0; side < 4; ++side)
      for (int k = 0; k + 1 < A; ++k) {
        int a0, b0, a1, b1;
        if (side == 0) a0 = k, b0 = 0, a1 = k + 1, b1 = 0;
        else if (side == 1) a0 = A - 1, b0 = k, a1 = A - 1, b1 = k + 1;
        else if (side == 2) a0 = k + 1, b0 = A - 1, a1 = k, b1 = A - 1;
        else a0 = 0, b0 = k + 1, a1 = 0, b1 = k;
        if (!quad_kept(std::min(std::min(a0, a1), A - 2), std::min(std::min(b0, b1), A - 2))) continue;
        const uint32_t ta = first_vertex + static_cast<uint32_t>(b0 * A + a0), tb = first_vertex + static_cast<uint32_t>(b1 * A + a1);
        const uint32_t sa = static_cast<uint32_t>(out.positions.size());
        for (uint32_t v : {ta, tb}) {
          foundry::vec3 p = out.positions[v];
          p.y = floor_y;
          out.positions.push_back(p);
          out.normals.push_back(out.normals[v]);
          out.colors.push_back(out.colors[v]);
        }
        out.indices.insert(out.indices.end(), {ta, tb, sa + 1, ta, sa + 1, sa});
        drawn += 2;
      }
  }
  for (const Floor &f : floors) {
    const uint32_t v = static_cast<uint32_t>(out.positions.size());
    for (int k = 0; k < 4; ++k) {
      out.positions.push_back({(k & 1) ? f.x1 : f.x0, ground_y, (k & 2) ? f.z1 : f.z0});
      out.normals.push_back({0, 1, 0});
      out.colors.push_back(f.colour | 0xff000000u);
    }
    out.indices.insert(out.indices.end(), {v, v + 2, v + 3, v, v + 3, v + 1});
  }
  facts["terrain_floor_triangles"] = floors.size() * 2;
  uint64_t total = 0;
  for (size_t n = 0; n < nodes; ++n) total += count[n] / 3;
  facts["terrain_nodes"] = nodes;
  facts["terrain_triangles"] = total;
  facts["terrain_cut_nodes"] = cut.size();
  facts["terrain_cut_triangles"] = drawn;
  facts["image_size"] = S;
}

}  // namespace pipeline
