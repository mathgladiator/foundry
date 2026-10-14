// What a built instance hands the game beyond its looks (ground.h): a boundary's figures and a
// terrain's ground, read from the build it draws now.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "internal.h"

namespace {

// A point in the instance's space to the world, by its transform's rows (3 x 4).
void to_world(const float rows[12], const float p[3], float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = rows[i * 4] * p[0] + rows[i * 4 + 1] * p[1] + rows[i * 4 + 2] * p[2] + rows[i * 4 + 3];
}

// A world point to the instance's space (a rigid transform: its rotation's transpose).
void to_local(const float rows[12], const float p[3], float out[3]) {
  const float d[3] = {p[0] - rows[3], p[1] - rows[7], p[2] - rows[11]};
  for (int i = 0; i < 3; ++i) out[i] = rows[i] * d[0] + rows[4 + i] * d[1] + rows[8 + i] * d[2];
}

std::shared_ptr<const runtime::Ground> ground_of(foundry_runtime *rt, foundry_instance instance, float rows[12]) {
  if (!rt) return nullptr;
  auto b = rt->world.built(instance, rows);
  return b ? b->ground : nullptr;
}

// The axial cell holding a point (pointy-top, circumradius 1 m).
void cell_of(float x, float z, int32_t &q, int32_t &r) {
  const float fr = z / 1.5f, fq = x / 1.7320508f - fr * 0.5f, fs = -fq - fr;
  int32_t rq = static_cast<int32_t>(std::lround(fq)), rr = static_cast<int32_t>(std::lround(fr));
  const int32_t rs = static_cast<int32_t>(std::lround(fs));
  const float eq = std::fabs(rq - fq), er = std::fabs(rr - fr), es = std::fabs(rs - fs);
  if (eq > er && eq > es) rq = -rr - rs;
  else if (er > es) rr = -rq - rs;
  q = rq;
  r = rr;
}

}  // namespace

extern "C" {

uint32_t foundry_instance_figures(foundry_runtime *rt, foundry_instance instance, foundry_figure *out, uint32_t capacity) {
  if (!rt) return 0;
  float rows[12];
  auto b = rt->world.built(instance, rows);
  if (!b) return 0;
  const float turn = std::atan2(rows[2], rows[0]) * 57.29578f;  // the instance's own yaw, degrees
  for (uint32_t i = 0; out && i < capacity && i < b->figures.size(); ++i) {
    const runtime::Figure &f = b->figures[i];
    foundry_figure &o = out[i];
    std::memset(&o, 0, sizeof o);
    to_world(rows, f.position, o.position);
    o.yaw = f.yaw + turn;
    o.kind = f.kind;
    std::strncpy(o.species, f.species.c_str(), sizeof o.species - 1);
    o.age = f.age;
    o.height = f.height;
    o.seed = f.seed;
  }
  return static_cast<uint32_t>(b->figures.size());
}

bool foundry_ground_cell(foundry_runtime *rt, foundry_instance instance, int32_t q, int32_t r, foundry_cell *cell,
                         foundry_span *spans, uint32_t capacity) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return false;
  const int32_t c = g->cell(q, r);
  if (c < 0) return false;
  const uint32_t a = g->span_first[size_t(c)], b = g->span_first[size_t(c) + 1];
  if (cell) {
    cell->spans = b - a;
    cell->flags = g->flags[size_t(c)];
    cell->water = g->water[size_t(c)];
  }
  for (uint32_t k = a; spans && k < b && k - a < capacity; ++k)
    spans[k - a] = {g->span_lo[k], g->span_hi[k], static_cast<foundry_material>(g->span_material[k])};
  return true;
}

bool foundry_ground_height(foundry_runtime *rt, foundry_instance instance, float x, float z, float *y) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return false;
  const float p[3] = {x, 0, z};
  float l[3];
  to_local(rows, p, l);
  int32_t q, r;
  cell_of(l[0], l[2], q, r);
  const int32_t c = g->cell(q, r);
  if (c < 0) return false;
  const float top[3] = {l[0], g->span_hi[g->span_first[size_t(c) + 1] - 1] * 0.125f, l[2]};
  float w[3];
  to_world(rows, top, w);
  if (y) *y = w[1];
  return true;
}

uint32_t foundry_ground_roads(foundry_runtime *rt, foundry_instance instance, int32_t *cells, uint32_t cell_capacity,
                              uint32_t *counts, uint32_t count_capacity) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return 0;
  for (uint32_t i = 0; cells && i < cell_capacity && i < g->road_cells.size(); ++i) cells[i] = g->road_cells[i];
  for (uint32_t i = 0; counts && i < count_capacity && i < g->road_counts.size(); ++i) counts[i] = g->road_counts[i];
  return static_cast<uint32_t>(g->road_counts.size());
}

uint32_t foundry_ground_bridges(foundry_runtime *rt, foundry_instance instance, foundry_bridge *out, uint32_t capacity) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return 0;
  for (uint32_t i = 0; out && i < capacity && i < g->bridges.size(); ++i) {
    to_world(rows, g->bridges[i].a, out[i].a);
    to_world(rows, g->bridges[i].b, out[i].b);
    out[i].width = g->bridges[i].width;
  }
  return static_cast<uint32_t>(g->bridges.size());
}

uint32_t foundry_ground_nav(foundry_runtime *rt, foundry_instance instance, foundry_nav_sizes *sizes, float *vertices,
                            uint32_t *polygons, uint32_t *cells, uint32_t *links) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return 0;
  if (sizes) *sizes = {static_cast<uint32_t>(g->nav_vertices.size() / 3), static_cast<uint32_t>(g->nav_cells.size()),
                       static_cast<uint32_t>(g->nav_links.size() / 2)};
  if (vertices)
    for (size_t i = 0; i + 2 < g->nav_vertices.size(); i += 3) to_world(rows, &g->nav_vertices[i], &vertices[i]);
  if (polygons) std::copy(g->nav_polys.begin(), g->nav_polys.end(), polygons);
  if (cells) std::copy(g->nav_cells.begin(), g->nav_cells.end(), cells);
  if (links) std::copy(g->nav_links.begin(), g->nav_links.end(), links);
  return static_cast<uint32_t>(g->nav_cells.size());
}

uint32_t foundry_ground_hull(foundry_runtime *rt, foundry_instance instance, foundry_hull_cell *out, uint32_t capacity) {
  float rows[12];
  auto g = ground_of(rt, instance, rows);
  if (!g) return 0;
  const size_t n = g->hull_cells.size() / 2;
  for (size_t i = 0; out && i < capacity && i < n; ++i) {
    foundry_hull_cell &c = out[i];
    c.q = g->hull_cells[2 * i];
    c.r = g->hull_cells[2 * i + 1];
    c.edge = static_cast<foundry_edge>(i < g->hull_edges.size() ? g->hull_edges[i] : 0u);
    c.height = i < g->hull_heights.size() ? g->hull_heights[i] : 0.0f;
    c.rise = i < g->hull_rises.size() ? g->hull_rises[i] : -1.0f;
    c.opening = 0;
    c.opening_width = 0;
    for (size_t k = 0; k + 1 < g->opening_cells.size(); k += 2)
      if (g->opening_cells[k] == c.q && g->opening_cells[k + 1] == c.r) {
        c.opening = (k / 2 < g->opening_kinds.size() ? g->opening_kinds[k / 2] : 0u) + 1;
        c.opening_width = k / 2 < g->opening_widths.size() ? g->opening_widths[k / 2] : 1u;
      }
  }
  return static_cast<uint32_t>(n);
}

}  // extern "C"
