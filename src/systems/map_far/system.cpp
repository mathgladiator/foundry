// map_far: a map's far land (design/map.md, "Executing it"): the whole map as a terrain quadtree
// (draw.terrain, as a boundary's) from its macro plan, for what a player sees beyond the chunks near
// them.
//
// 1. A quadtree over the map's grid, refined everywhere inside the map down to its leaves (the grid's
//    spacing); nodes wholly outside are left to the boundary.
// 2. Each node a grid of 32 x 32 quads at the plan's heights, sunk a little below the ground (the
//    chunks near the player cover it), with skirts and morph targets toward its parent.
// 3. Each node's image: each parcel's land by its kind (grass by the biome and season, woods by its
//    cover, rock and snow on the heights), water, roads, and sites' flats, with alpha marking steep
//    rock (the shader lays strata there).
#include <string>
#include <vector>

#include "boundary.h"
#include "channels/boundary_model.h"
#include "channels/map_far_spec.h"
#include "math.h"
#include "packer/boundary_model.h"
#include "packer/map_far_spec.h"

namespace map_far {

using namespace bmath;
using channels::boundary_model;
using channels::map_far_spec;
using channels::map_plan;

namespace {

constexpr int kGrid = 32;
constexpr uint32_t kNone = 0xffffffffu;
constexpr int kTex = 32;  // texels along a node's image
enum Kind : uint32_t { kPlains, kHills, kHighlands, kMountains, kValley, kLake, kMarsh, kCoast };

uint32_t pack_colour(vec3 c, float a = 1) {
  auto byte = [](float v) { return static_cast<uint32_t>(clampf(v, 0, 1) * 255.0f + 0.5f); };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (byte(a) << 24);
}

struct Field {
  const map_plan &p;
  int nx, nz;
  size_t at(int i, int k) const { return static_cast<size_t>(maxi_(0, mini_(nz - 1, k))) * static_cast<size_t>(nx) + static_cast<size_t>(maxi_(0, mini_(nx - 1, i))); }
  float height(float x, float z) const {
    const float fx = clampf((x - p.gx) / p.spacing, 0, static_cast<float>(nx - 1) - 1e-3f);
    const float fz = clampf((z - p.gz) / p.spacing, 0, static_cast<float>(nz - 1) - 1e-3f);
    const int i = static_cast<int>(std::floor(fx)), k = static_cast<int>(std::floor(fz));
    const float u = fx - static_cast<float>(i), v = fz - static_cast<float>(k);
    return mixf(mixf(p.heights[at(i, k)], p.heights[at(i + 1, k)], u), mixf(p.heights[at(i, k + 1)], p.heights[at(i + 1, k + 1)], u), v);
  }
  size_t nearest(float x, float z) const { return at(round_to_int((x - p.gx) / p.spacing), round_to_int((z - p.gz) / p.spacing)); }
  bool inside(float x, float z) const { return p.parcel_ids[nearest(x, z)] != 65535u; }
};

}  // namespace

bool run(const map_far_spec &in, boundary_model &out, std::string &error) {
  out = boundary_model{};
  const map_plan &plan = in.plan;
  const int nx = static_cast<int>(plan.nx), nz = static_cast<int>(plan.nz);
  if (nx < 2 || nz < 2 || plan.heights.size() != static_cast<size_t>(nx) * static_cast<size_t>(nz)) {
    error = "the plan has no grid";
    return false;
  }
  const Field field{plan, nx, nz};
  const float sink = clampf(in.sink, 0, 10);
  const auto &b = in.biome;
  const uint32_t seed = 977u;

  // 1. The quadtree: from a root covering the grid down to leaves at the grid's spacing.
  struct Node {
    float x0, z0, size;
    uint32_t child[4];
  };
  std::vector<Node> nodes;
  const float leaf = plan.spacing;
  const float wx = static_cast<float>(nx - 1) * plan.spacing, wz = static_cast<float>(nz - 1) * plan.spacing;
  float root = leaf * kGrid;
  while (root < maxf(wx, wz)) root *= 2;
  auto any_inside = [&](float x0, float z0, float size) {
    const int steps = 8;
    for (int j = 0; j <= steps; ++j)
      for (int i = 0; i <= steps; ++i)
        if (field.inside(x0 + size * static_cast<float>(i) / steps, z0 + size * static_cast<float>(j) / steps)) return true;
    return false;
  };
  struct Maker {
    std::vector<Node> &nodes;
    float leaf;
    const decltype(any_inside) &inside;
    uint32_t make(float x0, float z0, float size) {
      if (!inside(x0, z0, size)) return kNone;
      const uint32_t me = static_cast<uint32_t>(nodes.size());
      nodes.push_back({x0, z0, size, {kNone, kNone, kNone, kNone}});
      if (size / kGrid > leaf * 1.001f) {
        const float h = size * 0.5f;
        for (int q = 0; q < 4; ++q) {
          const uint32_t c = make(x0 + ((q & 1) ? h : 0.0f), z0 + ((q & 2) ? h : 0.0f), h);
          nodes[me].child[q] = c;
        }
      }
      return me;
    }
  } maker{nodes, leaf, any_inside};
  const float cx = plan.gx + wx * 0.5f, cz = plan.gz + wz * 0.5f;
  maker.make(cx - root * 0.5f, cz - root * 0.5f, root);
  if (nodes.empty()) {
    error = "the map has no land";
    return false;
  }
  const size_t count = nodes.size();
  out.image_size = kTex;
  out.images.assign(count * static_cast<size_t>(kTex * kTex), 0u);

  // The colour of the land at a place.
  const vec3 grass = in.season == 0 ? lerp(b.grass_dry, b.snow, 0.75f) : (in.season == 3 ? lerp(b.grass, b.grass_dry, 0.55f) : b.grass);
  const vec3 crowns = in.season == 3 ? lerp(b.foliage, b.autumn, 0.6f) : (in.season == 0 ? lerp(b.foliage_dark, b.snow, 0.35f) : b.foliage);
  auto colour = [&](float x, float z, float h, float slope, float &alpha) {
    const size_t n = field.nearest(x, z);
    const uint32_t pid = plan.parcel_ids[n];
    const auto *pc = pid < plan.parcels.size() ? &plan.parcels[pid] : nullptr;
    const uint32_t kind = pc ? pc->kind : kPlains;
    const float var = fbm(x / 60.0f, z / 60.0f, 3, seed + 3);
    vec3 c = lerp(grass, b.grass_dry, clampf(var - 0.35f, 0, 1) * 0.6f);
    if (kind == kMarsh) c = lerp(c, b.reed, 0.5f);
    if (kind == kCoast) c = lerp(c, b.sand, 0.35f);
    // Woods: patches by the parcel's cover.
    const float woods = pc ? clampf(pc->woods, 0, 1) : 0;
    if (woods > 0) {
      const float patch = fbm(x / 90.0f, z / 90.0f, 3, seed + 7);
      const float cover = step_between(1 - woods * 0.9f, 1.05f - woods * 0.85f, patch);
      c = lerp(c, crowns * (0.75f + 0.3f * fbm(x / 9.0f, z / 9.0f, 2, seed + 11)), cover);
    }
    alpha = 1;
    const float rock = step_between(0.55f, 0.9f, slope) + (kind == kMountains ? step_between(40, 90, h) * 0.6f : 0.0f);
    if (rock > 0) {
      c = lerp(c, lerp(b.rock, b.rock_dark, clampf(var, 0, 1)), clampf(rock, 0, 1));
      alpha = 1 - step_between(0.6f, 0.95f, slope);
    }
    if (b.snow_line > 0 && h > b.snow_line + 20 * (var - 0.5f)) c = lerp(c, b.snow, 0.85f);
    if (in.season == 0 && h > 25) c = lerp(c, b.snow, 0.6f);
    // Flats, roads, water.
    for (const auto &f : plan.flats) {
      const float d = foundry::det::sqrt((x - f.position.x) * (x - f.position.x) + (z - f.position.z) * (z - f.position.z));
      if (d < f.radius) c = lerp(c, lerp(b.soil, b.grass_dry, 0.5f), 0.55f);
    }
    const float wl = plan.water[n];
    if (wl > -999 && wl > h - 0.2f) {
      c = lerp(b.water_shallow, b.water_deep, clampf((wl - h) / 6.0f, 0, 1));
      alpha = 1;
    }
    return c;
  };
  // Roads and rivers in 64 m buckets, so a texel reads only those near it.
  constexpr float kBucket = 64.0f;
  const int bw = static_cast<int>(wx / kBucket) + 2, bh = static_cast<int>(wz / kBucket) + 2;
  std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(bw) * static_cast<size_t>(bh));
  for (size_t k = 0; k < plan.roads.size(); ++k) {
    const auto &sg = plan.roads[k];
    const float pad = sg.width * 0.5f + leaf;
    const int i0 = maxi_(0, static_cast<int>((minf(sg.a.x, sg.b.x) - pad - plan.gx) / kBucket));
    const int i1 = mini_(bw - 1, static_cast<int>((maxf(sg.a.x, sg.b.x) + pad - plan.gx) / kBucket));
    const int k0 = maxi_(0, static_cast<int>((minf(sg.a.z, sg.b.z) - pad - plan.gz) / kBucket));
    const int k1 = mini_(bh - 1, static_cast<int>((maxf(sg.a.z, sg.b.z) + pad - plan.gz) / kBucket));
    for (int kk = k0; kk <= k1; ++kk)
      for (int ii = i0; ii <= i1; ++ii) buckets[static_cast<size_t>(kk * bw + ii)].push_back(static_cast<uint32_t>(k));
  }
  auto road_shade = [&](float x, float z, float texel, vec3 &c) {
    const int bi = static_cast<int>((x - plan.gx) / kBucket), bk = static_cast<int>((z - plan.gz) / kBucket);
    if (bi < 0 || bk < 0 || bi >= bw || bk >= bh) return;
    for (uint32_t k : buckets[static_cast<size_t>(bk * bw + bi)]) {
      const auto &sg = plan.roads[k];
      const float abx = sg.b.x - sg.a.x, abz = sg.b.z - sg.a.z, l2 = abx * abx + abz * abz;
      const float t = l2 > 1e-9f ? clampf(((x - sg.a.x) * abx + (z - sg.a.z) * abz) / l2, 0, 1) : 0.0f;
      const float dx = sg.a.x + abx * t - x, dz = sg.a.z + abz * t - z;
      const float d = foundry::det::sqrt(dx * dx + dz * dz);
      const float half = maxf(sg.width * 0.5f, texel * 0.45f);
      if (d > half) continue;
      c = sg.kind == 3 ? b.water_shallow : (sg.kind == 0 ? lerp(b.rock, b.soil, 0.5f) * 1.15f : b.soil);
    }
  };

