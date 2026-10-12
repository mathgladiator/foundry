// A fixed asset's fracture (design/transforms.md, "Fracture"): its LOD1 cut and
// its break baked, stored beside the asset as fixed/<name>/fracture.bin with
// fracture.json (state, settings, stats, notes). The fixed asset itself stays
// final; the fracture is its own item, approved on its own.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "assets/fracture.h"

namespace assets {

// The fixed asset's LOD1 (the fracture's baseline) and its manifest.
bool fracture_baseline(const std::string &root, const std::string &name, runtime::Mesh &lod1, nlohmann::json &manifest,
                       std::string &error);
// Cuts and bakes the fracture, and writes it as a draft (a final fracture can't change).
bool fracture_generate(const std::string &root, const std::string &name, const fracture::Settings &settings,
                       fracture::Fracture &out, std::string &error);
bool fracture_load(const std::string &root, const std::string &name, fracture::Fracture &out);

}  // namespace assets
