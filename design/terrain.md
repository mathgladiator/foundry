# Terrain: building the playable ground

A **terrain** is the ground of a playable region: a shape of hex cells whose edges are given and whose inside is built. The game names the shape and swaps an **aspect** onto each of its sides (a ledge, a cliff, a forest, a sea, and so on) along with the height there. It marks **openings**: entrances and exits where roads cross the edge, and inlets and outlets where water flows in and out. It lists **flats**, cells that must stay level enough for a settlement planner. The terrain builder fills the inside: hills and valleys by its parameters, rivers from inlet to outlet, roads between the entrances, proved traversable on a navigation mesh, bridges where roads cross water, and cliffs, overhangs, and caves where the rock allows.

A boundary (`design/boundary.md`) walls the same shape in. It takes the terrain's own ring as its hull, so the land beyond starts at the heights the terrain ends at, and the roads and rivers that leave carry on into the invented world.

Asked for on 2026-10-14. The ground model is the hexbound document's (`design/import.hexbound.world.md`), adopted as written; this document says how a terrain is built on it. It is the pipeline `terrain` (`book/terrain.md`); nothing is approved yet.

## The shape and its sides

A **shape** is a polygon of hex corners, made from a kind and its sizes, or given corner by corner:

| Kind | Sizes | Sides |
|---|---|---|
| hexagon | radius | 6 |
| rectangle | width, depth | 4 |
| triangle | size | 3 |
| L | width, depth, notch | 6 |
| polygon | its corners (q, r) | one per corner |

- **Rotation** turns it in 60° steps.
- **Wobble** lets each side meander (an amplitude in cells, seeded), so a hexagon reads as a valley rather than a hexagon. The corners stay put, so a side is always the side between the same two corners.
- **Sides** run from corner i to corner i + 1, counterclockwise as seen from above. Each side has an **aspect**:
  - **edge:** what stops the player and what lies beyond (the boundary's: ledge, ridge, mountain, forest, jungle, sea, marsh);
  - **height:** the ground along the side, metres;
  - **rise:** how high the land climbs behind it, for the boundary.

  Swapping aspects changes a side without changing the shape. Heights blend along the side and round each corner, so the edge is continuous.
- **Openings** sit on a side at a fraction along it:
  - **road:** an entrance or exit, with its width in cells. Roads join every road opening.
  - **river in** and **river out:** where water enters and leaves, with its width. Rivers run from each inlet to an outlet, joining where they meet.

A shape is the same for the terrain and the boundary: `terrain_shape`, kept in `config/terrain_shapes/`, and a boundary hull can name one instead of listing cells (`design/boundary.md`, "Hulls from shapes").

**Flats** are optional: a terrain needs no town. Each is a round of cells (a middle and a radius) or listed cells, levelled to a given height or to its cells' mean, within a tolerance. Where two overlap, the first keeps the cells they share.

## The ground

The ground is the hexbound model (`design/import.hexbound.world.md`), unchanged:
- **Cells:** each is a column of spans, `y0, y1, material`, in integer 1/8 m units. The materials are grass, dirt, sand, snow, cobble, and rock.
- **Cliffs:** wherever neighbouring tops don't fuse (their difference beyond the pair's smoothness).
- **Caves:** two spans with air between them. A top is walkable with 2 m of headroom.
- **Drawing:** every top and bottom is a 13-point fan, its corners shared by fused groups, so there are no cracks. Walls are solid minus solid on each half-edge.
- **Movement:** an edge reports its height difference and whether it fused, and a body decides whether to walk, jump, climb, or drop.

Water is not a span: each cell may carry a **water level** over its top, which rivers and lakes set.

## Building it

Three stages, each a pure system:

1. **`terrain_shape`** resolves the shape:
   - its corners, its sides (wobbled), and its cells;
   - each cell's distance to the edge and its nearest side;
   - the openings' cells and the way each points in;
   - the flats' cells;
   - the ring outside it, which becomes the boundary's hull.
