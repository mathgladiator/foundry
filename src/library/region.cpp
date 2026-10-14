// Regions (design/plotter.md, design/api.md "Regions"): bookkeeping over a plotter.
// A region keeps its ground and what stands on it (occupants, by slot); growing
// runs the plotter's plot system natively with them, and the game spawns, owns,
// and saves what comes back.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "internal.h"
#include "plotters_internal.h"

namespace regions {

struct Region {
  bool alive = false;
  uint32_t generation = 1;
  foundry_plotter plotter = 0;
  uint32_t seed = 1;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<channels::plot_v1_region> ground;
  std::vector<channels::plot_v1_occupant> occupants;
  std::set<std::pair<int32_t, int32_t>> cells;  // the resolved cells
};

struct Store {
  std::vector<Region> regions;
};

void destroy(Store *s) { delete s; }

namespace {

Store &store(foundry_runtime *rt) {
  if (!rt->regions) rt->regions = new Store();
  return *rt->regions;
}

Region *find(foundry_runtime *rt, foundry_region h) {
  if (!rt) return nullptr;
  const uint32_t index = static_cast<uint32_t>(h & 0xffffffffu), generation = static_cast<uint32_t>(h >> 32);
  Store &s = store(rt);
  if (index == 0 || index > s.regions.size()) return nullptr;
  Region &r = s.regions[index - 1];
  return r.alive && r.generation == generation ? &r : nullptr;
}

int rings(uint32_t cells) { return cells >= 37 ? 3 : cells >= 19 ? 2 : cells >= 7 ? 1 : 0; }
int hex_distance(int dq, int dr) { return (std::abs(dq) + std::abs(dr) + std::abs(dq + dr)) / 2; }

channels::plot_v1_region convert(const foundry_hex_region &h) {
  channels::plot_v1_region g;
  g.shape = static_cast<uint32_t>(h.shape);
  for (uint32_t i = 0; h.cells && i < h.cell_count; ++i) {
    g.cells.push_back(h.cells[i * 2]);
    g.cells.push_back(h.cells[i * 2 + 1]);
  }
  g.q = h.q;
  g.r = h.r;
  g.q2 = h.q2;
  g.r2 = h.r2;
  g.radius = h.radius;
  g.priority = h.priority;
  g.cut = h.cut;
  g.set = h.set;
  g.ground = static_cast<uint32_t>(h.ground);
  g.available = h.available;
  g.keep_clear_m = h.keep_clear_m;
  g.height = h.height;
  g.moisture = h.moisture;
  return g;
}

channels::plot_v1_request request(const Region &r, uint32_t mode, uint32_t max, bool want_cells) {
  channels::plot_v1_request q;
  q.config = plotters::info(r.plotter)->config();
  q.ground.regions = r.ground;
  q.occupants = r.occupants;
  q.seed = r.seed;
  q.mode = mode;
  q.max = max;
  q.want_cells = want_cells;
  return q;
}

bool resolve(Region &r) {
  channels::plot_v1_result out;
  std::string error;
  if (!plotters::info(r.plotter)->run(request(r, channels::plot_v1_request::kModeSeed, 0, true), out, error)) return false;
  r.cells.clear();
  for (const auto &c : out.cells) r.cells.insert({c.q, c.r});
  return true;
}

// Whether an occupant's footprint (or its own cell) is all inside the region.
bool inside(const Region &r, const channels::plot_v1_occupant &o) {
  const int k = rings(o.footprint_cells);
  for (int dq = -k; dq <= k; ++dq)
    for (int dr = -k; dr <= k; ++dr)
      if (hex_distance(dq, dr) <= k && !r.cells.count({o.q + dq, o.r + dr})) return false;
  return true;
}

channels::plot_v1_occupant occupant_of(const foundry_placement &p) {
  channels::plot_v1_occupant o;
  o.q = p.slot.q;
  o.r = p.slot.r;
  o.layer = p.slot.layer;
  o.index = p.slot.index;
  o.kind = p.kind;
  o.position = {p.position[0], p.position[1], p.position[2]};
  o.footprint_m = p.footprint_m;
  o.footprint_cells = p.footprint_cells;
  return o;
}

bool same_slot(const channels::plot_v1_occupant &o, const foundry_slot &s) {
  return o.q == s.q && o.r == s.r && o.layer == s.layer && o.index == s.index;
}

}  // namespace

}  // namespace regions

