#include "assets/view.h"

#include <cstdlib>

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

}  // namespace

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
  const std::string sub = item.data.value("kind", "");
  if (item.kind == "meshy" && sub == "concept") {
    auto img = std::make_shared<viewport::Image>();
    if (viewport::load_image(item.path + "/image.png", *img)) v.image = img;
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
  } else if (item.kind == "conformance") {
    v.has_conformance = true;
    v.radius = item.data.value("radius", 0.0f);
    v.height = item.data.value("height", 0.0f);
    v.mesh = std::make_shared<runtime::Mesh>();  // empty: shows just the shape
  }
  return v;
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
                            uint32_t triangles) {
  if (content.image) return *content.image;
  std::shared_ptr<const runtime::Mesh> mesh = mesh_at(content, triangles);
  viewport::Scene scene;
  scene.mesh = mesh.get();
  scene.has_conformance = content.has_conformance;
  scene.conformance_radius = content.radius;
  scene.conformance_height = content.height;
  scene.camera = camera;
  scene.overlays = overlays;
  scene.silhouette = silhouette;
  return viewport::render(scene, width, height);
}

}  // namespace assets
