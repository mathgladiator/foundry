// plot_scatter: a plot system (design/plotter.md): fills a region of hex cells
// with what belongs in a biome, by layers.
//
// 1. Ground: the request's prioritized hex regions resolve to cells with
//    attributes (the highest priority covering a cell wins; cuts remove cells),
//    each cell's distance to the border, and a grove field (value noise).
// 2. Candidates come from cells, not from the region: each cell hashes its own
//    candidate spots per layer (position, kind, priority, seed), so a cell makes
//    the same candidates whatever region it is in.
// 3. Spacing is a local rule (Matern type II): a candidate is kept when no
//    statically valid candidate of higher priority in its layer lies within the
//    layer's spacing, and no footprint of one overlaps it. Whether a candidate
//    survives depends only on candidates within reach, so an edit changes only
//    things near it.
// 4. What exists is never moved: a kept candidate is placeable only if it is
//    clear of every occupant, every exclusion, earlier layers' things (keep
//    clear, prefer near), and cells' keep-clear distances, within the caps per
//    kind. Placeable candidates come back in the config's order, up to max, with
//    the volume left.
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "boundary.h"
#include "channels/plot_request.h"
#include "channels/plot_result.h"
#include "math.h"
#include "packer/plot_request.h"
#include "packer/plot_result.h"

