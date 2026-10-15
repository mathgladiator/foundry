#include "map/map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>

#include "core/util.h"
#include "pipeline/config.h"

namespace fs = std::filesystem;

namespace map {

using json = nlohmann::json;

namespace {

constexpr float kSqrt3 = 1.7320508f;

std::string path_of(const forge::Paths &paths, const std::string &name) { return paths.workspace + "/maps/" + name + ".json"; }

// The nearest cell to a place in metres (cube rounding).
void to_axial(float x, float z, int &q, int &r) {
  const float fr = z / 1.5f, fq = x / kSqrt3 - fr * 0.5f, fs = -fq - fr;
  int rq = static_cast<int>(std::lround(fq)), rr = static_cast<int>(std::lround(fr));
  const int rs = static_cast<int>(std::lround(fs));
  const float eq = std::fabs(rq - fq), er = std::fabs(rr - fr), es = std::fabs(rs - fs);
  if (eq > er && eq > es) rq = -rr - rs;
  else if (er > es) rr = -rq - rs;
  q = rq;
  r = rr;
}

// Where an op puts something: "at": [q, r], or q and r, or x and z in metres.
bool place_of(const json &op, json &target) {
  if (op.contains("at") && op["at"].is_array() && op["at"].size() == 2) {
    target["q"] = op["at"][0];
    target["r"] = op["at"][1];
    return true;
  }
  if (op.contains("x") && op.contains("z")) {
    int q, r;
    to_axial(op["x"].get<float>(), op["z"].get<float>(), q, r);
    target["q"] = q;
    target["r"] = r;
    return true;
  }
  if (op.contains("q") && op.contains("r")) {
    target["q"] = op["q"];
    target["r"] = op["r"];
    return true;
  }
  return false;
}

// A site's flat by its kind, cells.
uint32_t default_radius(const std::string &kind) {
  static const std::map<std::string, uint32_t> r = {{"town", 46},   {"village", 32}, {"hamlet", 20}, {"keep", 24},
                                                    {"castle", 28}, {"house", 7},    {"farm", 11},   {"ruin", 9},
                                                    {"camp", 6},    {"shrine", 4},   {"tower", 5},   {"landmark", 4}};
  auto it = r.find(kind);
  return it == r.end() ? 8u : it->second;
}

// A list's entry by name, or end.
json::iterator by_name(json &list, const std::string &name) {
  for (auto it = list.begin(); it != list.end(); ++it)
    if (it->value("name", "") == name) return it;
  return list.end();
}

// A channel struct's value with every field: its blank, overlaid by what's given.
json filled(const std::string &channel, const json &given) {
  json v = pipeline::config::blank(channel);
  if (given.is_object())
    for (auto it = given.begin(); it != given.end(); ++it)
      if (v.contains(it.key())) v[it.key()] = it.value();
  return v;
}

}  // namespace

std::vector<std::string> names(const forge::Paths &paths) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(paths.workspace + "/maps", ec))
    if (e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

bool load(const forge::Paths &paths, const std::string &name, json &file, std::string &error) {
  file = core::read_json(path_of(paths, name));
  if (!file.is_object()) {
    error = "no map " + name + " (maps/" + name + ".json)";
    return false;
  }
  return true;
}

bool save(const forge::Paths &paths, const std::string &name, const json &file, std::string &error) {
  std::error_code ec;
  fs::create_directories(paths.workspace + "/maps", ec);
  if (!core::write_json(path_of(paths, name), file)) {
    error = "can't write maps/" + name + ".json";
    return false;
  }
  return true;
}

json blank(const std::string &name, uint32_t radius) {
  json shape = {{"name", name},    {"kind", "hexagon"}, {"radius", radius}, {"width", 100}, {"depth", 100}, {"notch", 40},
                {"corners", json::array()}, {"rotation", 0}, {"wobble", 18}, {"seed", 7},
                {"sides", json::array({{{"edge", "forest"}, {"height", 4}, {"rise", 0.5}}})}, {"openings", json::array()}};
  return {{"name", name},
          {"doc", ""},
          {"seed", 1},
          {"biome", "temperate"},
          {"chunk_size", 64},
          {"shape", shape},
          {"parcels", json::array({{{"name", "heartland"}, {"q", 0}, {"r", 0}, {"kind", "plains"}, {"plotter", "happy_glen"}}})},
          {"sites", json::array()},
          {"links", json::array()},
          {"rivers", json::array()}};
}

bool apply(json &file, const json &ops, std::string &error) {
  const json list = ops.is_array() ? ops : json::array({ops});
  for (auto key : {"parcels", "sites", "links", "rivers"})
    if (!file.contains(key) || !file[key].is_array()) file[key] = json::array();
  for (const json &op : list) {
    const std::string kind = op.value("op", "");
    if (kind == "parcel" || kind == "site" || kind == "river") {
      const std::string name = op.value("name", "");
      if (name.empty()) {
        error = kind + " needs a name";
        return false;
      }
      json &l = file[kind + "s"];
      auto it = by_name(l, name);
      json e = it != l.end() ? *it : json{{"name", name}};
      place_of(op, e);
      if (!e.contains("q")) {
        error = kind + " " + name + " needs a place: at [q, r], or x and z in metres";
        return false;
      }
      for (auto f = op.begin(); f != op.end(); ++f)
        if (f.key() != "op" && f.key() != "at" && f.key() != "x" && f.key() != "z") e[f.key()] = f.value();
      if (kind == "site" && !e.contains("radius")) e["radius"] = default_radius(e.value("kind", "house"));
      if (kind == "site" && !e.contains("yaw")) e["yaw"] = -1;
      if (kind == "site" && e.contains("params") && e["params"].is_object()) e["params"] = e["params"].dump();
      if (it != l.end()) *it = e;
      else l.push_back(e);
    } else if (kind == "remove_parcel" || kind == "remove_site" || kind == "remove_river") {
      json &l = file[kind.substr(7) + "s"];
      auto it = by_name(l, op.value("name", ""));
      if (it == l.end()) {
        error = "no " + kind.substr(7) + " " + op.value("name", "");
        return false;
      }
      l.erase(it);
      if (kind == "remove_site") {  // and its links
        json kept = json::array();
        for (const json &lk : file["links"])
          if (lk.value("from", "") != op.value("name", "") && lk.value("to", "") != op.value("name", "")) kept.push_back(lk);
        file["links"] = kept;
      }
    } else if (kind == "link" || kind == "unlink") {
      const std::string from = op.value("from", ""), to = op.value("to", "");
      json &l = file["links"];
      auto it = l.end();
      for (auto i = l.begin(); i != l.end(); ++i)
        if ((i->value("from", "") == from && i->value("to", "") == to) || (i->value("from", "") == to && i->value("to", "") == from)) it = i;
      if (kind == "unlink") {
        if (it == l.end()) {
          error = "no link " + from + " - " + to;
          return false;
        }
        l.erase(it);
        continue;
      }
      json e = {{"from", from}, {"to", to}, {"kind", op.value("kind", "road")}, {"via", op.value("via", json::array())}};
      if (it != l.end()) *it = e;
      else l.push_back(e);
    } else if (kind == "shape") {
      for (auto f = op.begin(); f != op.end(); ++f)
        if (f.key() != "op") file["shape"][f.key()] = f.value();
    } else if (kind == "seed" || kind == "biome" || kind == "doc" || kind == "chunk_size") {
      file[kind] = op["value"];
    } else {
      error = "unknown op " + kind;
      return false;
    }
  }
  return true;
}

json spec_of(const json &file) {
  json spec = {{"name", file.value("name", "")}, {"seed", file.value("seed", 1)}, {"chunk_size", file.value("chunk_size", 64)}};
  json shape = filled("terrain_shape", file.value("shape", json::object()));
  json sides = json::array(), openings = json::array();
  for (const json &sd : shape["sides"]) sides.push_back(filled("terrain_side", sd));
  for (const json &o : shape["openings"]) openings.push_back(filled("terrain_opening", o));
  shape["sides"] = sides;
  shape["openings"] = openings;
  spec["shape"] = shape;
  static const json parcel_defaults = {{"kind", "plains"}, {"reach", 0},     {"height", 5},     {"relief", 8},    {"hill_size", 260},
                                       {"roughness", 0.4}, {"ridges", 0.2}, {"cliffs", 0.2},  {"caves", 0.1}, {"moisture", 0.5}};
  json parcels = json::array();
  for (const json &p : file.value("parcels", json::array())) {
    json v = parcel_defaults;
    v.update(p);
    parcels.push_back(filled("map_parcel", v));
  }
  spec["parcels"] = parcels;
  json sites = json::array();
  for (const json &st : file.value("sites", json::array())) {
    json v = {{"kind", "house"}, {"yaw", -1}, {"seed", 1}};
    v.update(st);
    if (v.contains("params") && v["params"].is_object()) v["params"] = v["params"].dump();
    sites.push_back(filled("map_site", v));
  }
  spec["sites"] = sites;
  json links = json::array();
  for (const json &l : file.value("links", json::array())) links.push_back(filled("map_link", l));
  spec["links"] = links;
  json rivers = json::array();
  for (const json &r : file.value("rivers", json::array())) {
    json v = {{"width", 3}};
    v.update(r);
    rivers.push_back(filled("map_river", v));
  }
  spec["rivers"] = rivers;
  return spec;
}

PlanRun plan(const forge::Paths &paths, wasm::Runner &runner, const json &file, bool verify_native) {
  PlanRun out;
  forge::RunResult r = forge::run(paths, runner, "map_plan", spec_of(file), verify_native);
  out.milliseconds = r.milliseconds;
  out.fuel = r.fuel;
  out.native_checked = r.native_checked;
  out.native_match = r.native_match;
  if (r.status != 0) {
    out.error = r.error.empty() ? "map_plan failed" : r.error;
    return out;
  }
  out.plan = r.output;
  out.ok = true;
  return out;
}

json report(const json &plan) {
  json r = json::object();
  for (auto it = plan.begin(); it != plan.end(); ++it)
    if (!it.value().is_array() && !it.value().is_object()) r[it.key()] = it.value();
  r["chunks"] = plan.value("chunk_q", json::array()).size();
  r["segments"] = plan.value("roads", json::array()).size();
  json parcels = json::array();
  const json &ps = plan.value("parcels", json::array());
  const json &cells = plan.value("parcel_cells", json::array());
  for (size_t i = 0; i < ps.size(); ++i) {
    const double area = i < cells.size() ? cells[i].get<double>() * 64.0 / 1e6 : 0;  // 8 m nodes, km2
    parcels.push_back({{"name", ps[i].value("name", "")}, {"kind", ps[i].value("kind", json())}, {"km2", std::round(area * 100) / 100}});
  }
  r["parcels"] = parcels;
  json places = json::array();
  for (const json &p : plan.value("places", json::array()))
    places.push_back({{"name", p.value("name", "")}, {"position", p["position"]}, {"yaw", p.value("yaw", 0.0)}});
  r["places"] = places;
  return r;
}

json chunk_spec(const json &plan, size_t chunk, uint32_t seed) {
  json c = json::object();
  const int q0 = plan["chunk_q"][chunk].get<int>(), r0 = plan["chunk_r"][chunk].get<int>();
  const int S = plan.value("chunk_size", 64);
  c["q0"] = q0;
  c["r0"] = r0;
  c["size"] = S;
  c["seed"] = seed;
  c["outline"] = plan["outline"];
  // The chunk's box in metres, with a margin for the grid's interpolation and what reaches into it.
  float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
  for (int corner = 0; corner < 4; ++corner) {
    const int q = q0 + ((corner & 1) ? S + 1 : -2), r = r0 + ((corner & 2) ? S + 1 : -2);
    const float x = kSqrt3 * (static_cast<float>(q) + static_cast<float>(r) * 0.5f), z = 1.5f * static_cast<float>(r);
    xmin = std::min(xmin, x);
    xmax = std::max(xmax, x);
    zmin = std::min(zmin, z);
    zmax = std::max(zmax, z);
  }
  const float gx = plan["gx"].get<float>(), gz = plan["gz"].get<float>(), sp = plan["spacing"].get<float>();
  const int nx = plan["nx"].get<int>(), nz = plan["nz"].get<int>();
  const int i0 = std::max(0, static_cast<int>(std::floor((xmin - gx) / sp)) - 2), i1 = std::min(nx - 1, static_cast<int>(std::ceil((xmax - gx) / sp)) + 2);
  const int k0 = std::max(0, static_cast<int>(std::floor((zmin - gz) / sp)) - 2), k1 = std::min(nz - 1, static_cast<int>(std::ceil((zmax - gz) / sp)) + 2);
  c["gx"] = gx + static_cast<float>(i0) * sp;
  c["gz"] = gz + static_cast<float>(k0) * sp;
  c["spacing"] = sp;
  c["nx"] = i1 - i0 + 1;
  c["nz"] = k1 - k0 + 1;
  json h = json::array(), p = json::array(), w = json::array();
  for (int k = k0; k <= k1; ++k)
    for (int i = i0; i <= i1; ++i) {
      const size_t a = static_cast<size_t>(k) * static_cast<size_t>(nx) + static_cast<size_t>(i);
      h.push_back(plan["heights"][a]);
      p.push_back(plan["parcel_ids"][a]);
      w.push_back(plan["water"][a]);
    }
  c["heights"] = h;
  c["parcel_ids"] = p;
  c["water"] = w;
  c["parcels"] = plan["parcels"];
  json roads = json::array();
  for (const json &sg : plan["roads"]) {
    const float reach = sg["width"].get<float>() * 0.5f + 45.0f;  // a river's valley reaches 40 m
    const float ax = sg["a"][0].get<float>(), az = sg["a"][2].get<float>(), bx = sg["b"][0].get<float>(), bz = sg["b"][2].get<float>();
    if (std::max(ax, bx) + reach < xmin || std::min(ax, bx) - reach > xmax || std::max(az, bz) + reach < zmin || std::min(az, bz) - reach > zmax)
      continue;
    roads.push_back(sg);
  }
  c["roads"] = roads;
  json flats = json::array();
  for (const json &f : plan["flats"]) {
    const float reach = f["radius"].get<float>() * 2 + 30.0f;
    const float fx = f["position"][0].get<float>(), fz = f["position"][2].get<float>();
    if (fx + reach < xmin || fx - reach > xmax || fz + reach < zmin || fz - reach > zmax) continue;
    flats.push_back(f);
  }
  c["flats"] = flats;
  return c;
}

json far_spec(const forge::Paths &paths, const json &file, const json &plan, const std::string &season, float sink) {
  const json biome = pipeline::config::entry(paths, "boundary_biomes", file.value("biome", "temperate"));
  return {{"plan", plan}, {"biome", biome.is_object() ? biome["value"] : json::object()}, {"season", season}, {"sink", sink}};
}

// ---- The atlas ---------------------------------------------------------------------------------

namespace {

struct Canvas {
  int w, h;
  std::vector<float> rgb;
  void blend(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    float *p = &rgb[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
    p[0] += (r - p[0]) * a;
    p[1] += (g - p[1]) * a;
    p[2] += (b - p[2]) * a;
  }
  // A thick line, antialiased by distance.
  void line(float x0, float y0, float x1, float y1, float width, float r, float g, float b, float a = 1) {
    const int xa = static_cast<int>(std::floor(std::min(x0, x1) - width)), xb = static_cast<int>(std::ceil(std::max(x0, x1) + width));
    const int ya = static_cast<int>(std::floor(std::min(y0, y1) - width)), yb = static_cast<int>(std::ceil(std::max(y0, y1) + width));
    const float dx = x1 - x0, dy = y1 - y0, l2 = std::max(dx * dx + dy * dy, 1e-6f);
    for (int y = ya; y <= yb; ++y)
      for (int x = xa; x <= xb; ++x) {
        const float t = std::clamp(((x - x0) * dx + (y - y0) * dy) / l2, 0.0f, 1.0f);
        const float ex = x0 + dx * t - x, ey = y0 + dy * t - y;
        const float d = std::sqrt(ex * ex + ey * ey);
        blend(x, y, r, g, b, a * std::clamp(width * 0.5f + 0.5f - d, 0.0f, 1.0f));
      }
  }
  void disc(float cx, float cy, float rad, float r, float g, float b, float a = 1) {
    for (int y = static_cast<int>(cy - rad - 1); y <= static_cast<int>(cy + rad + 1); ++y)
      for (int x = static_cast<int>(cx - rad - 1); x <= static_cast<int>(cx + rad + 1); ++x) {
        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        blend(x, y, r, g, b, a * std::clamp(rad + 0.5f - d, 0.0f, 1.0f));
      }
  }
  // Text in a 5 x 7 bitmap font, scaled, with a dark outline; x, y its top left.
  void text(const std::string &s, int x, int y, int scale, float r, float g, float b) {
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t *gl = glyph(s[i]);
        for (int row = 0; row < 7; ++row)
          for (int col = 0; col < 5; ++col) {
            if (!(gl[row] & (0x10 >> col))) continue;
            const int px = x + static_cast<int>(i) * 6 * scale + col * scale, py = y + row * scale;
            for (int a = 0; a < scale; ++a)
              for (int c = 0; c < scale; ++c) {
                if (pass == 0) {
                  for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) blend(px + c + ox, py + a + oy, 0.05f, 0.05f, 0.06f, 0.85f);
                } else {
                  blend(px + c, py + a, r, g, b, 1);
                }
              }
          }
      }
  }
  static int text_width(const std::string &s, int scale) { return static_cast<int>(s.size()) * 6 * scale - scale; }
  static const uint8_t *glyph(char c) {
    static const uint8_t font[][7] = {
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
        {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
        // digits
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
        // - : . ' ( ) / space
        {0, 0, 0, 0x1F, 0, 0, 0}, {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0}, {0, 0, 0, 0, 0, 0x0C, 0x0C}, {0x0C, 0x04, 0x08, 0, 0, 0, 0},
        {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
        {0, 0, 0, 0, 0, 0, 0}};
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return font[c - 'A'];
    if (c >= '0' && c <= '9') return font[26 + (c - '0')];
    switch (c) {
      case '-': return font[36];
      case ':': return font[37];
      case '.': return font[38];
      case '\'': return font[39];
      case '(': return font[40];
      case ')': return font[41];
      case '/': return font[42];
      default: return font[43];
    }
  }
};

// Each parcel kind's colour on the atlas.
void kind_colour(const std::string &kind, float c[3]) {
  static const std::map<std::string, std::array<float, 3>> k = {
      {"plains", {0.62f, 0.72f, 0.40f}},    {"hills", {0.48f, 0.64f, 0.34f}},  {"highlands", {0.56f, 0.58f, 0.40f}},
      {"mountains", {0.58f, 0.55f, 0.50f}}, {"valley", {0.55f, 0.74f, 0.38f}}, {"lake", {0.30f, 0.50f, 0.70f}},
      {"marsh", {0.42f, 0.54f, 0.40f}},     {"coast", {0.80f, 0.76f, 0.58f}}};
  auto it = k.find(kind);
  const auto v = it == k.end() ? std::array<float, 3>{0.6f, 0.6f, 0.6f} : it->second;
  c[0] = v[0];
  c[1] = v[1];
  c[2] = v[2];
}

}  // namespace

viewport::Image atlas(const json &file, const json &plan, const AtlasOptions &o) {
  const int W = std::clamp(o.size, 256, 4096);
  Canvas cv{W, W, std::vector<float>(static_cast<size_t>(W) * static_cast<size_t>(W) * 3, 0.12f)};
  const float gx = plan["gx"].get<float>(), gz = plan["gz"].get<float>(), sp = plan["spacing"].get<float>();
  const int nx = plan["nx"].get<int>(), nz = plan["nz"].get<int>();
  const std::vector<float> h = plan["heights"].get<std::vector<float>>(), water = plan["water"].get<std::vector<float>>();
  const std::vector<uint32_t> pid = plan["parcel_ids"].get<std::vector<uint32_t>>();
  const json &parcels = plan["parcels"];
  // The view: the grid's box, square, with a margin.
  const float wx = static_cast<float>(nx - 1) * sp, wz = static_cast<float>(nz - 1) * sp;
  const float span = std::max(wx, wz) * 1.04f;
  const float cx = gx + wx * 0.5f, cz = gz + wz * 0.5f;
  const float px_per_m = static_cast<float>(W) / span;
  auto to_px = [&](float x, float z, float &u, float &v) {
    u = (x - cx) * px_per_m + static_cast<float>(W) * 0.5f;
    v = (z - cz) * px_per_m + static_cast<float>(W) * 0.5f;
  };
  auto node = [&](int i, int k) { return static_cast<size_t>(std::clamp(k, 0, nz - 1)) * static_cast<size_t>(nx) + static_cast<size_t>(std::clamp(i, 0, nx - 1)); };
  std::vector<int> pix_parcel(static_cast<size_t>(W) * static_cast<size_t>(W), -1);
  float hmin = 1e30f, hmax = -1e30f;
  for (size_t a = 0; a < h.size(); ++a)
    if (pid[a] != 65535) {
      hmin = std::min(hmin, h[a]);
      hmax = std::max(hmax, h[a]);
    }
  for (int y = 0; y < W; ++y)
    for (int x = 0; x < W; ++x) {
      const float mx = cx + (static_cast<float>(x) + 0.5f - W * 0.5f) / px_per_m, mz = cz + (static_cast<float>(y) + 0.5f - W * 0.5f) / px_per_m;
      const float fi = (mx - gx) / sp, fk = (mz - gz) / sp;
      const int i = static_cast<int>(std::floor(fi)), k = static_cast<int>(std::floor(fk));
      if (i < 0 || k < 0 || i >= nx - 1 || k >= nz - 1) continue;
      const size_t a = node(static_cast<int>(std::lround(fi)), static_cast<int>(std::lround(fk)));
      if (pid[a] == 65535) continue;
      pix_parcel[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = static_cast<int>(pid[a]);
      const float u = fi - i, v = fk - k;
      auto H = [&](int ii, int kk) { return h[node(ii, kk)]; };
      const float hh = (H(i, k) * (1 - u) + H(i + 1, k) * u) * (1 - v) + (H(i, k + 1) * (1 - u) + H(i + 1, k + 1) * u) * v;
      // Hill shade from the north-west.
      const float dx = (H(i + 1, k) - H(i - 1, k)) / (2 * sp), dz = (H(i, k + 1) - H(i, k - 1)) / (2 * sp);
      const float shade = std::clamp(0.78f + (-dx * 0.7f - dz * 0.7f) * 1.6f, 0.45f, 1.25f);
      float c[3];
      kind_colour(pid[a] < parcels.size() ? parcels[pid[a]].value("kind", "plains") : "plains", c);
      const float lift = hmax > hmin ? (hh - hmin) / (hmax - hmin) : 0;
      float *p = &cv.rgb[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 3];
      for (int ch = 0; ch < 3; ++ch) p[ch] = std::clamp(c[ch] * shade * (0.85f + 0.3f * lift), 0.0f, 1.0f);
      if (lift > 0.82f) for (int ch = 0; ch < 3; ++ch) p[ch] = p[ch] + (0.95f - p[ch]) * (lift - 0.82f) * 3.0f;  // snowy tops
      if (water[a] > -999 && water[a] > hh - 0.05f) {
        const float depth = std::clamp((water[a] - hh) / 6.0f, 0.0f, 1.0f);
        p[0] = 0.32f - 0.15f * depth;
        p[1] = 0.55f - 0.18f * depth;
        p[2] = 0.78f - 0.12f * depth;
      }
    }
  // Parcel borders.
  for (int y = 1; y < W; ++y)
    for (int x = 1; x < W; ++x) {
      const int a = pix_parcel[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)];
      if (a < 0) continue;
      if (a != pix_parcel[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x - 1)] ||
          a != pix_parcel[static_cast<size_t>(y - 1) * static_cast<size_t>(W) + static_cast<size_t>(x)])
        cv.blend(x, y, 0.15f, 0.15f, 0.12f, 0.55f);
    }
  // A faint 250 m grid, to read places off.
  if (o.grid) {
    const float step = 250.0f;
    for (float m = std::ceil((cx - span) / step) * step; m <= cx + span; m += step) {
      float u, v;
      to_px(m, 0, u, v);
      cv.line(u, 0, u, static_cast<float>(W), 1.0f, 1, 1, 1, std::fabs(m) < 1 ? 0.22f : 0.08f);
    }
    for (float m = std::ceil((cz - span) / step) * step; m <= cz + span; m += step) {
      float u, v;
      to_px(0, m, u, v);
      cv.line(0, v, static_cast<float>(W), v, 1.0f, 1, 1, 1, std::fabs(m) < 1 ? 0.22f : 0.08f);
    }
  }
  if (o.chunks) {
    const int S = plan.value("chunk_size", 64);
    const json &cq = plan["chunk_q"], &cr = plan["chunk_r"];
    for (size_t c = 0; c < cq.size(); ++c) {
      const int q = cq[c].get<int>(), r = cr[c].get<int>();
      const int corners[5][2] = {{q, r}, {q + S, r}, {q + S, r + S}, {q, r + S}, {q, r}};
      for (int e = 0; e < 4; ++e) {
        float u0, v0, u1, v1;
        to_px(kSqrt3 * (corners[e][0] + corners[e][1] * 0.5f), 1.5f * corners[e][1], u0, v0);
        to_px(kSqrt3 * (corners[e + 1][0] + corners[e + 1][1] * 0.5f), 1.5f * corners[e + 1][1], u1, v1);
        cv.line(u0, v0, u1, v1, 1.0f, 0, 0, 0, 0.18f);
      }
    }
  }
  // Rivers, then roads by kind (bridges darker).
  for (int pass = 0; pass < 2; ++pass)
    for (const json &sg : plan["roads"]) {
      const uint32_t kind = sg.value("kind", 0u);
      if ((pass == 0) != (kind == 3)) continue;
      float u0, v0, u1, v1;
      to_px(sg["a"][0].get<float>(), sg["a"][2].get<float>(), u0, v0);
      to_px(sg["b"][0].get<float>(), sg["b"][2].get<float>(), u1, v1);
      const float wpx = std::max(kind == 3 ? 1.6f : 1.2f, sg["width"].get<float>() * px_per_m * (kind == 3 ? 1.0f : 1.6f));
      if (kind == 3) cv.line(u0, v0, u1, v1, wpx, 0.25f, 0.48f, 0.80f);
      else {
        const bool bridge = sg.value("bridge", false);
        const float col[3][3] = {{0.95f, 0.85f, 0.55f}, {0.86f, 0.70f, 0.45f}, {0.70f, 0.58f, 0.40f}};
        cv.line(u0, v0, u1, v1, wpx + 1.6f, 0.2f, 0.16f, 0.1f, 0.7f);
        cv.line(u0, v0, u1, v1, wpx, bridge ? 0.5f : col[kind][0], bridge ? 0.4f : col[kind][1], bridge ? 0.3f : col[kind][2]);
      }
    }
  // The outline.
  {
    const json &ol = plan["outline"];
    for (size_t i = 0; i < ol.size(); ++i) {
      const json &a = ol[i], &b = ol[(i + 1) % ol.size()];
      float u0, v0, u1, v1;
      to_px(a[0].get<float>(), a[2].get<float>(), u0, v0);
      to_px(b[0].get<float>(), b[2].get<float>(), u1, v1);
      cv.line(u0, v0, u1, v1, 2.0f, 0.95f, 0.95f, 0.95f, 0.8f);
    }
  }
  // Sites, then names.
  static const std::map<std::string, std::array<float, 3>> site_colour = {
      {"town", {0.85f, 0.20f, 0.18f}},  {"village", {0.92f, 0.45f, 0.15f}}, {"hamlet", {0.95f, 0.65f, 0.25f}},
      {"keep", {0.55f, 0.30f, 0.75f}},  {"castle", {0.45f, 0.20f, 0.65f}},  {"house", {0.98f, 0.98f, 0.92f}},
      {"farm", {0.95f, 0.90f, 0.55f}},  {"ruin", {0.55f, 0.52f, 0.48f}},    {"camp", {0.90f, 0.55f, 0.35f}},
      {"shrine", {0.70f, 0.85f, 0.95f}}, {"tower", {0.75f, 0.75f, 0.85f}},   {"landmark", {0.95f, 0.85f, 0.95f}}};
  const json &places = plan.value("places", json::array());
  const json &sites = file.value("sites", json::array());
  for (size_t i = 0; i < places.size(); ++i) {
    const json &pl = places[i];
    float u, v;
    to_px(pl["position"][0].get<float>(), pl["position"][2].get<float>(), u, v);
    const std::string kind = i < sites.size() ? sites[i].value("kind", "house") : "house";
    const auto it = site_colour.find(kind);
    const auto col = it == site_colour.end() ? std::array<float, 3>{1, 1, 1} : it->second;
    const float rad = std::max(3.0f, pl.value("radius", 4.0f) * px_per_m);
    cv.disc(u, v, rad + 1.5f, 0.08f, 0.08f, 0.08f, 0.8f);
    cv.disc(u, v, rad, col[0], col[1], col[2], 0.9f);
  }
  if (o.labels) {
    const int scale = W >= 1536 ? 3 : 2;
    for (size_t i = 0; i < parcels.size(); ++i) {
      const float x = 1.7320508f * (parcels[i].value("q", 0) + parcels[i].value("r", 0) * 0.5f), z = 1.5f * parcels[i].value("r", 0);
      float u, v;
      to_px(x, z, u, v);
      const std::string label = parcels[i].value("name", "");
      cv.text(label, static_cast<int>(u) - Canvas::text_width(label, scale - 1 > 0 ? scale - 1 : 1) / 2, static_cast<int>(v) - 4,
              std::max(1, scale - 1), 0.85f, 0.95f, 0.85f);
    }
    for (size_t i = 0; i < places.size(); ++i) {
      const json &pl = places[i];
      float u, v;
      to_px(pl["position"][0].get<float>(), pl["position"][2].get<float>(), u, v);
      const std::string label = pl.value("name", "");
      const float rad = std::max(3.0f, pl.value("radius", 4.0f) * px_per_m);
      cv.text(label, static_cast<int>(u) - Canvas::text_width(label, scale) / 2, static_cast<int>(v + rad) + 4, scale, 1, 1, 0.92f);
    }
  }
  // The scale bar (500 m) and north.
  {
    const float bar = 500.0f * px_per_m;
    const float x0 = 20, y0 = static_cast<float>(W) - 28;
    cv.line(x0, y0, x0 + bar, y0, 4, 0.05f, 0.05f, 0.05f);
    cv.line(x0, y0, x0 + bar, y0, 2, 1, 1, 1);
    cv.text("500 M", static_cast<int>(x0 + bar + 8), static_cast<int>(y0) - 7, 2, 1, 1, 1);
    cv.text("N", W - 30, 14, 3, 1, 1, 1);
    cv.line(static_cast<float>(W) - 22, 40, static_cast<float>(W) - 22, 70, 3, 1, 1, 1);
  }
  viewport::Image img;
  img.width = W;
  img.height = W;
  img.rgba.resize(static_cast<size_t>(W) * static_cast<size_t>(W) * 4);
  for (size_t i = 0; i < static_cast<size_t>(W) * static_cast<size_t>(W); ++i) {
    for (int c = 0; c < 3; ++c) img.rgba[i * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(cv.rgb[i * 3 + static_cast<size_t>(c)], 0.0f, 1.0f) * 255.0f + 0.5f);
    img.rgba[i * 4 + 3] = 255;
  }
  return img;
}

}  // namespace map
