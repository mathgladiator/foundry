// The Meshy mesh item (design/transforms.md): meshy/<name>_mesh/.
//
// Files: model.glb (Meshy's model), item.json (state, notes, provenance,
// sizing ops, fit, recommendation, performance), import.bin (the GLB import
// at full fidelity), full.bin (after the sizing ops), progressive.bin (the
// progressive mesh), and rec_lod0.bin, rec_lod1.bin, rec_lod2.bin (the
// recommended levels, ready to stamp).
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/meshio.h"

namespace assets {

struct Conformance {
  std::string name, shape;
  float radius = 0, height = 0;
};
bool load_conformance(const std::string &root, const std::string &name, Conformance &out, std::string &error);

// Default LOD quality limits: LOD0 under 1% error, LOD1 under 5%, LOD2 under
// 20%. Each mesh item can override them (item.json "lod_thresholds").
const std::vector<double> &lod_thresholds();
std::vector<double> item_thresholds(const nlohmann::json &item_json);
// The item's switch threshold: the share of pixels allowed to disagree with
// the full mesh at a LOD's switch distance ("switch_share", default 10%).
constexpr double kSwitchShareMin = 0.001, kSwitchShareMax = 0.5;
double item_switch_share(const nlohmann::json &item_json);

std::string item_dir(const std::string &root, const std::string &item);

// The mesh after its sizing ops (the import, if none yet).
bool load_full(const std::string &root, const std::string &item, runtime::Mesh &mesh, std::string &error);
bool load_item_progressive(const std::string &root, const std::string &item, ProgressiveMesh &pm);

// Appends sizing ops (or replaces them with reset) and replays them from the
// import. Clears any recommendation, since it no longer matches.
bool mesh_transform(const std::string &root, const std::string &item, const nlohmann::json &ops, bool reset,
                    nlohmann::json &item_json, std::string &error);

// Builds the progressive mesh, recommends LOD0/1/2 by error (or uses explicit
// triangle counts), prepares each as a vertex-cache-optimized list, and
// benchmarks them. Results go in item.json under "recommended" and "perf".
bool mesh_recommend(const std::string &root, const std::string &item, const std::vector<uint32_t> *triangles,
                    nlohmann::json &item_json, std::string &error);

// LOD error limits always stack: each is at least kLimitGap above the one
// before, within [kLimitMin, kLimitMax]. Setting LOD k's limit pushes the
// others out of its way (later ones up, earlier ones down), the way the
// workspace sliders do.
constexpr double kLimitMin = 0.001, kLimitMax = 0.6, kLimitGap = 0.001;
void cascade_limits(std::vector<double> &limits, size_t k, double value);

// Sets one LOD's error limit, cascading the others (cascade_limits). Every LOD
// whose limit changed is searched again (the coarsest progressive level under
// its limit, never finer than the LOD before it), rebaked, and re-benchmarked;
// the others stay as they are. Needs a recommendation already (mesh_recommend).
bool mesh_regenerate_lod(const std::string &root, const std::string &item, size_t lod, double max_error,
                         nlohmann::json &item_json, std::string &error);

// Sets the item's switch threshold (clamped to [kSwitchShareMin,
// kSwitchShareMax]) and recomputes every LOD's switch distance with it. The
// LODs themselves and their benchmarks don't change.
bool mesh_set_switch_share(const std::string &root, const std::string &item, double share, nlohmann::json &item_json,
                           std::string &error);

// Stamps an approved mesh item's recommended levels into fixed/<name>/ (the
// item name without "_mesh"), with a final manifest.
bool stamp(const std::string &root, const std::string &item, std::string &error);

}  // namespace assets
