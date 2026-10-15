// terrain_mesh: a terrain's ground drawn by the hexbound rules (design/import.hexbound.world.md,
// §3 and §4; design/terrain.md, "Building it").
//
// 1. Surfaces: each span's top (air above) and bottom (air below, above another span).
// 2. Pairing: across each edge a surface pairs with its mutual nearest of the same kind in the
//    neighbour, and fuses when their difference is within the lower of their materials' smoothness.
// 3. Corners: the surfaces fused round a corner (over its three cells) form a group, joined in order
//    of their difference with any link that would join two surfaces of one cell dropped; every member
//    takes the group's mean, so there are no cracks.
// 4. Fans: a centre at the surface's height, six corners, and six midpoints (fused: a quarter of
//    each side's height plus a quarter of each corner; unfused: half its own plus a quarter of each
//    corner), welded across fused vertices so fused ground shades smoothly.
// 5. Walls: along each half-edge, the cell's solid minus its neighbour's, facing the neighbour, in
//    rock for rock and cobble and earth for the rest.
// 6. Water at its levels; collision as convex columns; guides; bridge mounts; and the hull, nature
//    ground, and ground passed on.
#include <map>
#include <string>
#include <vector>

#include "boundary.h"
#include "channels/terrain_mesh_spec.h"
#include "channels/terrain_model.h"
#include "math.h"
#include "packer/terrain_mesh_spec.h"
#include "packer/terrain_model.h"

