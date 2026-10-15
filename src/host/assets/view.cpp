#include "assets/view.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include "assets/fracture_item.h"
#include "assets/lod.h"
#include "assets/mesh_item.h"
#include "core/util.h"

namespace assets {

namespace {

void conformance_from_fit(const nlohmann::json &d, ViewContent &v) {
  if (d.contains("fit") && d["fit"].contains("conformance")) {
    v.has_conformance = true;
    v.radius = d["fit"]["conformance"].value("radius", 0.0f);
    v.height = d["fit"]["conformance"].value("height", 0.0f);
  }
}

std::mutex g_provider_mutex;
LiveProvider g_provider;

}  // namespace

void set_live_provider(LiveProvider provider) {
  std::lock_guard<std::mutex> lock(g_provider_mutex);
  g_provider = std::move(provider);
}

ViewContent load_view(const workspace::Workspace &ws, const std::string &id) {
  ViewContent v;
  if (id == "preview") {
    workspace::Preview p = ws.preview();
    v.label = p.label;
    v.mesh = p.mesh;
    v.has_conformance = p.has_conformance;
    v.radius = p.radius;
    v.height = p.height;
    return v;
  }
  workspace::Item item;
  if (!ws.find(id, item)) return v;
  v.label = item.id;
  if (item.kind == "pipeline" || item.kind == "config") {
    LiveProvider provider;
    {
      std::lock_guard<std::mutex> lock(g_provider_mutex);
      provider = g_provider;
    }
    if (provider && provider(item, v)) return v;
  }
  const std::string sub = item.data.value("kind", "");
  if (item.kind == "meshy" && sub == "concept") {
    auto img = std::make_shared<viewport::Image>();
    if (viewport::load_image(item.path + "/image.png", *img)) v.image = img;
  } else if (item.kind == "reference") {
    auto img = std::make_shared<viewport::Image>();
    if (viewport::load_image(item.path, *img)) v.image = img;
  } else if (item.kind == "meshy" && sub == "mesh") {
    auto mesh = std::make_shared<runtime::Mesh>();
    std::string ignored;
    if (load_full(ws.root(), item.name, *mesh, ignored)) v.mesh = mesh;
    auto pm = std::make_shared<ProgressiveMesh>();
    if (load_item_progressive(ws.root(), item.name, *pm)) v.progressive = pm;
    conformance_from_fit(item.data, v);
  } else if (item.kind == "fixed") {
    for (int k = 0;; ++k) {
      auto lod = std::make_shared<runtime::Mesh>();
      if (!load_mesh(item.path + "/lod" + std::to_string(k) + ".bin", *lod)) break;
      v.lods.push_back(lod);
    }
    if (!v.lods.empty()) v.mesh = v.lods[0];
    conformance_from_fit(item.data, v);
  } else if (item.kind == "fracture") {
    // A fracture shows assembled (the moment it breaks); the inspector plays it.
    fracture::Fracture f;
    if (fracture_load(ws.root(), item.name, f)) v.mesh = std::make_shared<runtime::Mesh>(fracture::pose(f, 0));
    conformance_from_fit(core::read_json(ws.root() + "/fixed/" + item.name + "/manifest.json"), v);
  } else if (item.kind == "conformance") {
    v.has_conformance = true;
    v.radius = item.data.value("radius", 0.0f);
    v.height = item.data.value("height", 0.0f);
    v.mesh = std::make_shared<runtime::Mesh>();  // empty: shows just the shape
  }
  return v;
}

namespace {
std::mutex g_fire_mutex;
std::string g_fire_root;
std::vector<runtime::fire::Config> g_fire_configs;
std::chrono::steady_clock::time_point g_fire_loaded;
bool g_fire_any = false;

std::vector<float> floats(const nlohmann::json &v, size_t n) {
  std::vector<float> out(n, 0.0f);
  if (v.is_array())
    for (size_t i = 0; i < n && i < v.size(); ++i) out[i] = v[i].get<float>();
  return out;
}
}  // namespace

void set_fire_root(const std::string &workspace_root) {
  std::lock_guard<std::mutex> lock(g_fire_mutex);
  g_fire_root = workspace_root;
  g_fire_any = false;
}

std::vector<runtime::fire::Config> fire_configs() {
  std::lock_guard<std::mutex> lock(g_fire_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (g_fire_any && now - g_fire_loaded < std::chrono::seconds(2)) return g_fire_configs;
  g_fire_loaded = now;
  g_fire_any = true;
  g_fire_configs.clear();
  const std::string dir = g_fire_root + "/config/fire_configs";
  std::error_code ec;
  if (g_fire_root.empty() || !std::filesystem::is_directory(dir, ec)) return g_fire_configs;
  const char *kinds[] = {"flame", "ember", "spirit"};
  for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json" || e.path().filename() == "table.json") continue;
    try {
      const nlohmann::json v = core::read_json(e.path().string()).at("value");
      runtime::fire::Config c;
      c.id = v.value("id", 0u);
      const std::string kind = v.value("kind", "flame");
      for (uint32_t k = 0; k < 3; ++k)
        if (kind == kinds[k]) c.kind = k;
      auto put3 = [&](const char *name, float *out) {
        const auto f = floats(v.value(name, nlohmann::json::array()), 3);
        for (int k = 0; k < 3; ++k) out[k] = f[static_cast<size_t>(k)];
      };
      put3("core_color", c.core);
      put3("tip_color", c.tip);
      put3("smoke_color", c.smoke);
      put3("light_color", c.light);
      c.size = v.value("size", c.size);
      c.height = v.value("height", c.height);
      c.rate = v.value("rate", c.rate);
      c.flicker = v.value("flicker", c.flicker);
      c.sparks = v.value("sparks", c.sparks);
      c.smoke_rate = v.value("smoke_rate", c.smoke_rate);
      c.smoke_size = v.value("smoke_size", c.smoke_size);
      c.smoke_life = v.value("smoke_life", c.smoke_life);
      c.smoke_rise = v.value("smoke_rise", c.smoke_rise);
      c.light_radius = v.value("light_radius", c.light_radius);
      c.light_strength = v.value("light_strength", c.light_strength);
      g_fire_configs.push_back(c);
    } catch (const std::exception &) {
    }
  }
  return g_fire_configs;
}

void fire_snapshot(const std::vector<runtime::FirePoint> &fires, float burning, float time, float wind_speed, float wind_heading,
                   std::vector<runtime::fire::Sprite> &sprites, std::vector<runtime::fire::Light> &lights) {
  const std::vector<runtime::fire::Config> configs = fire_configs();
  if (configs.empty()) return;
  std::vector<runtime::fire::Emitter> emitters;
  for (size_t i = 0; i < fires.size(); ++i) {
    const runtime::FirePoint &f = fires[i];
    runtime::fire::Emitter e;
    if (f.lit == runtime::FirePoint::kBurning) {
      if (burning <= 0) continue;
      e.strength = burning;
    } else if (f.lit == runtime::FirePoint::kSmoke) {
      e.show = runtime::fire::Show::smoke;
    }
    if (f.lit != runtime::FirePoint::kBurning && burning > 0) e.strength = 1.0f + burning;
    for (int k = 0; k < 3; ++k) e.pos[k] = f.pos[k];
    e.config = f.config;
    e.key = 0x9e3779b97f4a7c15ull * (i + 1);
    emitters.push_back(e);
  }
  runtime::fire::Sim::snapshot(configs, emitters.data(), emitters.size(), wind_speed, wind_heading, time, sprites, &lights);
}

std::shared_ptr<const runtime::Mesh> mesh_at(const ViewContent &content, uint32_t triangles) {
  if (!content.lods.empty()) {
    if (triangles == 0) return content.lods[0];
    std::shared_ptr<const runtime::Mesh> best = content.lods[0];
    for (const auto &l : content.lods) {
      auto diff = [&](const runtime::Mesh &m) {
        return std::llabs(static_cast<long long>(m.triangles()) - static_cast<long long>(triangles));
      };
      if (diff(*l) < diff(*best)) best = l;
    }
    return best;
  }
  if (triangles == 0 || !content.progressive || triangles >= content.full_triangles()) return content.mesh;
  return std::make_shared<runtime::Mesh>(extract_lod(*content.progressive, triangles));
}

viewport::Image render_view(const ViewContent &content, const viewport::Camera &camera,
                            const viewport::Overlays &overlays, bool silhouette, int width, int height,
                            uint32_t triangles, const viewport::SkyScene *sky) {
  if (content.image) return *content.image;
  std::shared_ptr<const runtime::Mesh> mesh = mesh_at(content, triangles);
  viewport::Scene scene;
  scene.mesh = mesh.get();
  scene.has_conformance = content.has_conformance;
  scene.conformance_radius = content.radius;
  scene.conformance_height = content.height;
  scene.has_box = content.has_box;
  scene.box_min = content.box_min;
  scene.box_max = content.box_max;
  scene.collision = content.collision;
  scene.lines = content.lines;
  scene.camera = camera;
  scene.overlays = overlays;
  scene.silhouette = silhouette;
  viewport::SkyScene no_ground;
  if (sky && content.own_ground) {
    no_ground = *sky;
    no_ground.ground = false;
    sky = &no_ground;
  }
  scene.sky = sky;
  if (!content.fires.empty())
    fire_snapshot(content.fires, content.burning, content.fire_time, content.wind_speed, content.wind_heading, scene.sprites, scene.fire_lights);
  return viewport::render(scene, width, height);
}

}  // namespace assets
