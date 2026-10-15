// map_ground: one chunk of a map's ground (design/map.md, "Executing it"), as hexbound columns of
// spans (design/import.hexbound.world.md) for its cells and an apron of one cell round them.
//
// Every height is a function of a cell's place in the world and the plan's data alone, so two
// chunks agree exactly where they meet:
// 1. The macro height (the plan's 8 m grid, interpolated) and fine relief by the parcel's character,
//    fading near roads, flats, and water.
// 2. Flats level at their sites' heights; roads and lanes along their segments at the road's own
//    height, their shoulders easing to them; rivers' beds under their water, their banks climbing.
// 3. Lakes and marshes: water where the ground lies under the plan's level.
// 4. Rock: outcrops break into faces by the parcel's cliffs; a tall face may open into a cave mouth.
// 5. Materials, the columns, the navigation mesh over the chunk's own cells, and the nature ground
//    grouped by parcel (each parcel's plotter grows its own). A settlement's streets take their
//    surface's material (the ground draws them, not its road pieces); nothing grows on what it has
//    built, and its open ground (and any site's flat) grows grass but nothing planted.
#include <string>
#include <vector>

#include "boundary.h"
#include "channels/map_chunk.h"
#include "channels/terrain_ground.h"
#include "math.h"
#include "packer/map_chunk.h"
#include "packer/terrain_ground.h"

