// boundary_terrain: the world a boundary invents beyond a playable region (design/boundary.md).
//
// 1. The rim, indexed (field.h): every point's signed distance from it and the edges it grows
//    from, blended over more of the rim the farther the point lies, so the hull's cells seed the
//    world and their seeds spread into the distance.
// 2. The land (field.h): each edge's profile near the rim (a ledge's drop to a valley floor with
//    its river, a ridge's cliff and plateau, a mountain's slope, a forest's and jungle's canopy
//    beyond their dressing, a sea's beach and depths, a marsh's pools), and the far land climbing
//    toward peaks by the rise; every edge's share blended, so transitions are smooth.
// 3. A quadtree from the region's middle out past the view distance, refined only where a player
//    inside the region could ever be near enough to see the difference (nearer than kRefine times
//    a node's spacing), and never inside the region. Each node is a grid of 32 x 32 quads with
//    skirts down its sides, each vertex with its rise to its parent's surface (the runtime morphs
//    it there as the node nears its parent's distance), and its own image (paint.h).
// 4. The rim's dressing (dress.h): cliff faces, ledges' lips, trees and undergrowth, scree,
//    reeds, surf, and driftwood.
// 5. What keeps the player in: walls along the rim and the floor of the hull's cells; blaze points
//    in its forests; the hull's cells as guides.
#include <vector>

#include "boundary.h"
#include "channels/boundary_model.h"
#include "channels/boundary_terrain_spec.h"
#include "dress.h"
#include "field.h"
#include "math.h"
#include "packer/boundary_model.h"
#include "packer/boundary_terrain_spec.h"
#include "paint.h"

namespace boundary_terrain {

using channels::boundary_model;
using channels::boundary_terrain_spec;

namespace {

// The runtime refines a node while the eye is nearer than this many of its grid spacings, at the
// highest quality (design/boundary.md, "Levels"); the build refines wherever a player inside the
// region could be that near.
constexpr float kRefine = 48.0f;
constexpr int kGrid = 32;  // quads along a node's side
constexpr uint32_t kNone = 0xffffffffu;
constexpr uint32_t kBlazeFire = 4;  // config/fire_configs: blaze
constexpr float kInside = -0.9f;    // a point this far inside the rim is the game's ground

struct Node {
  float x0 = 0, z0 = 0, size = 0;
  uint32_t child[4] = {kNone, kNone, kNone, kNone};
};

struct Tree {
  const RimIndex &rim;
  float leaf = 2;
  std::vector<Node> nodes;

