# The terrain pipeline (`terrain`)

`terrain` builds the ground of a playable region inside a shape whose edges are given. It fills the inside with hills, rivers, cliffs, and caves, plans roads between the entrances and exits on a navigation mesh, keeps flats level for a settlement planner, and marks where bridges stand. The ground is the hexbound model: columns of spans in 1/8 m units (`design/import.hexbound.world.md`). Its design is `design/terrain.md`.

- **The pipeline:** `demo/pipelines/terrain.json` (draft)
- **Systems:** `terrain_relief` and `terrain_mesh` (drafts)
- **Channels:** `terrain_shape`, `side`, `opening`, `flat`, `spec`, `ground`, `bridge`, `mesh_spec`, and `model`, approved on 2026-10-14 (drafts)
- **Config:**
  - 7 shapes in `demo/config/terrain_shapes/`: river_valley, crossroads, coast_town, highland, marsh_basin, canyon_pass, glade.
  - 4 flats in `demo/config/terrain_flats/`: none, town, village, hamlet.
  - The biome is the boundary's (`boundary_biomes`), so the two match.

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `shape` | config | 7 shapes | The corners (a hexagon, rectangle, triangle, L, or polygon, turned and wobbled), each side's aspect (edge, height, rise), and the openings (roads, lanes, rivers in and out) | yes |
| `flat`, `second_flat` | config | 4 flats | Cells kept level for a settlement planner, within a tolerance. A town is optional: both default to none | yes |
| `biome` | config | 7 biomes | Colours, and the snow line | mesh only |
| `season` | enum | winter, spring, summer, fall | Grass dried in fall, snow-covered in winter | mesh only |
| `relief` | f32 | 0 to 60 m (10) | How high hills stand above the sides' blend | yes |
| `hill_size` | f32 | 8 to 200 m (60) | How wide a hill is | yes |
| `roughness` | f32 | 0 to 1 (0.4) | Small bumps and hollows | yes |
| `ridges` | f32 | 0 to 1 (0.3) | Ridged rather than rolling | yes |
| `cliffs` | f32 | 0 to 1 (0.4) | How much steep land breaks into rock faces | yes |
| `caves` | f32 | 0 to 1 (0.3) | How often a tall face opens into a cave mouth | yes |
| `river_depth` | f32 | 0.5 to 6 m (1.5) | How deep rivers run below their banks | yes |
| `seed` | u32 | | The terrain's seed | yes |

In the inspector, **edit** beside a config parameter edits that entry in place for the view: swap a side's aspect, move an opening, change the corners. Picking a preset again drops the edits. Through MCP, a parameter can be given inline as a whole object.

### Reading the ground

The C API (`ground.h` in `runtime.h`) reads the built ground of any instance whose pipeline has `draw.ground`:
- `foundry_ground_cell`: a cell's spans, flags, and water level.
- `foundry_ground_height`: the ground under a point.
- `foundry_ground_roads`: the roads as cells.
- `foundry_ground_bridges`: each bridge's two mounts and width.
- `foundry_ground_nav`: the navigation mesh.
- `foundry_ground_hull`: the ring a boundary takes, with each opening on its cell. Pass it to `foundry_boundary_spawn_hull` and the boundary meets the terrain, carrying its roads and rivers outward.

The enum lookups `foundry_<pipeline>_<param>_find` turn a name into a value, such as a figure's species.

## How it's built

