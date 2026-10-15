// grass_cell: what one cell of a grass field shows (design/plotter.md, "Fields").
//
// A pure function of a cell's axial coordinates (q, r), its ground, its moisture,
// and the world's seed, so a field of any size is the same function iterated
// over its cells and never needs an instance per cell:
// 1. Ground. Water, rock, sand, and path cells show their plate (the tiles after
//    the grass variants); soil shows grass.
// 2. Patches. Value noise over the cells' centres, at the genome's patch size,
//    leaves the genome's share of soil cells bare.
// 3. The tile. A hash of the cell picks the grass variant and a turn by sixths
//    (a hex looks the same turned), so neighbours rarely repeat.
// 4. Lushness. Wetter cells grow taller and greener, by the genome's `lush`, with
//    a little noise so a meadow isn't flat.
// Only integer hashing, + - * /, and floor: WebAssembly and native agree to the bit.
#include <cmath>

#include "boundary.h"
#include "channels/grass_cell.h"
#include "channels/grass_choice.h"
#include "packer/grass_cell.h"
#include "packer/grass_choice.h"

namespace grass_cell {

namespace {

uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

uint32_t hash_str(const char *s, uint32_t h = 2166136261u) {
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }  // [0, 1)

float lattice(int x, int z, uint32_t seed) {
  return unit(hash_u32(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 83492791u ^ seed));
}

float smooth(float t) { return t * t * (3 - 2 * t); }

// 2D value noise in [0, 1].
float noise(float x, float z, uint32_t seed) {
  const float fx = std::floor(x), fz = std::floor(z);
  const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  const float tx = smooth(x - fx), tz = smooth(z - fz);
  const float a = lattice(ix, iz, seed) + (lattice(ix + 1, iz, seed) - lattice(ix, iz, seed)) * tx;
  const float b = lattice(ix, iz + 1, seed) + (lattice(ix + 1, iz + 1, seed) - lattice(ix, iz + 1, seed)) * tx;
  return a + (b - a) * tz;
}

}  // namespace

bool run(const channels::grass_cell &in, channels::grass_choice &out, std::string &error) {
  const channels::grass_genome &g = in.genome;
  if (in.season > 3 || in.ground > 4) {
    error = "season and ground must be in range";
    return false;
  }
  if (g.variants == 0 || g.patch_cells <= 0) {
    error = "genome needs at least one variant and a patch size";
    return false;
  }
  const uint32_t species = hash_str(g.name.c_str());
  const uint32_t h = hash_u32(in.seed ^ hash_u32(static_cast<uint32_t>(in.q) * 2654435761u ^ hash_u32(static_cast<uint32_t>(in.r) + 0x9e3779b9u)) ^ species);
  out.present = true;
  out.turn = hash_u32(h ^ 0x51ed27u) % 6u;
  out.scale = 1.0f;
  out.tint = foundry::vec3{1, 1, 1};
  // Plates: water 0, rock 1, sand 2, path 3 after the grass variants; bare soil is 4.
  if (in.ground != channels::grass_cell::kGroundSoil) {
    out.variant = g.variants + (in.ground - 1u);
    const float shade = 0.95f + 0.1f * unit(hash_u32(h ^ 0xa11ceu));
    out.tint = foundry::vec3{shade, shade, shade};
    return true;
  }
  // The cell's centre, in metres, for noise that runs smoothly across cells.
  const float x = 1.7320508f * (static_cast<float>(in.q) + 0.5f * static_cast<float>(in.r)), z = 1.5f * static_cast<float>(in.r);
  const float scale = 1.0f / (g.patch_cells * 1.7320508f);
  const float patch = noise(x * scale, z * scale, in.seed ^ species ^ 0x6a09e667u);
  if (patch < g.bald) {
    out.variant = g.variants + 4u;
    return true;
  }
  out.variant = h % g.variants;
  // Wetter cells grow taller and greener; a second noise keeps a meadow from being flat.
  const float wet = in.moisture < 0 ? 0.0f : (in.moisture > 1 ? 1.0f : in.moisture);
  const float swell = noise(x * scale * 2.3f, z * scale * 2.3f, in.seed ^ species ^ 0xbb67ae85u);
  out.scale = (1.0f - 0.3f * g.lush + 0.6f * g.lush * wet) * (0.85f + 0.3f * swell) * (0.94f + 0.12f * unit(hash_u32(h ^ 0x3c6ef372u)));
  const float green = 1.0f + 0.12f * g.lush * (wet - 0.5f) + 0.06f * (swell - 0.5f);
  const float light = 0.94f + 0.12f * unit(hash_u32(h ^ 0xa54ff53au));
  out.tint = foundry::vec3{light * (2.0f - green), light * green, light * (2.0f - green)};
  return true;
}

}  // namespace grass_cell

FOUNDRY_SYSTEM(grass_cell, channels::grass_cell, channels::grass_choice)
