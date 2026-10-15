// A terrain and its boundary as a game uses them (design/terrain.md, design/boundary.md): the river
// valley spawned natively, its ground read (a column, the height under a point, roads, bridges, the
// navigation mesh), its hull handed to a boundary so the two meet, and the boundary's figures filled
// with trees and bushes from their species' names. Prints the counts the books quote.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "runtime.h"
#include "runtime_diag.h"

namespace {

int failures = 0;
#define CHECK(c, msg)                          \
  do {                                         \
    if (!(c)) {                                \
      std::printf("FAIL: %s (%s)\n", msg, #c); \
      ++failures;                              \
    }                                          \
  } while (0)

bool built(foundry_runtime *rt, foundry_instance i) {
  for (int k = 0; k < 20000; ++k) {
    foundry_update(rt, 1.0f / 60.0f);
    foundry_diag_instance info;
    if (foundry_diag_instance_get(rt, i, &info) == FOUNDRY_OK && info.ready) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

}  // namespace

int main() {
  foundry_runtime_desc desc = {};
  foundry_runtime *rt = foundry_runtime_create(&desc);
  if (!rt) {
    std::printf("FAIL: runtime: %s\n", foundry_last_error());
    return 1;
  }
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  // The terrain: the river valley, and its ground.
  foundry_terrain_params tp;
  foundry_terrain_default_params(&tp);
  tp.shape = FOUNDRY_TERRAIN_SHAPE_RIVER_VALLEY;
  const auto t0 = std::chrono::steady_clock::now();
  const foundry_instance terrain = foundry_terrain_spawn(rt, &tp, identity);
  CHECK(terrain != FOUNDRY_NO_INSTANCE, "the terrain spawns");
  CHECK(built(rt, terrain), "the terrain builds");
  std::printf("terrain built in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  foundry_cell cell;
  foundry_span spans[4];
  CHECK(foundry_ground_cell(rt, terrain, 0, 0, &cell, spans, 4), "the middle cell is in the ground");
  CHECK(cell.spans >= 1 && spans[0].y0 < spans[0].y1, "its column has a span");
  float y = -1000;
  CHECK(foundry_ground_height(rt, terrain, 0.3f, 0.2f, &y), "there is ground under the middle");
  CHECK(cell.spans >= 1 && y == spans[cell.spans - 1].y1 * 0.125f, "the height is the column's top");
  std::vector<int32_t> road_cells(4096);
  std::vector<uint32_t> road_counts(64);
  const uint32_t roads = foundry_ground_roads(rt, terrain, road_cells.data(), 4096, road_counts.data(), 64);
  CHECK(roads >= 1, "a road joins its openings");
  foundry_bridge bridges[8];
  const uint32_t nb = foundry_ground_bridges(rt, terrain, bridges, 8);
  foundry_nav_sizes nav;
  const uint32_t polys = foundry_ground_nav(rt, terrain, &nav, nullptr, nullptr, nullptr, nullptr);
  CHECK(polys > 1000 && nav.vertices == polys * 6 && nav.links > polys, "a navigation mesh over the walkable tops");
  std::vector<float> verts(size_t(nav.vertices) * 3);
  std::vector<uint32_t> poly(size_t(polys) * 6), cells(polys), links(size_t(nav.links) * 2);
  foundry_ground_nav(rt, terrain, &nav, verts.data(), poly.data(), cells.data(), links.data());
  bool links_ok = true;
  for (uint32_t l : links) links_ok = links_ok && l < polys;
  CHECK(links_ok, "every link joins two polygons");
  std::printf("ground: %u roads, %u bridges, %u nav polygons, %u links\n", roads, nb, polys, nav.links);

  // Its hull to a boundary.
  const uint32_t n = foundry_ground_hull(rt, terrain, nullptr, 0);
  std::vector<foundry_hull_cell> hull(n);
  foundry_ground_hull(rt, terrain, hull.data(), n);
  uint32_t openings = 0;
  for (const auto &c : hull) openings += c.opening ? 1 : 0;
  CHECK(n > 100, "the hull rings the terrain");
  CHECK(openings == 4, "its four openings leave through the hull (two roads, a river in and out)");
  foundry_boundary_params bp;
  foundry_boundary_default_params(&bp);
  bp.detail = FOUNDRY_BOUNDARY_DETAIL_MEDIUM;
  bp.view_distance = 2000;
  const auto t1 = std::chrono::steady_clock::now();
  const foundry_instance boundary = foundry_boundary_spawn_hull(rt, &bp, identity, hull.data(), n);
  CHECK(boundary != FOUNDRY_NO_INSTANCE, "the boundary spawns round the terrain");
  CHECK(built(rt, boundary), "the boundary builds");
  std::printf("boundary built in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());

  // Its figures, filled as a game does: each species by its name.
  const uint32_t nf = foundry_instance_figures(rt, boundary, nullptr, 0);
  std::vector<foundry_figure> figs(nf);
  foundry_instance_figures(rt, boundary, figs.data(), nf);
  CHECK(nf > 100, "the boundary leaves trees for the game");
  uint32_t trees = 0, bushes = 0, unknown = 0;
  for (const auto &f : figs) {
    const float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, f.position[0], f.position[1], f.position[2], 1};
    if (f.kind == 0) {
      const int s = foundry_tree_species_find(f.species);
      if (s < 0) {
        ++unknown;
        continue;
      }
      foundry_tree_params p;
      foundry_tree_default_params(&p);
      p.species = static_cast<foundry_tree_species>(s);
      p.age = static_cast<foundry_tree_age>(f.age < 4 ? f.age : 3);
      p.seed = 1 + f.seed % 4;
      trees += foundry_tree_spawn(rt, &p, t) != FOUNDRY_NO_INSTANCE;
    } else {
      const int s = foundry_bush_species_find(f.species);
      if (s < 0) {
        ++unknown;
        continue;
      }
      foundry_bush_params p;
      foundry_bush_default_params(&p);
      p.species = static_cast<foundry_bush_species>(s);
      p.age = static_cast<foundry_bush_age>(f.age < 3 ? f.age : 2);
      bushes += foundry_bush_spawn(rt, &p, t) != FOUNDRY_NO_INSTANCE;
    }
  }
  std::printf("figures: %u (%u trees, %u bushes spawned, %u of unknown species)\n", nf, trees, bushes, unknown);
  CHECK(unknown == 0, "every figure's species is a genome");
  CHECK(trees + bushes == nf, "every figure spawns");

  foundry_runtime_destroy(rt);
  if (failures) std::printf("%d failed\n", failures);
  else std::printf("ok\n");
  return failures ? 1 : 0;
}
