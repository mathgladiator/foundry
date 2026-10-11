// boundary.h: small header-only helpers for packing and unpacking channels.
//
// Shared by systems (compiled to WebAssembly and natively), the generated
// packers and disk formats, and the host. Everything is little-endian, with
// 4-byte alignment, as described in design/flow.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace foundry {

struct vec3 {
  float x = 0, y = 0, z = 0;
};

// FNV-1a, used to tag packed channels with their type.
constexpr uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}

constexpr uint32_t kChannelMagic = 0x43594446u;  // "FDYC"
constexpr uint32_t kDiskMagic = 0x44594446u;     // "FDYD"
constexpr uint32_t kDiskFormatVersion = 1;

// Deterministic math. C library functions such as sinf and hypotf may differ
// in the last bit between glibc (native) and wasi-libc (WebAssembly), which
// would make the native build's output differ from what was approved in
// WebAssembly. These use only + - * and floor, which are exact IEEE operations
// on every target (with -ffp-contract=off, which the build sets).
namespace det {

constexpr float kPi = 3.14159265358979f;

// sin and cos of an angle already reduced to [-pi/4, pi/4].
inline float sin_kernel(float x) {
  float x2 = x * x;
  return x * (1.0f + x2 * (-1.6666667e-1f + x2 * (8.3333338e-3f + x2 * (-1.9841270e-4f + x2 * 2.7557319e-6f))));
}
inline float cos_kernel(float x) {
  float x2 = x * x;
  return 1.0f + x2 * (-0.5f + x2 * (4.1666668e-2f + x2 * (-1.3888889e-3f + x2 * (2.4801587e-5f - x2 * 2.7557319e-7f))));
}

// Splits x into an octant quadrant q and a remainder in [-pi/4, pi/4].
inline float reduce(float x, int &q) {
  const float k = 0.63661977f;  // 2 / pi
  float n = std::floor(x * k + 0.5f);
  q = static_cast<int>(static_cast<long long>(n) & 3);
  // pi/2 split into two parts for a more accurate remainder.
  return (x - n * 1.5703125f) - n * 4.8382679e-4f;
}

inline float sin(float x) {
  int q;
  float r = reduce(x, q);
  switch (q) {
    case 0: return sin_kernel(r);
    case 1: return cos_kernel(r);
    case 2: return -sin_kernel(r);
    default: return -cos_kernel(r);
  }
}

inline float cos(float x) {
  int q;
  float r = reduce(x, q);
  switch (q) {
    case 0: return cos_kernel(r);
    case 1: return -sin_kernel(r);
    case 2: return -cos_kernel(r);
    default: return sin_kernel(r);
  }
}

inline float sqrt(float x) { return std::sqrt(x); }  // correctly rounded everywhere
inline float hypot(float a, float b) { return std::sqrt(a * a + b * b); }

}  // namespace det

class Writer {
 public:
  explicit Writer(std::vector<uint8_t> &out) : out_(out) {}
  void u32(uint32_t v) { raw(&v, 4); }
  void i32(int32_t v) { raw(&v, 4); }
  void f32(float v) { raw(&v, 4); }
  void boolean(bool v) { u32(v ? 1u : 0u); }
  void vec(const vec3 &v) { f32(v.x); f32(v.y); f32(v.z); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    raw(s.data(), s.size());
    pad();
  }
  template <typename T, typename F>
  void array(const std::vector<T> &a, F each) {
    u32(static_cast<uint32_t>(a.size()));
    for (const T &v : a) each(*this, v);
  }
  size_t size() const { return out_.size(); }
  void patch_u32(size_t at, uint32_t v) { std::memcpy(out_.data() + at, &v, 4); }

 private:
  // Assumes a little-endian host, which covers every current target.
  void raw(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    out_.insert(out_.end(), b, b + n);
  }
  void pad() {
    while (out_.size() % 4) out_.push_back(0);
  }
  std::vector<uint8_t> &out_;
};

