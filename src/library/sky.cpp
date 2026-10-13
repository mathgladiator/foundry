// The sky's C API (sky.h) over the runtime's sky (src/runtime/sky).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "internal.h"

namespace {

void copy3(float out[3], const float in[3]) { std::memcpy(out, in, 3 * sizeof(float)); }

}  // namespace

extern "C" {

void foundry_observatory_default(foundry_observatory_desc *desc) {
  if (!desc) return;
  const runtime::sky::Observatory o;
  *desc = {};
  desc->axial_tilt_deg = o.axial_tilt_deg;
  desc->year_days = o.year_days;
  desc->equinox = o.equinox;
  desc->pole_ra_deg = o.pole_ra_deg;
  desc->pole_dec_deg = o.pole_dec_deg;
  copy3(desc->sun_color, o.sun_color);
  desc->sun_size_deg = o.sun_size_deg;
  desc->sun_intensity = o.sun_intensity;
  desc->planet_km = o.air.planet_km;
  desc->air_km = o.air.top_km;
  copy3(desc->rayleigh, o.air.rayleigh);
  desc->rayleigh_km = o.air.rayleigh_km;
  copy3(desc->mie, o.air.mie);
  copy3(desc->mie_absorb, o.air.mie_absorb);
  desc->mie_km = o.air.mie_km;
  copy3(desc->mie_g, o.air.mie_g);
  copy3(desc->ozone, o.air.ozone);
  desc->stars = o.stars;
  desc->star_seed = o.star_seed;
  desc->milky_way = o.milky_way;
  desc->cloud_height_m = o.cloud_height_m;
  desc->cloud_size_m = o.cloud_size_m;
  desc->cloud_seed = o.cloud_seed;
  copy3(desc->cloud_color, o.cloud_color);
  desc->aurora_latitude_deg = o.aurora_latitude_deg;
  copy3(desc->aurora_low, o.aurora_low);
  copy3(desc->aurora_high, o.aurora_high);
  copy3(desc->lit_color, o.lit_color);
  copy3(desc->night, o.night);
  copy3(desc->ground, o.ground);
}

foundry_status foundry_sky_set_observatory(foundry_runtime *rt, const foundry_observatory_desc *d) {
  if (!d) {
    rt->world.set_sky(nullptr);
    return FOUNDRY_OK;
  }
  if (d->year_days <= 0 || d->planet_km <= 0 || d->air_km <= 0 || d->rayleigh_km <= 0 || d->mie_km <= 0)
    return FOUNDRY_STATUS_BAD_PARAMETER;
  auto o = std::make_shared<runtime::sky::Observatory>();
  o->axial_tilt_deg = d->axial_tilt_deg;
  o->year_days = d->year_days;
  o->equinox = d->equinox;
  o->pole_ra_deg = d->pole_ra_deg;
  o->pole_dec_deg = d->pole_dec_deg;
  copy3(o->sun_color, d->sun_color);
  o->sun_size_deg = d->sun_size_deg;
  o->sun_intensity = d->sun_intensity;
  o->air.planet_km = d->planet_km;
  o->air.top_km = d->air_km;
  copy3(o->air.rayleigh, d->rayleigh);
  o->air.rayleigh_km = d->rayleigh_km;
  copy3(o->air.mie, d->mie);
  copy3(o->air.mie_absorb, d->mie_absorb);
  o->air.mie_km = d->mie_km;
  copy3(o->air.mie_g, d->mie_g);
  copy3(o->air.ozone, d->ozone);
  for (uint32_t i = 0; i < d->moon_count; ++i) {
    const foundry_sky_moon &m = d->moons[i];
    runtime::sky::Moon x;
    x.period_days = m.period_days;
    x.inclination_deg = m.inclination_deg;
    x.node_deg = m.node_deg;
    x.phase = m.phase;
    x.size_deg = m.size_deg;
    x.distance_km = m.distance_km;
    x.brightness = m.brightness;
    copy3(x.color, m.color);
    x.equatorial = m.equatorial;
    o->moons.push_back(x);
  }
  for (uint32_t i = 0; i < d->constellation_count; ++i) {
    const foundry_sky_constellation &c = d->constellations[i];
    runtime::sky::Constellation x;
    x.name = c.name ? c.name : "";
    for (uint32_t k = 0; k < c.star_count; ++k)
      x.stars.push_back({c.stars[k].ra_hours, c.stars[k].dec_deg, c.stars[k].magnitude, c.stars[k].color_index});
    for (uint32_t k = 0; k < c.line_count * 2; ++k) {
      if (c.lines[k] >= c.star_count) return FOUNDRY_STATUS_BAD_PARAMETER;
      x.lines.push_back(c.lines[k]);
    }
    o->constellations.push_back(std::move(x));
  }
  o->stars = d->stars;
  o->star_seed = d->star_seed;
  o->milky_way = d->milky_way;
  o->cloud_height_m = d->cloud_height_m;
  o->cloud_size_m = d->cloud_size_m;
  o->cloud_seed = d->cloud_seed;
  copy3(o->cloud_color, d->cloud_color);
  o->aurora_latitude_deg = d->aurora_latitude_deg;
  copy3(o->aurora_low, d->aurora_low);
  copy3(o->aurora_high, d->aurora_high);
  copy3(o->lit_color, d->lit_color);
  copy3(o->night, d->night);
  copy3(o->ground, d->ground);
  rt->world.set_sky(std::move(o));
  return FOUNDRY_OK;
}

void foundry_sky_state_default(foundry_sky_state *state) {
  if (!state) return;
  const runtime::sky::State s;
  *state = {};
  state->time_of_day = s.time_of_day;
  state->year = s.year;
  state->latitude_deg = s.latitude_deg;
  state->clouds = s.clouds;
  state->cloud_wind[0] = s.cloud_wind[0];
  state->cloud_wind[1] = s.cloud_wind[1];
  state->aurora = s.aurora;
  state->star_density = s.star_density;
  state->moon_distance_km[0] = state->moon_distance_km[1] = 0;
  state->shadows = s.shadows;
}

void foundry_sky_set_state(foundry_runtime *rt, const foundry_sky_state *state) {
  if (!state) return;
  runtime::sky::State s;
  s.time_of_day = state->time_of_day - std::floor(state->time_of_day);
  s.year = state->year - std::floor(state->year);
  s.latitude_deg = std::clamp(state->latitude_deg, -90.0f, 90.0f);
  s.clouds = std::clamp(state->clouds, 0.0f, 1.0f);
  s.cloud_wind[0] = state->cloud_wind[0];
  s.cloud_wind[1] = state->cloud_wind[1];
  s.aurora = std::clamp(state->aurora, 0.0f, 1.0f);
  s.star_density = std::clamp(state->star_density, 0.0f, runtime::sky::kMaxStarDensity);
  s.moon_distance_km.assign(state->moon_distance_km, state->moon_distance_km + 2);
  if (state->constellations) s.constellations.assign(state->constellations, state->constellations + state->constellation_count);
  s.shadows = state->shadows;
  rt->world.set_sky_state(s);
  rt->world.set_shadow_focus(state->shadow_centre, state->shadow_radius);
}

bool foundry_sky_get_light(foundry_runtime *rt, foundry_sky_light *out) {
  runtime::sky::Frame f;
  if (!out || !rt->world.sky_frame(f)) return false;
  *out = {};
  copy3(out->sun_direction, f.sun);
  out->sun_altitude_deg = f.sun_altitude_deg;
  copy3(out->key_direction, f.key_dir);
  copy3(out->key_color, f.key);
  copy3(out->ambient, f.ambient);
  out->exposure = f.exposure;
  if (!f.moons.empty()) {
    copy3(out->moon_direction, f.moons[0].dir);
    out->moon_lit = f.moons[0].lit;
  }
  return true;
}

}  // extern "C"
