// Fracture (design/transforms.md, "Fracture"): a fixed asset's LOD1 cut into
// pieces, with a baked break animation, so breaking a vase or a gnome in the
// game is a cached animation rather than a simulation.
//
// Every algorithm is a Voronoi diagram; they differ in where the seeds go and
// how distance is measured. Each cell clips the mesh's surface to itself, and
// each face it shares with a neighbour is capped with the mesh's cross-section
// there (holes included), so pieces are closed even where the mesh has small
// gaps. The animation is a small rigid-body simulation (gravity, the blow, the
// ground, and spheres between pieces), baked at a fixed frame rate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/mesh.h"

namespace assets::fracture {

struct AlgorithmInfo {
  const char *name;
  const char *doc;
};
// voronoi, impact, radial, slab, splinter.
const std::vector<AlgorithmInfo> &algorithms();
bool known(const std::string &algorithm);

struct Settings {
  std::string algorithm = "impact";
  int pieces = 16;              // seeds; cells that miss the mesh are dropped
  uint32_t seed = 1;
  bool has_impact = false;      // otherwise: where the blow's ray first meets the mesh, at 60% height
  foundry::vec3 impact = {0, 0, 0};
  foundry::vec3 direction = {0, 0, -1};  // the blow's direction (default: from the front, toward -Z)
  float strength = 1.0f;        // scales the blow
  float duration = 3.0f;        // seconds baked
  float fps = 30.0f;
  uint32_t interior = 0;        // packed RGBA of cut faces; 0: the surface's mean colour, darkened

  nlohmann::json to_json() const;
  // Missing fields keep their defaults; returns false (with error) on a bad value.
  bool from_json(const nlohmann::json &j, std::string &error);
};

struct Fracture {
  runtime::Mesh mesh;                    // every piece at rest, in asset space
  std::vector<uint32_t> piece;           // per vertex: its piece
  std::vector<foundry::vec3> centres;    // per piece: centre of mass at rest
  std::vector<float> volumes;            // per piece, m3
  float fps = 30;
  uint32_t frames = 0;
  // frames x pieces x 7 floats: the centre's position and the rotation (quaternion x, y, z, w);
  // a rest point x of piece p moves to position + rotate(q, x - centre).
  std::vector<float> track;
  foundry::vec3 impact = {0, 0, 0};      // where the blow landed
  nlohmann::json stats;

  size_t pieces() const { return centres.size(); }
  float seconds() const { return frames > 1 ? static_cast<float>(frames - 1) / fps : 0; }
};

// Cuts the mesh into pieces (no animation yet).
bool cut(const runtime::Mesh &mesh, const Settings &settings, Fracture &out, std::string &error);
// Bakes the break animation into out.track.
void bake(Fracture &f, const Settings &settings);

// The pieces at time t (seconds since the break), as a drawable mesh.
runtime::Mesh pose(const Fracture &f, float t);
// The pieces pushed apart from the centre by amount (0..1 of the asset's size), each tinted its own
// colour if tint, so the cut can be judged.
runtime::Mesh exploded(const Fracture &f, float amount, bool tint);

// An algorithm for this asset, from its provenance prompt and its shape, with the reason.
nlohmann::json recommend(const runtime::Mesh &mesh, const std::string &prompt);

bool save(const std::string &path, const Fracture &f);
bool load(const std::string &path, Fracture &f);

}  // namespace assets::fracture
