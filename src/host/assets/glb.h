// Imports glTF/GLB files (such as Meshy output) as a mesh channel.
#pragma once

#include <string>

#include "render/mesh.h"

namespace assets {

// Flattens every mesh in the scene (with node transforms) into one triangle
// mesh. Vertex colors come from the base color texture sampled at each vertex,
// times the material's base color factor, so the result fits flat-palette
// shading without textures.
bool import_gltf(const std::string &path, runtime::Mesh &out, std::string &error);

}  // namespace assets