namespace plot_scatter {

using namespace plot_math;
using channels::plot_kind;
using channels::plot_layer;
using channels::plot_region;
using channels::plot_request;
using channels::plot_result;

namespace {

constexpr size_t kMaxCells = 200000;
constexpr uint32_t kSetGround = 1, kSetAvailable = 2, kSetKeepClear = 4, kSetHeight = 8, kSetMoisture = 16;
constexpr uint32_t kEdgeCap = 64;

uint64_t key_of(int32_t q, int32_t r) { return (static_cast<uint64_t>(static_cast<uint32_t>(q)) << 32) | static_cast<uint32_t>(r); }

struct Cell {
  int32_t q = 0, r = 0;
  uint32_t ground = 0;  // soil
  bool available = true;
  float keep_clear_m = 0, height = 0, moisture = 0.5f;
  uint32_t edge = kEdgeCap;
  float grove = 0.5f;
  bool reserved = false;
};

struct Ground {
  std::map<uint64_t, Cell> cells;  // ordered: every walk over cells is deterministic
  const Cell *at(int32_t q, int32_t r) const {
    auto it = cells.find(key_of(q, r));
    return it == cells.end() ? nullptr : &it->second;
  }
  Cell *at(int32_t q, int32_t r) {
    auto it = cells.find(key_of(q, r));
    return it == cells.end() ? nullptr : &it->second;
  }
};

const int32_t kDirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};

// The cells of one region's shape.
void shape_cells(const plot_region &g, std::vector<std::pair<int32_t, int32_t>> &out) {
  out.clear();
  switch (g.shape) {
    case plot_region::kShapeCells:
      for (size_t i = 0; i + 1 < g.cells.size(); i += 2) out.push_back({g.cells[i], g.cells[i + 1]});
      break;
    case plot_region::kShapeHex: {
      const int32_t R = static_cast<int32_t>(g.radius);
      for (int32_t dq = -R; dq <= R; ++dq)
        for (int32_t dr = -R; dr <= R; ++dr)
          if (hex_distance(dq, dr) <= R) out.push_back({g.q + dq, g.r + dr});
      break;
    }
    case plot_region::kShapeRing: {
      const int32_t R = static_cast<int32_t>(g.radius);
      for (int32_t dq = -R; dq <= R; ++dq)
        for (int32_t dr = -R; dr <= R; ++dr)
          if (hex_distance(dq, dr) == R) out.push_back({g.q + dq, g.r + dr});
      break;
    }
    case plot_region::kShapeLine: {
      const int32_t n = hex_distance(g.q2 - g.q, g.r2 - g.r);
      for (int32_t i = 0; i <= n; ++i) {
        // Cube lerp, nudged off the edges so ties round the same way every time.
        const float t = n == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(n);
        const float fq = static_cast<float>(g.q) + (static_cast<float>(g.q2 - g.q)) * t + 1e-4f;
        const float fr = static_cast<float>(g.r) + (static_cast<float>(g.r2 - g.r)) * t + 1e-4f;
        int32_t q = 0, r = 0;
        cell_of(cell_x(0, 0) + kSqrt3 * (fq + 0.5f * fr), 1.5f * fr, q, r);
        out.push_back({q, r});
      }
      break;
    }
    default:
      break;
  }
}

bool resolve(const plot_request &in, Ground &g, std::string &error) {
  // Regions in priority order (ties keep list order), so a higher priority overrides a lower.
  std::vector<size_t> order(in.ground.regions.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return in.ground.regions[a].priority < in.ground.regions[b].priority;
  });
  std::vector<std::pair<int32_t, int32_t>> cells;
  for (size_t i : order) {
    const plot_region &rg = in.ground.regions[i];
    shape_cells(rg, cells);
    for (const auto &[q, r] : cells) {
      const uint64_t k = key_of(q, r);
      if (rg.cut) {
        g.cells.erase(k);
        continue;
      }
      auto it = g.cells.find(k);
      if (it == g.cells.end()) {
        if (g.cells.size() >= kMaxCells) {
          error = "the region has more than 200000 cells";
          return false;
        }
        Cell c;
        c.q = q;
        c.r = r;
        it = g.cells.emplace(k, c).first;
      }
      Cell &c = it->second;
      if (rg.set & kSetGround) c.ground = rg.ground;
      if (rg.set & kSetAvailable) c.available = rg.available;
      if (rg.set & kSetKeepClear) c.keep_clear_m = rg.keep_clear_m;
      if (rg.set & kSetHeight) c.height = rg.height;
      if (rg.set & kSetMoisture) c.moisture = rg.moisture;
    }
  }
  // Distance to the border, in cells: a breadth-first walk in from the cells that touch the outside.
  std::vector<Cell *> frontier;
  for (auto &[k, c] : g.cells) {
    bool border = false;
    for (const auto &d : kDirs) border = border || !g.at(c.q + d[0], c.r + d[1]);
    if (border) {
      c.edge = 0;
      frontier.push_back(&c);
    }
  }
  for (uint32_t step = 1; !frontier.empty() && step < kEdgeCap; ++step) {
    std::vector<Cell *> next;
    for (Cell *c : frontier)
      for (const auto &d : kDirs) {
        Cell *n = g.at(c->q + d[0], c->r + d[1]);
        if (n && n->edge > step) {
          n->edge = step;
          next.push_back(n);
        }
      }
    frontier.swap(next);
  }
  // The grove field: octaves of value noise over the ground, in world metres.
  const float scale = maxf(in.config.grove_scale_cells, 1.0f) * kSqrt3;
  const uint32_t octaves = in.config.grove_octaves < 1 ? 1 : (in.config.grove_octaves > 6 ? 6 : in.config.grove_octaves);
  for (auto &[k, c] : g.cells) {
    float sum = 0, amp = 1, norm = 0, f = 1;
    for (uint32_t o = 0; o < octaves; ++o) {
      sum += amp * noise2(cell_x(c.q, c.r) / scale * f, cell_z(c.r) / scale * f, hash_combine(in.seed, 101u + o));
      norm += amp;
      amp *= 0.5f;
      f *= 2;
    }
    c.grove = clampf(norm > 0 ? sum / norm : 0.5f, 0, 1);
  }
  return true;
}

// One thing on the ground: a candidate, or an occupant.
struct Thing {
  int32_t q = 0, r = 0;  // its slot's cell
  uint32_t layer = 0, index = 0, kind = 0;
  int32_t cq = 0, cr = 0;  // the cell it stands in (its footprint's centre)
  float x = 0, z = 0, y = 0, yaw = 0;
  float footprint_m = 0;
  uint32_t footprint_cells = 0;
  uint32_t priority = 0, seed = 0;
  float thin = 0;
  float max_share = 1;
};

bool beats(const Thing &a, const Thing &b) {  // a's priority over b's, with stable ties
  if (a.priority != b.priority) return a.priority > b.priority;
  if (a.q != b.q) return a.q < b.q;
  if (a.r != b.r) return a.r < b.r;
  return a.index < b.index;
}

float dist2(const Thing &a, float x, float z) {
  const float dx = a.x - x, dz = a.z - z;
  return dx * dx + dz * dz;
}

