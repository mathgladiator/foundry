#include "sky/sky.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace runtime::sky {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRad = kPi / 180.0f;
constexpr float kFaintest = 7.6f;  // the background stars' faintest magnitude at density 1

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 0 ? a * (1.0f / l) : V3{0, 1, 0};
}
float lum(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float smoothstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// A 3x3 matrix, row-major.
struct M3 {
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  V3 operator*(V3 v) const {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
  }
};
M3 mul(const M3 &a, const M3 &b) {
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
  return r;
}
M3 transpose(const M3 &a) {
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[j * 3 + i];
  return r;
}

// From Earth's equatorial frame (the star chart's) to the planet's: rows are the planet's axes.
M3 pole_frame(const Observatory &o) {
  const float a = o.pole_ra_deg * kRad, d = o.pole_dec_deg * kRad;
  const V3 z = {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
  V3 x = cross({0, 0, 1}, z);
  x = dot(x, x) < 1e-8f ? V3{1, 0, 0} : normalize(x);
  const V3 y = cross(z, x);
  return {{x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z}};
}

// From the planet's equatorial frame to the world (east, up, south), at a latitude and local
// sidereal time (radians).
M3 horizon_frame(float lat, float lst) {
  const float sl = std::sin(lst), cl = std::cos(lst), sp = std::sin(lat), cp = std::cos(lat);
  return {{-sl, cl, 0, cp * cl, cp * sl, sp, sp * cl, sp * sl, -cp}};
}

// Ecliptic (the planet's orbital plane) to its equatorial frame.
V3 ecliptic_to_equatorial(V3 v, float tilt) {
  const float c = std::cos(tilt), s = std::sin(tilt);
  return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}

// --- the air ---

V3 density(const Air &a, float h) {  // rayleigh, mie, ozone at a height (km)
  return {std::exp(-std::max(h, 0.0f) / a.rayleigh_km), std::exp(-std::max(h, 0.0f) / a.mie_km),
          std::max(0.0f, 1.0f - std::fabs(h - 25.0f) / 15.0f)};
}

// Where a ray from o (planet-centred, km) along d leaves a sphere of radius r; negative: never.
float exit_sphere(V3 o, V3 d, float r) {
  const float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
  if (disc < 0) return -1;
  return -b + std::sqrt(disc);
}
// Where it first meets the planet's surface; negative: it doesn't.
float hit_ground(V3 o, V3 d, float r) {
  const float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
  if (disc < 0) return -1;
  const float t = -b - std::sqrt(disc);
  return t > 0 ? t : -1;
}

void extinction(const Air &a, V3 od, float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = a.rayleigh[i] * od.x + (a.mie[i] + a.mie_absorb[i]) * od.y + a.ozone[i] * od.z;
}

// Optical depth from p to the top of the air toward the sun; false if the planet is in the way.
bool sun_depth(const Air &a, V3 p, V3 sun, V3 &od) {
  const float rp = a.planet_km, top = rp + a.top_km;
  if (hit_ground(p, sun, rp) > 0) return false;
  const float t = exit_sphere(p, sun, top);
  od = {};
  if (t <= 0) return true;
  const int n = 4;
  const float ds = t / n;
  for (int i = 0; i < n; ++i) {
    const V3 q = p + sun * ((i + 0.5f) * ds);
    od = od + density(a, std::sqrt(dot(q, q)) - rp) * ds;
  }
  return true;
}

float phase_rayleigh(float mu) { return 3.0f / (16.0f * kPi) * (1 + mu * mu); }
float phase_mie(float mu, float g) {
  const float g2 = g * g;
  return 3.0f / (8.0f * kPi) * ((1 - g2) * (1 + mu * mu)) / ((2 + g2) * std::pow(std::max(1 + g2 - 2 * g * mu, 1e-4f), 1.5f));
}

void transmittance(const Air &a, V3 dir, float out[3]) {
  const V3 o = {0, a.planet_km + 0.002f, 0};
  V3 od;
  if (!sun_depth(a, o, dir, od)) {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  float e[3];
  extinction(a, od, e);
  for (int i = 0; i < 3; ++i) out[i] = std::exp(-e[i]);
}

// Sky irradiance on flat ground (sun intensity units), over the upper hemisphere.
void irradiance(const Observatory &o, const float sun[3], float out[3]) {
  out[0] = out[1] = out[2] = 0;
  const int rings = 6, around = 12;
  float weight = 0;
  for (int i = 0; i < rings; ++i) {
    const float el = (i + 0.5f) / rings * (kPi * 0.5f);
    const float w = std::cos(el) * std::sin(el);  // solid angle (cos el) times the cosine to the ground's normal
    for (int j = 0; j < around; ++j) {
      const float az = (j + 0.5f) / around * 2 * kPi;
      const float d[3] = {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
      float L[3], T[3];
      radiance(o, d, sun, L, T);
      for (int k = 0; k < 3; ++k) out[k] += L[k] * w;
      weight += w;
    }
  }
  // The integral of cos over the hemisphere is pi; the samples' weights stand for it.
  for (int k = 0; k < 3; ++k) out[k] = out[k] / weight * kPi;
}

// As sky.hlsli's sky_tone: by luminance, keeping the hue, rolling toward white where it overflows.
void tone(const float in[3], float out[3]) {
  float x[3];
  for (int i = 0; i < 3; ++i) x[i] = std::max(in[i], 0.0f);
  const float l = lum(x);
  const float k = (1.0f - std::exp(-1.25f * l)) / std::max(l, 1e-5f);
  float over = 0;
  for (int i = 0; i < 3; ++i) {
    out[i] = x[i] * k;
    over = std::max(over, out[i]);
  }
  if (over > 1) {
    const float w = std::clamp((over - 1) * 0.35f, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) out[i] = out[i] / over * (1 - w) + w;
  }
}

// --- the stars ---

uint32_t hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

struct Rng {
  uint32_t s;
  float next() {
    s = hash(s + 0x9e3779b9u);
    return static_cast<float>(s >> 8) / 16777216.0f;
  }
};

V3 radec(float ra_rad, float dec_rad) {
  return {std::cos(dec_rad) * std::cos(ra_rad), std::cos(dec_rad) * std::sin(ra_rad), std::sin(dec_rad)};
}

// The colour of a star of colour index B-V.
V3 star_color(float bv) {
  struct K {
    float bv;
    V3 c;
  };
  static const K k[] = {{-0.3f, {0.62f, 0.72f, 1.0f}}, {0.0f, {0.80f, 0.86f, 1.0f}}, {0.3f, {1.0f, 1.0f, 1.0f}},
                        {0.8f, {1.0f, 0.93f, 0.78f}},  {1.4f, {1.0f, 0.78f, 0.55f}}, {2.0f, {1.0f, 0.60f, 0.40f}}};
  if (bv <= k[0].bv) return k[0].c;
  for (int i = 1; i < 6; ++i)
    if (bv <= k[i].bv) {
      const float t = (bv - k[i - 1].bv) / (k[i].bv - k[i - 1].bv);
      return k[i - 1].c * (1 - t) + k[i].c * t;
    }
  return k[5].c;
}

// The face and texel coordinates of a direction (the inverse of cube_dir).
void cube_uv(V3 d, int &face, float &u, float &v) {
  const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
  float sc, tc, ma;
  if (ax >= ay && ax >= az) {
    ma = ax;
    face = d.x > 0 ? 0 : 1;
    sc = d.x > 0 ? -d.z : d.z;
    tc = -d.y;
  } else if (ay >= az) {
    ma = ay;
    face = d.y > 0 ? 2 : 3;
    sc = d.x;
    tc = d.y > 0 ? d.z : -d.z;
  } else {
    ma = az;
    face = d.z > 0 ? 4 : 5;
    sc = d.z > 0 ? d.x : -d.x;
    tc = -d.y;
  }
  u = 0.5f * (sc / ma + 1);
  v = 0.5f * (tc / ma + 1);
}

// Adds (or maxes, for the glow) a Gaussian spot at a direction into a cube of floats.
template <int C>
void splat(std::vector<float> &cube, int size, V3 d, float sigma_texels, const float value[C], bool max_blend) {
  int face;
  float u, v;
  cube_uv(normalize(d), face, u, v);
  const float x = u * size - 0.5f, y = v * size - 0.5f;
  const int r = static_cast<int>(std::ceil(sigma_texels * 2.5f));
  const int cx = static_cast<int>(std::lround(x)), cy = static_cast<int>(std::lround(y));
  for (int j = cy - r; j <= cy + r; ++j)
    for (int i = cx - r; i <= cx + r; ++i) {
      if (i < 0 || j < 0 || i >= size || j >= size) continue;
      const float dx = i - x, dy = j - y;
      const float w = std::exp(-(dx * dx + dy * dy) / (2 * sigma_texels * sigma_texels));
      float *t = &cube[(size_t(face) * size * size + size_t(j) * size + i) * C];
      for (int c = 0; c < C; ++c) t[c] = max_blend ? std::max(t[c], value[c] * w) : t[c] + value[c] * w;
    }
}

// A star's rank into the chart's alpha where it shows: the highest under background stars (a rare
// overlap shows with the fainter one), and -1 under a constellation's, which always shows.
void rank_at(std::vector<float> &cube, int size, V3 d, float sigma_texels, float rank) {
  int face;
  float u, v;
  cube_uv(normalize(d), face, u, v);
  const float x = u * size - 0.5f, y = v * size - 0.5f;
  const int r = static_cast<int>(std::ceil(sigma_texels * 2.5f));
  const int cx = static_cast<int>(std::lround(x)), cy = static_cast<int>(std::lround(y));
  for (int j = cy - r; j <= cy + r; ++j)
    for (int i = cx - r; i <= cx + r; ++i) {
      if (i < 0 || j < 0 || i >= size || j >= size) continue;
      float &a = cube[(size_t(face) * size * size + size_t(j) * size + i) * 4 + 3];
      a = rank < 0 ? rank : std::max(a, rank);
    }
}

float lattice3(int x, int y, int z, uint32_t seed) {
  return static_cast<float>(hash(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
                                 static_cast<uint32_t>(z) * 83492791u ^ seed) >> 8) / 16777216.0f;
}
float noise3(V3 p, uint32_t seed) {
  const int ix = static_cast<int>(std::floor(p.x)), iy = static_cast<int>(std::floor(p.y)), iz = static_cast<int>(std::floor(p.z));
  auto s = [](float t) { return t * t * (3 - 2 * t); };
  const float fx = s(p.x - ix), fy = s(p.y - iy), fz = s(p.z - iz);
  float c[2][2];
  for (int j = 0; j < 2; ++j)
    for (int k = 0; k < 2; ++k) {
      const float a = lattice3(ix, iy + j, iz + k, seed), b = lattice3(ix + 1, iy + j, iz + k, seed);
      c[j][k] = a + (b - a) * fx;
    }
  const float e = c[0][0] + (c[1][0] - c[0][0]) * fy, f = c[0][1] + (c[1][1] - c[0][1]) * fy;
  return e + (f - e) * fz;
}
float fbm3(V3 p, uint32_t seed, int octaves) {
  float sum = 0, amp = 0.5f, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += noise3(p, seed + 101u * o) * amp;
    norm += amp;
    amp *= 0.5f;
    p = p * 2.03f;
  }
  return sum / norm;
}

}  // namespace

void cube_dir(int face, float u, float v, float out[3]) {
  const float sc = 2 * u - 1, tc = 2 * v - 1;
  V3 d;
  switch (face) {
    case 0: d = {1, -tc, -sc}; break;
    case 1: d = {-1, -tc, sc}; break;
    case 2: d = {sc, 1, tc}; break;
    case 3: d = {sc, -1, -tc}; break;
    case 4: d = {sc, -tc, 1}; break;
    default: d = {-sc, -tc, -1}; break;
  }
  d = normalize(d);
  out[0] = d.x;
  out[1] = d.y;
  out[2] = d.z;
}

void radiance(const Observatory &o, const float dir_in[3], const float sun_in[3], float out[3], float transmit[3]) {
  const Air &a = o.air;
  const V3 dir = normalize({dir_in[0], dir_in[1], dir_in[2]}), sun = normalize({sun_in[0], sun_in[1], sun_in[2]});
  const float rp = a.planet_km, top = rp + a.top_km;
  const V3 origin = {0, rp + 0.002f, 0};
  float t = exit_sphere(origin, dir, top);
  const float g = hit_ground(origin, dir, rp);
  if (g > 0) t = g;
  const int n = 16;
  const float ds = std::max(t, 0.0f) / n;
  V3 od, sum_r, sum_m;
  for (int i = 0; i < n; ++i) {
    const V3 p = origin + dir * ((i + 0.5f) * ds);
    const V3 d = density(a, std::sqrt(dot(p, p)) - rp);
    od = od + d * (0.5f * ds);
    V3 odl;
    if (sun_depth(a, p, sun, odl)) {
      float e[3];
      extinction(a, od + odl, e);
      sum_r = sum_r + V3{std::exp(-e[0]), std::exp(-e[1]), std::exp(-e[2])} * (d.x * ds);
      sum_m = sum_m + V3{std::exp(-e[0]), std::exp(-e[1]), std::exp(-e[2])} * (d.y * ds);
    }
    od = od + d * (0.5f * ds);
  }
  const float mu = dot(dir, sun);
  const float pr = phase_rayleigh(mu);
  const float sr[3] = {sum_r.x, sum_r.y, sum_r.z}, sm[3] = {sum_m.x, sum_m.y, sum_m.z};
  float e[3];
  extinction(a, od, e);
  for (int i = 0; i < 3; ++i) {
    out[i] = o.sun_intensity * (sr[i] * a.rayleigh[i] * pr + sm[i] * a.mie[i] * phase_mie(mu, a.mie_g[i]));
    transmit[i] = std::exp(-e[i]);
  }
}

Frame compute(const Observatory &o, const State &s) {
  Frame f;
  const float lat = std::clamp(s.latitude_deg, -89.9f, 89.9f) * kRad;
  const float tilt = o.axial_tilt_deg * kRad;
  // The sun, in the planet's equatorial frame, from its place in the year.
  const float lambda = 2 * kPi * (s.year - o.equinox);
  const V3 sun_eq = ecliptic_to_equatorial({std::cos(lambda), std::sin(lambda), 0}, tilt);
  const float ra_sun = std::atan2(sun_eq.y, sun_eq.x);
  const float lst = ra_sun + 2 * kPi * (s.time_of_day - 0.5f);  // noon: the sun on the meridian
  const M3 h = horizon_frame(lat, lst);
  const M3 chart = mul(h, pole_frame(o));  // the star chart to the world
  const M3 back = transpose(chart);
  std::copy(back.m, back.m + 9, f.to_stars);
  const V3 sun = normalize(h * sun_eq);
  f.sun[0] = sun.x;
  f.sun[1] = sun.y;
  f.sun[2] = sun.z;
  f.sun_altitude_deg = std::asin(std::clamp(sun.y, -1.0f, 1.0f)) / kRad;

  // Moons: each on its orbit by the days gone since the year began.
  const float days = s.year * o.year_days + s.time_of_day;
  size_t i_moon = 0;
  for (const Moon &m : o.moons) {
    const float theta = 2 * kPi * (days / (m.period_days == 0 ? 1e9f : m.period_days) + m.phase);
    const float beta = m.inclination_deg * kRad * std::sin(theta - m.node_deg * kRad);
    V3 v = {std::cos(beta) * std::cos(theta), std::cos(beta) * std::sin(theta), std::sin(beta)};
    if (!m.equatorial) v = ecliptic_to_equatorial(v, tilt);
    const V3 d = normalize(h * v);
    MoonAt at;
    at.dir[0] = d.x;
    at.dir[1] = d.y;
    at.dir[2] = d.z;
    at.lit = 0.5f * (1 - dot(d, sun));
    // Nearer than usual, bigger (and, below, brighter on the ground).
    const float dist = i_moon < s.moon_distance_km.size() ? s.moon_distance_km[i_moon] : 0.0f;
    const float near = m.distance_km > 0 && dist > 0 ? m.distance_km / dist : 1.0f;
    ++i_moon;
    at.size_rad = std::min(0.5f * m.size_deg * kRad * near, 0.6f);
    at.near = near;
    for (int i = 0; i < 3; ++i) at.color[i] = m.color[i] * m.brightness * 0.55f;
    f.moons.push_back(at);
  }

  // Calibration against the sun 60 degrees up: its light is about 1, the shade about 0.55, as the
  // classic look; everything else follows from the air.
  const float ref_sun[3] = {0, std::sin(60 * kRad), std::cos(60 * kRad)};
  float t_ref[3], e_ref[3];
  transmittance(o.air, {ref_sun[0], ref_sun[1], ref_sun[2]}, t_ref);
  irradiance(o, ref_sun, e_ref);
  float key_ref[3];
  for (int i = 0; i < 3; ++i) key_ref[i] = o.sun_intensity * o.sun_color[i] * t_ref[i];
  const float k_key = 1.0f / std::max(lum(key_ref), 1e-6f);
  for (float &v : e_ref) v /= kPi;
  const float k_amb = 0.55f / std::max(lum(e_ref), 1e-6f);
  f.sky_scale = k_amb * 0.9f;

  // The sun's light, lower and redder as it sets, gone below the horizon.
  float t_sun[3], e_sky[3];
  transmittance(o.air, sun, t_sun);
  irradiance(o, f.sun, e_sky);
  const float low = 0.35f + 0.65f * std::clamp(sun.y / 0.5f, 0.0f, 1.0f);
  float key_sun[3], ambient[3];
  for (int i = 0; i < 3; ++i) {
    key_sun[i] = k_key * o.sun_intensity * o.sun_color[i] * t_sun[i] * low;
    ambient[i] = k_amb * e_sky[i] / kPi;
  }
  // The brightest moon's light, whenever it is up (by day the sun outshines it, so there is no gap at
  // dusk between the sun's light going and the moon's coming).
  float key_moon[3] = {0, 0, 0};
  V3 moon_dir = {0, -1, 0};
  for (size_t i = 0; i < o.moons.size(); ++i) {
    const MoonAt &m = f.moons[i];
    if (m.dir[1] <= 0) continue;
    float tm[3];
    transmittance(o.air, {m.dir[0], m.dir[1], m.dir[2]}, tm);
    const float k = 0.04f * o.moons[i].brightness * std::pow(m.lit, 1.5f) * std::min(m.near * m.near, 25.0f) *
                    (0.35f + 0.65f * std::clamp(m.dir[1] / 0.5f, 0.0f, 1.0f));
    // Moonlight looks blue to night eyes.
    const float blue[3] = {0.72f, 0.86f, 1.15f};
    float c[3];
    for (int j = 0; j < 3; ++j) c[j] = k * o.moons[i].color[j] * blue[j] * tm[j] / std::max(t_ref[j], 1e-4f);
    if (lum(c) > lum(key_moon)) {
      std::copy(c, c + 3, key_moon);
      moon_dir = {m.dir[0], m.dir[1], m.dir[2]};
    }
  }
  // Night: the sky's own glow, and the moon's light scattered by the air.
  const float moon_tint[3] = {0.75f, 0.88f, 1.15f};
  for (int i = 0; i < 3; ++i) {
    f.night[i] = o.night[i] + key_moon[i] * 0.12f * moon_tint[i];
    ambient[i] += f.night[i] * 0.3f;
  }
  // The sky's light, half as saturated as the air makes it (the house look's shade is a soft blue).
  {
    const float l = lum(ambient);
    for (float &v : ambient) v = l + (v - l) * 0.5f;
  }
  // Under cloud the light spreads: less difference between lit and shade, and greyer.
  const float cloud = std::clamp(s.clouds, 0.0f, 1.0f);
  {
    const float l = lum(ambient);
    for (float &v : ambient) v = (v + (l - v) * 0.5f * cloud) * (1 + 0.3f * cloud);
  }
  const bool by_sun = lum(key_sun) >= lum(key_moon);
  const float *key = by_sun ? key_sun : key_moon;
  const V3 kd = by_sun ? sun : moon_dir;
  f.key_dir[0] = kd.x;
  f.key_dir[1] = kd.y;
  f.key_dir[2] = kd.z;
  std::copy(key, key + 3, f.key);
  std::copy(ambient, ambient + 3, f.ambient);

  // Exposure: the eye opens as the light goes, to fourteen times at night, but never all the way, so
  // what it sees only darkens from noon to midnight (the light to the power 0.35).
  const float b = lum(f.key) + lum(f.ambient);
  f.exposure = std::clamp(std::pow(1.55f / std::max(b, 1e-5f), 0.65f), 0.85f, 14.0f);
  f.sky_exposure = std::pow(f.exposure, 0.75f);

  // The sky's colour at the zenith and the horizon (for fog, and renderers without a sky pass).
  float hz[3] = {0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    const float az = kPi * 0.5f * k + 0.4f;
    const float d[3] = {std::cos(az) * 0.9976f, 0.0698f, std::sin(az) * 0.9976f};
    float L[3], T[3];
    radiance(o, d, f.sun, L, T);
    for (int i = 0; i < 3; ++i) hz[i] += 0.25f * L[i];
  }
  float zen[3], tz[3];
  const float up[3] = {0, 1, 0};
  radiance(o, up, f.sun, zen, tz);
  for (int i = 0; i < 3; ++i) {
    f.cloud_lit[i] = (f.key[i] * 1.15f + f.ambient[i] * 0.8f) * o.cloud_color[i];
    f.cloud_shade[i] = (f.ambient[i] * 0.8f + f.key[i] * 0.08f) * (1 - 0.35f * cloud) * o.cloud_color[i];
  }
  float hzc[3], znc[3];
  for (int i = 0; i < 3; ++i) {
    const float hzd = hz[i] * f.sky_scale + f.night[i];
    hzc[i] = (hzd + (lum(f.cloud_shade) - hzd) * cloud * 0.8f) * f.sky_exposure;
    znc[i] = (zen[i] * f.sky_scale + f.night[i]) * f.sky_exposure;
  }
  tone(hzc, f.horizon);
  tone(znc, f.zenith);
  std::copy(f.horizon, f.horizon + 3, f.fog);
  f.fog_density = (0.25f + 0.75f * cloud) / 2500.0f;

  // The aurora: on dark nights, stronger near the equinoxes, its oval nearer as storms grow.
  const float season = 1 + 0.3f * std::cos(4 * kPi * (s.year - o.equinox));
  f.aurora = std::clamp(s.aurora, 0.0f, 1.0f) * season * smoothstep(-6.0f, -15.0f, f.sun_altitude_deg);
  f.aurora_km = (o.aurora_latitude_deg - 14.0f * std::clamp(s.aurora, 0.0f, 1.0f) - std::fabs(s.latitude_deg)) * 111.2f;
  f.aurora_north = s.latitude_deg >= 0 ? 1.0f : -1.0f;
  return f;
}

void bake_milky_way(const Observatory &o, int size, std::vector<float> &glow) {
  glow.assign(size_t(6) * size * size, 0.0f);
  // The galaxy's glow: a band along its plane, brighter toward its centre, broken by dust.
  if (o.milky_way > 0) {
    const V3 ngp = radec(192.859f * kRad, 27.128f * kRad), gc = radec(266.405f * kRad, -28.936f * kRad);
    const V3 gy = cross(ngp, gc);
    std::vector<std::thread> threads;
    for (int face = 0; face < 6; ++face)
      threads.emplace_back([&, face] {
        for (int j = 0; j < size; ++j)
          for (int i = 0; i < size; ++i) {
            float d3[3];
            cube_dir(face, (i + 0.5f) / size, (j + 0.5f) / size, d3);
            const V3 d = {d3[0], d3[1], d3[2]};
            const float b = std::asin(std::clamp(dot(d, ngp), -1.0f, 1.0f));
            const float l = std::atan2(dot(d, gy), dot(d, gc));
            const float band = std::exp(-(b * b) / (0.17f * 0.17f));
            if (band < 0.01f) continue;
            const float core = 0.3f + 0.7f * std::pow(0.5f + 0.5f * std::cos(l), 2.0f);
            const float n = fbm3(d * 7.0f, o.star_seed + 11u, 4);
            const float rift = std::exp(-((b - 0.03f) * (b - 0.03f)) / (0.035f * 0.035f)) *
                               smoothstep(1.4f, 0.3f, std::fabs(l)) * smoothstep(0.45f, 0.65f, fbm3(d * 11.0f, o.star_seed + 29u, 3));
            const float v = o.milky_way * 0.03f * band * core * (0.45f + 0.9f * n) * (1 - 0.8f * rift);
            glow[size_t(face) * size * size + size_t(j) * size + i] = v;
          }
      });
    for (auto &t : threads) t.join();
  }
}

void bake_stars(const Observatory &o, int size, std::vector<float> &rgba) {
  rgba.assign(size_t(6) * size * size * 4, 0.0f);
  // rank: the star density (State::star_density) above which it shows; the constellations' always do (-1).
  auto add = [&](V3 d, float mag, float bv, float rank) {
    // Stylized: brightness falls 10^-0.3 per magnitude rather than 10^-0.4, so the faint field reads
    // as a field instead of vanishing (the brightest stars keep their light).
    const float flux = 1.6f * std::pow(10.0f, -0.3f * mag);
    const V3 c = star_color(bv);
    const float sigma = 0.55f + 0.18f * std::clamp(3.0f - mag, 0.0f, 4.5f);
    // A spot's peak is its flux spread over its area, kept so a star is as bright at any size.
    const float peak = flux * (0.55f * 0.55f) / (sigma * sigma) * 2.2f;
    const float v[4] = {peak * c.x, peak * c.y, peak * c.z, 0};
    splat<4>(rgba, size, d, sigma, v, false);
    rank_at(rgba, size, d, sigma, rank);
  };
  // Background stars: faint, two in five crowding the galaxy's plane.
  const V3 ngp = radec(192.859f * kRad, 27.128f * kRad);
  V3 ax = normalize(cross(ngp, {0, 0, 1})), ay = cross(ngp, ax);
  const uint32_t count = static_cast<uint32_t>(o.stars * kMaxStarDensity);
  for (uint32_t k = 0; k < count; ++k) {
    Rng rng{hash(o.star_seed * 2654435761u ^ hash(k + 0x9e3779b9u))};  // star k's own draws
    V3 d;
    if (rng.next() < 0.4f) {
      const float a = 2 * kPi * rng.next();
      const float u1 = std::max(rng.next(), 1e-6f), u2 = rng.next();
      const float b = 0.18f * std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
      d = (ax * std::cos(a) + ay * std::sin(a)) * std::cos(b) + ngp * std::sin(b);
    } else {
      const float z = 2 * rng.next() - 1, a = 2 * kPi * rng.next();
      const float r = std::sqrt(std::max(0.0f, 1 - z * z));
      d = {r * std::cos(a), r * std::sin(a), z};
    }
    // Magnitudes: many more faint than bright (counts grow about 10^(0.5 m)). Its draw is its rank,
    // so a lower density keeps the brighter stars, as a hazier or brighter night does.
    // Density 1 reaches kFaintest; each four times the stars, 1.2 magnitudes fainter.
    const float faintest = kFaintest + 2.0f * std::log10(kMaxStarDensity);
    const float lo = std::pow(10.0f, 0.5f * 3.2f), hi = std::pow(10.0f, 0.5f * faintest);
    const float u = rng.next();
    const float mag = 2.0f * std::log10(lo + u * (hi - lo));
    const float u1 = std::max(rng.next(), 1e-6f), u2 = rng.next();
    const float bv = 0.6f + 0.45f * std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
    add(d, mag, bv, u * kMaxStarDensity);
  }
  for (const Constellation &c : o.constellations)
    for (const Star &s : c.stars) add(radec(s.ra_hours * 15 * kRad, s.dec_deg * kRad), s.magnitude, s.color_index, -1);
}

void bake_glow(const Observatory &o, const std::vector<uint8_t> &states, int size, std::vector<float> &glow) {
  glow.assign(size_t(6) * size * size, 0.0f);
  const float texel = 0.5f * kPi / size;
  for (size_t ci = 0; ci < o.constellations.size(); ++ci) {
    const uint8_t st = ci < states.size() ? states[ci] : static_cast<uint8_t>(kDark);
    if (st == kDark) continue;
    const Constellation &c = o.constellations[ci];
    std::vector<V3> at;
    for (const Star &s : c.stars) at.push_back(radec(s.ra_hours * 15 * kRad, s.dec_deg * kRad));
    for (size_t k = 0; k < at.size(); ++k) {
      const float sigma = 1.6f + 0.35f * std::clamp(3.5f - c.stars[k].magnitude, 0.0f, 4.0f);
      const float v[1] = {1.0f};
      splat<1>(glow, size, at[k], sigma, v, true);
    }
    if (st != kConnected) continue;
    // Lines along great circles, stopping short of each star (a chart's gap).
    const float gap = 0.9f * kRad;
    for (size_t k = 0; k + 1 < c.lines.size(); k += 2) {
      if (c.lines[k] >= at.size() || c.lines[k + 1] >= at.size()) continue;
      const V3 a = at[c.lines[k]], b = at[c.lines[k + 1]];
      const float ang = std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
      if (ang <= 2 * gap) continue;
      const int steps = static_cast<int>(ang / (texel * 0.35f)) + 1;
      const float s = std::sin(ang);
      for (int i = 0; i <= steps; ++i) {
        const float t = ang * i / steps;
        if (t < gap || t > ang - gap) continue;
        const V3 d = a * (std::sin(ang - t) / s) + b * (std::sin(t) / s);
        const float v[1] = {0.55f};
        splat<1>(glow, size, d, 0.75f, v, true);
      }
    }
  }
}

void bake_clouds(uint32_t seed, int size, std::vector<uint8_t> &rg) {
  rg.assign(size_t(size) * size * 2, 0);
  // Value noise on a lattice that wraps, so the texture tiles.
  auto lattice = [&](int x, int y, int period, uint32_t s) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return static_cast<float>(hash(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 83492791u ^ s) >> 8) / 16777216.0f;
  };
  auto noise = [&](float x, float y, int period, uint32_t s) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    auto sm = [](float t) { return t * t * (3 - 2 * t); };
    const float fx = sm(x - ix), fy = sm(y - iy);
    const float a = lattice(ix, iy, period, s), b = lattice(ix + 1, iy, period, s);
    const float c = lattice(ix, iy + 1, period, s), d = lattice(ix + 1, iy + 1, period, s);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
  };
  auto fbm = [&](float u, float v, int base, int octaves, uint32_t s) {
    float sum = 0, amp = 0.5f, norm = 0;
    int period = base;
    for (int o = 0; o < octaves; ++o) {
      sum += noise(u * period, v * period, period, s + 131u * o) * amp;
      norm += amp;
      amp *= 0.5f;
      period *= 2;
    }
    return sum / norm;
  };
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) {
      const float u = (i + 0.5f) / size, v = (j + 0.5f) / size;
      // Broad shapes, a little billowed; and a finer, puffier detail.
      const float a = fbm(u, v, 4, 6, seed);
      const float b = 1.0f - std::fabs(2.0f * fbm(u, v, 16, 4, seed + 7u) - 1.0f);
      const float stretch = std::clamp((a - 0.5f) * 1.9f + 0.5f, 0.0f, 1.0f);
      rg[(size_t(j) * size + i) * 2 + 0] = static_cast<uint8_t>(std::lround(stretch * 255));
      rg[(size_t(j) * size + i) * 2 + 1] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
    }
}

}  // namespace runtime::sky