2. **`terrain_relief`** builds the ground and plans it, in order:
   1. **Relief.** Inside a band along the edge, the height is the sides', blended. Inward it rises and falls by the parameters: `relief` (how high hills stand), `hill_size`, `roughness`, and `ridges` (how much of it is ridged rather than rolling).
   2. **Flats.** Each flat is levelled to its height (given, or its cells' mean), within its tolerance, and blended into the land round it over a few cells so no cliff rings it.
   3. **Rivers.** Each inlet's river finds the cheapest way to an outlet: downhill is cheap, climbing is dear, and flats are avoided unless nothing else reaches. Its bed is carved so it never rises, its banks are eased, and its cells take a water level. Inlets join into one river where their paths meet; an inlet with no outlet ends in a lake.
   4. **Rock.** `cliffs` sets how much of the steep land is rock in terraces, and so cliffs. `caves` opens overhangs and cave mouths in cliff faces, each with its 2 m headroom.
   5. **Roads.** Roads join every road opening and every flat's middle, as a tree of the cheapest ways:
      - **Cost:** distance plus grade, and a crossing of water costs a bridge.
      - **Barriers:** no road takes a step it couldn't grade or climbs a cliff it can't cut.
      - **Grading:** each road is graded so every step along it is under 0.5 m, a quarter metre inside cobble's smoothness, as the hexbound document asks for anything a route depends on. Then it is paved: cobble for a road, dirt for a lane.
   6. **Bridges.** Where a road crosses water, the cells on the water become a bridge. Its two ends on the banks are **bridge mounts**, each with its place, height, facing, span, and width, for a bridge pipeline or the game to stand a bridge on.
   7. **Materials.** Sand by water, rock where steep, snow above the biome's snow line, dirt in worn patches, grass elsewhere.
   8. **The navigation mesh**, by the hexbound rules: a polygon on each walkable top, linked where an edge fuses or steps no more than 0.5 m, and over bridges. The report proves every road opening reaches every other and every flat, and gives the longest route, the steepest grade, the bridges, and anything unreached (which must be none).
3. **`terrain_mesh`** draws it:
   - every top and bottom as its fan, and every wall, coloured by material from the biome;
   - water surfaces at their levels;
   - its collision: the ground as triangles, and invisible walls round the shape;
   - guides: the sides by aspect, openings, flats, roads, rivers, and bridge mounts;
   - the **nature ground** for a plotter: the open land, as `plot_regions` (heights, moisture rising by water, ground by material; not on roads, water, or bridges, and kept clear of the roads);
   - the **hull** for the boundary: the ring outside the shape, each cell with its side's edge, height, and rise, and the openings that leave through it.

The biome is the boundary's (`boundary_biome`), so a terrain and its boundary share their colours.

## In the foundry

The pipeline view of `terrain` composes three things around the model, each named in the pipeline's `draw`, never by the host (`src/host/pipeline/compose.*`, in the background):
- **The plotter fill** (`draw.nature`: the model's regions and a default plotter): a plotter grows the nature ground. The inspector turns it off or picks another plotter.
- **The boundary** (`draw.surround`: a pipeline, the model's hull, the parameters it shares, and its own): a boundary pipeline is built around the terrain's hull. It starts off (`"on": false`); the inspector turns it on and sets its own parameters, such as detail and view distance.
- **Its figures**: the near trees the boundary hands back are filled with tree and bush builds (`design/boundary.md`, "Figures").

The inspector keeps these apart from the pipeline's parameters: **Into the systems** holds the parameters (each marked with the stages it feeds), **Only the view** what is composed round the model.

`pipeline_view` does the same (`compose` turns any of them on or off) and stands an eye on the ground with `on_ground`.

**Swapping aspects:** a config parameter's **edit** button edits its entry in place for the view. You can change a shape's sides one by one (each side's edge, height, and rise), its openings, and its corners, and add, copy, or remove sides and openings. The preset itself is unchanged.

Guides show the sides, openings, roads, rivers, flats, and bridge mounts. Play starts at the middle and walks the ground's collision.

## The runtime and the API

The terrain spawns as an instance (`foundry_terrain_spawn`): its mesh and its collision in the game's Jolt world. Its build keeps the ground (`draw.ground`), which the C API reads (`ground.h`):
- `foundry_ground_cell`: a cell's spans, flags, and water level;
- `foundry_ground_height`: the height under a point;
- `foundry_ground_roads`: the roads as cells;
- `foundry_ground_nav`: the navigation mesh;
- `foundry_ground_bridges`: the bridge mounts;
- `foundry_ground_hull`: the hull, with each opening on its cell, which a game hands to its boundary (`foundry_boundary_spawn_hull`).

A hull cell carries its opening (`foundry_hull_cell.opening`, `opening_width`), so the boundary carries the road or river on outward.

## Not built

- **Edits.** Digging and harvesting rewrite spans (the hexbound document's "Edits re-derive"); the runtime doesn't take edits yet.
- **Simulated water.** Water is a level per cell, not a simulated volume.
- **Images.** The ground is vertex-coloured by material; it has no painted image like a boundary's terrain nodes.
- **Bridges.** The terrain marks their mounts; nothing stands a bridge on them yet.
- **Triangle collision.** Collision is a convex column per span, because `physics_shape` has no triangle mesh.
