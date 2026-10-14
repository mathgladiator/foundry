// A terrain model as the foundry draws it (draw.terrain; design/boundary.md, "Levels"): the
// runtime draws a quadtree of terrain nodes as a cut by distance, each node textured with its own
// image and morphing toward its parent. The foundry's viewport draws vertex-coloured meshes, so a
// view takes one cut, from the region's middle at a player's height, and retessellates each node it
// keeps at (up to) 32 texels a side, each vertex coloured by its image, with skirts down the sides.
// The region inside the rim is the game's ground, which the model leaves out; the view stands in a
// flat floor for it at the ground's height, coloured by the images.
#pragma once

#include <nlohmann/json.hpp>

#include "render/mesh.h"

namespace pipeline {

// The draw.terrain field names (node_min, node_max, children, spacing, first, count, positions,
// normals, uvs, morph, indices, image_size, images) read from model; appends the cut to out and
// reports nodes, terrain triangles, and the cut's under facts. ground_y: the region's ground (the
// player's eye is 1.7 m above it).
void flatten_terrain(const nlohmann::json &model, const nlohmann::json &names, float ground_y, runtime::Mesh &out,
                     nlohmann::json &facts);

}  // namespace pipeline
