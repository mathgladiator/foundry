// Lots for this system's buildings: brick-laid rectangles of hex cells (rectWxH: H rows of W cells,
// row r starting at q = -floor(r / 2)) or flowers (every cell within `rings` of the middle), each
// anchored at the cell nearest its middle. Each system carries its own copy (spine.md).
#pragma once

#include <cstdint>
#include <vector>

#include "math.h"

namespace crop_build_v1 {

using namespace crop_math;

constexpr float kSqrt3 = 1.7320508f;
inline float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
inline float cell_z(int r) { return 1.5f * static_cast<float>(r); }
inline int hex_dist(int q, int r) {
  const int s = -q - r;
  const int a = q < 0 ? -q : q, b = r < 0 ? -r : r, c = s < 0 ? -s : s;
  return (a + b + c) / 2;
}

struct Lot {
  std::vector<int> cells;  // q, r pairs relative to the anchor (the origin)
  float cx = 0, cz = 0, hx = 0, hz = 0;  // a rectangle inside the cells, shrunk by 0.1 m
  uint32_t round = 7;                    // the round footprint that holds it (7, 19, 37, ... cells)
  float round_r = 2.5f;                  // its inner circle's radius, shrunk by 0.1 m
};

// w > 0: rect w x h; w == 0: a flower of h rings.
inline Lot make_lot(int w, int h) {
  Lot l;
  std::vector<int> raw;
  if (w > 0) {
    for (int r = 0; r < h; ++r)
      for (int i = 0; i < w; ++i) {
        raw.push_back(i - r / 2);
        raw.push_back(r);
      }
  } else {
    for (int q = -h; q <= h; ++q)
      for (int r = -h; r <= h; ++r)
        if (hex_dist(q, r) <= h) {
          raw.push_back(q);
          raw.push_back(r);
        }
  }
  // The anchor: the cell nearest the cells' middle (the first such, in order).
  float mx = 0, mz = 0;
  const int n = static_cast<int>(raw.size() / 2);
  for (int k = 0; k < n; ++k) {
    mx += cell_x(raw[2 * k], raw[2 * k + 1]);
    mz += cell_z(raw[2 * k + 1]);
  }
  mx /= static_cast<float>(n);
  mz /= static_cast<float>(n);
  int best = 0;
  float best_d = 1e30f;
  for (int k = 0; k < n; ++k) {
    const float dx = cell_x(raw[2 * k], raw[2 * k + 1]) - mx, dz = cell_z(raw[2 * k + 1]) - mz;
    if (dx * dx + dz * dz < best_d - 1e-6f) {
      best_d = dx * dx + dz * dz;
      best = k;
    }
  }
  const int aq = raw[2 * best], ar = raw[2 * best + 1];
  int rings = 0;
  for (int k = 0; k < n; ++k) {
    l.cells.push_back(raw[2 * k] - aq);
    l.cells.push_back(raw[2 * k + 1] - ar);
    rings = maxi_(rings, hex_dist(raw[2 * k] - aq, raw[2 * k + 1] - ar));
  }
  l.round = static_cast<uint32_t>(3 * rings * (rings + 1) + 1);
  l.round_r = (static_cast<float>(rings) + 0.5f) * kSqrt3 - 0.1f;
  // The rectangle: x inside every row's run of cells, z between the first and last rows' straight sides.
  float xl = -1e30f, xr = 1e30f, z0 = 1e30f, z1 = -1e30f;
  for (int r = -40; r <= 40; ++r) {
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < n; ++k)
      if (l.cells[2 * k + 1] == r) {
        lo = minf(lo, cell_x(l.cells[2 * k], r));
        hi = maxf(hi, cell_x(l.cells[2 * k], r));
      }
    if (lo > hi) continue;
    if (w == 0 && (r < -rings / 2 || r > rings / 2)) continue;  // a flower's: its middle rows
    xl = maxf(xl, lo - 0.45f);
    xr = minf(xr, hi + 0.45f);
    z0 = minf(z0, cell_z(r) - 0.5f);
    z1 = maxf(z1, cell_z(r) + 0.5f);
  }
  l.cx = (xl + xr) * 0.5f;
  l.cz = (z0 + z1) * 0.5f;
  l.hx = (xr - xl) * 0.5f - 0.1f;
  l.hz = (z1 - z0) * 0.5f - 0.1f;
  return l;
}

// The cells just outside a door at x on the lot's front (+z): the front-row cell nearest x, and its
// front neighbours beyond the lot.
inline std::vector<int> door_cells(const Lot &l, float x) {
  int front_r = -1000;
  for (size_t k = 0; k + 1 < l.cells.size(); k += 2) front_r = maxi_(front_r, l.cells[k + 1]);
  int best = -1;
  float best_d = 1e30f;
  for (size_t k = 0; k + 1 < l.cells.size(); k += 2) {
    if (l.cells[k + 1] != front_r) continue;
    const float d = absf(cell_x(l.cells[k], l.cells[k + 1]) - x);
    if (d < best_d - 1e-4f) {
      best_d = d;
      best = static_cast<int>(k);
    }
  }
  std::vector<int> out;
  if (best < 0) return out;
  for (int d = 0; d < 2; ++d) {
    const int q = l.cells[static_cast<size_t>(best)] + (d == 0 ? -1 : 0), r = front_r + 1;
    bool inside = false;
    for (size_t k = 0; k + 1 < l.cells.size(); k += 2) inside = inside || (l.cells[k] == q && l.cells[k + 1] == r);
    if (inside) continue;
    out.push_back(q);
    out.push_back(r);
  }
  return out;
}

// The lot's box on the ground: its cells' bounds (a cell reaches sqrt(3) / 2 across, 1 along z).
inline void lot_box(const Lot &l, float top, vec3 &lo, vec3 &hi) {
  float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
  for (size_t k = 0; k + 1 < l.cells.size(); k += 2) {
    const float x = cell_x(l.cells[k], l.cells[k + 1]), z = cell_z(l.cells[k + 1]);
    x0 = minf(x0, x - 0.5f * kSqrt3);
    x1 = maxf(x1, x + 0.5f * kSqrt3);
    z0 = minf(z0, z - 1.0f);
    z1 = maxf(z1, z + 1.0f);
  }
  lo = v3(x0, 0, z0);
  hi = v3(x1, top, z1);
}

}  // namespace crop_build_v1