using namespace regions;

extern "C" {

foundry_region foundry_region_create(foundry_runtime *rt, const foundry_region_desc *desc) {
  if (!rt || !desc || !plotters::info(desc->plotter)) return FOUNDRY_NO_REGION;
  Store &s = store(rt);
  size_t index = s.regions.size();
  for (size_t i = 0; i < s.regions.size(); ++i)
    if (!s.regions[i].alive) {
      index = i;
      break;
    }
  if (index == s.regions.size()) s.regions.emplace_back();
  Region &r = s.regions[index];
  const uint32_t generation = r.generation;
  r = Region{};
  r.generation = generation;
  r.alive = true;
  r.plotter = desc->plotter;
  r.seed = desc->world_seed;
  bool zero = true;
  for (float v : desc->transform) zero = zero && v == 0;
  if (!zero) std::memcpy(r.transform, desc->transform, sizeof r.transform);
  for (uint32_t i = 0; desc->regions && i < desc->region_count; ++i) r.ground.push_back(convert(desc->regions[i]));
  if (!resolve(r)) {
    r.alive = false;
    return FOUNDRY_NO_REGION;
  }
  return (static_cast<uint64_t>(r.generation) << 32) | (index + 1);
}

foundry_status foundry_region_destroy(foundry_runtime *rt, foundry_region region) {
  Region *r = find(rt, region);
  if (!r) return FOUNDRY_STATUS_STALE_HANDLE;
  r->alive = false;
  ++r->generation;
  r->occupants.clear();
  r->ground.clear();
  r->cells.clear();
  return FOUNDRY_OK;
}

foundry_status foundry_region_restore(foundry_runtime *rt, foundry_region region, const foundry_placement *placements,
                                      uint32_t count) {
  Region *r = find(rt, region);
  if (!r) return FOUNDRY_STATUS_STALE_HANDLE;
  const plotters::Info *info = plotters::info(r->plotter);
  std::vector<channels::plot_v1_occupant> restored;
  for (uint32_t i = 0; i < count; ++i) {
    const foundry_placement &p = placements[i];
    if (p.kind >= info->kind_count) return FOUNDRY_STATUS_FAILED;
    const channels::plot_v1_occupant o = occupant_of(p);
    if (!inside(*r, o)) return FOUNDRY_STATUS_FAILED;
    for (const auto &e : restored) {
      if (e.q == o.q && e.r == o.r && e.layer == o.layer && e.index == o.index) return FOUNDRY_STATUS_FAILED;
      if (e.footprint_cells && o.footprint_cells &&
          hex_distance(e.q - o.q, e.r - o.r) <= rings(e.footprint_cells) + rings(o.footprint_cells))
        return FOUNDRY_STATUS_FAILED;
    }
    restored.push_back(o);
  }
  r->occupants.insert(r->occupants.end(), restored.begin(), restored.end());
  return FOUNDRY_OK;
}

uint32_t foundry_region_grow(foundry_runtime *rt, foundry_region region, foundry_grow_mode mode,
                             const foundry_exclusion *exclusions, uint32_t exclusion_count, const float *focus,
                             foundry_placement *out, uint32_t max, uint32_t *volume) {
  Region *r = find(rt, region);
  if (volume) *volume = 0;
  if (!r || (!out && max > 0)) return 0;
  channels::plot_v1_request q = request(*r, mode == FOUNDRY_GROW_REGROW ? channels::plot_v1_request::kModeRegrow
                                                                        : channels::plot_v1_request::kModeSeed,
                                        max, false);
  for (uint32_t i = 0; exclusions && i < exclusion_count; ++i) {
    channels::plot_v1_exclusion e;
    e.position = {exclusions[i].position[0], exclusions[i].position[1], exclusions[i].position[2]};
    e.radius = exclusions[i].radius;
    q.exclusions.push_back(e);
  }
  if (focus) {
    q.use_focus = true;
    q.focus = {focus[0], focus[1], focus[2]};
  }
  channels::plot_v1_result res;
  std::string error;
  if (!plotters::info(r->plotter)->run(q, res, error)) return 0;
  uint32_t n = 0;
  for (const auto &p : res.placements) {
    if (n >= max) break;
    foundry_placement &o = out[n++];
    o.slot = {p.q, p.r, p.layer, p.index};
    o.kind = p.kind;
    o.position[0] = p.position.x;
    o.position[1] = p.position.y;
    o.position[2] = p.position.z;
    o.yaw = p.yaw;
    o.seed = p.seed;
    o.footprint_m = p.footprint_m;
    o.footprint_cells = p.footprint_cells;
    o.regrown = p.regrown;
    r->occupants.push_back(occupant_of(o));  // it stands there now
  }
  if (volume) *volume = res.volume;
  return n;
}

foundry_instance foundry_placement_spawn(foundry_runtime *rt, foundry_region region, const foundry_placement *p) {
  Region *r = find(rt, region);
  if (!r || !p) return FOUNDRY_NO_INSTANCE;
  const plotters::Info *info = plotters::info(r->plotter);
  if (p->kind >= info->kind_count) return FOUNDRY_NO_INSTANCE;
  // The placement in region space (turned about +Y, then moved), then the region's transform.
  const float c = std::cos(p->yaw), s = std::sin(p->yaw);
  const float local[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, p->position[0], p->position[1], p->position[2], 1};
  float world[16];
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += r->transform[k * 4 + row] * local[col * 4 + k];
      world[col * 4 + row] = sum;
    }
  return info->kinds[p->kind].spawn(rt, p->seed, p->regrown, world);
}

