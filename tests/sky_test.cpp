// Tests for src/runtime/sky: the sun, stars, and moons where the sky says they
// are (against known positions), the light it gives, and its bakes. Run with ctest.
#include <cmath>
#include <cstdio>

#include "sky/sky.h"

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

using namespace runtime::sky;

static float altitude(const float d[3]) { return std::asin(d[1]) * 57.29578f; }

int main() {
  Observatory earth;
  State s;
  // The equinox at the equator: the sun overhead at noon, rising due east, setting due west.
  s.year = earth.equinox;
  s.latitude_deg = 0;
  s.time_of_day = 0.5f;
  Frame f = compute(earth, s);
  std::printf("equinox noon at the equator: altitude %.2f\n", f.sun_altitude_deg);
  CHECK(f.sun_altitude_deg > 89.5f);
  s.time_of_day = 0.25f;
  f = compute(earth, s);
  std::printf("equinox 6:00: sun (%.3f, %.3f, %.3f)\n", f.sun[0], f.sun[1], f.sun[2]);
  CHECK(std::fabs(f.sun[1]) < 0.01f && f.sun[0] > 0.99f);  // east is +X
  s.time_of_day = 0.75f;
  f = compute(earth, s);
  CHECK(std::fabs(f.sun[1]) < 0.01f && f.sun[0] < -0.99f);

  // 45 degrees north: noon altitude 90 - 45 + declination, and the noon sun in the south (+Z).
  s.latitude_deg = 45;
  s.time_of_day = 0.5f;
  s.year = earth.equinox + 0.25f;  // the June solstice
  f = compute(earth, s);
  std::printf("June solstice noon at 45 N: altitude %.2f (expect 68.44)\n", f.sun_altitude_deg);
  CHECK(std::fabs(f.sun_altitude_deg - 68.44f) < 0.2f);
  CHECK(f.sun[2] > 0 && std::fabs(f.sun[0]) < 1e-3f);
  s.year = earth.equinox + 0.75f;
  f = compute(earth, s);
  std::printf("December solstice noon at 45 N: altitude %.2f (expect 21.56)\n", f.sun_altitude_deg);
  CHECK(std::fabs(f.sun_altitude_deg - 21.56f) < 0.2f);
  // Southern hemisphere: the noon sun in the north (-Z).
  s.latitude_deg = -35;
  f = compute(earth, s);
  CHECK(f.sun[2] < 0);

  // Polaris (RA 2.53 h, Dec 89.26) stands at the latitude's altitude, due north, at any hour.
  s.latitude_deg = 52;
  for (float t : {0.0f, 0.3f, 0.7f}) {
    s.time_of_day = t;
    f = compute(earth, s);
    // The star's world direction: the chart's transpose of to_stars.
    const float ra = 2.53f * 15 * 0.017453293f, dec = 89.26f * 0.017453293f;
    const float e[3] = {std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
    float w[3];
    for (int i = 0; i < 3; ++i) w[i] = f.to_stars[0 * 3 + i] * e[0] + f.to_stars[1 * 3 + i] * e[1] + f.to_stars[2 * 3 + i] * e[2];
    std::printf("Polaris at 52 N, time %.2f: altitude %.2f, toward (%.3f, %.3f)\n", t, altitude(w), w[0], w[2]);
    CHECK(std::fabs(altitude(w) - 52) < 0.8f);
    CHECK(w[2] < 0);  // north is -Z
  }

  // The moon: full when opposite the sun, new beside it, about every 29.5 days.
  Moon moon;
  moon.phase = 0.8467f;
  earth.moons.push_back(moon);
  s.latitude_deg = 45;
  s.time_of_day = 0;
  float best_full = 0, best_new = 1;
  int full_day = -1, first_full = -1, second_full = -1;
  float last = -1, before = -1;
  for (int day = 0; day < 70; ++day) {
    s.year = (day + 0.5f) / earth.year_days;
    f = compute(earth, s);
    const float lit = f.moons[0].lit;
    if (lit > best_full) {
      best_full = lit;
      full_day = day;
    }
    best_new = std::fmin(best_new, lit);
    if (before >= 0 && last > before && last > lit && last > 0.95f) (first_full < 0 ? first_full : second_full) = day - 1;
    before = last;
    last = lit;
  }
  std::printf("moon: fullest %.3f on day %d, darkest %.3f; full on days %d and %d\n", best_full, full_day, best_new, first_full,
              second_full);
  CHECK(best_full > 0.99f && best_new < 0.01f);
  CHECK(first_full >= 10 && first_full <= 15);  // full moon about January 13
  CHECK(second_full - first_full >= 29 && second_full - first_full <= 30);

  // The light: redder at sunset than at noon; the eye opens at night.
  earth.moons.clear();
  s.year = 0.47f;
  s.latitude_deg = 45;
  s.time_of_day = 0.5f;
  const Frame noon = compute(earth, s);
  // The time the sun is 3 degrees up in the evening.
  Frame low = noon;
  for (float t = 0.5f; t < 1.0f; t += 0.0005f) {
    s.time_of_day = t;
    low = compute(earth, s);
    if (low.sun_altitude_deg < 3) break;
  }
  s.time_of_day = 0.0f;
  const Frame night = compute(earth, s);
  std::printf("key at noon (%.2f %.2f %.2f), at 3 degrees (%.2f %.2f %.2f); ambient at noon (%.2f %.2f %.2f); exposure noon %.2f night %.2f\n",
              noon.key[0], noon.key[1], noon.key[2], low.key[0], low.key[1], low.key[2], noon.ambient[0], noon.ambient[1],
              noon.ambient[2], noon.exposure, night.exposure);
  CHECK(low.key[0] / low.key[2] > 1.5f * noon.key[0] / noon.key[2]);
  CHECK(noon.ambient[2] > noon.ambient[0]);  // the sky's light is blue
  CHECK(night.exposure > 4 * noon.exposure);
  CHECK(noon.key[1] > 0.6f && noon.key[1] < 1.3f);

  // From noon to a full-moon midnight, what the eye sees ((key + ambient) times the exposure) only
  // darkens until the moon rises (no dip at dusk waiting for its light), and the moonlit midnight
  // stays darker than sunset.
  {
    Observatory lit = earth;
    lit.moons.push_back(moon);
    State d = s;
    d.year = 12.5f / lit.year_days;  // the full moon about January 13
    d.latitude_deg = 45;
    auto lum = [](const float *c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    float prev = 1e9f, worst_rise = 0, sunset = 0, noon_seen = 0, midnight = 0;
    for (float t = 0.5f; t <= 1.0f; t += 0.0025f) {
      d.time_of_day = std::fmin(t, 0.9999f);
      const Frame fr = compute(lit, d);
      const float seen = (lum(fr.key) + lum(fr.ambient)) * fr.exposure;
      if (t == 0.5f) noon_seen = seen;
      if (sunset == 0 && fr.sun_altitude_deg < 0) sunset = seen;
      if (fr.moons[0].dir[1] < 0 && prev < 1e9f) worst_rise = std::fmax(worst_rise, seen / prev);
      prev = midnight = seen;
    }
    std::printf("noon %.2f, sunset %.2f, full-moon midnight %.2f; before moonrise the worst step up is x%.3f\n", noon_seen,
                sunset, midnight, worst_rise);
    CHECK(worst_rise < 1.01f);
    CHECK(midnight < sunset && midnight < 0.3f * noon_seen);
  }

  // A seed always bakes the same chart; another seed bakes another.
  {
    Observatory a = earth;
    a.stars = 3000;
    std::vector<float> one, two, other;
    bake_stars(a, 64, one);
    bake_stars(a, 64, two);
    a.star_seed = 8;
    bake_stars(a, 64, other);
    size_t differ = 0;
    for (size_t i = 0; i < one.size(); ++i) differ += one[i] != other[i];
    CHECK(one == two);
    CHECK(differ > one.size() / 50);
  }

  // The cloud map tiles: its first and last columns meet.
  std::vector<uint8_t> rg;
  bake_clouds(3, 128, rg);
  int worst = 0;
  for (int j = 0; j < 128; ++j) worst = std::max(worst, std::abs(int(rg[(j * 128 + 0) * 2]) - int(rg[(j * 128 + 127) * 2])));
  std::printf("cloud map seam: %d\n", worst);
  CHECK(worst < 40);

  if (failures) std::printf("%d failures\n", failures);
  else std::printf("ok\n");
  return failures ? 1 : 0;
}