bool footprints_overlap(const Thing &a, const Thing &b) {
  if (a.footprint_cells == 0 || b.footprint_cells == 0) return false;
  return hex_distance(a.cq - b.cq, a.cr - b.cr) <= footprint_rings(a.footprint_cells) + footprint_rings(b.footprint_cells);
}

// A grid of things by position, for neighbour queries.
struct Grid {
  float cell = 1;
  std::unordered_map<uint64_t, std::vector<size_t>> buckets;
  explicit Grid(float c) : cell(maxf(c, 0.5f)) {}
  static int32_t floori(float v) {
    const int32_t t = static_cast<int32_t>(v);
    return static_cast<float>(t) > v ? t - 1 : t;
  }
  void add(size_t i, float x, float z) { buckets[key_of(floori(x / cell), floori(z / cell))].push_back(i); }
  template <typename F>
  void near(float x, float z, float radius, F &&f) const {
    const int32_t x0 = floori((x - radius) / cell), x1 = floori((x + radius) / cell);
    const int32_t z0 = floori((z - radius) / cell), z1 = floori((z + radius) / cell);
    for (int32_t gx = x0; gx <= x1; ++gx)
      for (int32_t gz = z0; gz <= z1; ++gz) {
        auto it = buckets.find(key_of(gx, gz));
        if (it == buckets.end()) continue;
        for (size_t i : it->second) f(i);
      }
  }
};

const std::vector<plot_kind> &kinds_for(const plot_layer &L, bool regrow) {
  return regrow && !L.regrow_kinds.empty() ? L.regrow_kinds : L.kinds;
}

bool ground_ok(uint32_t mask, uint32_t ground) { return mask == 0 || (mask & (1u << ground)) != 0; }

}  // namespace