foundry_status foundry_region_vacate(foundry_runtime *rt, foundry_region region, const foundry_slot *slots, uint32_t count) {
  Region *r = find(rt, region);
  if (!r) return FOUNDRY_STATUS_STALE_HANDLE;
  for (uint32_t i = 0; slots && i < count; ++i)
    r->occupants.erase(std::remove_if(r->occupants.begin(), r->occupants.end(),
                                      [&](const channels::plot_v1_occupant &o) { return same_slot(o, slots[i]); }),
                       r->occupants.end());
  return FOUNDRY_OK;
}

uint32_t foundry_region_set_regions(foundry_runtime *rt, foundry_region region, const foundry_hex_region *ground,
                                    uint32_t count, foundry_slot *lost, uint32_t lost_capacity) {
  Region *r = find(rt, region);
  if (!r) return 0;
  r->ground.clear();
  for (uint32_t i = 0; ground && i < count; ++i) r->ground.push_back(convert(ground[i]));
  if (!resolve(*r)) return 0;
  uint32_t n = 0;
  std::vector<channels::plot_v1_occupant> kept;
  for (const auto &o : r->occupants) {
    if (inside(*r, o)) {
      kept.push_back(o);
      continue;
    }
    if (lost && n < lost_capacity) lost[n] = {o.q, o.r, o.layer, o.index};
    ++n;
  }
  r->occupants.swap(kept);
  return n;
}

uint32_t foundry_region_find(foundry_runtime *rt, foundry_region region, const float center[3], float radius,
                             foundry_slot *out, uint32_t capacity) {
  Region *r = find(rt, region);
  if (!r || !center) return 0;
  uint32_t n = 0;
  for (const auto &o : r->occupants) {
    const float dx = o.position.x - center[0], dz = o.position.z - center[2];
    if (dx * dx + dz * dz > radius * radius) continue;
    if (out && n < capacity) out[n] = {o.q, o.r, o.layer, o.index};
    ++n;
  }
  return n;
}

uint32_t foundry_region_count(foundry_runtime *rt, foundry_region region) {
  Region *r = find(rt, region);
  return r ? static_cast<uint32_t>(r->occupants.size()) : 0;
}

