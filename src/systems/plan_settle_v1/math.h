// Small math, hashing, seeded randomness, and value noise for this system. Each
// system carries its own copy (spine.md: write everything twice). Only + - * /
// floor and foundry::det, so WebAssembly and native agree to the bit.
#pragma once

#include <cstdint>

#include "boundary.h"

namespace plan_settle_v1::plan_math {

constexpr float kPi = foundry::det::kPi;
constexpr float kSqrt3 = 1.7320508f;
constexpr float kCellArea = 2.5980762f;  // a pointy-top hex of circumradius 1, square metres

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float smooth(float t) { return t * t * (3 - 2 * t); }

inline uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
inline uint32_t hash_combine(uint32_t a, uint32_t b) { return hash_u32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2))); }
inline uint32_t hash_cell(int32_t q, int32_t r) {
  return hash_combine(hash_u32(static_cast<uint32_t>(q) * 73856093u), static_cast<uint32_t>(r) * 19349663u);
}
inline uint32_t hash_str(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) {
    h ^= static_cast<uint8_t>(*s);
    h *= 16777619u;
  }
  return h;
}

// PCG32, seeded per stream.
struct Rng {
  uint64_t state = 0, inc = 1;
  Rng(uint32_t seed, uint32_t stream) {
    inc = (static_cast<uint64_t>(stream) << 1) | 1u;
    next();
    state += static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    next();
  }
  uint32_t next() {
    uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    uint32_t x = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    uint32_t r = static_cast<uint32_t>(old >> 59u);
    return (x >> r) | (x << ((32u - r) & 31u));
  }
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
};

// ln(y) for y in (0, 1], as 2 atanh((y - 1) / (y + 1)): a series of + - * / only.
inline float ln_unit(float y) {
  const float t = (y - 1) / (y + 1);
  const float t2 = t * t;
  float term = t, sum = 0;
  for (int k = 1; k < 120; k += 2) {
    sum += term / static_cast<float>(k);
    term *= t2;
  }
  return 2 * sum;
}

// 2D value noise in [0, 1], smooth, from integer hashes.
inline float lattice(int32_t x, int32_t y, uint32_t seed) {
  return static_cast<float>(hash_combine(hash_cell(x, y), seed) >> 8) * (1.0f / 16777216.0f);
}
inline float noise2(float x, float y, uint32_t seed) {
  const float fx = static_cast<float>(static_cast<int32_t>(x >= 0 ? x : x - 1));
  const float fy = static_cast<float>(static_cast<int32_t>(y >= 0 ? y : y - 1));
  const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
  const float u = smooth(x - fx), v = smooth(y - fy);
  const float a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
  const float c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
  const float top = a + (b - a) * u, bottom = c + (d - c) * u;
  return top + (bottom - top) * v;
}

// Axial hex coordinates (pointy-top, circumradius 1 m; design/plotter.md, Coordinates).
inline float cell_x(int32_t q, int32_t r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
inline float cell_z(int32_t r) { return 1.5f * static_cast<float>(r); }
inline int32_t hex_distance(int32_t dq, int32_t dr) {
  const int32_t a = dq < 0 ? -dq : dq, b = dr < 0 ? -dr : dr, c = (dq + dr) < 0 ? -(dq + dr) : (dq + dr);
  return (a + b + c) / 2;
}
inline int32_t round_to_int(float v) { return static_cast<int32_t>(v >= 0 ? v + 0.5f : v - 0.5f); }
// The cell containing a point.
inline void cell_of(float x, float z, int32_t &q, int32_t &r) {
  const float fq = (kSqrt3 / 3.0f * x - z / 3.0f), fr = (2.0f / 3.0f * z), fs = -fq - fr;
  int32_t rq = round_to_int(fq), rr = round_to_int(fr), rs = round_to_int(fs);
  const float dq = rq - fq < 0 ? fq - rq : rq - fq, dr = rr - fr < 0 ? fr - rr : rr - fr, ds = rs - fs < 0 ? fs - rs : rs - fs;
  if (dq > dr && dq > ds) rq = -rr - rs;
  else if (dr > ds) rr = -rq - rs;
  q = rq;
  r = rr;
}
// The ring radius of a footprint of n cells (1 -> 0, 7 -> 1, 19 -> 2, 37 -> 3).
inline int32_t footprint_rings(uint32_t cells) { return cells >= 37 ? 3 : cells >= 19 ? 2 : cells >= 7 ? 1 : 0; }

}  // namespace plan_settle_v1::plan_math
