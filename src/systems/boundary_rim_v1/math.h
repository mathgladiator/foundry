// Small vector math, seeded hashes, and value noise for this system. Every system carries its own
// copy (spine.md: write everything twice), so a fork of one never changes another. Only + - * /,
// floor, and foundry::det, so WebAssembly and native agree to the bit.
#pragma once

#include <cmath>
#include <cstdint>

#include "boundary.h"

namespace boundary_rim_v1::bmath {

using foundry::vec3;

inline vec3 v3(float x, float y, float z) { return vec3{x, y, z}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
inline vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline vec3 operator*(float s, vec3 a) { return a * s; }
inline vec3 mul(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(vec3 a) { return foundry::det::sqrt(dot(a, a)); }
inline vec3 normalize(vec3 a) {
  float l = length(a);
  return l > 1e-12f ? a * (1.0f / l) : v3(0, 1, 0);
}
inline vec3 lerp(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float smooth(float t) { return t * t * (3 - 2 * t); }
inline float absf(float v) { return v < 0 ? -v : v; }
inline int maxi_(int a, int b) { return a > b ? a : b; }
inline int mini_(int a, int b) { return a < b ? a : b; }
inline int absi_(int a) { return a < 0 ? -a : a; }
inline int round_to_int(float v) { return static_cast<int>(std::floor(v + 0.5f)); }
// 0 below a, 1 above b, smooth between.
inline float step_between(float a, float b, float v) { return smooth(clampf((v - a) / (b - a), 0, 1)); }

constexpr float kPi = foundry::det::kPi;
constexpr float kSqrt3 = 1.7320508f;

// 2^x, for x in a game's range, from a polynomial on the fraction (exact enough for falloffs).
inline float exp2_(float x) {
  x = clampf(x, -60, 60);
  const float fi = std::floor(x);
  const float f = x - fi;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  int i = static_cast<int>(fi);
  while (i > 0) p *= 2.0f, --i;
  while (i < 0) p *= 0.5f, ++i;
  return p;
}
inline float exp_(float x) { return exp2_(x * 1.4426950f); }

// atan2 from a polynomial (about 1e-4 rad), in (-pi, pi].
inline float atan2_(float y, float x) {
  const float ax = absf(x), ay = absf(y);
  if (ax < 1e-20f && ay < 1e-20f) return 0;
  const float a = minf(ax, ay) / maxf(ax, ay), s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = kPi - r;
  if (y < 0) r = -r;
  return r;
}

inline uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
inline uint32_t hash2(int x, int y, uint32_t seed) {
  return hash_u32(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed * 83492791u);
}
inline float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }  // [0, 1)

// PCG32. One stream per knob, so adding a knob never reshuffles the others.
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
  float uniform() { return unit(next()); }               // [0, 1)
  float signed_unit() { return uniform() * 2 - 1; }      // [-1, 1)
};

// 2D value noise in [0, 1], smooth.
inline float noise2(float x, float y, uint32_t seed) {
  const float fx = std::floor(x), fy = std::floor(y);
  const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
  const float tx = smooth(x - fx), ty = smooth(y - fy);
  const float a = unit(hash2(ix, iy, seed)), b = unit(hash2(ix + 1, iy, seed));
  const float c = unit(hash2(ix, iy + 1, seed)), d = unit(hash2(ix + 1, iy + 1, seed));
  return mixf(mixf(a, b, tx), mixf(c, d, tx), ty);
}
// Fractal sums of it, in [0, 1]: octaves halving in size and in weight.
inline float fbm(float x, float y, int octaves, uint32_t seed) {
  float sum = 0, amp = 0.5f, total = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += noise2(x, y, seed + static_cast<uint32_t>(o) * 101u) * amp;
    total += amp;
    // Turn each octave a little so their grids don't line up.
    const float nx = x * 1.6f - y * 1.2f, ny = x * 1.2f + y * 1.6f;
    x = nx + 17.3f;
    y = ny - 9.1f;
    amp *= 0.5f;
  }
  return sum / total;
}
// Ridged: sharp crests where the noise crosses its middle, in [0, 1].
inline float ridged(float x, float y, int octaves, uint32_t seed) {
  float sum = 0, amp = 0.5f, total = 0, weight = 1;
  for (int o = 0; o < octaves; ++o) {
    float n = 1.0f - absf(noise2(x, y, seed + static_cast<uint32_t>(o) * 131u) * 2 - 1);
    n *= n;
    sum += n * amp * weight;
    weight = clampf(n * 1.5f, 0.3f, 1.0f);
    total += amp;
    const float nx = x * 1.7f - y * 1.1f, ny = x * 1.1f + y * 1.7f;
    x = nx - 5.7f;
    y = ny + 13.9f;
    amp *= 0.5f;
  }
  return sum / total;
}
// Cellular noise: the nearest and second-nearest feature points of a jittered grid (distances in
// cells), and the nearest's hash.
struct Cell2 {
  float d1 = 9, d2 = 9;
  uint32_t id = 0;
};
inline Cell2 cells2(float x, float y, uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
  Cell2 c;
  for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
      const uint32_t h = hash2(ix + i, iy + j, seed);
      const float px = static_cast<float>(ix + i) + 0.15f + 0.7f * unit(h);
      const float py = static_cast<float>(iy + j) + 0.15f + 0.7f * unit(hash_u32(h ^ 0x9e3779b9u));
      const float dx = px - x, dy = py - y;
      const float d = foundry::det::sqrt(dx * dx + dy * dy);
      if (d < c.d1) {
        c.d2 = c.d1;
        c.d1 = d;
        c.id = h;
      } else if (d < c.d2) {
        c.d2 = d;
      }
    }
  return c;
}

// Hex cells (design/plotter.md, "Coordinates"): pointy-top, circumradius 1 m.
inline float hex_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + static_cast<float>(r) * 0.5f); }
inline float hex_z(int r) { return 1.5f * static_cast<float>(r); }
inline int hex_distance(int q0, int r0, int q1, int r1) {
  const int dq = q1 - q0, dr = r1 - r0;
  return (absi_(dq) + absi_(dr) + absi_(dq + dr)) / 2;
}

}  // namespace boundary_rim_v1::bmath