const char *foundry_plotter_kind_name(foundry_plotter plotter, uint32_t kind) {
  const plotters::Info *info = plotters::info(plotter);
  return info && kind < info->kind_count ? info->kinds[kind].label : nullptr;
}

uint32_t foundry_plotter_kind_count(foundry_plotter plotter) {
  const plotters::Info *info = plotters::info(plotter);
  return info ? info->kind_count : 0;
}

uint32_t foundry_region_spawn_fields(foundry_runtime *rt, foundry_region region, foundry_instance *out, uint32_t max) {
  regions::Region *r = regions::find(rt, region);
  if (!r) return 0;
  const plotters::Info *info = plotters::info(r->plotter);
  if (!info || info->field_count == 0) return 0;
  // The region's resolved cells, with their ground, moisture, and height.
  channels::plot_v1_result res;
  std::string error;
  if (!info->run(regions::request(*r, channels::plot_v1_request::kModeSeed, 0, true), res, error)) return 0;
  std::vector<foundry_field_cell> cells;
  cells.reserve(res.cells.size());
  for (const auto &c : res.cells)
    cells.push_back({c.q, c.r, static_cast<foundry_ground>(c.ground), c.moisture, c.height});
  for (uint32_t f = 0; f < info->field_count; ++f) {
    const foundry_instance h = info->fields[f].spawn(rt, r->seed, r->transform, cells.data(), static_cast<uint32_t>(cells.size()));
    if (out && f < max) out[f] = h;
  }
  return info->field_count;
}

const char *foundry_plotter_field_name(foundry_plotter plotter, uint32_t field) {
  const plotters::Info *info = plotters::info(plotter);
  return info && field < info->field_count ? info->fields[field].label : "";
}

uint32_t foundry_plotter_field_count(foundry_plotter plotter) {
  const plotters::Info *info = plotters::info(plotter);
  return info ? info->field_count : 0;
}

foundry_plotter foundry_plotter_find(const char *name) {
  for (uint32_t i = 0; i < plotters::count(); ++i)
    if (name && std::strcmp(plotters::info(i)->name, name) == 0) return i;
  return FOUNDRY_NO_PLOTTER;
}

const char *foundry_plotter_name(foundry_plotter plotter) {
  const plotters::Info *info = plotters::info(plotter);
  return info ? info->name : "";
}

}  // extern "C"

// Boundaries (design/boundary.md): the ring of cells around a region, and the plotter's boundary.
namespace {

// The six neighbours, counterclockwise as seen from above, from east (north is -z).
const int kDq[6] = {1, 1, 0, -1, -1, 0}, kDr[6] = {0, -1, -1, 0, 1, 1};

// A hex corner's place, as integers (pointy-top, circumradius 1; corner j at 30 + 60 j degrees,
// counterclockwise as seen from above): exact keys for chaining edges.
std::pair<long, long> corner(int q, int r, int j) {
  static const double kC[6] = {0.8660254, 0, -0.8660254, -0.8660254, 0, 0.8660254};
  static const double kS[6] = {0.5, 1, 0.5, -0.5, -1, -0.5};
  const double x = 1.7320508 * (q + r * 0.5) + kC[j], z = 1.5 * r - kS[j];
  return {std::lround(x * 1000), std::lround(z * 1000)};
}

}  // namespace

