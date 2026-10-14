// Tests for the play test's walker (src/host/physics/walker.h): it falls to the floor, walks at its
// speed, stops at a solid trunk, passes through a soft bush, climbs a step, and jumps. Run with ctest.
#include <cmath>
#include <cstdio>

#include "physics/walker.h"

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

using physics::Walker;

static void run(Walker &w, float seconds, const Walker::Input &in, float *peak = nullptr) {
  Walker::Input step = in;
  for (int i = 0; i < static_cast<int>(seconds * 60); ++i) {
    w.step(1.0f / 60, step);
    step.jump = false;
    float f[3];
    w.feet(f);
    if (peak) *peak = std::fmax(*peak, f[1]);
  }
}

static runtime::CollisionBody trunk(uint32_t purpose) {
  runtime::CollisionBody b;
  b.purpose = purpose;
  runtime::CollisionShape s;
  s.type = runtime::CollisionShape::kCylinder;
  s.radius = 0.4f;
  s.half_height = 2;
  s.position[1] = 2;
  b.shapes.push_back(s);
  return {b};
}

int main() {
  float f[3];
  {  // falls to the floor, then walks north at its walking pace
    Walker w;
    w.add_floor(0);
    const float at[3] = {0, 3, 0};
    w.spawn(at);
    run(w, 1.5f, {});
    w.feet(f);
    CHECK(std::fabs(f[1]) < 0.05f);
    CHECK(w.grounded());
    Walker::Input in;
    in.forward = 1;
    run(w, 1, in);
    w.feet(f);
    std::printf("walked %.2f m north in a second\n", -f[2]);
    CHECK(-f[2] > 3.5f && -f[2] < 4.5f);
    CHECK(std::fabs(f[0]) < 0.05f);
  }
  {  // a solid trunk stops it; a soft one doesn't
    for (uint32_t purpose : {runtime::CollisionBody::kSolid, runtime::CollisionBody::kSoft}) {
      Walker w;
      w.add_floor(0);
      const float tree_at[3] = {0, 0, -3};
      w.add_bodies({trunk(purpose)}, tree_at, 0);
      const float at[3] = {0, 0.05f, 0};
      w.spawn(at);
      Walker::Input in;
      in.forward = 1;
      run(w, 2, in);
      w.feet(f);
      std::printf("%s trunk: stood at z %.2f\n", purpose ? "soft" : "solid", f[2]);
      if (purpose == runtime::CollisionBody::kSolid) CHECK(f[2] > -3 + 0.4f + Walker::kRadius - 0.1f);
      else CHECK(f[2] < -5);
    }
  }
  {  // climbs a 0.3 m step, not a 1 m wall; jumps about 1.2 m
    for (float rise : {0.3f, 1.0f}) {
      Walker w;
      w.add_floor(0);
      const float t[] = {-5, rise, -2, 5, rise, -2, 5, rise, -20, -5, rise, -2, 5, rise, -20, -5, rise, -20,
                         -5, 0, -2, 5, 0, -2, 5, rise, -2, -5, 0, -2, 5, rise, -2, -5, rise, -2};
      w.add_triangles(std::vector<float>(t, t + sizeof t / sizeof *t));
      const float at[3] = {0, 0.05f, 0};
      w.spawn(at);
      Walker::Input in;
      in.forward = 1;
      run(w, 2, in);
      w.feet(f);
      std::printf("a %.1f m rise: stood at y %.2f, z %.2f\n", rise, f[1], f[2]);
      if (rise < 0.5f) CHECK(std::fabs(f[1] - rise) < 0.05f && f[2] < -4);
      else CHECK(f[1] < 0.05f && f[2] > -2);
    }
    Walker w;
    w.add_floor(0);
    const float at[3] = {0, 0.05f, 0};
    w.spawn(at);
    run(w, 0.5f, {});
    Walker::Input in;
    in.jump = true;
    float peak = 0;
    run(w, 1.5f, in, &peak);
    w.feet(f);
    std::printf("jumped %.2f m\n", peak);
    CHECK(peak > 0.9f && peak < 1.6f);
    CHECK(std::fabs(f[1]) < 0.05f && w.grounded());
  }
  {  // starts at the middle in the first place it fits: beside a trunk, out from under a roof, on ground
    Walker w;
    w.add_floor(0);
    const float g[] = {-10, 1, -10, -10, 1, 10, 10, 1, 10, -10, 1, -10, 10, 1, 10, 10, 1, -10};  // ground at 1 m
    w.add_triangles(std::vector<float>(g, g + sizeof g / sizeof *g));
    const float tree_at[3] = {0, 1, 0};
    w.add_bodies({trunk(runtime::CollisionBody::kSolid)}, tree_at, 0);
    CHECK(w.find_spawn(0, 0, 8, false, f));
    std::printf("beside a trunk: started at %.2f, %.2f, %.2f\n", f[0], f[1], f[2]);
    CHECK(std::hypot(f[0], f[2]) > 0.4f + Walker::kRadius && std::hypot(f[0], f[2]) < 1.6f && std::fabs(f[1] - 1.1f) < 0.05f);
    const float roof[] = {-3, 4, -3, -3, 4, 3, 3, 4, 3, -3, 4, -3, 3, 4, 3, 3, 4, -3};  // a flat roof over the middle
    w.add_triangles(std::vector<float>(roof, roof + sizeof roof / sizeof *roof));
    CHECK(w.find_spawn(0, 0, 8, false, f));
    std::printf("under a roof: started at %.2f, %.2f, %.2f\n", f[0], f[1], f[2]);
    CHECK(std::fmax(std::fabs(f[0]), std::fabs(f[2])) > 3 && std::fabs(f[1] - 1.1f) < 0.05f);
    CHECK(!w.find_spawn(30, 0, 5, false, f));  // only the floor out there
    const bool floor_fit = w.find_spawn(30, 0, 5, true, f);
    std::printf("on the floor: %d at %.2f, %.2f, %.2f\n", floor_fit, f[0], f[1], f[2]);
    CHECK(floor_fit && std::fabs(f[1] - 0.1f) < 0.05f);
  }
  if (failures) std::printf("%d failed\n", failures);
  else std::printf("ok\n");
  return failures ? 1 : 0;
}
