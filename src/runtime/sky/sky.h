// The sky (design/observatory.md): a planet's observatory (its sun, moons,
// air, stars, and constellations) and the state a game sets on it (time of day,
// time of year, latitude, clouds, aurora, which constellations are lit), turned
// into a frame: where the sun, moons, and stars are, the colour of the sky, and
// the light it gives the ground (the key light, the ambient, fog, and exposure).
//
// The world's axes: +Y up, +X east, -Z north. Angles in the data are degrees.
// An observatory isn't versioned like a pipeline: it is data, a few numbers and
// a star chart, and the runtime draws it with its own renderer (sky_renderer.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace runtime::sky {

struct Star {
  float ra_hours = 0, dec_deg = 0;  // J2000, among Earth's stars
  float magnitude = 3;
  float color_index = 0.6f;  // B-V: -0.3 blue to 2 red
};

struct Constellation {
  std::string name;
  std::vector<Star> stars;
  std::vector<uint16_t> lines;  // pairs of indices into stars
};

struct Moon {
  std::string name;
  float period_days = 27.32f;    // sidereal, in the planet's days (negative: retrograde)
  float inclination_deg = 5.14f;  // to the orbit's reference plane
  float node_deg = 0;             // where it crosses it
  float phase = 0;                // its place in its orbit when the year starts, 0 to 1
  float size_deg = 0.52f;        // as seen from the ground, at its distance
  float distance_km = 0;         // its usual distance (0: unknown, and State::moon_distance_km does nothing)
  float brightness = 1;          // its light at full, relative to Earth's moon
  float color[3] = {0.92f, 0.90f, 0.86f};
  bool equatorial = false;  // orbits in the planet's equator (close moons), not its orbital plane
};

