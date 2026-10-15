// Small vector math, seeded randomness, and value noise for this system. Every
// flower system carries its own copy (spine.md: write everything twice), so a
// fork of one never changes another. Only + - * / floor and foundry::det, so
// WebAssembly and native agree to the bit.
#pragma once

#include <cmath>
#include <cstdint>

#include "boundary.h"

namespace flower_grow::flower_math {

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

constexpr float kPi = foundry::det::kPi;
constexpr float kDeg = kPi / 180.0f;

// Rotates v about a unit axis by an angle (Rodrigues).
inline vec3 rotate(vec3 v, vec3 axis, float angle) {
  float c = foundry::det::cos(angle), s = foundry::det::sin(angle);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

// Some unit vector perpendicular to v.
inline vec3 perpendicular(vec3 v) {
  vec3 ref = (v.y < 0.9f && v.y > -0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
  return normalize(cross(v, ref));
}

inline uint32_t hash_str(const char *s, uint32_t h = 2166136261u) {
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}

inline uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

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
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
  float signed_unit() { return uniform() * 2 - 1; }                                    // [-1, 1)
};

// 3D value noise in [0, 1], smooth, from integer hashes.
inline float lattice(int x, int y, int z, uint32_t seed) {
  uint32_t h = hash_u32(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
                        static_cast<uint32_t>(z) * 83492791u ^ seed);
  return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}
inline float noise(vec3 p, uint32_t seed) {
  float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  int x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
  float tx = smooth(p.x - fx), ty = smooth(p.y - fy), tz = smooth(p.z - fz);
  float c00 = mixf(lattice(x, y, z, seed), lattice(x + 1, y, z, seed), tx);
  float c10 = mixf(lattice(x, y + 1, z, seed), lattice(x + 1, y + 1, z, seed), tx);
  float c01 = mixf(lattice(x, y, z + 1, seed), lattice(x + 1, y, z + 1, seed), tx);
  float c11 = mixf(lattice(x, y + 1, z + 1, seed), lattice(x + 1, y + 1, z + 1, seed), tx);
  return mixf(mixf(c00, c10, ty), mixf(c01, c11, ty), tz);
}

}  // namespace flower_grow::flower_math