namespace terrain_mesh {

using namespace bmath;
using channels::boundary_biome;
using channels::physics_body;
using channels::physics_shape;
using channels::terrain_ground;
using channels::terrain_mesh_spec;
using channels::terrain_model;

namespace {

constexpr float kUnit = 0.125f;
constexpr int kDq[6] = {1, 1, 0, -1, -1, 0}, kDr[6] = {0, -1, -1, 0, 1, 1};
// Corner j of a cell, on the lattice of (sqrt 3 / 2, 1/2) steps: x = 2q + r + kCx[j], z = 3r + kCz[j].
constexpr int kCx[6] = {1, 0, -1, -1, 0, 1}, kCz[6] = {-1, -2, -1, 1, 2, 1};
enum Material : uint32_t { kGrass = 0, kDirt = 1, kSand = 2, kSnow = 3, kCobble = 4, kRock = 5 };
const int kSmoothUnits[6] = {12, 12, 12, 10, 8, 4};  // hexbound §4 (1.5, 1.5, 1.5, 1.25, 1.0, 0.6 m; rock's 0.6 rounds down)
enum Flag : uint32_t { kRoad = 2, kLane = 4, kRiver = 8, kLake = 16, kBridge = 32, kFlat = 64, kOpening = 128, kApron = 512 };

uint32_t pack_colour(vec3 c, float a = 1) {
  auto byte = [](float v) { return static_cast<uint32_t>(clampf(v, 0, 1) * 255.0f + 0.5f); };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (byte(a) << 24);
}

struct Surface {
  int cell, span;
  bool top;
  int y;  // units
  uint32_t material;
};

}  // namespace

bool run(const terrain_mesh_spec &in, terrain_model &out, std::string &error) {
  const terrain_ground &gr = in.ground;
  const boundary_biome &bio = in.biome;
  out = terrain_model{};
  const size_t C = gr.cells.size() / 2;
  if (C == 0 || gr.span_first.size() != C + 1) {
    error = "the ground has no cells";
    return false;
  }
  // The cells' grid.
  int q0 = 1 << 30, r0 = 1 << 30, q1 = -(1 << 30), r1 = -(1 << 30);
  for (size_t i = 0; i < C; ++i) {
    q0 = mini_(q0, gr.cells[2 * i]);
    q1 = maxi_(q1, gr.cells[2 * i]);
    r0 = mini_(r0, gr.cells[2 * i + 1]);
    r1 = maxi_(r1, gr.cells[2 * i + 1]);
  }
  const int W = q1 - q0 + 1, Hh = r1 - r0 + 1;
  std::vector<int> grid(static_cast<size_t>(W) * static_cast<size_t>(Hh), -1);
  for (size_t i = 0; i < C; ++i) grid[static_cast<size_t>((gr.cells[2 * i + 1] - r0) * W + (gr.cells[2 * i] - q0))] = static_cast<int>(i);
  auto cell_at = [&](int q, int r) {
    if (q < q0 || q > q1 || r < r0 || r > r1) return -1;
    return grid[static_cast<size_t>((r - r0) * W + (q - q0))];
  };
  auto neighbour = [&](int c, int d) { return cell_at(gr.cells[2 * static_cast<size_t>(c)] + kDq[d], gr.cells[2 * static_cast<size_t>(c) + 1] + kDr[d]); };
  auto centre = [&](int c) {
    const int q = gr.cells[2 * static_cast<size_t>(c)], r = gr.cells[2 * static_cast<size_t>(c) + 1];
    return v3(hex_x(q, r), 0, hex_z(r));
  };
  auto corner_xz = [&](int c, int j) {
    const int q = gr.cells[2 * static_cast<size_t>(c)], r = gr.cells[2 * static_cast<size_t>(c) + 1];
    return v3(static_cast<float>(2 * q + r + kCx[j]) * (kSqrt3 * 0.5f), 0, static_cast<float>(3 * r + kCz[j]) * 0.5f);
  };

  // 1. Surfaces.
  std::vector<Surface> surf;
  std::vector<std::vector<int>> tops(C), bottoms(C);
  int floor_u = 1 << 30;
  for (size_t c = 0; c < C; ++c) {
    const uint32_t a = gr.span_first[c], b = gr.span_first[c + 1];
    for (uint32_t k = a; k < b; ++k) {
      floor_u = mini_(floor_u, gr.span_lo[k]);
      if (k + 1 == b || gr.span_lo[k + 1] > gr.span_hi[k]) {
        tops[c].push_back(static_cast<int>(surf.size()));
        surf.push_back({static_cast<int>(c), static_cast<int>(k), true, gr.span_hi[k], gr.span_material[k] % 6});
      }
      if (k > a && gr.span_hi[k - 1] < gr.span_lo[k]) {
        bottoms[c].push_back(static_cast<int>(surf.size()));
        surf.push_back({static_cast<int>(c), static_cast<int>(k), false, gr.span_lo[k], gr.span_material[k] % 6});
      }
    }
  }
  const size_t S = surf.size();
  auto smooth_of = [&](int a, int b) { return mini_(kSmoothUnits[surf[static_cast<size_t>(a)].material], kSmoothUnits[surf[static_cast<size_t>(b)].material]); };

  // 2. Pairing across each edge: partner[s * 6 + d], and whether it fused.
  std::vector<int> partner(S * 6, -1);
  std::vector<uint8_t> fused(S * 6, 0);
  auto nearest = [&](int s, const std::vector<int> &list) {
    int best = -1, bd = 1 << 30;
    for (int o : list) {
      const int d = absi_(surf[static_cast<size_t>(o)].y - surf[static_cast<size_t>(s)].y);
      if (d < bd) {
        bd = d;
        best = o;
      }
    }
    return best;
  };
  for (size_t c = 0; c < C; ++c)
    for (int d = 0; d < 6; ++d) {
      const int n = neighbour(static_cast<int>(c), d);
      if (n < 0) continue;
      for (int kind = 0; kind < 2; ++kind) {
        const std::vector<int> &mine = kind ? bottoms[c] : tops[c], &theirs = kind ? bottoms[static_cast<size_t>(n)] : tops[static_cast<size_t>(n)];
        for (int s : mine) {
          const int o = nearest(s, theirs);
          if (o < 0 || nearest(o, mine) != s) continue;
          partner[static_cast<size_t>(s) * 6 + static_cast<size_t>(d)] = o;
          fused[static_cast<size_t>(s) * 6 + static_cast<size_t>(d)] = absi_(surf[static_cast<size_t>(s)].y - surf[static_cast<size_t>(o)].y) <= smooth_of(s, o);
        }
      }
    }

  // 3. Corners: each surface's value at each of its corners, and its group's root (for welding).
  std::vector<float> corner_y(S * 6, 0.0f);
  std::vector<int> corner_root(S * 6, -1);
  std::vector<vec3> corner_colour(S * 6);
  // Winter lays snow over the grass; fall dries it.
  const vec3 grass = in.season == 0 ? lerp(bio.grass_dry, bio.snow, 0.75f) : (in.season == 3 ? lerp(bio.grass, bio.grass_dry, 0.55f) : bio.grass);
  auto material_colour = [&](uint32_t m, int y_units) {
    vec3 c;
    switch (m) {
      case kDirt: c = bio.soil; break;
      case kSand: c = bio.sand; break;
      case kSnow: c = bio.snow; break;
      case kCobble: c = lerp(bio.rock, bio.soil, 0.3f) * 1.1f; break;
      case kRock: c = bio.rock; break;
      default: c = grass;
    }
    // Snow above the biome's snow line, over open ground.
    if (bio.snow_line > 0 && m != kCobble && static_cast<float>(y_units) * kUnit > bio.snow_line) c = lerp(c, bio.snow, 0.85f);
    return c;
  };
  {
    std::vector<int> parent(S);
    for (size_t i = 0; i < S; ++i) parent[i] = static_cast<int>(i);
    auto find = [&](int x) {
      while (parent[static_cast<size_t>(x)] != x) x = parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
      return x;
    };
    struct Link {
      int dy, a, b;
    };
    for (size_t c = 0; c < C; ++c)
      for (int j = 0; j < 6; ++j) {
        const int cells[3] = {static_cast<int>(c), neighbour(static_cast<int>(c), j), neighbour(static_cast<int>(c), (j + 1) % 6)};
        // The directions between them: c to n_j is j, c to n_j+1 is j+1, n_j to n_j+1 is j+2.
        const int dirs[3][3] = {{-1, j, (j + 1) % 6}, {-1, -1, (j + 2) % 6}, {-1, -1, -1}};
        for (int kind = 0; kind < 2; ++kind) {
          std::vector<int> members;
          for (int k = 0; k < 3; ++k)
            if (cells[k] >= 0)
              for (int s : kind ? bottoms[static_cast<size_t>(cells[k])] : tops[static_cast<size_t>(cells[k])]) members.push_back(s);
          if (members.empty()) continue;
          std::vector<Link> links;
          for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b) {
              if (cells[a] < 0 || cells[b] < 0) continue;
              const int d = dirs[a][b];
              for (int s : kind ? bottoms[static_cast<size_t>(cells[a])] : tops[static_cast<size_t>(cells[a])]) {
                const int o = partner[static_cast<size_t>(s) * 6 + static_cast<size_t>(d)];
                if (o >= 0 && fused[static_cast<size_t>(s) * 6 + static_cast<size_t>(d)])
                  links.push_back({absi_(surf[static_cast<size_t>(s)].y - surf[static_cast<size_t>(o)].y), mini_(s, o), maxi_(s, o)});
              }
            }
          // Smallest difference first; a link joining two surfaces of one cell is dropped.
          for (size_t i = 1; i < links.size(); ++i)
            for (size_t k = i; k > 0; --k) {
              const Link &x = links[k], &y = links[k - 1];
              const bool before = x.dy < y.dy || (x.dy == y.dy && (x.a < y.a || (x.a == y.a && x.b < y.b)));
              if (!before) break;
              const Link t = links[k];
              links[k] = links[k - 1];
              links[k - 1] = t;
            }
          for (int s : members) parent[static_cast<size_t>(s)] = s;
          for (const Link &l : links) {
            const int ra = find(l.a), rb = find(l.b);
            if (ra == rb) continue;
            bool clash = false;
            for (int x : members)
              if (find(x) == ra)
                for (int y : members)
                  if (find(y) == rb && surf[static_cast<size_t>(x)].cell == surf[static_cast<size_t>(y)].cell) clash = true;
            if (clash) continue;
            if (ra < rb) parent[static_cast<size_t>(rb)] = ra;
            else parent[static_cast<size_t>(ra)] = rb;
          }
          // This cell's surfaces take their group's mean (summed in units, in member order).
          for (int s : kind ? bottoms[c] : tops[c]) {
            const int root = find(s);
            long sum = 0;
            int count = 0;
            vec3 col = v3(0, 0, 0);
            int lowest = 1 << 30;
            for (int x : members)
              if (find(x) == root) {
                sum += surf[static_cast<size_t>(x)].y;
                ++count;
                col = col + material_colour(surf[static_cast<size_t>(x)].material, surf[static_cast<size_t>(x)].y);
                lowest = mini_(lowest, x);
              }
            corner_y[static_cast<size_t>(s) * 6 + static_cast<size_t>(j)] = static_cast<float>(sum) * kUnit / static_cast<float>(count);
            corner_root[static_cast<size_t>(s) * 6 + static_cast<size_t>(j)] = lowest;
            corner_colour[static_cast<size_t>(s) * 6 + static_cast<size_t>(j)] = col * (1.0f / static_cast<float>(count));
          }
          for (int s : members) parent[static_cast<size_t>(s)] = s;
        }
      }
  }
  // The midpoint of edge d (between corners d - 1 and d) for a surface.
  auto mid_y = [&](int s, int d) {
    const size_t b = static_cast<size_t>(s) * 6;
    const float c1 = corner_y[b + static_cast<size_t>((d + 5) % 6)], c2 = corner_y[b + static_cast<size_t>(d)];
    const float self = static_cast<float>(surf[static_cast<size_t>(s)].y) * kUnit;
    const int o = partner[b + static_cast<size_t>(d)];
    if (o >= 0 && fused[b + static_cast<size_t>(d)]) return 0.25f * (self + static_cast<float>(surf[static_cast<size_t>(o)].y) * kUnit) + 0.25f * (c1 + c2);
    return 0.5f * self + 0.25f * (c1 + c2);
  };