1. **`terrain_relief`** builds and plans the ground (`design/terrain.md`, "Building it"):
   - **The shape** (`shape.h`, shared with the boundary's rim): its corners, wobbled sides, cells, each border cell's side, the ring outside, and the openings' cells.
   - **Relief:** the sides' heights carried inward by inverse distance, with hills fading in from the edge.
   - **Flats:** each flat is levelled within its tolerance, and blended into the land round it over as many cells as keep each ring's step under 0.9 m (5 to 30). Overlapping flats keep their cells in order: the first flat claims a shared cell.
   - **Rivers:** the cheapest path from each inlet to an outlet, avoiding flats. The bed never rises, and the banks climb about 0.9 m a cell, so the river cuts a valley. An inlet with no outlet ends in a lake, and an outlet with no inlet starts at a spring.
   - **Rock and caves:** outcrops break into rock faces where a noise mask meets a steep slope. A cave mouth opens in a face at least 3.5 m tall: a two-span column with 2 m of headroom, up to three cells deep.
   - **Roads:** a tree of the cheapest paths joining every road opening and every flat's middle. Each road is graded so no step between cells exceeds 3/8 m: openings and roads already laid are held exactly, flat cells within their tolerance, and decks over water level with the higher bank. A road is two cells wide and a lane one. Cobble paves a road, dirt a lane, and the land beside eases toward them.
   - **Materials:** cobble, dirt, sand by water, rock on steep ground and in caves, and grass elsewhere.
   - **The navigation mesh:** a hexagon on each walkable top, linked where an edge fuses or steps no more than 0.5 m, plus along bridge decks.
   - **The report:** proves every road opening reaches every other and every flat.
   - **Outputs for others:** the boundary's hull, and the plotter's nature ground grouped by height and moisture.
2. **`terrain_mesh`** draws it by the hexbound rules:
   - **Fans:** each top and bottom is a 13-point fan. Its corners are shared by fused groups (mutual-nearest pairs within the lower smoothness, any link that would join two surfaces of one cell dropped), and its midpoints follow the fused or unfused formula.
   - **Walls:** solid minus solid on each half-edge.
   - **Water:** a sloping surface whose corners take the mean level of the wet cells round them.
   - **Collision:** a convex column for each span.
   - **Guides:** sides by edge, openings, flats, roads, rivers, and bridge mounts.
   - **Bridge mounts:** as the model's places (`draw.places`).

## Looking at it

The inspector frames its settings in two groups: **Into the systems** (the pipeline's parameters, each marked with the stages it feeds) and **Only the view** (what is composed round the model to look at it, never part of its output). The view composes three things (`draw.nature`, `draw.surround`; `src/host/pipeline/compose.*`), each turned on or off there:
- **The plotter fill:** a plotter grows the open land (happy_glen by default, or any plotter).
- **The surround:** `boundary` built round the terrain's hull, sharing its biome, season, and seed, with its own detail and view distance. It is off by default (`draw.surround.on: false`).
- **The figures:** the boundary's near trees, filled with tree and bush builds.

`pipeline_view` takes `compose` to turn these off, and `on_ground` to stand an eye on the ground. Play walks the ground's collision.

## Checked

Each shape at its defaults, with `pipeline_set` and `verify_native` (2026-10-14):

| Shape | Cells | Triangles | Bridges | Longest walk | Steepest road step |
|---|---|---|---|---|---|
| river_valley | 4,015 | 53,300 | 1 | 128 m | 0.22 (3/8 m a cell) |
| crossroads | 3,625 | 45,508 | 0 | 123 m | 0.22 |
| coast_town | 3,666 | 48,064 | 1 | 144 m | 0.22 |
| highland | 2,523 | 34,352 | 0 | 109 m | 0.22 |
| marsh_basin | 4,630 | 62,234 | 1 | 97 m | 0.22 |
| canyon_pass | 1,564 | 28,696 | 0 | 156 m | 0.22 |
| glade | 1,269 | 16,332 | 0 | (one opening) | 0.00 |

- **Native:** every build matches WebAssembly byte for byte. Every road opening reaches every other, and every flat is reached.
- **The sweep:** a further 21 builds (every shape at seeds 2, 5, and 9, with a town and a hamlet, cliffs 0.7, caves 0.6, relief 18 m) all match, all reach, and hold their flats within 0.23 to 0.35 m, with 0 to 36 caves.
- **Overlapping flats:** the river valley's town and village overlap at different levels. They keep their tolerances (0.45 m at most), but the road between them climbs 0.36 a cell where they meet, and the report's note says so.
- **Cliffs and caves at their strongest** (highland, relief 30 m, alpine winter): 35 caves.
- **Composed:** the river valley with its boundary, the happy glen's fill, and the boundary's figures is 1,188 plants and 736 figures in 85 distinct builds, 8.2 M triangles, composed in 1.4 to 1.8 s.

## Performance

`stress_terrain --plan terrain` (`perf/plans/terrain.json`), `perf/report-terrain-20261014T210752Z.json`:
- **Generation (24 configurations spawned at once):** 714 ms at p50. Natively, the relief stage averages 8.2 ms (at most 15.9 ms) and the mesh 10.1 ms (at most 18.9 ms).
- **Triangles:** 47,548 at p50, 65,972 at most.
- **Walking inside one or two:** a frame at 0.1 ms CPU p95.

`tests/ground_test.cpp` (ctest `ground`) spawns the river valley natively and reads its ground:
- **The terrain:** 36 ms, 2 roads, 1 bridge, 3,800 navigation polygons with 10,974 links.
- **The boundary:** gets its hull with all four openings and builds in 276 ms.
- **The figures:** all 736 spawn as trees and bushes by their species names.

## Known limits

- **Vertex colour:** the ground has no painted image, unlike a boundary's terrain nodes.
- **Water:** a level per cell, not a simulated volume.
- **Bridges:** only their mounts. No bridge pipeline stands on them yet (road's bridge kind is a village cell).
- **Grading:** a road's grade can't always hold where it is held at both ends, between flats at different levels or onto a high deck. The report gives the steepest step and a note.
- **Collision:** convex columns, one per span (4,016 shapes for the river valley), not a triangle mesh (`physics_shape` has none).
- **Edits:** the runtime takes no edits to spans yet.