  // 2 and 3: each node's grid, skirts, morph targets, and image.
  const int G = kGrid + 1, B = kGrid + 3;
  std::vector<float> top(static_cast<size_t>(B * B));
  float highest = -1e30f, farthest = 0;
  for (size_t ni = 0; ni < count; ++ni) {
    const Node &node = nodes[ni];
    const float sp = node.size / kGrid;
    for (int j = 0; j < B; ++j)
      for (int i = 0; i < B; ++i) {
        const float x = node.x0 + static_cast<float>(i - 1) * sp, z = node.z0 + static_cast<float>(j - 1) * sp;
        const size_t n = field.nearest(x, z);
        const float h = field.height(x, z) - sink;
        const float wl = plan.water[n];
        top[static_cast<size_t>(j * B + i)] = wl > -999 ? maxf(h, wl - sink * 0.5f) : h;
      }
    auto T = [&](int i, int j) { return top[static_cast<size_t>((j + 1) * B + (i + 1))]; };
    const uint32_t first_vertex = static_cast<uint32_t>(out.terrain_positions.size());
    float y_lo = 1e30f, y_hi = -1e30f;
    for (int j = 0; j < G; ++j)
      for (int i = 0; i < G; ++i) {
        const float x = node.x0 + static_cast<float>(i) * sp, z = node.z0 + static_cast<float>(j) * sp;
        const float y = T(i, j);
        y_lo = minf(y_lo, y);
        y_hi = maxf(y_hi, y);
        out.terrain_positions.push_back(v3(x, y, z));
        out.terrain_normals.push_back(normalize(v3(T(i - 1, j) - T(i + 1, j), 2 * sp, T(i, j - 1) - T(i, j + 1))));
        out.terrain_uvs.push_back((0.5f + static_cast<float>(i) / kGrid * static_cast<float>(kTex - 1)) / static_cast<float>(kTex));
        out.terrain_uvs.push_back((0.5f + static_cast<float>(j) / kGrid * static_cast<float>(kTex - 1)) / static_cast<float>(kTex));
        float target = y;
        if ((i & 1) && (j & 1)) target = 0.5f * (T(i - 1, j - 1) + T(i + 1, j + 1));
        else if (i & 1) target = 0.5f * (T(i - 1, j) + T(i + 1, j));
        else if (j & 1) target = 0.5f * (T(i, j - 1) + T(i, j + 1));
        out.terrain_morph.push_back(target - y);
      }
    const uint32_t first_index = static_cast<uint32_t>(out.terrain_indices.size());
    for (int j = 0; j < kGrid; ++j)
      for (int i = 0; i < kGrid; ++i) {
        const float x = node.x0 + (static_cast<float>(i) + 0.5f) * sp, z = node.z0 + (static_cast<float>(j) + 0.5f) * sp;
        if (!field.inside(x, z)) continue;  // beyond the map: the boundary's
        const uint32_t v00 = first_vertex + static_cast<uint32_t>(j * G + i), v10 = v00 + 1, v01 = v00 + static_cast<uint32_t>(G), v11 = v01 + 1;
        out.terrain_indices.insert(out.terrain_indices.end(), {v00, v01, v11, v00, v11, v10});
      }
    const float skirt = clampf(maxf(sp * 1.5f, 0.15f * (y_hi - y_lo)), 1.0f, 150.0f);
    for (int side = 0; side < 4; ++side) {
      const uint32_t first_skirt = static_cast<uint32_t>(out.terrain_positions.size());
      for (int k = 0; k < G; ++k) {
        const int i = side == 0 ? k : (side == 1 ? kGrid : (side == 2 ? kGrid - k : 0));
        const int j = side == 0 ? 0 : (side == 1 ? k : (side == 2 ? kGrid : kGrid - k));
        const uint32_t v = first_vertex + static_cast<uint32_t>(j * G + i);
        vec3 p = out.terrain_positions[v];
        p.y -= skirt;
        out.terrain_positions.push_back(p);
        out.terrain_normals.push_back(out.terrain_normals[v]);
        out.terrain_uvs.push_back(out.terrain_uvs[2 * v]);
        out.terrain_uvs.push_back(out.terrain_uvs[2 * v + 1]);
        out.terrain_morph.push_back(out.terrain_morph[v]);
      }
      for (int k = 0; k < kGrid; ++k) {
        const int i0 = side == 0 ? k : (side == 1 ? kGrid : (side == 2 ? kGrid - k : 0));
        const int j0 = side == 0 ? 0 : (side == 1 ? k : (side == 2 ? kGrid : kGrid - k));
        const int i1 = side == 0 ? k + 1 : (side == 1 ? kGrid : (side == 2 ? kGrid - k - 1 : 0));
        const int j1 = side == 0 ? 0 : (side == 1 ? k + 1 : (side == 2 ? kGrid : kGrid - k - 1));
        const uint32_t a = first_vertex + static_cast<uint32_t>(j0 * G + i0), c = first_vertex + static_cast<uint32_t>(j1 * G + i1);
        const vec3 pa = out.terrain_positions[a], pc = out.terrain_positions[c];
        if (!field.inside((pa.x + pc.x) * 0.5f, (pa.z + pc.z) * 0.5f)) continue;
        const uint32_t sa = first_skirt + static_cast<uint32_t>(k), sb = sa + 1;
        out.terrain_indices.insert(out.terrain_indices.end(), {a, c, sb, a, sb, sa});
      }
    }
    out.node_first.push_back(first_index);
    out.node_count.push_back(static_cast<uint32_t>(out.terrain_indices.size()) - first_index);
    out.node_min.push_back(v3(node.x0, y_lo - skirt, node.z0));
    out.node_max.push_back(v3(node.x0 + node.size, y_hi, node.z0 + node.size));
    out.node_spacing.push_back(sp);
    for (int q = 0; q < 4; ++q) out.node_children.push_back(node.child[q]);
    highest = maxf(highest, y_hi);
    for (int c = 0; c < 4; ++c) {
      const float px = node.x0 + ((c & 1) ? node.size : 0.0f), pz = node.z0 + ((c & 2) ? node.size : 0.0f);
      farthest = maxf(farthest, length(v3(px, 0, pz)));
    }
    // Its image.
    const float texel = node.size / static_cast<float>(kTex - 1);
    uint32_t *img = out.images.data() + ni * static_cast<size_t>(kTex * kTex);
    for (int bj = 0; bj < kTex; ++bj)
      for (int ai = 0; ai < kTex; ++ai) {
        const float x = node.x0 + static_cast<float>(ai) * texel, z = node.z0 + static_cast<float>(bj) * texel;
        const float h = field.height(x, z);
        const float dx = field.height(x + texel, z) - field.height(x - texel, z), dz = field.height(x, z + texel) - field.height(x, z - texel);
        const float slope = foundry::det::sqrt(dx * dx + dz * dz) / (2 * texel);
        float alpha;
        vec3 c = colour(x, z, h, slope, alpha);
        if (texel < 40) road_shade(x, z, texel, c);
        img[bj * kTex + ai] = (pack_colour(c) & 0x00ffffffu) | (static_cast<uint32_t>(clampf(alpha, 0, 1) * 255.0f + 0.5f) << 24);
      }
  }
  // A drawable model needs a mesh: one hidden triangle far below.
  const vec3 low = v3(cx, -500, cz);
  out.positions = {low, low + v3(0, 0, 0.01f), low + v3(0.01f, 0, 0)};
  out.normals = {v3(0, 1, 0), v3(0, 1, 0), v3(0, 1, 0)};
  out.colors = {pack_colour(b.rock), pack_colour(b.rock), pack_colour(b.rock)};
  out.indices = {0, 1, 2};
  out.height = highest;
  out.radius = farthest;
  out.rim_cells = 0;
  out.rim_length = 0;
  return true;
}

}  // namespace map_far

FOUNDRY_SYSTEM(map_far, channels::map_far_spec, channels::boundary_model)