  uint32_t make(float x0, float z0, float size) {
    const float dr = rim.distance_to_box(x0, z0, x0 + size, z0 + size);
    if (dr > 1.0f) {
      const Influence f = rim.at(x0 + size * 0.5f, z0 + size * 0.5f);
      if (f.d < 0) return kNone;  // wholly inside the region
    }
    const uint32_t me = static_cast<uint32_t>(nodes.size());
    Node node;
    node.x0 = x0;
    node.z0 = z0;
    node.size = size;
    nodes.push_back(node);
    const float spacing = size / kGrid;
    if (spacing > leaf * 1.001f && dr < kRefine * spacing) {
      const float h = size * 0.5f;
      for (int q = 0; q < 4; ++q) {
        const uint32_t c = make(x0 + ((q & 1) ? h : 0.0f), z0 + ((q & 2) ? h : 0.0f), h);
        nodes[me].child[q] = c;
      }
    }
    return me;
  }
};

uint32_t edge_colour(uint32_t e) {
  static const vec3 colours[8] = {{0.6f, 0.6f, 0.6f}, {0.95f, 0.85f, 0.3f}, {0.7f, 0.45f, 0.3f}, {0.85f, 0.85f, 0.95f},
                                  {0.25f, 0.7f, 0.3f}, {0.1f, 0.85f, 0.55f}, {0.25f, 0.55f, 0.95f}, {0.55f, 0.65f, 0.35f}};
  return pack_colour(colours[e < 8 ? e : 0]);
}

}  // namespace

bool run(const boundary_terrain_spec &in, boundary_model &out, std::string &error) {
  out = boundary_model{};
  const auto &rim = in.rim;
  if (rim.points.size() < 8 || rim.outward.size() != rim.points.size()) {
    error = "the rim has too few samples";
    return false;
  }
  const RimIndex index(rim);
  const uint32_t detail = in.detail > 2 ? 2 : in.detail;
  const uint32_t seed = hash_u32(in.seed * 2654435761u + 17u);
  World world_{in.biome, in.season, seed};
  world_.figure_range = clampf(in.figure_range, 0, 300);
  for (size_t k = 0; k < rim.opening_points.size(); ++k) {
    World::Opening o;
    o.at = rim.opening_points[k];
    o.dir = k < rim.opening_dirs.size() ? rim.opening_dirs[k] : v3(1, 0, 0);
    o.kind = k < rim.opening_kinds.size() ? rim.opening_kinds[k] : 0;
    o.width = k < rim.opening_widths.size() ? rim.opening_widths[k] : 3.0f;
    world_.openings.push_back(o);
  }
  const World &world = world_;
  const Painter paint{world, in.biome};

  // 3. The quadtree.
  const float leaf = detail == 0 ? 2.0f : (detail == 1 ? 4.0f : 8.0f);
  const int S = detail == 0 ? 64 : 32;
  const float reach = rim.extent + clampf(in.view_distance, 500, 12000);
  float size = leaf * kGrid;
  while (size < 2 * reach) size *= 2;
  Tree tree{index, leaf, {}};
  tree.make(rim.centre.x - size * 0.5f, rim.centre.z - size * 0.5f, size);
  const size_t count = tree.nodes.size();
  out.image_size = static_cast<uint32_t>(S);
  out.images.assign(count * static_cast<size_t>(S * S), 0u);

  const int G = kGrid + 1;   // vertices along a side
  const int B = kGrid + 3;   // with a border, for normals
  std::vector<Influence> inf(static_cast<size_t>(B * B));
  std::vector<Land> lands(static_cast<size_t>(B * B));
  std::vector<float> top(static_cast<size_t>(B * B));
  std::vector<float> fine(static_cast<size_t>((S + 2) * (S + 2)));
  float highest = -1e30f, farthest = 0;
  for (size_t ni = 0; ni < count; ++ni) {
    const Node &node = tree.nodes[ni];
    const float sp = node.size / kGrid;
    // The grid's influences and land, with a border of one.
    for (int j = 0; j < B; ++j)
      for (int i = 0; i < B; ++i) {
        const float x = node.x0 + static_cast<float>(i - 1) * sp, z = node.z0 + static_cast<float>(j - 1) * sp;
        const size_t at = static_cast<size_t>(j * B + i);
        inf[at] = index.at(x, z);
        lands[at] = world.land(x, z, inf[at]);
        top[at] = inf[at].d < kInside ? inf[at].h0 - 4.0f : lands[at].top();
      }
    auto T = [&](int i, int j) { return top[static_cast<size_t>((j + 1) * B + (i + 1))]; };
    auto inside = [&](int i, int j) { return inf[static_cast<size_t>((j + 1) * B + (i + 1))].d < kInside; };
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
        // The texel centres at the node's edges, so neighbours' images meet.
        out.terrain_uvs.push_back((0.5f + static_cast<float>(i) / kGrid * static_cast<float>(S - 1)) / static_cast<float>(S));
        out.terrain_uvs.push_back((0.5f + static_cast<float>(j) / kGrid * static_cast<float>(S - 1)) / static_cast<float>(S));
        // The parent's surface here: its vertices are the even ones; between them, its edges and
        // its quads' diagonals (the same direction as these).
        float target = y;
        if ((i & 1) && (j & 1)) target = 0.5f * (T(i - 1, j - 1) + T(i + 1, j + 1));
        else if (i & 1) target = 0.5f * (T(i - 1, j) + T(i + 1, j));
        else if (j & 1) target = 0.5f * (T(i, j - 1) + T(i, j + 1));
        out.terrain_morph.push_back(target - y);
      }
    const uint32_t first_index = static_cast<uint32_t>(out.terrain_indices.size());
    for (int j = 0; j < kGrid; ++j)
      for (int i = 0; i < kGrid; ++i) {
        const uint32_t v00 = first_vertex + static_cast<uint32_t>(j * G + i), v10 = v00 + 1, v01 = v00 + static_cast<uint32_t>(G),
                       v11 = v01 + 1;
        const bool i00 = inside(i, j), i10 = inside(i + 1, j), i01 = inside(i, j + 1), i11 = inside(i + 1, j + 1);
        if (!(i00 && i01 && i11)) {
          out.terrain_indices.push_back(v00);
          out.terrain_indices.push_back(v01);
          out.terrain_indices.push_back(v11);
        }
        if (!(i00 && i11 && i10)) {
          out.terrain_indices.push_back(v00);
          out.terrain_indices.push_back(v11);
          out.terrain_indices.push_back(v10);
        }
      }
    // Skirts down the four sides, deep enough to hide a coarser neighbour's step.
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
        if (inside(i0, j0) && inside(i1, j1)) continue;
        const uint32_t a = first_vertex + static_cast<uint32_t>(j0 * G + i0), b = first_vertex + static_cast<uint32_t>(j1 * G + i1);
        const uint32_t sa = first_skirt + static_cast<uint32_t>(k), sb = sa + 1;
        out.terrain_indices.push_back(a);
        out.terrain_indices.push_back(b);
        out.terrain_indices.push_back(sb);
        out.terrain_indices.push_back(a);
        out.terrain_indices.push_back(sb);
        out.terrain_indices.push_back(sa);
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
      const float cx = node.x0 + ((c & 1) ? node.size : 0.0f), cz = node.z0 + ((c & 2) ? node.size : 0.0f);
      farthest = maxf(farthest, length(v3(cx, 0, cz)));
    }