// The air, for single scattering: coefficients per kilometre, heights in kilometres.
struct Air {
  float planet_km = 6360, top_km = 60;
  float rayleigh[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
  float rayleigh_km = 8;
  float mie[3] = {3.996e-3f, 3.996e-3f, 3.996e-3f};
  float mie_absorb[3] = {0.444e-3f, 0.444e-3f, 0.444e-3f};
  float mie_km = 1.2f;
  float mie_g[3] = {0.8f, 0.8f, 0.8f};  // forward scattering, per channel (dust scatters blue ahead)
  float ozone[3] = {0.650e-3f, 1.881e-3f, 0.085e-3f};  // absorbs, in a layer about 25 km up
};

struct Observatory {
  std::string name;
  float axial_tilt_deg = 23.44f;
  float year_days = 365.25f;   // in its own days
  float equinox = 0.2164f;     // the year's fraction at the northern spring equinox
  float pole_ra_deg = 0, pole_dec_deg = 90;  // where its north pole points among the stars
  float sun_color[3] = {1.0f, 0.96f, 0.90f};
  float sun_size_deg = 0.53f;
  float sun_intensity = 20;
  Air air;
  std::vector<Moon> moons;  // the first two are drawn
  std::vector<Constellation> constellations;
  uint32_t stars = 60000;  // background stars at density 1, crowding the galaxy's plane
  uint32_t star_seed = 7;  // the whole star chart and Milky Way from it: one seed, one sky, every time
  float milky_way = 1;
  float cloud_height_m = 1600, cloud_size_m = 600;  // a cloud's size (the map tiles every four)
  uint32_t cloud_seed = 3;
  float cloud_color[3] = {1, 1, 1};
  float aurora_latitude_deg = 67;  // where its aurora's oval sits on a quiet night
  float aurora_low[3] = {0.15f, 1.0f, 0.42f}, aurora_high[3] = {0.80f, 0.22f, 0.62f};
  float lit_color[3] = {0.55f, 0.82f, 1.0f};  // a lit constellation's glow
  float night[3] = {0.010f, 0.013f, 0.026f};  // the night sky's own glow (airglow), at full exposure
  float ground[3] = {0.36f, 0.40f, 0.28f};    // the ground under it, for views that draw one
};

// The star chart holds this many times the observatory's star count, fainter and fainter (each four
// times the stars reaches 1.2 magnitudes fainter), so State::star_density can go past 1.
constexpr float kMaxStarDensity = 4;

// A constellation's state: dark, lit (its stars glow), or lit and connected (its lines glow too).
enum : uint8_t { kDark = 0, kLit = 1, kConnected = 2 };

struct State {
  float time_of_day = 0.5f;  // 0 to 1 from midnight; 0.5 is local noon (the sun on the meridian)
  float year = 0.47f;        // 0 to 1 from the start of the year
  float latitude_deg = 45;
  float clouds = 0.3f;              // 0 clear to 1 overcast
  float cloud_wind[2] = {8, 3};     // the clouds' drift, metres per second (x east, z south)
  float aurora = 0;                 // 0 none to 1 a great storm
  float star_density = 0.5f;        // 0 only the constellations' stars, 1 the observatory's star count (to magnitude 7.6), up to kMaxStarDensity
  std::vector<float> moon_distance_km;  // per moon; 0 or missing: its usual distance. Nearer is bigger and brighter
  std::vector<uint8_t> constellations;  // per constellation, kDark, kLit, or kConnected
  bool shadows = true;
};

struct MoonAt {
  float dir[3] = {0, -1, 0};
  float lit = 0;       // the fraction of its face lit
  float size_rad = 0;  // angular radius
  float near = 1;      // its usual distance over its distance now
  float color[3] = {0, 0, 0};
};

// A moment of sky, in display units: the classic look's sun is about 1.
struct Frame {
  float sun[3] = {0, 1, 0};
  float sun_altitude_deg = 90;
  std::vector<MoonAt> moons;
  float to_stars[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // world to the star chart, row-major
  // The light for the ground: toward the key light (the sun, or a moon at night), its colour,
  // the unlit colour, fog toward the horizon, and the exposure everything is multiplied by.
  float key_dir[3] = {0, 1, 0}, key[3] = {1, 1, 1}, ambient[3] = {0.5f, 0.5f, 0.5f};
  float fog[3] = {0.6f, 0.7f, 0.8f}, fog_density = 0;
  float exposure = 1;
  float sky_exposure = 1;  // the sky's: it adapts less than the ground (exposure^0.75), so dusk and night stay deep
  float sky_scale = 1;  // sky radiance to display units
  float zenith[3] = {0.3f, 0.5f, 0.9f}, horizon[3] = {0.7f, 0.8f, 0.9f};  // after exposure and tone
  float cloud_lit[3] = {1, 1, 1}, cloud_shade[3] = {0.5f, 0.5f, 0.5f};
  float night[3] = {0, 0, 0};
  float aurora = 0;          // its strength tonight, with the season and darkness
  float aurora_km = 0;       // how far poleward its oval stands, kilometres (negative: behind)
  float aurora_north = 1;    // +1 toward -Z (the north), -1 toward +Z (the south)
};

Frame compute(const Observatory &o, const State &s);

// Sky radiance (sun intensity units) along a world direction, from just above the ground, and the
// air's transmittance along it; shared with sky.hlsl, which does the same per pixel.
void radiance(const Observatory &o, const float dir[3], const float sun[3], float out[3], float transmit[3]);

// The star chart baked into a cube map (faces +X, -X, +Y, -Y, +Z, -Z, each size x size, RGBA as
// floats): the background and constellations' stars in RGB, and in A the star density each shows
// above (its rank by brightness, 0 to kMaxStarDensity; -1 for the constellations' stars, which always
// show). Star k's place, brightness, and colour come from (star_seed, k) alone, so the same seed bakes
// the same sky, and a larger count only adds stars.
void bake_stars(const Observatory &o, int size, std::vector<float> &rgba);
// One channel each, size x size per face: the galaxy's glow (in sky radiance, about 0.04 at its
// brightest), and the lit and connected constellations' glow (0 to 1), rebaked as their states change.
void bake_milky_way(const Observatory &o, int size, std::vector<float> &glow);
void bake_glow(const Observatory &o, const std::vector<uint8_t> &states, int size, std::vector<float> &glow);

// The clouds' noise (size x size, two channels, tiling): broad shapes in R, detail in G.
void bake_clouds(uint32_t seed, int size, std::vector<uint8_t> &rg);

// A cube face's direction for texel coordinates u, v in [0, 1] (Vulkan's face order and axes).
void cube_dir(int face, float u, float v, float out[3]);

}  // namespace runtime::sky