  // 4. Fans, welded.
  std::map<uint64_t, uint32_t> welded;
  std::vector<vec3> nsum;
  auto vertex = [&](vec3 p, vec3 col) {
    out.positions.push_back(p);
    nsum.push_back(v3(0, 0, 0));
    out.colors.push_back(pack_colour(col));
    return static_cast<uint32_t>(out.positions.size() - 1);
  };
  auto shared = [&](uint64_t key, vec3 p, vec3 col) {
    const auto it = welded.find(key);
    if (it != welded.end()) return it->second;
    const uint32_t v = vertex(p, col);
    welded[key] = v;
    return v;
  };
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
    const vec3 n = cross(out.positions[b] - out.positions[a], out.positions[c] - out.positions[a]);
    nsum[a] = nsum[a] + n;
    nsum[b] = nsum[b] + n;
    nsum[c] = nsum[c] + n;
  };
  auto apron = [&](int c) { return (gr.flags[static_cast<size_t>(c)] & kApron) != 0; };
  for (size_t s = 0; s < S; ++s) {
    const Surface &sf = surf[s];
    const int c = sf.cell;
    if (apron(c)) continue;  // a neighbouring chunk's: read for its corners and walls, drawn there
    const vec3 mid = centre(c);
    const vec3 own = material_colour(sf.material, sf.y);
    // A little variety cell to cell, so open ground doesn't read as one sheet.
    const float jitter = 0.94f + 0.12f * unit(hash2(gr.cells[2 * static_cast<size_t>(c)], gr.cells[2 * static_cast<size_t>(c) + 1], 77));
    const uint32_t centre_v = vertex(v3(mid.x, static_cast<float>(sf.y) * kUnit, mid.z), own * jitter);
    uint32_t corner_v[6], mid_v[6];
    for (int j = 0; j < 6; ++j) {
      const vec3 xz = corner_xz(c, j);
      const size_t k = s * 6 + static_cast<size_t>(j);
      const uint64_t X = static_cast<uint64_t>(2 * gr.cells[2 * static_cast<size_t>(c)] + gr.cells[2 * static_cast<size_t>(c) + 1] + kCx[j] + 4096);
      const uint64_t Z = static_cast<uint64_t>(3 * gr.cells[2 * static_cast<size_t>(c) + 1] + kCz[j] + 8192);
      const uint64_t key = (1ull << 62) | (static_cast<uint64_t>(sf.top) << 61) | (X << 47) | (Z << 32) | static_cast<uint64_t>(corner_root[k]);
      corner_v[j] = shared(key, v3(xz.x, corner_y[k], xz.z), corner_colour[k]);
    }
    for (int d = 0; d < 6; ++d) {
      const vec3 a = corner_xz(c, (d + 5) % 6), b = corner_xz(c, d);
      const vec3 p = v3((a.x + b.x) * 0.5f, mid_y(static_cast<int>(s), d), (a.z + b.z) * 0.5f);
      const int o = partner[s * 6 + static_cast<size_t>(d)];
      if (o >= 0 && fused[s * 6 + static_cast<size_t>(d)]) {
        const uint64_t lo = static_cast<uint64_t>(mini_(static_cast<int>(s), o)), hi = static_cast<uint64_t>(maxi_(static_cast<int>(s), o));
        const vec3 col = (own + material_colour(surf[static_cast<size_t>(o)].material, surf[static_cast<size_t>(o)].y)) * 0.5f;
        mid_v[d] = shared((2ull << 62) | (lo << 31) | hi, p, col);
      } else {
        mid_v[d] = vertex(p, own);
      }
    }
    for (int d = 0; d < 6; ++d) {
      const uint32_t c0 = corner_v[(d + 5) % 6], m = mid_v[d], c1 = corner_v[d];
      if (sf.top) {
        tri(centre_v, c0, m);
        tri(centre_v, m, c1);
      } else {
        tri(centre_v, m, c0);
        tri(centre_v, c1, m);
      }
    }
  }

  // 5. Walls: A's solid minus B's along each half-edge, facing B.
  // A surface's height at a point of an edge: t 0 at corner d - 1, 1 at the midpoint, 2 at corner d.
  auto profile = [&](int s, int d, int point) {
    if (point == 0) return corner_y[static_cast<size_t>(s) * 6 + static_cast<size_t>((d + 5) % 6)];
    if (point == 2) return corner_y[static_cast<size_t>(s) * 6 + static_cast<size_t>(d)];
    return mid_y(s, d);
  };
  const float floor_m = static_cast<float>(floor_u) * kUnit;
  // A cell's solid along an edge at a point: its spans' (bottom, top) profiles.
  auto solids = [&](int c, int d, int point, std::vector<std::pair<float, float>> &outv) {
    outv.clear();
    if (c < 0) return;
    const uint32_t a = gr.span_first[static_cast<size_t>(c)], b = gr.span_first[static_cast<size_t>(c) + 1];
    for (uint32_t k = a; k < b; ++k) {
      float lo = floor_m, hi = static_cast<float>(gr.span_hi[k]) * kUnit;
      for (int s : bottoms[static_cast<size_t>(c)])
        if (surf[static_cast<size_t>(s)].span == static_cast<int>(k)) lo = profile(s, d, point);
      for (int s : tops[static_cast<size_t>(c)])
        if (surf[static_cast<size_t>(s)].span == static_cast<int>(k)) hi = profile(s, d, point);
      outv.push_back({lo, hi});
    }
  };
  std::vector<std::pair<float, float>> a0, a1, b0, b1;
  for (size_t c = 0; c < C; ++c) {
    if (apron(static_cast<int>(c))) continue;
    for (int d = 0; d < 6; ++d) {
      const int n = neighbour(static_cast<int>(c), d);
      const int nd = (d + 3) % 6;  // the same edge from the neighbour; its points run the other way
      const float ang = static_cast<float>(d) * (kPi / 3);
      const vec3 facing = v3(foundry::det::cos(ang), 0, -foundry::det::sin(ang));
      const vec3 p_at[3] = {corner_xz(static_cast<int>(c), (d + 5) % 6),
                            (corner_xz(static_cast<int>(c), (d + 5) % 6) + corner_xz(static_cast<int>(c), d)) * 0.5f,
                            corner_xz(static_cast<int>(c), d)};
      const uint32_t mat = gr.span_material[gr.span_first[c + 1] - 1] % 6;
      const bool rocky = mat == kRock || mat == kCobble;
      for (int half = 0; half < 2; ++half) {
        solids(static_cast<int>(c), d, half, a0);
        solids(static_cast<int>(c), d, half + 1, a1);
        solids(n, nd, 2 - half, b0);
        solids(n, nd, 1 - half, b1);
        // A's solid minus B's at both ends, matched span by span (the common case: one span each).
        for (size_t k = 0; k < a0.size(); ++k) {
          // Pieces of [lo, hi] uncovered by B at each end; for one B span this is at most two pieces.
          auto pieces = [](const std::pair<float, float> &a, const std::vector<std::pair<float, float>> &bs) {
            std::vector<std::pair<float, float>> left = {a};
            for (const auto &b : bs) {
              std::vector<std::pair<float, float>> next;
              for (const auto &p : left) {
                if (b.second <= p.first || b.first >= p.second) {
                  next.push_back(p);
                  continue;
                }
                if (b.first > p.first) next.push_back({p.first, b.first});
                if (b.second < p.second) next.push_back({b.second, p.second});
              }
              left.swap(next);
            }
            return left;
          };
          const auto e0 = pieces(a0[k], b0), e1 = pieces(a1[k], b1);
          // Pair the pieces end to end: the same count pairs in order; otherwise each end's piece
          // tapers to a point at the other end's matching boundary.
          const size_t np = e0.size() > e1.size() ? e0.size() : e1.size();
          for (size_t p = 0; p < np; ++p) {
            std::pair<float, float> x0, x1;
            if (p < e0.size()) x0 = e0[p];
            if (p < e1.size()) x1 = e1[p];
            if (p >= e0.size()) x0 = {x1.first, x1.first};
            if (p >= e1.size()) x1 = {x0.first, x0.first};
            if (x0.second - x0.first < 0.01f && x1.second - x1.first < 0.01f) continue;
            const vec3 P0 = p_at[half], P1 = p_at[half + 1];
            auto wall_colour = [&](float y) {
              const vec3 base = rocky ? bio.rock : lerp(bio.soil, bio.rock, 0.2f) * 0.85f;
              const float band = y / 2.2f - std::floor(y / 2.2f);
              return base * (band < 0.3f ? 0.8f : 1.0f);
            };
            const uint32_t v0 = vertex(v3(P0.x, x0.first, P0.z), wall_colour(x0.first));
            const uint32_t v1 = vertex(v3(P1.x, x1.first, P1.z), wall_colour(x1.first));
            const uint32_t v2 = vertex(v3(P1.x, x1.second, P1.z), wall_colour(x1.second));
            const uint32_t v3_ = vertex(v3(P0.x, x0.second, P0.z), wall_colour(x0.second));
            // Facing the neighbour: CCW seen from it.
            out.indices.insert(out.indices.end(), {v0, v1, v2, v0, v2, v3_});
            for (uint32_t v : {v0, v1, v2, v3_}) nsum[v] = facing;
          }
        }
      }
    }
  }
  out.normals.resize(out.positions.size());
  for (size_t i = 0; i < out.positions.size(); ++i) out.normals[i] = length(nsum[i]) > 1e-9f ? normalize(nsum[i]) : v3(0, 1, 0);

  // 6. Water: a fan at each wet cell, its corners the mean level of the wet cells round them and its
  // midpoints the mean of the two either side, so a river's surface runs down without steps.
  auto level_of = [&](int c) { return static_cast<float>(gr.water[static_cast<size_t>(c)]) * kUnit; };
  auto wet = [&](int c) { return c >= 0 && gr.water[static_cast<size_t>(c)] != -32768; };
  for (size_t c = 0; c < C; ++c) {
    if (!wet(static_cast<int>(c)) || apron(static_cast<int>(c))) continue;
    const float level = level_of(static_cast<int>(c));
    const float bed = static_cast<float>(gr.span_hi[gr.span_first[c + 1] - 1]) * kUnit;
    const vec3 col = lerp(bio.water_shallow, bio.water_deep, clampf((level - bed) / 3.0f, 0, 1));
    const vec3 m = centre(static_cast<int>(c));
    const uint32_t cv = static_cast<uint32_t>(out.positions.size());
    auto add = [&](vec3 p) {
      out.positions.push_back(p);
      out.normals.push_back(v3(0, 1, 0));
      out.colors.push_back(pack_colour(col));
    };
    add(v3(m.x, level, m.z));
    for (int j = 0; j < 6; ++j) {  // corners
      const int a = neighbour(static_cast<int>(c), j), b = neighbour(static_cast<int>(c), (j + 1) % 6);
      float sum = level;
      int n = 1;
      if (wet(a)) {
        sum += level_of(a);
        ++n;
      }
      if (wet(b)) {
        sum += level_of(b);
        ++n;
      }
      const vec3 p = corner_xz(static_cast<int>(c), j);
      add(v3(p.x, sum / static_cast<float>(n), p.z));
    }
    for (int d = 0; d < 6; ++d) {  // midpoints
      const int n = neighbour(static_cast<int>(c), d);
      const vec3 a = corner_xz(static_cast<int>(c), (d + 5) % 6), b = corner_xz(static_cast<int>(c), d);
      add(v3((a.x + b.x) * 0.5f, wet(n) ? 0.5f * (level + level_of(n)) : level, (a.z + b.z) * 0.5f));
    }
    for (uint32_t d = 0; d < 6; ++d) {
      const uint32_t c0 = cv + 1 + (d + 5) % 6, mid = cv + 7 + d, c1 = cv + 1 + d;
      out.indices.insert(out.indices.end(), {cv, c0, mid, cv, mid, c1});
    }
  }

  // Collision: a convex column for each span, its top fan over its bottom fan (or down past its
  // lowest neighbour), so a walker never slips under a cliff.
  {
    physics_body ground;
    ground.purpose = physics_body::kPurposeSolid;
    ground.name = "ground";
    for (size_t c = 0; c < C; ++c) {
      if (apron(static_cast<int>(c))) continue;
      int lowest_near = 1 << 30;
      for (int d = 0; d < 6; ++d) {
        const int n = neighbour(static_cast<int>(c), d);
        if (n >= 0) lowest_near = mini_(lowest_near, gr.span_hi[gr.span_first[static_cast<size_t>(n)]]);
      }
      const uint32_t a = gr.span_first[c], b = gr.span_first[c + 1];
      for (uint32_t k = a; k < b; ++k) {
        physics_shape sh;
        sh.type = physics_shape::kTypeConvexHull;
        int top_s = -1, bottom_s = -1;
        for (int s : tops[c])
          if (surf[static_cast<size_t>(s)].span == static_cast<int>(k)) top_s = s;
        for (int s : bottoms[c])
          if (surf[static_cast<size_t>(s)].span == static_cast<int>(k)) bottom_s = s;
        const vec3 m = centre(static_cast<int>(c));
        float low = 1e30f;
        auto fan = [&](int s) {
          sh.points.push_back(v3(m.x, static_cast<float>(surf[static_cast<size_t>(s)].y) * kUnit, m.z));
          for (int j = 0; j < 6; ++j) {
            const vec3 p = corner_xz(static_cast<int>(c), j);
            sh.points.push_back(v3(p.x, corner_y[static_cast<size_t>(s) * 6 + static_cast<size_t>(j)], p.z));
            low = minf(low, corner_y[static_cast<size_t>(s) * 6 + static_cast<size_t>(j)]);
          }
        };
        if (top_s >= 0) fan(top_s);
        if (bottom_s >= 0) {
          fan(bottom_s);
        } else {
          float base = minf(low, static_cast<float>(lowest_near) * kUnit) - 1.0f;
          if (k > a) base = static_cast<float>(gr.span_lo[k]) * kUnit;
          for (int j = 0; j < 6; ++j) {
            const vec3 p = corner_xz(static_cast<int>(c), j);
            sh.points.push_back(v3(p.x, base, p.z));
          }
        }
        ground.shapes.push_back(sh);
      }
    }
    out.collision.push_back(ground);
  }

  // Guides: the sides by edge, openings, flats, roads, rivers, and bridge mounts.
  static const vec3 kEdgeColour[8] = {v3(0.7f, 0.7f, 0.7f), v3(0.95f, 0.85f, 0.3f), v3(0.6f, 0.45f, 0.35f), v3(0.55f, 0.55f, 0.6f),
                                      v3(0.2f, 0.65f, 0.25f), v3(0.1f, 0.5f, 0.2f),   v3(0.2f, 0.5f, 0.95f),  v3(0.45f, 0.6f, 0.45f)};
  auto guide = [&](vec3 a, vec3 b, vec3 col) {
    out.guide_lines.push_back(a);
    out.guide_lines.push_back(b);
    out.guide_colors.push_back(pack_colour(col));
  };
  for (size_t i = 0; i + 1 < gr.side_lines.size(); i += 2) {
    const uint32_t side = i / 2 < gr.side_of_line.size() ? gr.side_of_line[i / 2] : 0;
    const uint32_t edge = side < gr.side_edges.size() ? gr.side_edges[side] % 8 : 0;
    guide(gr.side_lines[i], gr.side_lines[i + 1], kEdgeColour[edge]);
  }
  auto top_of = [&](int c) { return static_cast<float>(gr.span_hi[gr.span_first[static_cast<size_t>(c) + 1] - 1]) * kUnit; };
  auto hex_outline = [&](int c, float lift, vec3 col) {
    for (int j = 0; j < 6; ++j) {
      const vec3 a = corner_xz(c, j), b = corner_xz(c, (j + 1) % 6);
      guide(v3(a.x, top_of(c) + lift, a.z), v3(b.x, top_of(c) + lift, b.z), col);
    }
  };
  for (size_t i = 0; i + 1 < gr.opening_cells.size(); i += 2) {
    const int c = cell_at(gr.opening_cells[i], gr.opening_cells[i + 1]);
    if (c < 0) continue;
    const vec3 m = centre(c);
    const bool water = gr.flags[static_cast<size_t>(c)] & kRiver;
    const vec3 col = water ? v3(0.2f, 0.6f, 1) : v3(1, 0.55f, 0.1f);
    guide(v3(m.x, top_of(c), m.z), v3(m.x, top_of(c) + 6, m.z), col);
    hex_outline(c, 0.15f, col);
  }
  for (size_t c = 0; c < C; ++c) {
    if (!(gr.flags[c] & kFlat)) continue;
    for (int d = 0; d < 6; ++d) {
      const int n = neighbour(static_cast<int>(c), d);
      if (n >= 0 && (gr.flags[static_cast<size_t>(n)] & kFlat)) continue;
      const vec3 a = corner_xz(static_cast<int>(c), (d + 5) % 6), b = corner_xz(static_cast<int>(c), d);
      guide(v3(a.x, top_of(static_cast<int>(c)) + 0.2f, a.z), v3(b.x, top_of(static_cast<int>(c)) + 0.2f, b.z), v3(0.95f, 0.3f, 0.85f));
    }
  }
  auto path_guide = [&](const std::vector<int32_t> &cells, const std::vector<uint32_t> &counts, vec3 col, bool on_water) {
    size_t at = 0;
    for (uint32_t n : counts) {
      for (size_t k = 1; k < n; ++k) {
        const int a = cell_at(cells[2 * (at + k - 1)], cells[2 * (at + k - 1) + 1]), b = cell_at(cells[2 * (at + k)], cells[2 * (at + k) + 1]);
        if (a < 0 || b < 0) continue;
        auto y = [&](int c) {
          return on_water && gr.water[static_cast<size_t>(c)] != -32768 ? static_cast<float>(gr.water[static_cast<size_t>(c)]) * kUnit : top_of(c);
        };
        guide(centre(a) + v3(0, y(a) + 0.35f, 0), centre(b) + v3(0, y(b) + 0.35f, 0), col);
      }
      at += n;
    }
  };
  path_guide(gr.road_cells, gr.road_counts, v3(1, 0.8f, 0.2f), false);
  path_guide(gr.river_cells, gr.river_counts, v3(0.3f, 0.75f, 1), true);
  for (const auto &b : gr.bridges) {
    for (const vec3 &m : {b.a, b.b}) {
      guide(m, m + v3(0, 3, 0), v3(1, 0.2f, 0.2f));
      guide(m + v3(-0.8f, 0.05f, 0), m + v3(0.8f, 0.05f, 0), v3(1, 0.2f, 0.2f));
      guide(m + v3(0, 0.05f, -0.8f), m + v3(0, 0.05f, 0.8f), v3(1, 0.2f, 0.2f));
    }
    guide(b.a + v3(0, 0.3f, 0), b.b + v3(0, 0.3f, 0), v3(1, 0.2f, 0.2f));
    // The mounts as places: facing across the water from each bank.
    const vec3 ab = b.b - b.a;
    const float yaw = atan2_(ab.x, -ab.z) * (180.0f / kPi);
    out.mount_positions.push_back(b.a);
    out.mount_yaws.push_back(yaw);
    out.mount_widths.push_back(b.width);
    out.mount_positions.push_back(b.b);
    out.mount_yaws.push_back(yaw + 180.0f);
    out.mount_widths.push_back(b.width);
  }

  // Facts, and what passes on.
  float top = -1e30f, reach = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::sqrt(p.x * p.x + p.z * p.z));
  }
  out.height = top;
  out.radius = reach;
  out.cell_count = static_cast<uint32_t>(C);
  out.hull = gr.hull;
  out.nature = gr.nature;
  out.ground = gr;
  return true;
}

}  // namespace terrain_mesh

FOUNDRY_SYSTEM(terrain_mesh, channels::terrain_mesh_spec, channels::terrain_model)
