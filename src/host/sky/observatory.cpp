#include "sky/observatory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "core/util.h"

namespace sky {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr float kPi = 3.14159265f;

void read3(const json &j, const char *key, float out[3]) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return;
  for (int i = 0; i < 3; ++i) out[i] = j[key][i].get<float>();
}

const char *kStates[] = {"dark", "lit", "connected"};

// JSON with arrays of numbers kept on one line, so a star chart stays readable.
void compact(const json &j, std::string &out, int indent) {
  const std::string pad(indent, ' '), inner(indent + 2, ' ');
  if (j.is_object()) {
    if (j.empty()) {
      out += "{}";
      return;
    }
    out += "{\n";
    size_t k = 0;
    for (auto it = j.begin(); it != j.end(); ++it, ++k) {
      out += inner + json(it.key()).dump() + ": ";
      compact(it.value(), out, indent + 2);
      out += k + 1 < j.size() ? ",\n" : "\n";
    }
    out += pad + "}";
  } else if (j.is_array()) {
    const bool flat = std::all_of(j.begin(), j.end(), [](const json &v) { return v.is_number(); });
    if (flat || j.empty()) {
      out += "[";
      for (size_t i = 0; i < j.size(); ++i) out += (i ? ", " : "") + j[i].dump();
      out += "]";
      return;
    }
    out += "[\n";
    for (size_t i = 0; i < j.size(); ++i) {
      out += inner;
      compact(j[i], out, indent + 2);
      out += i + 1 < j.size() ? ",\n" : "\n";
    }
    out += pad + "]";
  } else {
    out += j.dump();
  }
}

// A mesh with flat faces, for the sundial.
struct Builder {
  runtime::Mesh m;
  static uint32_t pack(float r, float g, float b) {
    auto q = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return q(r) | q(g) << 8 | q(b) << 16 | 0xff000000u;
  }
  void tri(foundry::vec3 a, foundry::vec3 b, foundry::vec3 c, uint32_t col) {
    const foundry::vec3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z}, e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
    foundry::vec3 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (l <= 0) return;
    n = {n.x / l, n.y / l, n.z / l};
    const uint32_t base = static_cast<uint32_t>(m.positions.size());
    for (const foundry::vec3 &p : {a, b, c}) {
      m.positions.push_back(p);
      m.normals.push_back(n);
      m.colors.push_back(col);
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
  }
  void quad(foundry::vec3 a, foundry::vec3 b, foundry::vec3 c, foundry::vec3 d, uint32_t col) {
    tri(a, b, c, col);
    tri(a, c, d, col);
  }
  // A prism of n sides about the Y axis at (x, z), from y0 to y1, turned by `turn`.
  void prism(float x, float z, float y0, float y1, float r, int n, uint32_t side, uint32_t top, float turn = 0) {
    for (int i = 0; i < n; ++i) {
      const float a0 = turn + 2 * kPi * i / n, a1 = turn + 2 * kPi * (i + 1) / n;
      const foundry::vec3 p0 = {x + r * std::cos(a0), 0, z + r * std::sin(a0)}, p1 = {x + r * std::cos(a1), 0, z + r * std::sin(a1)};
      quad({p0.x, y0, p0.z}, {p0.x, y1, p0.z}, {p1.x, y1, p1.z}, {p1.x, y0, p1.z}, side);
      tri({x, y1, z}, {p1.x, y1, p1.z}, {p0.x, y1, p0.z}, top);
    }
  }
  // A box centred at c with half extents h, turned about Y by yaw.
  void box(foundry::vec3 c, foundry::vec3 h, float yaw, uint32_t col) {
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    auto at = [&](float x, float y, float z) {
      return foundry::vec3{c.x + x * cs - z * sn, c.y + y, c.z + x * sn + z * cs};
    };
    const foundry::vec3 p[8] = {at(-h.x, -h.y, -h.z), at(h.x, -h.y, -h.z), at(h.x, h.y, -h.z), at(-h.x, h.y, -h.z),
                                at(-h.x, -h.y, h.z),  at(h.x, -h.y, h.z),  at(h.x, h.y, h.z),  at(-h.x, h.y, h.z)};
    quad(p[0], p[3], p[2], p[1], col);
    quad(p[4], p[5], p[6], p[7], col);
    quad(p[0], p[4], p[7], p[3], col);
    quad(p[1], p[2], p[6], p[5], col);
    quad(p[3], p[7], p[6], p[2], col);
    quad(p[0], p[1], p[5], p[4], col);
  }
};

}  // namespace

