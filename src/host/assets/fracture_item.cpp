#include "assets/fracture_item.h"

#include <chrono>

#include "assets/meshio.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"

namespace assets {

bool fracture_baseline(const std::string &root, const std::string &name, runtime::Mesh &lod1, nlohmann::json &manifest,
                       std::string &error) {
  if (!core::valid_name(name)) {
    error = "bad asset name";
    return false;
  }
  const std::string dir = root + "/fixed/" + name;
  manifest = core::read_json(dir + "/manifest.json");
  if (!manifest.is_object()) {
    error = "no fixed asset " + name;
    return false;
  }
  if (!load_mesh(dir + "/lod1.bin", lod1)) {
    error = "fixed/" + name + " has no lod1.bin";
    return false;
  }
  return true;
}

bool fracture_generate(const std::string &root, const std::string &name, const fracture::Settings &settings,
                       fracture::Fracture &out, std::string &error) {
  core::Job job("Fracture: " + name);
  runtime::Mesh lod1;
  nlohmann::json manifest;
  if (!fracture_baseline(root, name, lod1, manifest, error)) return false;
  const std::string dir = root + "/fixed/" + name;
  nlohmann::json item = core::read_json(dir + "/fracture.json");
  if (item.is_object() && item.value("state", "") == "final") {
    error = "fixed/" + name + "'s fracture is final";
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (!fracture::cut(lod1, settings, out, error)) return false;
  fracture::bake(out, settings);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!fracture::save(dir + "/fracture.bin", out)) {
    error = "can't write " + dir + "/fracture.bin";
    return false;
  }
  if (!item.is_object()) item = {{"notes", nlohmann::json::array()}, {"created_at", core::now_iso()}};
  item["kind"] = "fracture";
  item["name"] = name;
  item["state"] = "draft";
  item.erase("approved_at");
  item.erase("approved_by");
  item["baseline"] = "lod1.bin";
  item["settings"] = settings.to_json();
  item["stats"] = out.stats;
  item["stats"]["ms"] = ms;
  item["generated_at"] = core::now_iso();
  item["doc"] = "The break of fixed/" + name + ": its LOD1 cut by the " + settings.algorithm + " algorithm into " +
                std::to_string(out.pieces()) + " pieces, with the fall baked.";
  if (!core::write_json(dir + "/fracture.json", item)) {
    error = "can't write " + dir + "/fracture.json";
    return false;
  }
  core::log().add("fracture", name + ": " + settings.algorithm + ", " + std::to_string(out.pieces()) + " pieces, " +
                                  std::to_string(out.mesh.indices.size() / 3) + " triangles");
  return true;
}

bool fracture_load(const std::string &root, const std::string &name, fracture::Fracture &out) {
  return core::valid_name(name) && fracture::load(root + "/fixed/" + name + "/fracture.bin", out);
}

}  // namespace assets