bool run(const plot_request &in, plot_result &out, std::string &error) {
  out = plot_result{};
  Ground g;
  if (!resolve(in, g, error)) return false;
  out.cell_count = static_cast<uint32_t>(g.cells.size());
  const bool regrow = in.mode == plot_request::kModeRegrow;
  const size_t layer_count = in.config.layers.size();

  // Occupants: what stands already. Each reserves its footprint's cells, and counts as its layer's.
  std::vector<Thing> occupants;
  std::map<uint64_t, std::vector<size_t>> occupied_slots;  // (q, r) -> occupants made by that cell
  for (const auto &o : in.occupants) {
    Thing t;
    t.q = o.q;
    t.r = o.r;
    t.layer = o.layer;
    t.index = o.index;
    t.kind = o.kind;
    t.x = o.position.x;
    t.y = o.position.y;
    t.z = o.position.z;
    cell_of(t.x, t.z, t.cq, t.cr);
    t.footprint_m = o.footprint_m;
    t.footprint_cells = o.footprint_cells;
    occupied_slots[key_of(o.q, o.r)].push_back(occupants.size());
    occupants.push_back(t);
    const int32_t k = footprint_rings(t.footprint_cells);
    if (t.footprint_cells > 0)
      for (int32_t dq = -k; dq <= k; ++dq)
        for (int32_t dr = -k; dr <= k; ++dr)
          if (hex_distance(dq, dr) <= k)
            if (Cell *c = g.at(t.cq + dq, t.cr + dr)) c->reserved = true;
  }
  Grid occupant_grid(4.0f);
  for (size_t i = 0; i < occupants.size(); ++i) occupant_grid.add(i, occupants[i].x, occupants[i].z);

  // Cells that keep things clear of them.
  std::vector<const Cell *> clear_cells;
  float clear_reach = 0;
  for (const auto &[k, c] : g.cells)
    if (c.keep_clear_m > 0) {
      clear_cells.push_back(&c);
      clear_reach = maxf(clear_reach, c.keep_clear_m);
    }
  Grid clear_grid(maxf(clear_reach, 1.0f));
  for (size_t i = 0; i < clear_cells.size(); ++i)
    clear_grid.add(i, cell_x(clear_cells[i]->q, clear_cells[i]->r), cell_z(clear_cells[i]->r));

  // Each layer's placeable things, as later layers see them.
  std::vector<std::vector<Thing>> placeable(layer_count);
  std::vector<Grid> placeable_grid;
  placeable_grid.reserve(layer_count);

  for (size_t li = 0; li < layer_count; ++li) {
    const plot_layer &L = in.config.layers[li];
    const std::vector<plot_kind> &kinds = kinds_for(L, regrow);
    placeable_grid.emplace_back(maxf(L.spacing_m, 1.0f));
    if (kinds.empty() || (regrow && !L.regrows)) continue;
    float total_weight = 0, max_boost = 1;
    for (const auto &k : kinds) total_weight += maxf(k.weight, 0);
    for (const auto &p : L.prefer_near) max_boost = maxf(max_boost, p.boost);
    if (total_weight <= 0) continue;
    const float s = maxf(L.spacing_m, 0.05f);
    const float area = kPi * s * s;  // the Matern II hard core's area
    const uint32_t layer_salt = hash_combine(hash_str(L.name.c_str()), static_cast<uint32_t>(li));

    // 2. Candidates, cell by cell, with the static checks: ground, availability, slope, cells' keep-clear.
    std::vector<Thing> cand;
    for (const auto &[key, c] : g.cells) {
      float mult = L.grove_low + (L.grove_high - L.grove_low) * c.grove;
      if (L.edge_cells > 0) {
        const float e = clampf(static_cast<float>(c.edge) / static_cast<float>(L.edge_cells), 0, 1);
        mult *= L.edge_low + (1 - L.edge_low) * e;
      }
      // The kept density this cell asks for, per square metre, held under the hard core's ceiling.
      const float want = clampf(maxf(L.density, 0) / 100.0f * maxf(mult, 0) / kCellArea, 0, 0.9f / area);
      if (want <= 0) continue;
      // Candidates per square metre so Matern II keeps `want`: lambda = -ln(1 - want * area) / area.
      const float lambda = -ln_unit(1 - want * area) / area;
      const float expect = lambda * kCellArea * max_boost;
      Rng rng(hash_combine(hash_combine(in.seed, layer_salt), hash_cell(c.q, c.r)), 17u);
      const uint32_t whole = static_cast<uint32_t>(expect);
      const float frac_draw = rng.uniform();
      const uint32_t count = whole + (frac_draw < expect - static_cast<float>(whole) ? 1u : 0u);
      for (uint32_t i = 0; i < count && i < 64; ++i) {
        // One draw per statement: argument order is unspecified, and GCC and Clang differ.
        const float u = rng.uniform();
        const float v = rng.uniform();
        const uint32_t priority = rng.next();
        const float thin = rng.uniform();
        const float yaw = rng.uniform() * 2 * kPi;
        const uint32_t seed = rng.next();
        float pick = rng.uniform() * total_weight;
        const plot_kind *kind = &kinds.back();
        for (const auto &k : kinds) {
          if (pick < k.weight) {
            kind = &k;
            break;
          }
          pick -= k.weight;
        }
        if (!ground_ok(kind->ground ? kind->ground : L.ground, c.ground)) continue;
        Thing t;
        t.q = c.q;
        t.r = c.r;
        t.layer = static_cast<uint32_t>(li);
        t.index = i;
        t.kind = kind->id;
        t.footprint_m = kind->footprint_m;
        t.footprint_cells = kind->footprint_m > 0 ? kind->footprint_cells : 0;
        t.max_share = kind->max_share <= 0 ? 1.0f : kind->max_share;
        t.priority = priority;
        t.seed = seed;
        t.thin = thin;
        t.yaw = yaw;
        t.cq = c.q;
        t.cr = c.r;
        // A position in the cell: things with a footprint stand near its centre (their ground
        // contact stays inside it); the rest anywhere in the hex.
        const float inner = t.footprint_cells >= 37 ? 4.9f : t.footprint_cells >= 19 ? 3.51f : t.footprint_cells >= 7 ? 1.9f : 0.766f;
        const float jitter = t.footprint_cells > 0 ? clampf(inner - t.footprint_m, 0, 0.6f) : 0.8f;
        t.x = cell_x(c.q, c.r) + (u * 2 - 1) * jitter;
        t.z = cell_z(c.r) + (v * 2 - 1) * jitter;
        t.y = c.height;
        // Static checks: its footprint's cells are in the region, available, and smooth enough.
        if (t.footprint_cells > 0) {
          const int32_t k = footprint_rings(t.footprint_cells);
          float lo = c.height, hi = c.height;
          bool ok = true;
          for (int32_t dq = -k; dq <= k && ok; ++dq)
            for (int32_t dr = -k; dr <= k && ok; ++dr) {
              if (hex_distance(dq, dr) > k) continue;
              const Cell *f = g.at(c.q + dq, c.r + dr);
              if (!f || !f->available) ok = false;
              else {
                lo = minf(lo, f->height);
                hi = maxf(hi, f->height);
              }
            }
          if (!ok || hi - lo > L.max_height_delta_m) continue;
          // Cells that keep blocking things clear of them.
          bool clear = true;
          clear_grid.near(t.x, t.z, clear_reach + t.footprint_m, [&](size_t ci) {
            const Cell *kc = clear_cells[ci];
            const float dx = cell_x(kc->q, kc->r) - t.x, dz = cell_z(kc->r) - t.z;
            const float need = kc->keep_clear_m + t.footprint_m;
            if (dx * dx + dz * dz < need * need) clear = false;
          });
          if (!clear) continue;
        }
        cand.push_back(t);
      }
    }

    // 3. Matern II: kept when no higher-priority candidate lies within the spacing, or overlaps its footprint.
    Grid grid(s);
    for (size_t i = 0; i < cand.size(); ++i) grid.add(i, cand[i].x, cand[i].z);
    // Reach: the spacing, or two of the largest footprints side by side (centre cells apart).
    int32_t rings = 0;
    for (const auto &k : kinds)
      if (k.footprint_m > 0) rings = rings > footprint_rings(k.footprint_cells) ? rings : footprint_rings(k.footprint_cells);
    const float reach = maxf(s, static_cast<float>(2 * rings + 1) * kSqrt3 + 1.5f);
    std::vector<size_t> kept;
    for (size_t i = 0; i < cand.size(); ++i) {
      const Thing &t = cand[i];
      bool keep = true;
      grid.near(t.x, t.z, reach, [&](size_t j) {
        if (!keep || j == i) return;
        const Thing &o = cand[j];
        if (!beats(o, t)) return;
        if (dist2(o, t.x, t.z) < s * s || footprints_overlap(o, t)) keep = false;
      });
      if (keep) kept.push_back(i);
    }

    // 4. Placeable: clear of occupants, exclusions, and earlier layers; thinned away from preferred layers.
    std::vector<Thing> ok;
    for (size_t i : kept) {
      const Thing &t = cand[i];
      bool fine = true;
      // Already there: its own slot is occupied.
      auto slot = occupied_slots.find(key_of(t.q, t.r));
      if (slot != occupied_slots.end())
        for (size_t oi : slot->second)
          if (occupants[oi].layer == t.layer && occupants[oi].index == t.index) fine = false;
      if (!fine) continue;
      // Occupants: the same layer's spacing, any footprint, and earlier layers' keep-clear distances.
      float occupant_reach = maxf(s, 8.0f);
      for (const auto &kc : L.keep_clear) occupant_reach = maxf(occupant_reach, kc.metres);
      occupant_grid.near(t.x, t.z, occupant_reach, [&](size_t oi) {
        if (!fine) return;
        const Thing &o = occupants[oi];
        const float d2 = dist2(o, t.x, t.z);
        if (o.layer == t.layer && d2 < s * s) fine = false;
        else if (footprints_overlap(o, t)) fine = false;
        else
          for (const auto &kc : L.keep_clear)
            if (kc.layer == o.layer && d2 < kc.metres * kc.metres) fine = false;
      });
      if (!fine) continue;
      // Earlier layers' placeable things: their footprints, and keep-clear distances.
      for (size_t pl = 0; pl < li && fine; ++pl) {
        float need = 0;
        for (const auto &kc : L.keep_clear)
          if (kc.layer == pl) need = maxf(need, kc.metres);
        placeable_grid[pl].near(t.x, t.z, maxf(need, 8.0f), [&](size_t pi) {
          if (!fine) return;
          const Thing &o = placeable[pl][pi];
          if (footprints_overlap(o, t) || (need > 0 && dist2(o, t.x, t.z) < need * need)) fine = false;
        });
      }
      if (!fine) continue;
      // Exclusions for this call.
      for (const auto &ex : in.exclusions) {
        const float dx = ex.position.x - t.x, dz = ex.position.z - t.z, need = ex.radius + t.footprint_m;
        if (dx * dx + dz * dz < need * need) fine = false;
      }
      if (!fine) continue;
      // Reserved cells under the footprint (occupants' footprints).
      if (t.footprint_cells > 0) {
        const int32_t k = footprint_rings(t.footprint_cells);
        for (int32_t dq = -k; dq <= k && fine; ++dq)
          for (int32_t dr = -k; dr <= k && fine; ++dr)
            if (hex_distance(dq, dr) <= k) {
              const Cell *f = g.at(t.cq + dq, t.cr + dr);
              if (f && f->reserved) fine = false;
            }
      }
      if (!fine) continue;
      // Preferred neighbours: away from them, keep only 1 / boost of the candidates.
      if (!L.prefer_near.empty()) {
        float boost = 1;
        for (const auto &p : L.prefer_near) {
          bool near = false;
          if (p.layer < li)
            placeable_grid[p.layer].near(t.x, t.z, p.within_m, [&](size_t pi) {
              near = near || dist2(placeable[p.layer][pi], t.x, t.z) < p.within_m * p.within_m;
            });
          occupant_grid.near(t.x, t.z, p.within_m, [&](size_t oi) {
            near = near || (occupants[oi].layer == p.layer && dist2(occupants[oi], t.x, t.z) < p.within_m * p.within_m);
          });
          if (near) boost = maxf(boost, p.boost);
        }
        if (t.thin * max_boost >= boost) continue;
      }
      ok.push_back(t);
    }

    // Caps per kind within the region: in priority order, a kind past its share is left off.
    std::sort(ok.begin(), ok.end(), [](const Thing &a, const Thing &b) { return beats(a, b); });
    std::map<uint32_t, uint32_t> count;
    uint32_t layer_occupants = 0;
    for (const auto &o : occupants)
      if (o.layer == li) {
        ++count[o.kind];
        ++layer_occupants;
      }
    const float total = static_cast<float>(layer_occupants + ok.size());
    for (const Thing &t : ok) {
      if (t.max_share < 1 && static_cast<float>(count[t.kind] + 1) > t.max_share * total + 0.5f) continue;
      ++count[t.kind];
      placeable_grid[li].add(placeable[li].size(), t.x, t.z);
      placeable[li].push_back(t);
    }
  }

  // Order: by layer, then priority; or nearest the focus first.
  std::vector<const Thing *> all;
  for (const auto &layer : placeable)
    for (const Thing &t : layer) all.push_back(&t);
  if (in.use_focus) {
    std::stable_sort(all.begin(), all.end(), [&](const Thing *a, const Thing *b) {
      const float da = dist2(*a, in.focus.x, in.focus.z), db = dist2(*b, in.focus.x, in.focus.z);
      if (da != db) return da < db;
      return beats(*a, *b);
    });
  }
  const size_t n = std::min<size_t>(all.size(), in.max);
  for (size_t i = 0; i < n; ++i) {
    const Thing &t = *all[i];
    channels::plot_placement p;
    p.q = t.q;
    p.r = t.r;
    p.layer = t.layer;
    p.index = t.index;
    p.kind = t.kind;
    p.position = foundry::vec3{t.x, t.y, t.z};
    p.yaw = t.yaw;
    p.seed = t.seed;
    p.footprint_m = t.footprint_m;
    p.footprint_cells = t.footprint_cells;
    p.regrown = regrow;
    out.placements.push_back(p);
  }
  out.volume = static_cast<uint32_t>(all.size() - n);
  if (in.want_cells) {
    for (const auto &[k, c] : g.cells) {
      channels::plot_cell oc;
      oc.q = c.q;
      oc.r = c.r;
      oc.ground = c.ground;
      oc.available = c.available;
      oc.keep_clear_m = c.keep_clear_m;
      oc.height = c.height;
      oc.moisture = c.moisture;
      oc.edge = c.edge;
      oc.grove = c.grove;
      oc.reserved = c.reserved;
      out.cells.push_back(oc);
    }
  }
  return true;
}

}  // namespace plot_scatter

FOUNDRY_SYSTEM(plot_scatter, channels::plot_request, channels::plot_result)