class Reader {
 public:
  Reader(const uint8_t *data, size_t size)
      : p_(data), end_(data + size), base_(reinterpret_cast<uintptr_t>(data)) {}
  bool ok() const { return ok_; }
  const uint8_t *cursor() const { return p_; }
  uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
  int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
  float f32() { float v = 0; raw(&v, 4); return v; }
  bool boolean() { return u32() != 0; }
  vec3 vec() { vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
  std::string str() {
    uint32_t n = u32();
    if (!check(n)) return {};
    std::string s(reinterpret_cast<const char *>(p_), n);
    p_ += n;
    while (ok_ && (reinterpret_cast<uintptr_t>(p_) - base()) % 4) {
      if (!check(1)) break;
      ++p_;
    }
    return s;
  }
  template <typename T, typename F>
  void array(std::vector<T> &a, size_t min_element_bytes, F each) {
    uint32_t n = u32();
    if (!check(static_cast<size_t>(n) * min_element_bytes)) return;
    a.resize(n);
    for (T &v : a) each(*this, v);
  }

 private:
  uintptr_t base() const { return base_; }
  bool check(size_t n) {
    if (!ok_ || static_cast<size_t>(end_ - p_) < n) {
      ok_ = false;
      return false;
    }
    return true;
  }
  void raw(void *v, size_t n) {
    if (!check(n)) return;
    std::memcpy(v, p_, n);
    p_ += n;
  }
  const uint8_t *p_;
  const uint8_t *end_;
  uintptr_t base_;
  bool ok_ = true;
};

}  // namespace foundry

// ---------------------------------------------------------------------------
// FOUNDRY_SYSTEM(ns, In, Out): declares a system's entry point.
//
// A system is `bool ns::run(const In &in, Out &out, std::string &error)`.
// In WebAssembly, this macro exports the ABI the host calls:
//   foundry_alloc(size) -> pointer to an input buffer of that size
//   foundry_run(ptr, len) -> 0 on success, 1 on a system error, 2 on bad input
//   foundry_output_ptr(), foundry_output_len() -> the packed output, or the
//     error message when foundry_run returned nonzero
// Natively, it defines ns::native_entry(), which does the same with byte
// vectors; it is used by the per-system native runner.
// ---------------------------------------------------------------------------
#if defined(__wasm__)
#define FOUNDRY_SYSTEM(ns, In, Out)                                              \
  namespace ns {                                                                 \
  static std::vector<uint8_t> g_input, g_output;                                 \
  }                                                                              \
  extern "C" __attribute__((export_name("foundry_alloc"))) uint32_t              \
  foundry_alloc(uint32_t size) {                                                 \
    ns::g_input.assign(size, 0);                                                 \
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ns::g_input.data())); \
  }                                                                              \
  extern "C" __attribute__((export_name("foundry_run"))) uint32_t                \
  foundry_run(uint32_t ptr, uint32_t len) {                                      \
    In in;                                                                       \
    Out out;                                                                     \
    std::string error;                                                           \
    ns::g_output.clear();                                                        \
    if (!foundry::unpack(reinterpret_cast<const uint8_t *>(                      \
                             static_cast<uintptr_t>(ptr)), len, in)) {           \
      error = "input does not unpack as " #In;                                   \
      ns::g_output.assign(error.begin(), error.end());                           \
      return 2;                                                                  \
    }                                                                            \
    if (!ns::run(in, out, error)) {                                              \
      ns::g_output.assign(error.begin(), error.end());                           \
      return 1;                                                                  \
    }                                                                            \
    foundry::pack(out, ns::g_output);                                            \
    return 0;                                                                    \
  }                                                                              \
  extern "C" __attribute__((export_name("foundry_output_ptr"))) uint32_t         \
  foundry_output_ptr() {                                                         \
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ns::g_output.data())); \
  }                                                                              \
  extern "C" __attribute__((export_name("foundry_output_len"))) uint32_t         \
  foundry_output_len() {                                                         \
    return static_cast<uint32_t>(ns::g_output.size());                           \
  }
#else
#define FOUNDRY_SYSTEM(ns, In, Out)                                              \
  namespace ns {                                                                 \
  int native_entry(const uint8_t *data, size_t size, std::vector<uint8_t> &output) { \
    In in;                                                                       \
    Out out;                                                                     \
    std::string error;                                                           \
    output.clear();                                                              \
    if (!foundry::unpack(data, size, in)) {                                      \
      error = "input does not unpack as " #In;                                   \
      output.assign(error.begin(), error.end());                                 \
      return 2;                                                                  \
    }                                                                            \
    if (!run(in, out, error)) {                                                  \
      output.assign(error.begin(), error.end());                                 \
      return 1;                                                                  \
    }                                                                            \
    foundry::pack(out, output);                                                  \
    return 0;                                                                    \
  }                                                                              \
  }
#endif
