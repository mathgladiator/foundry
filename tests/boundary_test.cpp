// Tests for src/common/boundary.h: deterministic math accuracy and the
// little-endian writer/reader. Run with ctest.
#include <cmath>
#include <cstdio>

#include "boundary.h"

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

int main() {
  // det::sin/cos stay close to the C library over a wide range.
  double worst = 0;
  for (int i = -200000; i <= 200000; ++i) {
    float x = static_cast<float>(i) * 1e-4f;
    worst = std::fmax(worst, std::fabs(foundry::det::sin(x) - std::sin(static_cast<double>(x))));
    worst = std::fmax(worst, std::fabs(foundry::det::cos(x) - std::cos(static_cast<double>(x))));
  }
  std::printf("det sin/cos max error on [-20, 20]: %.3g\n", worst);
  CHECK(worst < 2e-6);

  // Round trip: scalars, a padded string, vectors, and arrays.
  std::vector<uint8_t> bytes;
  foundry::Writer w(bytes);
  w.u32(42);
  w.str("crate");  // 5 bytes, padded to 8
  w.f32(0.5f);
  w.vec({1, 2, 3});
  std::vector<uint32_t> list = {7, 8, 9};
  w.array(list, [](foundry::Writer &w2, uint32_t v) { w2.u32(v); });
  w.boolean(true);
  CHECK(bytes.size() % 4 == 0);

  foundry::Reader r(bytes.data(), bytes.size());
  CHECK(r.u32() == 42);
  CHECK(r.str() == "crate");
  CHECK(r.f32() == 0.5f);
  foundry::vec3 v = r.vec();
  CHECK(v.x == 1 && v.y == 2 && v.z == 3);
  std::vector<uint32_t> back;
  r.array(back, 4, [](foundry::Reader &r2, uint32_t &x) { x = r2.u32(); });
  CHECK(back == list);
  CHECK(r.boolean());
  CHECK(r.ok());

  // Truncated input is rejected rather than misread.
  foundry::Reader t(bytes.data(), bytes.size() - 6);
  t.u32();
  t.str();
  t.f32();
  t.vec();
  std::vector<uint32_t> partial;
  t.array(partial, 4, [](foundry::Reader &r2, uint32_t &x) { x = r2.u32(); });
  t.boolean();
  CHECK(!t.ok());

  std::printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