    // Its image: the land interpolated from the grid, painted texel by texel, shaded by a fine
    // relief (the grid's surface with a bump finer than it).
    const float texel = node.size / static_cast<float>(S - 1);
    auto sample = [&](float gx, float gz, Influence &f, Land &l) {
      gx = clampf(gx, -1.0f, static_cast<float>(kGrid) + 1.0f);
      gz = clampf(gz, -1.0f, static_cast<float>(kGrid) + 1.0f);
      const int i = mini_(kGrid, maxi_(-1, static_cast<int>(std::floor(gx))));
      const int j = mini_(kGrid, maxi_(-1, static_cast<int>(std::floor(gz))));
      const float fx = gx - static_cast<float>(i), fz = gz - static_cast<float>(j);
      const size_t a = static_cast<size_t>((j + 1) * B + (i + 1)), b = a + 1, c = a + static_cast<size_t>(B), d = c + 1;
      f = mix(mix(inf[a], inf[b], fx), mix(inf[c], inf[d], fx), fz);
      auto lerp_land = [](const Land &p, const Land &q, float t) {
        Land o;
        o.ground = mixf(p.ground, q.ground, t);
        o.canopy = mixf(p.canopy, q.canopy, t);
        o.water = (p.water > -1e8f && q.water > -1e8f) ? mixf(p.water, q.water, t) : (t < 0.5f ? p.water : q.water);
        o.valley = mixf(p.valley, q.valley, t);
        o.woods = mixf(p.woods, q.woods, t);
        o.jungle = mixf(p.jungle, q.jungle, t);
        o.river = mixf(p.river, q.river, t);
        return o;
      };
      l = lerp_land(lerp_land(lands[a], lands[b], fx), lerp_land(lands[c], lands[d], fx), fz);
    };
    const int F = S + 2;
    for (int b = 0; b < F; ++b)
      for (int a = 0; a < F; ++a) {
        const float gx = static_cast<float>(a - 1) / static_cast<float>(S - 1) * kGrid;
        const float gz = static_cast<float>(b - 1) / static_cast<float>(S - 1) * kGrid;
        Influence f;
        Land l;
        sample(gx, gz, f, l);
        const float x = node.x0 + gx * sp, z = node.z0 + gz * sp;
        const float bump = l.water > l.ground ? 0.0f : (fbm(x / 14.0f, z / 14.0f, 2, seed + 149) - 0.5f) * 3.0f;
        fine[static_cast<size_t>(b * F + a)] = maxf(l.ground + l.canopy * 0.6f + bump, l.water);
      }
    uint32_t *img = out.images.data() + ni * static_cast<size_t>(S * S);
    for (int b = 0; b < S; ++b)
      for (int a = 0; a < S; ++a) {
        const float gx = static_cast<float>(a) / static_cast<float>(S - 1) * kGrid;
        const float gz = static_cast<float>(b) / static_cast<float>(S - 1) * kGrid;
        Influence f;
        Land l;
        sample(gx, gz, f, l);
        const float x = node.x0 + gx * sp, z = node.z0 + gz * sp;
        const float dx = fine[static_cast<size_t>((b + 1) * F + a + 2)] - fine[static_cast<size_t>((b + 1) * F + a)];
        const float dz = fine[static_cast<size_t>((b + 2) * F + a + 1)] - fine[static_cast<size_t>(b * F + a + 1)];
        const float gxs = dx / (2 * texel), gzs = dz / (2 * texel);
        const float ny = 1.0f / foundry::det::sqrt(1 + gxs * gxs + gzs * gzs);
        // Alpha marks steep rock (0): a top-down image smears down a cliff, so the shader lays strata
        // there by height instead.
        const float steep = l.water > l.ground ? 0.0f : step_between(0.45f, 0.7f, 1 - ny);
        const uint32_t alpha = static_cast<uint32_t>((1 - steep) * 255.0f + 0.5f);
        img[b * S + a] = (pack_colour(paint.colour(x, z, f, l, ny, texel)) & 0x00ffffffu) | (alpha << 24);
      }
  }

  // 4. The dressing.
  Mesh mesh{out.positions, out.normals, out.colors, out.indices};
  Dresser dress{mesh, index, world, paint, in.biome, detail, seed, {}};
  dress.faces(kRidge, true);
  dress.faces(kLedge, false);
  dress.boulders();
  dress.forests();
  dress.figure_woods();
  dress.slope_pines();
  dress.marsh();
  dress.beach();
  for (const vec3 &p : out.positions) highest = maxf(highest, p.y);
  if (out.indices.empty()) {
    // Nothing stands on the rim (a bare edge): one hidden triangle under the middle, so the
    // dressing's mesh is never empty.
    const vec3 c = rim.centre - v3(0, 50, 0);
    mesh.face(c, c + v3(0, 0, 0.01f), c + v3(0.01f, 0, 0), in.biome.rock);
  }

  out.figures = dress.figures;

  // 5. Fire points, walls and floors, and guides.
  for (const vec3 &p : dress.fires) {
    out.fire_points.push_back(p);
    out.fire_configs.push_back(kBlazeFire);
    out.fire_lit.push_back(0);
  }
  channels::physics_body walls;
  walls.purpose = channels::physics_body::kPurposeSolid;
  walls.name = "boundary";
  const size_t n = index.samples();
  for (size_t k = 0; k < n; k += 3) {
    const vec3 a = index.point(k), b = index.point((k + 3) % n);
    const float wl = index.weight(k, kLedge);
    const vec3 o = normalize(index.outward(k) + index.outward((k + 3) % n));
    const float off = 0.3f + 0.5f * wl;
    vec3 mid = (a + b) * 0.5f + o * off;
    vec3 t = b - a;
    t.y = 0;
    const float len = length(t);
    t = normalize(t);
    channels::physics_shape sh;
    sh.type = channels::physics_shape::kTypeBox;
    mid.y = minf(a.y, b.y) + 18.0f;
    sh.position = mid;
    sh.rotation = v3(0, atan2_(-t.z, t.x), 0);
    sh.half_extents = v3(len * 0.5f + 0.25f, 20.0f, 0.2f);
    walls.shapes.push_back(sh);
  }
  // The hull's cells' floors: hexagonal prisms at each cell's height.
  for (size_t c = 0; c + 1 < rim.cells.size(); c += 2) {
    const int q = rim.cells[c], r = rim.cells[c + 1];
    const float h = c / 2 < rim.cell_heights.size() ? rim.cell_heights[c / 2] : 0.0f;
    channels::physics_shape sh;
    sh.type = channels::physics_shape::kTypeConvexHull;
    sh.position = v3(hex_x(q, r), h, hex_z(r));
    sh.rotation = v3(0, 0, 0);
    for (int k = 0; k < 6; ++k) {
      const float a = kPi / 6 + kPi / 3 * static_cast<float>(k);
      const float x = foundry::det::cos(a), z = foundry::det::sin(a);
      sh.points.push_back(v3(x, 0, z));
      sh.points.push_back(v3(x, -2.0f, z));
    }
    walls.shapes.push_back(sh);
    // Its outline, coloured by its edge.
    const uint32_t e = c / 2 < rim.cell_edges.size() ? rim.cell_edges[c / 2] : 0;
    for (int k = 0; k < 6; ++k) {
      const float a0 = kPi / 6 + kPi / 3 * static_cast<float>(k), a1 = a0 + kPi / 3;
      out.guide_lines.push_back(v3(hex_x(q, r) + foundry::det::cos(a0) * 0.97f, h + 0.05f, hex_z(r) + foundry::det::sin(a0) * 0.97f));
      out.guide_lines.push_back(v3(hex_x(q, r) + foundry::det::cos(a1) * 0.97f, h + 0.05f, hex_z(r) + foundry::det::sin(a1) * 0.97f));
      out.guide_colors.push_back(edge_colour(e));
    }
  }
  out.collision.push_back(walls);

  out.height = highest;
  out.radius = farthest;
  out.rim_cells = static_cast<uint32_t>(rim.cells.size() / 2);
  out.rim_length = rim.length;
  return true;
}

}  // namespace boundary_terrain

FOUNDRY_SYSTEM(boundary_terrain, channels::boundary_terrain_spec, channels::boundary_model)