extern "C" {

uint32_t foundry_region_hull(foundry_runtime *rt, foundry_region region, foundry_hull_cell *out, uint32_t capacity) {
  regions::Region *r = regions::find(rt, region);
  if (!r) return 0;
  const plotters::Info *info = plotters::info(r->plotter);
  channels::plot_v1_result res;
  std::string error;
  if (!info || !info->run(regions::request(*r, channels::plot_v1_request::kModeSeed, 0, true), res, error)) return 0;
  std::map<std::pair<int32_t, int32_t>, float> height;
  for (const auto &c : res.cells) height[{c.q, c.r}] = c.height;
  // Every edge between a region cell and one outside, running counterclockwise around the region
  // (the region on its left as seen from above): from the cell's corner before the neighbour's
  // direction to the one after. They chain into loops: the outer one runs counterclockwise.
  struct Edge {
    std::pair<long, long> from, to;
    int32_t q, r;  // the cell outside
    float height;  // the region's cell's
    bool used = false;
  };
  std::vector<Edge> edges;
  std::multimap<std::pair<long, long>, size_t> starting;
  for (const auto &[cell, h] : height)
    for (int k = 0; k < 6; ++k) {
      const int32_t nq = cell.first + kDq[k], nr = cell.second + kDr[k];
      if (height.count({nq, nr})) continue;
      Edge e;
      e.from = corner(cell.first, cell.second, (k + 5) % 6);
      e.to = corner(cell.first, cell.second, k);
      e.q = nq;
      e.r = nr;
      e.height = h;
      starting.insert({e.from, edges.size()});
      edges.push_back(e);
    }
  std::vector<foundry_hull_cell> best;
  double best_area = 0;
  for (size_t s = 0; s < edges.size(); ++s) {
    if (edges[s].used) continue;
    std::vector<size_t> loop;
    size_t at = s;
    while (!edges[at].used) {
      edges[at].used = true;
      loop.push_back(at);
      size_t next = edges.size();
      auto range = starting.equal_range(edges[at].to);
      for (auto it = range.first; it != range.second; ++it)
        if (!edges[it->second].used) {
          next = it->second;
          break;
        }
      if (next == edges.size()) break;
      at = next;
    }
    // The loop's area as seen from above (counterclockwise is positive with x east and z south).
    double area = 0;
    for (size_t i : loop) area += static_cast<double>(edges[i].to.first) * edges[i].from.second -
                                  static_cast<double>(edges[i].from.first) * edges[i].to.second;
    if (area <= best_area) continue;
    best_area = area;
    best.clear();
    for (size_t i : loop) {
      const Edge &e = edges[i];
      if (!best.empty() && best.back().q == e.q && best.back().r == e.r) continue;
      best.push_back({e.q, e.r, FOUNDRY_EDGE_AUTO, e.height, -1.0f, 0u, 0u});
    }
    while (best.size() > 1 && best.back().q == best.front().q && best.back().r == best.front().r) best.pop_back();
  }
  for (uint32_t i = 0; out && i < capacity && i < best.size(); ++i) out[i] = best[i];
  return static_cast<uint32_t>(best.size());
}

foundry_instance foundry_region_spawn_boundary(foundry_runtime *rt, foundry_region region, const foundry_hull_cell *cells,
                                               uint32_t count) {
  regions::Region *r = regions::find(rt, region);
  if (!r || (count && !cells)) return FOUNDRY_NO_INSTANCE;
  const plotters::Info *info = plotters::info(r->plotter);
  if (!info || !info->boundary) return FOUNDRY_NO_INSTANCE;
  return info->boundary->spawn(rt, r->seed, r->transform, cells, count);
}

uint32_t foundry_region_spawn_figures(foundry_runtime *rt, foundry_region region, foundry_instance boundary, foundry_instance *out,
                                      uint32_t capacity) {
  regions::Region *r = regions::find(rt, region);
  if (!r) return 0;
  const plotters::Info *info = plotters::info(r->plotter);
  if (!info || !info->boundary || !info->boundary->figure) return 0;
  const uint32_t n = foundry_instance_figures(rt, boundary, nullptr, 0);
  std::vector<foundry_figure> figures(n);
  foundry_instance_figures(rt, boundary, figures.data(), n);
  uint32_t spawned = 0;
  for (const foundry_figure &f : figures) {
    const float a = f.yaw * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    // Turned about +Y by its yaw, standing at its place (column-major).
    const float t[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, f.position[0], f.position[1], f.position[2], 1};
    const foundry_instance i = info->boundary->figure(rt, &f, t);
    if (i == FOUNDRY_NO_INSTANCE) continue;
    if (out && spawned < capacity) out[spawned] = i;
    ++spawned;
  }
  return spawned;
}

const char *foundry_plotter_boundary_name(foundry_plotter plotter) {
  const plotters::Info *info = plotters::info(plotter);
  return info && info->boundary ? info->boundary->label : "";
}

}  // extern "C"