namespace map_ground {

using namespace bmath;
using channels::map_chunk;
using channels::map_parcel;
using channels::map_segment;
using channels::plot_region;
using channels::terrain_ground;

namespace {

constexpr float kUnit = 0.125f;
constexpr float kRun = kSqrt3;
constexpr float kDry = -1000.0f;
constexpr int kMargin = 3;  // cells evaluated round the chunk: the apron, and what its rock and caves read
constexpr int kDq[6] = {1, 1, 0, -1, -1, 0}, kDr[6] = {0, -1, -1, 0, 1, 1};
enum Material : uint32_t { kGrass = 0, kDirt = 1, kSand = 2, kSnow = 3, kCobble = 4, kRock = 5 };
const float kSmooth[6] = {1.5f, 1.5f, 1.5f, 1.25f, 1.0f, 0.6f};
enum Flag : uint32_t {
  kRoad = 2, kLane = 4, kRiver = 8, kLake = 16, kBridge = 32, kFlat = 64, kCave = 256, kApron = 512, kBuilt = 1024,
  kTerrace = 1u << 16,
};
enum Kind : uint32_t { kPlains, kHills, kHighlands, kMountains, kValley, kLakeKind, kMarsh, kCoast };

int to_units(float m) { return round_to_int(m / kUnit); }

void to_axial(float x, float z, int &q, int &r) {  // as shape.h's
  const float fr = z / 1.5f, fq = x / kSqrt3 - fr * 0.5f, fs = -fq - fr;
  int rq = round_to_int(fq), rr = round_to_int(fr);
  const int rs = round_to_int(fs);
  const float eq = absf(static_cast<float>(rq) - fq), er = absf(static_cast<float>(rr) - fr), es = absf(static_cast<float>(rs) - fs);
  if (eq > er && eq > es) rq = -rr - rs;
  else if (er > es) rr = -rq - rs;
  q = rq;
  r = rr;
}

float to_segment(float px, float pz, vec3 a, vec3 b, float &t) {
  const float abx = b.x - a.x, abz = b.z - a.z;
  const float l2 = abx * abx + abz * abz;
  t = l2 > 1e-9f ? clampf(((px - a.x) * abx + (pz - a.z) * abz) / l2, 0, 1) : 0.0f;
  const float dx = a.x + abx * t - px, dz = a.z + abz * t - pz;
  return foundry::det::sqrt(dx * dx + dz * dz);
}

struct Cell {
  float h = 0;        // the top, metres
  float water = kDry;
  uint32_t flags = 0;
  uint32_t parcel = 65535;
  float moist = 0.5f;
  float cliffs = 0, caves = 0;
  bool inside = false;
};

}  // namespace

bool run(const map_chunk &in, terrain_ground &out, std::string &error) {
  out = terrain_ground{};
  const int S = static_cast<int>(in.size ? in.size : 64u);
  const int nx = static_cast<int>(in.nx), nz = static_cast<int>(in.nz);
  if (nx < 2 || nz < 2 || in.heights.size() != static_cast<size_t>(nx) * static_cast<size_t>(nz)) {
    error = "the chunk's grid window is empty";
    return false;
  }
  const uint32_t seed = in.seed ? in.seed : 1u;
  // The evaluated area: the chunk and a margin, as an axial box.
  const int q0 = in.q0 - kMargin, r0 = in.r0 - kMargin, W = S + 2 * kMargin;
  auto idx = [&](int q, int r) { return (r - r0) * W + (q - q0); };
  auto in_box = [&](int q, int r) { return q >= q0 && r >= r0 && q < q0 + W && r < r0 + W; };
  std::vector<Cell> cells(static_cast<size_t>(W) * static_cast<size_t>(W));

  // Which cells are inside the map: the outline's even-odd test with shape.h's nudges, and every cell
  // the outline passes through.
  {
    const size_t m = in.outline.size();
    std::vector<float> cross;
    for (int r = r0; r < r0 + W; ++r) {
      const float z = hex_z(r) + 2e-3f;
      cross.clear();
      for (size_t i = 0, j = m - 1; i < m; j = i++) {
        const vec3 &pi = in.outline[i], &pj = in.outline[j];
        if ((pi.z > z) != (pj.z > z)) cross.push_back((pj.x - pi.x) * (z - pi.z) / (pj.z - pi.z) + pi.x);
      }
      for (size_t i = 1; i < cross.size(); ++i)
        for (size_t k = i; k > 0 && cross[k] < cross[k - 1]; --k) {
          const float t = cross[k];
          cross[k] = cross[k - 1];
          cross[k - 1] = t;
        }
      // Only this box's cells: count the crossings at or before each cell, from the box's start.
      for (int q = q0; q < q0 + W; ++q) {
        const float x = hex_x(q, r) + 1e-3f;
        size_t beyond = 0;
        for (float c : cross)
          if (x < c) ++beyond;
        if (beyond % 2 == 1) cells[static_cast<size_t>(idx(q, r))].inside = true;
      }
    }
    const float bx0 = hex_x(q0, r0 + W) - 4, bx1 = hex_x(q0 + W, r0) + 4, bz0 = hex_z(r0) - 4, bz1 = hex_z(r0 + W) + 4;
    for (size_t i = 0; i < m; ++i) {
      const vec3 &a = in.outline[i], &b = in.outline[(i + 1) % m];
      if (maxf(a.x, b.x) < bx0 || minf(a.x, b.x) > bx1 || maxf(a.z, b.z) < bz0 || minf(a.z, b.z) > bz1) continue;
      const int steps = maxi_(1, round_to_int(length(b - a) * 2));
      for (int k = 0; k <= steps; ++k) {
        const vec3 p = lerp(a, b, static_cast<float>(k) / static_cast<float>(steps));
        int q, r;
        to_axial(p.x, p.z, q, r);
        if (in_box(q, r)) cells[static_cast<size_t>(idx(q, r))].inside = true;
      }
    }
  }

  // Settlements' cells: what stands on them (nothing grows there), and their streets' materials,
  // which the ground draws (a town's road pieces keep only their boards).
  std::vector<uint8_t> built(cells.size(), 0), street(cells.size(), 0);
  for (const auto &f : in.flats) {
    for (size_t k = 0; k + 1 < f.built.size(); k += 2)
      if (in_box(f.built[k], f.built[k + 1])) built[static_cast<size_t>(idx(f.built[k], f.built[k + 1]))] = 1;
    for (size_t k = 0; k + 1 < f.streets.size(); k += 2)
      if (in_box(f.streets[k], f.streets[k + 1]) && k / 2 < f.street_materials.size())
        street[static_cast<size_t>(idx(f.streets[k], f.streets[k + 1]))] = static_cast<uint8_t>(f.street_materials[k / 2] & 7u);
  }

  // 1 to 3: each cell's ground, water, and what it is.
  auto node = [&](int i, int k) { return static_cast<size_t>(maxi_(0, mini_(nz - 1, k))) * static_cast<size_t>(nx) + static_cast<size_t>(maxi_(0, mini_(nx - 1, i))); };
  const size_t P = in.parcels.size();
  for (int r = r0; r < r0 + W; ++r)
    for (int q = q0; q < q0 + W; ++q) {
      Cell &c = cells[static_cast<size_t>(idx(q, r))];
      if (!c.inside) continue;
      const float x = hex_x(q, r), z = hex_z(r);
      const float fx = clampf((x - in.gx) / in.spacing, 0, static_cast<float>(nx - 1) - 1e-3f);
      const float fz = clampf((z - in.gz) / in.spacing, 0, static_cast<float>(nz - 1) - 1e-3f);
      const int i = static_cast<int>(std::floor(fx)), k = static_cast<int>(std::floor(fz));
      const float u = fx - static_cast<float>(i), v = fz - static_cast<float>(k);
      const float hm = mixf(mixf(in.heights[node(i, k)], in.heights[node(i + 1, k)], u), mixf(in.heights[node(i, k + 1)], in.heights[node(i + 1, k + 1)], u), v);
      const size_t near = node(round_to_int(fx), round_to_int(fz));
      c.parcel = in.parcel_ids[near];
      const map_parcel *pc = c.parcel < P ? &in.parcels[c.parcel] : nullptr;
      const float rough = pc ? clampf(pc->roughness, 0, 1) : 0.3f, relief = pc ? clampf(pc->relief, 0, 400) : 5.0f;
      c.cliffs = pc ? clampf(pc->cliffs, 0, 1) : 0;
      c.caves = pc ? clampf(pc->caves, 0, 1) : 0;
      c.moist = pc ? clampf(pc->moisture, 0, 1) : 0.5f;
      // Fine relief, fading near what is built or wet.
      float mask = 1;
      float h_flat = 0;
      bool on_flat = false;
      for (const auto &f : in.flats) {
        const float dx = x - f.position.x, dz = z - f.position.z, d = foundry::det::sqrt(dx * dx + dz * dz);
        if (d < f.radius) {
          on_flat = true;
          h_flat = f.position.y;
          mask = 0;
        } else {
          mask = minf(mask, smooth(clampf((d - f.radius) / maxf(30.0f, f.radius), 0, 1)));
        }
      }
      float road_h = 0, road_d = 1e30f;
      const map_segment *road = nullptr;
      for (const map_segment &sg : in.roads) {
        float t;
        const float d = to_segment(x, z, sg.a, sg.b, t);
        const float half = sg.width * 0.5f;
        if (sg.kind == 3) {
          mask = minf(mask, smooth(clampf((d - half) / 12.0f, 0, 1)));
          continue;
        }
        mask = minf(mask, smooth(clampf((d - half) / 6.0f, 0, 1)));
        if (d - half < road_d) {
          road_d = d - half;
          road_h = mixf(sg.a.y, sg.b.y, t);
          road = &sg;
        }
      }
      const float detail = rough * 1.4f * (fbm(x / 6.0f, z / 6.0f, 3, seed + 31) - 0.5f) + relief * 0.05f * (fbm(x / 23.0f, z / 23.0f, 3, seed + 37) - 0.5f);
      float h = hm + detail * mask;
      if (on_flat) {
        h = h_flat;
        c.flags |= kFlat;
        if (built[static_cast<size_t>(idx(q, r))]) c.flags |= kBuilt;
      }
      // Roads: the road's own height under it, the shoulders easing to it over 3 m.
      if (road && road_d < 0) {
        h = road_h;
        c.flags |= road->kind == 0 ? kRoad : kLane;
        c.flags &= ~kFlat;
        if (road->bridge) c.flags |= kBridge;
      } else if (road && road_d < 3) {
        h = mixf(road_h, h, smooth(road_d / 3.0f));
      }
      // Rivers: the bed under the water, banks climbing gently.
      for (const map_segment &sg : in.roads) {
        if (sg.kind != 3) continue;
        float t;
        const float d = to_segment(x, z, sg.a, sg.b, t);
        const float half = sg.width * 0.5f, level = mixf(sg.a.y, sg.b.y, t);
        if (d < half && !(c.flags & (kRoad | kLane)) ) {
          h = minf(h, level - (1.0f + 0.06f * sg.width));
          c.water = maxf(c.water, level);
          c.flags |= kRiver;
        } else if (d < half && (c.flags & kBridge)) {
          c.water = maxf(c.water, level);
          c.flags |= kRiver;
        } else if (d < half + 12 && !(c.flags & (kRoad | kLane | kFlat))) {
          h = minf(h, level + 0.3f + (d - half) * 0.6f);
        }
      }
      if (c.flags & kBridge) h = minf(h, c.water > kDry ? c.water - 1.2f : h);  // the river keeps its bed under a deck
      // Lakes and marshes: the plan's level, where the ground lies under it.
      const float wl = in.water[near];
      if (wl > kDry && !(c.flags & (kRiver | kRoad | kLane | kFlat)) && h < wl) {
        c.water = wl;
        c.flags |= kLake;
      }
      c.h = h;
    }

  // 4. Rock: outcrops where a parcel's cliffs meet a steep slope; then cave mouths in tall faces.
  auto neighbour_h = [&](int q, int r, int d, float &h) {
    const int nq = q + kDq[d], nr = r + kDr[d];
    if (!in_box(nq, nr) || !cells[static_cast<size_t>(idx(nq, nr))].inside) return false;
    h = cells[static_cast<size_t>(idx(nq, nr))].h;
    return true;
  };
  {
    std::vector<float> terraced(cells.size(), 0.0f);
    for (int r = r0 + 1; r < r0 + W - 1; ++r)
      for (int q = q0 + 1; q < q0 + W - 1; ++q) {
        Cell &c = cells[static_cast<size_t>(idx(q, r))];
        terraced[static_cast<size_t>(idx(q, r))] = c.h;
        if (!c.inside || c.cliffs <= 0 || (c.flags & (kRoad | kLane | kFlat | kRiver | kLake))) continue;
        float slope = 0, nh;
        for (int d = 0; d < 6; ++d)
          if (neighbour_h(q, r, d, nh)) slope = maxf(slope, absf(nh - c.h) / kRun);
        const float x = hex_x(q, r), z = hex_z(r);
        const float mask = fbm(x / 30.0f, z / 30.0f, 3, seed + 9);
        const float strength = step_between(1 - c.cliffs * 0.75f, 1.05f - c.cliffs * 0.65f, mask) * step_between(0.2f, 0.45f, slope);
        if (strength <= 0.05f) continue;
        const float step = 5.5f + 5.0f * fbm(x / 25.0f, z / 25.0f, 2, seed + 10);
        const float offset = 4.0f * fbm(x / 45.0f, z / 45.0f, 2, seed + 11);
        const float ledge = std::floor((c.h + offset) / step + 0.5f) * step - offset;
        terraced[static_cast<size_t>(idx(q, r))] = mixf(c.h, ledge, strength);
        if (strength > 0.5f) c.flags |= kTerrace;
      }
    for (int r = r0 + 1; r < r0 + W - 1; ++r)
      for (int q = q0 + 1; q < q0 + W - 1; ++q) cells[static_cast<size_t>(idx(q, r))].h = terraced[static_cast<size_t>(idx(q, r))];
  }
  std::vector<float> ceiling(cells.size(), 0.0f);
  for (int r = r0 + 2; r < r0 + W - 2; ++r)
    for (int q = q0 + 2; q < q0 + W - 2; ++q) {
      Cell &c = cells[static_cast<size_t>(idx(q, r))];
      if (!c.inside || c.caves <= 0 || (c.flags & (kRoad | kLane | kFlat | kRiver | kLake))) continue;
      if (unit(hash2(q, r, seed * 13 + 5)) > c.caves * 0.25f) continue;
      for (int d = 0; d < 6; ++d) {
        float nh;
        if (!neighbour_h(q, r, d, nh) || c.h - nh < 3.5f) continue;
        ceiling[static_cast<size_t>(idx(q, r))] = nh + 2.25f;
        c.flags |= kCave;
        break;
      }
    }

  // 5. Materials, then the columns of the chunk and its apron.
  auto material = [&](int q, int r) -> uint32_t {
    const Cell &c = cells[static_cast<size_t>(idx(q, r))];
    if ((c.flags & kFlat) && street[static_cast<size_t>(idx(q, r))]) return static_cast<uint32_t>(street[static_cast<size_t>(idx(q, r))]);
    if (c.flags & kFlat) return kGrass;  // a site's level ground: grass, whatever the noise
    if (c.flags & kRoad) return kCobble;
    if (c.flags & kLane) return kDirt;
    if (c.water > kDry) return kSand;
    float steep = 0, nh;
    bool shore = false;
    for (int d = 0; d < 6; ++d) {
      if (neighbour_h(q, r, d, nh)) steep = maxf(steep, absf(nh - c.h) / kRun);
      const int nq = q + kDq[d], nr = r + kDr[d];
      if (in_box(nq, nr)) {
        const Cell &n = cells[static_cast<size_t>(idx(nq, nr))];
        if (n.inside && n.water > kDry && c.h < n.water + 1.0f) shore = true;
      }
    }
    if ((c.flags & (kTerrace | kCave)) || steep > 0.85f) return kRock;
    if (shore) return kSand;
    if (fbm(hex_x(q, r) / 14.0f, hex_z(r) / 14.0f, 3, seed + 20) > 0.7f) return kDirt;
    return kGrass;
  };
  float lowest = 1e30f;
  for (int r = in.r0 - 1; r < in.r0 + S + 1; ++r)
    for (int q = in.q0 - 1; q < in.q0 + S + 1; ++q)
      if (cells[static_cast<size_t>(idx(q, r))].inside) lowest = minf(lowest, cells[static_cast<size_t>(idx(q, r))].h);
  if (lowest > 1e29f) {
    error = "the chunk holds none of the map's cells";
    return false;
  }
  const int floor_u = to_units(lowest - 16.0f);
  std::vector<int> cell_index(cells.size(), -1), top_u(cells.size(), 0);
  std::vector<uint32_t> mats(cells.size(), kGrass);
  for (int r = in.r0 - 1; r < in.r0 + S + 1; ++r)
    for (int q = in.q0 - 1; q < in.q0 + S + 1; ++q) {
      const size_t a = static_cast<size_t>(idx(q, r));
      const Cell &c = cells[a];
      if (!c.inside) continue;
      const bool apron = q < in.q0 || r < in.r0 || q >= in.q0 + S || r >= in.r0 + S;
      cell_index[a] = static_cast<int>(out.cells.size() / 2);
      out.cells.push_back(q);
      out.cells.push_back(r);
      out.span_first.push_back(static_cast<uint32_t>(out.span_lo.size()));
      top_u[a] = maxi_(floor_u + 1, to_units(c.h));
      mats[a] = material(q, r);
      if (c.flags & kCave) {
        const int ceil_u = to_units(ceiling[a]);
        out.span_lo.push_back(floor_u);
        out.span_hi.push_back(ceil_u - 16);
        out.span_material.push_back(kRock);
        out.span_lo.push_back(ceil_u);
        out.span_hi.push_back(maxi_(ceil_u + 4, top_u[a]));
        out.span_material.push_back(kRock);
      } else {
        out.span_lo.push_back(floor_u);
        out.span_hi.push_back(top_u[a]);
        out.span_material.push_back(mats[a]);
      }
      out.water.push_back(c.water > kDry ? to_units(c.water) : -32768);
      out.flags.push_back((c.flags & 0xFFFFu) | (apron ? kApron : 0u));
    }
  out.span_first.push_back(static_cast<uint32_t>(out.span_lo.size()));

  // The navigation mesh over the chunk's own walkable tops.
  static const float kC[6] = {0.8660254f, 0, -0.8660254f, -0.8660254f, 0, 0.8660254f};
  static const float kS[6] = {0.5f, 1, 0.5f, -0.5f, -1, -0.5f};
  std::vector<int> poly(cells.size(), -1);
  std::vector<float> poly_h;
  for (int r = in.r0; r < in.r0 + S; ++r)
    for (int q = in.q0; q < in.q0 + S; ++q) {
      const size_t a = static_cast<size_t>(idx(q, r));
      const Cell &c = cells[a];
      if (!c.inside || (c.water > kDry && !(c.flags & kBridge))) continue;
      const float y = (c.flags & kBridge) ? c.water + 1.6f : static_cast<float>(top_u[a]) * kUnit;
      poly[a] = static_cast<int>(out.nav_cells.size());
      const float x = hex_x(q, r), z = hex_z(r);
      for (int j = 0; j < 6; ++j) {
        out.nav_polys.push_back(static_cast<uint32_t>(out.nav_vertices.size()));
        out.nav_vertices.push_back(v3(x + kC[j], y, z - kS[j]));
      }
      out.nav_cells.push_back(static_cast<uint32_t>(cell_index[a]));
      poly_h.push_back(y);
    }
  for (int r = in.r0; r < in.r0 + S; ++r)
    for (int q = in.q0; q < in.q0 + S; ++q) {
      const size_t a = static_cast<size_t>(idx(q, r));
      if (poly[a] < 0) continue;
      for (int d = 0; d < 3; ++d) {
        const int nq = q + kDq[d], nr = r + kDr[d];
        if (nq >= in.q0 + S || nr < in.r0 || nr >= in.r0 + S) continue;
        const size_t b = static_cast<size_t>(idx(nq, nr));
        if (poly[b] < 0) continue;
        const float limit = maxf(0.5f, minf(kSmooth[mats[a]], kSmooth[mats[b]]));
        if (absf(poly_h[static_cast<size_t>(poly[a])] - poly_h[static_cast<size_t>(poly[b])]) <= limit) {
          out.nav_links.push_back(static_cast<uint32_t>(poly[a]));
          out.nav_links.push_back(static_cast<uint32_t>(poly[b]));
        }
      }
    }

  // The nature ground, by parcel: open land available, water as water, roads a path, rock as rock;
  // grouped by parcel, kind, height (5 cm), and moisture.
  {
    struct Key {
      uint32_t parcel, kind;
      int h, m;
    };
    std::vector<Key> keys;
    std::vector<std::vector<int32_t>> groups;
    for (int r = in.r0; r < in.r0 + S; ++r)
      for (int q = in.q0; q < in.q0 + S; ++q) {
        const size_t a = static_cast<size_t>(idx(q, r));
        const Cell &c = cells[a];
        if (!c.inside || (c.flags & kBuilt)) continue;  // nothing grows where a settlement stands
        uint32_t kind = 0;  // 0 open, 1 water, 2 path, 3 rock, 4 beside a road (or on a site's flat)
        if (c.water > kDry) kind = 1;
        else if (c.flags & (kRoad | kLane | kBridge)) kind = 2;
        else if (mats[a] == kRock || (c.flags & kCave)) kind = 3;
        else if (c.flags & kFlat) kind = 4;  // a site's open ground: grass, but nothing planted
        else
          for (int d = 0; d < 6; ++d) {
            const int nq = q + kDq[d], nr = r + kDr[d];
            if (in_box(nq, nr) && (cells[static_cast<size_t>(idx(nq, nr))].flags & (kRoad | kLane))) kind = 4;
          }
        const float moist = kind == 1 ? 1.0f : c.moist;
        const Key key{c.parcel, kind, round_to_int(static_cast<float>(top_u[a]) * kUnit / 0.05f), round_to_int(moist / 0.05f)};
        size_t gi = keys.size();
        for (size_t k = 0; k < keys.size(); ++k)
          if (keys[k].parcel == key.parcel && keys[k].kind == key.kind && keys[k].h == key.h && keys[k].m == key.m) {
            gi = k;
            break;
          }
        if (gi == keys.size()) {
          keys.push_back(key);
          groups.emplace_back();
        }
        groups[gi].push_back(q);
        groups[gi].push_back(r);
      }
    for (size_t k = 0; k < keys.size(); ++k) {
      plot_region rg;
      rg.shape = plot_region::kShapeCells;
      rg.cells = groups[k];
      rg.priority = 10;
      rg.cut = false;
      rg.set = 1 | 2 | 8 | 16;
      const uint32_t kind = keys[k].kind;
      rg.ground = kind == 1 ? plot_region::kGroundWater
                            : (kind == 2 ? plot_region::kGroundPath : (kind == 3 ? plot_region::kGroundRock : plot_region::kGroundSoil));
      rg.available = kind == 0;
      rg.keep_clear_m = 0;
      rg.height = static_cast<float>(keys[k].h) * 0.05f;
      rg.moisture = static_cast<float>(keys[k].m) * 0.05f;
      out.nature.regions.push_back(rg);
      out.nature_parcels.push_back(keys[k].parcel);
    }
  }
  return true;
}

}  // namespace map_ground

FOUNDRY_SYSTEM(map_ground, channels::map_chunk, channels::terrain_ground)