bool parse(const json &j, Observatory &o, std::string &error, const std::function<bool(const std::string &, json &)> &from) {
  if (!j.is_object()) {
    error = "an observatory is a JSON object";
    return false;
  }
  try {
    o.axial_tilt_deg = j.value("axial_tilt_deg", o.axial_tilt_deg);
    o.year_days = j.value("year_days", o.year_days);
    o.equinox = j.value("equinox", o.equinox);
    if (j.contains("pole")) {
      o.pole_ra_deg = j["pole"].value("ra_deg", o.pole_ra_deg);
      o.pole_dec_deg = j["pole"].value("dec_deg", o.pole_dec_deg);
    }
    if (j.contains("sun")) {
      const json &s = j["sun"];
      read3(s, "color", o.sun_color);
      o.sun_size_deg = s.value("size_deg", o.sun_size_deg);
      o.sun_intensity = s.value("intensity", o.sun_intensity);
    }
    if (j.contains("air")) {
      const json &a = j["air"];
      o.air.planet_km = a.value("planet_km", o.air.planet_km);
      o.air.top_km = a.value("top_km", o.air.top_km);
      read3(a, "rayleigh", o.air.rayleigh);
      o.air.rayleigh_km = a.value("rayleigh_km", o.air.rayleigh_km);
      read3(a, "mie", o.air.mie);
      read3(a, "mie_absorb", o.air.mie_absorb);
      o.air.mie_km = a.value("mie_km", o.air.mie_km);
      read3(a, "mie_g", o.air.mie_g);
      read3(a, "ozone", o.air.ozone);
    }
    for (const json &m : j.value("moons", json::array())) {
      runtime::sky::Moon x;
      x.name = m.value("name", "");
      x.period_days = m.value("period_days", x.period_days);
      x.inclination_deg = m.value("inclination_deg", x.inclination_deg);
      x.node_deg = m.value("node_deg", x.node_deg);
      x.phase = m.value("phase", x.phase);
      x.size_deg = m.value("size_deg", x.size_deg);
      x.distance_km = m.value("distance_km", x.distance_km);
      x.brightness = m.value("brightness", x.brightness);
      read3(m, "color", x.color);
      x.equatorial = m.value("equatorial", false);
      o.moons.push_back(x);
    }
    if (j.contains("stars")) {
      o.stars = j["stars"].value("count", o.stars);
      o.star_seed = j["stars"].value("seed", o.star_seed);
      o.milky_way = j["stars"].value("milky_way", o.milky_way);
    }
    if (j.contains("clouds")) {
      const json &c = j["clouds"];
      o.cloud_height_m = c.value("height_m", o.cloud_height_m);
      o.cloud_size_m = c.value("size_m", o.cloud_size_m);
      o.cloud_seed = c.value("seed", o.cloud_seed);
      read3(c, "color", o.cloud_color);
    }
    if (j.contains("aurora")) {
      o.aurora_latitude_deg = j["aurora"].value("latitude_deg", o.aurora_latitude_deg);
      read3(j["aurora"], "low", o.aurora_low);
      read3(j["aurora"], "high", o.aurora_high);
    }
    read3(j, "lit_color", o.lit_color);
    read3(j, "night", o.night);
    read3(j, "ground", o.ground);
    json chart = j.value("constellations", json::array());
    if (j.contains("constellations_from")) {
      json other;
      const std::string name = j["constellations_from"].get<std::string>();
      if (!from || !from(name, other)) {
        error = "constellations_from: no observatory " + name;
        return false;
      }
      chart = other.value("constellations", json::array());
    }
    for (const json &c : chart) {
      runtime::sky::Constellation x;
      x.name = c.value("name", "");
      for (const json &s : c.value("stars", json::array())) {
        if (!s.is_array() || s.size() < 3) continue;
        x.stars.push_back({s[0].get<float>(), s[1].get<float>(), s[2].get<float>(), s.size() > 3 ? s[3].get<float>() : 0.6f});
      }
      for (const json &l : c.value("lines", json::array())) {
        if (!l.is_array() || l.size() != 2) continue;
        const uint32_t a = l[0].get<uint32_t>(), b = l[1].get<uint32_t>();
        if (a >= x.stars.size() || b >= x.stars.size()) {
          error = x.name + ": a line names a star it doesn't have";
          return false;
        }
        x.lines.push_back(static_cast<uint16_t>(a));
        x.lines.push_back(static_cast<uint16_t>(b));
      }
      o.constellations.push_back(std::move(x));
    }
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  if (o.year_days <= 0 || o.air.planet_km <= 0 || o.air.rayleigh_km <= 0 || o.air.mie_km <= 0) {
    error = "year_days and the air's sizes must be positive";
    return false;
  }
  return true;
}

State state_of(const json &j, const Observatory &o, State s) {
  if (!j.is_object()) return s;
  auto frac = [](float v) { return v - std::floor(v); };
  if (j.contains("time_of_day")) s.time_of_day = frac(j["time_of_day"].get<float>());
  if (j.contains("year")) s.year = frac(j["year"].get<float>());
  if (j.contains("latitude_deg")) s.latitude_deg = std::clamp(j["latitude_deg"].get<float>(), -90.0f, 90.0f);
  if (j.contains("clouds")) s.clouds = std::clamp(j["clouds"].get<float>(), 0.0f, 1.0f);
  if (j.contains("cloud_wind") && j["cloud_wind"].is_array() && j["cloud_wind"].size() == 2) {
    s.cloud_wind[0] = j["cloud_wind"][0].get<float>();
    s.cloud_wind[1] = j["cloud_wind"][1].get<float>();
  }
  if (j.contains("aurora")) s.aurora = std::clamp(j["aurora"].get<float>(), 0.0f, 1.0f);
  if (j.contains("star_density")) s.star_density = std::clamp(j["star_density"].get<float>(), 0.0f, runtime::sky::kMaxStarDensity);
  // Moons' distances by name (0: its usual one), kept for moons the JSON doesn't name.
  s.moon_distance_km.resize(o.moons.size(), 0.0f);
  if (j.contains("moon_distance_km") && j["moon_distance_km"].is_object())
    for (size_t i = 0; i < o.moons.size(); ++i)
      if (j["moon_distance_km"].contains(o.moons[i].name))
        s.moon_distance_km[i] = std::max(0.0f, j["moon_distance_km"][o.moons[i].name].get<float>());
  if (j.contains("shadows")) s.shadows = j["shadows"].get<bool>();
  s.constellations.resize(o.constellations.size(), runtime::sky::kDark);
  if (j.contains("constellations")) {
    const json &c = j["constellations"];
    if (c.is_string() && (c == "dark" || c == "lit" || c == "connected")) {  // all of them
      const uint8_t v = c == "dark" ? 0 : c == "lit" ? 1 : 2;
      std::fill(s.constellations.begin(), s.constellations.end(), v);
    } else if (c.is_object()) {
      for (size_t i = 0; i < o.constellations.size(); ++i) {
        if (!c.contains(o.constellations[i].name)) continue;
        const json &v = c[o.constellations[i].name];
        if (v.is_boolean()) s.constellations[i] = v.get<bool>() ? runtime::sky::kConnected : runtime::sky::kDark;
        else if (v.is_string())
          for (uint8_t k = 0; k < 3; ++k)
            if (v == kStates[k]) s.constellations[i] = k;
      }
    }
  }
  return s;
}

json state_json(const State &s, const Observatory &o) {
  json c = json::object();
  for (size_t i = 0; i < o.constellations.size() && i < s.constellations.size(); ++i)
    if (s.constellations[i] != runtime::sky::kDark) c[o.constellations[i].name] = kStates[std::min<int>(s.constellations[i], 2)];
  auto r3 = [](float v) { return std::round(v * 10000.0f) / 10000.0f; };
  json moons = json::object();
  for (size_t i = 0; i < o.moons.size(); ++i)
    moons[o.moons[i].name] = i < s.moon_distance_km.size() && s.moon_distance_km[i] > 0 ? std::round(s.moon_distance_km[i]) : 0.0f;
  return {{"time_of_day", r3(s.time_of_day)}, {"year", r3(s.year)}, {"latitude_deg", r3(s.latitude_deg)},
          {"clouds", r3(s.clouds)},           {"cloud_wind", {r3(s.cloud_wind[0]), r3(s.cloud_wind[1])}},
          {"aurora", r3(s.aurora)}, {"star_density", r3(s.star_density)},           {"shadows", s.shadows},
          {"moon_distance_km", moons}, {"constellations", c}};
}

std::string clock_text(float t) {
  const int minutes = static_cast<int>(std::floor((t - std::floor(t)) * 24 * 60 + 0.5f)) % (24 * 60);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

std::string date_text(const Observatory &o, float year) {
  const float day = (year - std::floor(year)) * o.year_days;
  char buf[64];
  if (std::fabs(o.year_days - 365.25f) < 1) {
    static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int d = static_cast<int>(day), m = 0;
    while (m < 11 && d >= lengths[m]) d -= lengths[m++];
    std::snprintf(buf, sizeof buf, "%s %d", names[m], d + 1);
  } else {
    std::snprintf(buf, sizeof buf, "day %d of %.0f", static_cast<int>(day) + 1, o.year_days);
  }
  return buf;
}

json describe(const Observatory &o, const State &s) {
  const runtime::sky::Frame f = runtime::sky::compute(o, s);
  auto azimuth = [](const float d[3]) {  // from north (-Z) toward east (+X)
    float a = std::atan2(d[0], -d[2]) * 180 / kPi;
    return a < 0 ? a + 360 : a;
  };
  auto r1 = [](float v) { return std::round(static_cast<double>(v) * 10) / 10; };
  json moons = json::array();
  State later = s;
  later.time_of_day += 0.25f;
  if (later.time_of_day >= 1) {
    later.time_of_day -= 1;
    later.year += 1.0f / o.year_days;
  }
  const runtime::sky::Frame g = runtime::sky::compute(o, later);
  for (size_t i = 0; i < o.moons.size() && i < f.moons.size(); ++i) {
    const auto &m = f.moons[i];
    const bool waxing = g.moons[i].lit > m.lit;
    const char *phase = m.lit < 0.03f ? "new" : m.lit > 0.97f ? "full" : m.lit < 0.47f ? (waxing ? "waxing crescent" : "waning crescent")
                        : m.lit < 0.53f ? (waxing ? "first quarter" : "last quarter") : (waxing ? "waxing gibbous" : "waning gibbous");
    moons.push_back({{"name", o.moons[i].name}, {"altitude_deg", r1(std::asin(m.dir[1]) * 180 / kPi)},
                     {"azimuth_deg", r1(azimuth(m.dir))}, {"lit", std::round(static_cast<double>(m.lit) * 100) / 100}, {"phase", phase}});
  }
  const bool by_sun = std::fabs(f.key_dir[0] - f.sun[0]) + std::fabs(f.key_dir[1] - f.sun[1]) + std::fabs(f.key_dir[2] - f.sun[2]) < 1e-4f;
  auto r2 = [](float v) { return std::round(static_cast<double>(v) * 100) / 100; };
  auto rgb = [&](const float c[3]) { return json::array({r2(c[0]), r2(c[1]), r2(c[2])}); };
  return {{"clock", clock_text(s.time_of_day)},
          {"date", date_text(o, s.year)},
          {"latitude_deg", s.latitude_deg},
          {"sun", {{"altitude_deg", r1(f.sun_altitude_deg)}, {"azimuth_deg", r1(azimuth(f.sun))}}},
          {"moons", moons},
          {"light", {{"key", f.key_dir[1] <= 0 || (f.key[0] + f.key[1] + f.key[2]) < 1e-4f ? "none" : by_sun ? "sun" : "moon"},
                     {"key_color", rgb(f.key)}, {"ambient", rgb(f.ambient)}, {"exposure", r2(f.exposure)}}},
          {"stars", f.sun_altitude_deg < -6 ? "out" : f.sun_altitude_deg < 0 ? "the brightest" : "hidden by day"},
          {"aurora", r2(f.aurora)}};
}

runtime::Mesh sundial(float latitude_deg) {
  Builder b;
  const float lat = std::clamp(std::fabs(latitude_deg), 5.0f, 85.0f) * kPi / 180;
  const float north = latitude_deg >= 0 ? -1.0f : 1.0f;  // the pole's way along Z
  const uint32_t stone = Builder::pack(0.70f, 0.68f, 0.62f), stone_dark = Builder::pack(0.58f, 0.56f, 0.51f);
  const uint32_t paving[2] = {Builder::pack(0.64f, 0.61f, 0.55f), Builder::pack(0.58f, 0.55f, 0.50f)};
  const uint32_t bronze = Builder::pack(0.62f, 0.47f, 0.27f), bronze_dark = Builder::pack(0.42f, 0.31f, 0.17f);
  const uint32_t ink = Builder::pack(0.20f, 0.14f, 0.09f);
  // The paved round: two rings of flagstones, then its edge.
  const float pave = 0.06f;
  for (int ring = 0; ring < 2; ++ring) {
    const float r0 = ring == 0 ? 0 : 1.3f, r1 = ring == 0 ? 1.3f : 2.4f;
    const int n = ring == 0 ? 12 : 24;
    for (int i = 0; i < n; ++i) {
      const float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
      const uint32_t c = paving[(i + ring) % 2];
      const foundry::vec3 p00 = {r0 * std::cos(a0), pave, r0 * std::sin(a0)}, p01 = {r0 * std::cos(a1), pave, r0 * std::sin(a1)};
      const foundry::vec3 p10 = {r1 * std::cos(a0), pave, r1 * std::sin(a0)}, p11 = {r1 * std::cos(a1), pave, r1 * std::sin(a1)};
      if (ring == 0) b.tri({0, pave, 0}, p11, p10, c);
      else b.quad(p00, p01, p11, p10, c);
    }
  }
  for (int i = 0; i < 48; ++i) {
    const float a0 = 2 * kPi * i / 48, a1 = 2 * kPi * (i + 1) / 48, r = 2.4f;
    b.quad({r * std::cos(a0), 0, r * std::sin(a0)}, {r * std::cos(a0), pave, r * std::sin(a0)}, {r * std::cos(a1), pave, r * std::sin(a1)},
           {r * std::cos(a1), 0, r * std::sin(a1)}, stone_dark);
  }
  // Posts around the round, one per two hours of a clock, to throw long shadows.
  for (int i = 0; i < 12; ++i) {
    const float a = 2 * kPi * i / 12;
    b.box({2.05f * std::sin(a), pave + 0.28f, -2.05f * std::cos(a)}, {0.07f, 0.28f, 0.07f}, -a, i == 0 ? stone_dark : stone);
  }
  // The pedestal: a plinth, a shaft, and a capital.
  b.prism(0, 0, pave, 0.18f, 0.36f, 8, stone_dark, stone, kPi / 8);
  b.prism(0, 0, 0.18f, 1.0f, 0.2f, 8, stone, stone, kPi / 8);
  b.prism(0, 0, 1.0f, 1.06f, 0.3f, 8, stone_dark, stone, kPi / 8);
  // The dial: a bronze disc, its hour lines from 6 to 18 radiating from where the gnomon stands.
  const float top = 1.10f;
  b.prism(0, 0, 1.06f, top, 0.46f, 48, bronze_dark, bronze);
  for (int half = 12; half <= 36; ++half) {  // half-hours from 6:00 to 18:00
    const float hour = half * 0.5f;
    const float h = (hour - 12) * 15 * kPi / 180;
    const float theta = std::atan2(std::sin(lat) * std::sin(h), std::cos(h));
    const bool whole = half % 2 == 0;
    const float r0 = whole ? (hour == 12 ? 0.08f : 0.14f) : 0.33f, r1 = 0.42f;
    const float w = whole ? (hour == 12 || hour == 6 || hour == 18 ? 0.006f : 0.004f) : 0.0025f;
    const foundry::vec3 d = {std::sin(theta), 0, north * std::cos(theta)};
    const float mid = 0.5f * (r0 + r1);
    b.box({d.x * mid, top + 0.002f, d.z * mid}, {w, 0.002f, 0.5f * (r1 - r0)}, std::atan2(-d.x, d.z), ink);
  }
  // A notch at the pole's side of the rim.
  b.box({0, top + 0.002f, north * 0.445f}, {0.02f, 0.003f, 0.012f}, 0, ink);
  // The gnomon: a bronze plate in the meridian, its upper edge (the style) pointing at the pole.
  // One length at every latitude, so the gnomon only tilts as the latitude changes: up toward the
  // vertical near a pole, down toward the dial near the equator.
  const float len = 0.36f;
  const float t = 0.007f;
  const foundry::vec3 root = {0, top, 0};
  const foundry::vec3 tip = {0, top + len * std::sin(lat), north * len * std::cos(lat)};
  const foundry::vec3 foot = {0, top, tip.z};
  auto off = [](foundry::vec3 p, float x) { return foundry::vec3{p.x + x, p.y, p.z}; };
  b.tri(off(root, t), off(tip, t), off(foot, t), bronze_dark);
  b.tri(off(root, -t), off(foot, -t), off(tip, -t), bronze_dark);
  b.quad(off(root, -t), off(tip, -t), off(tip, t), off(root, t), bronze);
  b.quad(off(tip, -t), off(foot, -t), off(foot, t), off(tip, t), bronze);
  return std::move(b.m);
}

// --- Observatories ---

Observatories &observatories() {
  static Observatories o;
  return o;
}

double Observatories::now() const {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void Observatories::set_root(const std::string &workspace) {
  std::lock_guard<std::mutex> lock(mutex_);
  root_ = workspace;
  entries_.clear();
}

std::vector<std::string> Observatories::names() const {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    root = root_;
  }
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(root + "/observatories", ec))
    if (e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

bool Observatories::load_locked(const std::string &name, Entry &e, std::string &error) {
  if (!core::valid_name(name)) {
    error = "bad observatory name: " + name;
    return false;
  }
  const std::string path = root_ + "/observatories/" + name + ".json";
  std::error_code ec;
  const auto t = fs::last_write_time(path, ec);
  if (ec) {
    error = "no observatory " + name + " (observatories/" + name + ".json)";
    return false;
  }
  const long long mtime = t.time_since_epoch().count();
  if (e.observatory && e.mtime == mtime) return true;
  json j = core::read_json(path);
  auto o = std::make_shared<Observatory>();
  o->name = name;
  const std::string root = root_;
  auto from = [&root](const std::string &other, json &out) {
    if (!core::valid_name(other)) return false;
    out = core::read_json(root + "/observatories/" + other + ".json");
    return out.is_object();
  };
  if (!parse(j, *o, error, from)) {
    error = name + ": " + error;
    return false;
  }
  const bool first = !e.observatory;
  e.observatory = o;
  e.file = j;
  e.mtime = mtime;
  if (first) {
    e.state = state_of(j.value("start", json::object()), *o, State());
    e.state_at = now();
  } else {
    e.state = state_of(json::object(), *o, e.state);  // constellations follow the chart
  }
  ++version_;
  return true;
}

State Observatories::advanced(const Entry &e) const {
  State s = e.state;
  if (e.hours_per_second != 0) {
    const double days = (now() - e.state_at) * e.hours_per_second / 24.0;
    const double t = s.time_of_day + days;
    const double whole = std::floor(t);
    s.time_of_day = static_cast<float>(t - whole);
    const double y = s.year + whole / e.observatory->year_days;
    s.year = static_cast<float>(y - std::floor(y));
  }
  return s;
}

bool Observatories::get(const std::string &name, std::shared_ptr<const Observatory> &out, State &state, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry &e = entries_[name];
  if (!load_locked(name, e, error)) {
    entries_.erase(name);
    return false;
  }
  out = e.observatory;
  state = advanced(e);
  return true;
}

bool Observatories::set_state(const std::string &name, const State &state, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry &e = entries_[name];
  if (!load_locked(name, e, error)) {
    entries_.erase(name);
    return false;
  }
  e.state = state;
  e.state.constellations.resize(e.observatory->constellations.size(), runtime::sky::kDark);
  e.state_at = now();
  ++version_;
  return true;
}

void Observatories::set_play(const std::string &name, float hours_per_second) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(name);
  if (it == entries_.end() || !it->second.observatory) return;
  it->second.state = advanced(it->second);
  it->second.state_at = now();
  it->second.hours_per_second = hours_per_second;
  ++version_;
}

float Observatories::play(const std::string &name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(name);
  return it == entries_.end() ? 0.0f : it->second.hours_per_second;
}

bool Observatories::save(const std::string &name, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(name);
  if (it == entries_.end() || !it->second.observatory) {
    error = "observatory " + name + " isn't loaded";
    return false;
  }
  Entry &e = it->second;
  json j = e.file;
  j["start"] = state_json(advanced(e), *e.observatory);
  std::string text;
  compact(j, text, 0);
  const std::string path = root_ + "/observatories/" + name + ".json";
  if (!core::write_text(path, text + "\n")) {
    error = "can't write " + path;
    return false;
  }
  e.file = j;
  std::error_code ec;
  e.mtime = fs::last_write_time(path, ec).time_since_epoch().count();
  return true;
}

bool Observatories::set_seeds(const std::string &name, long long star_seed, long long cloud_seed, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry &e = entries_[name];
  if (!load_locked(name, e, error)) {
    entries_.erase(name);
    return false;
  }
  json j = e.file;
  if (star_seed >= 0) j["stars"]["seed"] = static_cast<uint32_t>(star_seed);
  if (cloud_seed >= 0) j["clouds"]["seed"] = static_cast<uint32_t>(cloud_seed);
  if (j == e.file) return true;
  std::string text;
  compact(j, text, 0);
  const std::string path = root_ + "/observatories/" + name + ".json";
  if (!core::write_text(path, text + "\n")) {
    error = "can't write " + path;
    return false;
  }
  e.mtime = 0;  // read it again (keeping the state), so its sky is baked from the new seeds
  return load_locked(name, e, error);
}

bool Observatories::reset(const std::string &name, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry &e = entries_[name];
  if (!load_locked(name, e, error)) {
    entries_.erase(name);
    return false;
  }
  e.state = state_of(e.file.value("start", json::object()), *e.observatory, State());
  e.state_at = now();
  e.hours_per_second = 0;
  ++version_;
  return true;
}

std::string Observatories::view_of(const std::string &item) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = views_.find(item);
  return it == views_.end() ? "" : it->second;
}

void Observatories::set_view(const std::string &item, const std::string &name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (name.empty()) views_.erase(item);
  else views_[item] = name;
  ++version_;
}

bool Observatories::scene(const std::string &name, viewport::SkyScene &out) {
  std::string error;
  if (name.empty() || !get(name, out.observatory, out.state, error)) return false;
  out.time = now();
  return true;
}

bool Observatories::scene_for(const std::string &item, viewport::SkyScene &out) { return scene(view_of(item), out); }

std::shared_ptr<const runtime::Mesh> Observatories::sundial_for(float latitude_deg) {
  const int key = static_cast<int>(std::lround(latitude_deg * 10));
  std::lock_guard<std::mutex> lock(mutex_);
  std::shared_ptr<const runtime::Mesh> m = sundials_[key];
  if (!m) m = std::make_shared<runtime::Mesh>(sundial(key / 10.0f));
  if (sundials_.size() > 16) sundials_.clear();  // a slider swept through latitudes
  sundials_[key] = m;
  return m;
}

int Observatories::version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

}  // namespace sky
