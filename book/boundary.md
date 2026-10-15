# The boundary pipeline (`boundary`)

`boundary` walls a playable region in so the player can't leave, while it looks as if the world goes on: cliffs, forests, beaches, and a valley below a ledge near the rim, and land out to the view distance beyond. Its design is `design/boundary.md`.

- **The pipeline:** `demo/pipelines/boundary.json` (draft)
- **Systems:** `boundary_rim` and `boundary_terrain` (drafts)
- **Channels:** `boundary_hull`, `biome`, `rim_spec`, `rim`, `terrain_spec`, `model`, and `figure`, approved on 2026-10-14 (drafts). The hull also takes `terrain_shape`.
- **Config:** 19 hulls in `demo/config/boundary_hulls/`:
  - 10 shapes by their cells: glade, valley, coast, plateau, canyon, bay, jungle_clearing, marsh_edge, mountain_pass, l_shape;
  - 2 by a shape and its sides: shape_hexagon, and shape_river_valley (the river valley terrain's own shape);
  - 7 sector presets for any region's ring: around_forest, around_valley, around_coast, around_highland, around_jungle, around_marsh, around_mountains.

  7 biomes in `demo/config/boundary_biomes/`: temperate, alpine, tropical, desert, boreal, mediterranean, volcanic.
- **In a plotter:** `happy_glen`'s `boundary` (around_valley, temperate)

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `hull` | config | 19 hulls | The cells around the region with their edges, heights, and rises, a shape with its sides' aspects and openings, or sectors by direction for any ring. A game gives its own cells with `foundry_boundary_spawn_hull` (each may carry an opening), and the entry's sectors still give auto edges | yes |
| `biome` | config | 7 biomes | Colours, tree forms, and scale (cliffs, the ledge's drop, peaks, snow and tree lines, sea depth, fields, rivers), and its figures' species (tree and bush genomes by where they grow) | yes |
| `season` | enum | winter, spring, summer, fall | Crowns, fields, grass, and snow | yes |
| `detail` | enum | high, medium, low | Finest terrain spacing 2, 4, or 8 m; images of 64, 32, or 32 texels a node; how dense the dressing is | yes |
| `view_distance` | f32 | 500 to 12,000 m (3,000) | How far beyond the region the world reaches | yes |
| `figure_range` | f32 | 0 to 300 m (100) | How far beyond the rim trees and bushes are figures for the game to fill; past it the boundary draws its own woods. 0: it draws them all | yes |
| `blend` | f32 | 1 to 20 m (6) | How far edges blend into each other along the rim | yes |
| `seed` | u32 | | The world's seed | yes |

Also in the C API:
- `foundry_region_hull` (a region's ring of cells);
- `foundry_region_spawn_boundary` (a region's plotter's boundary around it) and `foundry_region_spawn_figures` (its figures, filled from the systems its `draw.figures` names);
- `foundry_instance_figures` (a boundary's figures, for a game to fill from its own systems);
- `foundry_plotter_boundary_name`.

## How it's built

1. **`boundary_rim`** resolves the hull:
   - **The cells:** the entry's own, the ring just outside its shape (`shape.h`, shared with the terrain builder, each cell with its side's values), or the hex ring of its radius. Gaps are filled with hex lines, the loop is turned counterclockwise as seen from above, and auto edges come from their direction's sector.
   - **The rim:** the line through the cells' centres with its corners cut twice, sampled about every metre.
   - **Each sample** carries its height, its outward direction, its rise, and its share of each edge, blended along the rim.
   - **Openings:** where each road or river crosses the rim, its way out, and its width.
2. **`boundary_terrain`** invents the world from the rim (`design/boundary.md`, "Seeds that spread" and "Levels"):
   - the land, by each edge's profile and the far climb toward peaks;
   - a quadtree of 32 × 32-quad nodes, refined where a player inside could be within 48 spacings, each node with skirts, morph targets toward its parent, and its own painted image;
   - each opening carried on outward: a road graded gently, a river in its channel with its water, and nothing standing in either;
   - the rim's dressing, with its trees and bushes within the figure range as figures (and rows of figures out to the range where the land would carry woods);
   - walls along the rim and floors under the hull's cells;
   - blaze points in its forests and jungles;
   - the hull's cells as guides, coloured by edge.
3. **The runtime** draws the dressing as an ordinary instance and the terrain as a cut through the quadtree by distance, each vertex morphing toward its parent's surface before the parent takes over (`draw.terrain`; `src/runtime/render/terrain_renderer.*`, `terrain.hlsl`).

## Looking at it

- **From inside the region:** `pipeline_view` with `eye` (for example `[0, 1.7, 0]`), `heading` (degrees from north toward east), and `elevation`, under an observatory, shows what a player sees. The foundry draws one cut from the region's middle, retessellated at up to 32 texels a node, with a flat stand-in for the region's ground.
- **In a plotter:**
  - The static picture builds the boundary around the region's ring; `plotter_view` takes `boundary` and `eye`.
  - **Live** runs it in the native runtime, and Play walks inside its walls.
  - The inspector's **Boundary** section sets the hull preset, biome, detail, and view distance.

## Figures

Within `figure_range` (100 m) the trees are figures (`boundary_figure`: kind, species, age, height, turn, seed), and the terrain carries no canopy there. Hulls at medium detail give from 0 figures (plateau, around_highland: no woods) to 800 (around_forest). The valley gives 237, and the river valley's shape 736.

A game fills them from its own systems. The foundry fills them with tree and bush at low detail, four seeds a species: the river valley's 736 take 85 distinct builds. The live plotter fills them natively once the boundary is built.

## Checked

34 builds through `pipeline_set` with `verify_native` (2026-10-14):
- every hull at high detail, under one biome each (valley, canyon, and coast also under temperate and alpine);
- the glade, coast, and marsh edge in boreal winter and fall at medium and low detail;
- the valley at a 12 km view distance, and the mountain pass at 8 km.

Every build matches WebAssembly byte for byte, and Jolt takes its collision.

| Measure | Min | Median | Max |
|---|---|---|---|
| Terrain nodes | 213 | 341 | 485 |
| Terrain triangles | 490,582 | 783,458 | 1,113,411 |
| The foundry's mesh (dressing and one cut) | 353,434 | 599,386 | 836,410 triangles |
| The terrain stage in WebAssembly | 194 ms (4.4 billion fuel) | 600 ms | 1,059 ms (23.7 billion fuel) |
| The model | 16 MB | 30 MB | 42 MB |

Both maximums are the valley at 12 km. That is past the old 20 billion fuel guard, so the guard is now 60 billion (`src/host/wasm/runner.cpp`).

After figures, shapes, and openings were added (2026-10-14), a further 25 builds were checked natively:
- every one of the 19 hulls at medium detail;
- the valley at 12 km;
- jungle_clearing in the tropics;
- glade in boreal winter at low detail;
- shape_hexagon in the desert with a 200 m range;
- around_forest in a Mediterranean fall;
- shape_river_valley in the alpine biome with no figures.

All match WebAssembly.

## Performance

`stress_boundary --plan terrain` (a plan for terrains: `perf/plans/terrain.json`), `perf/report-boundary-20261014T210801Z.json` (with figures), on the AMD Ryzen AI Max+ 395 with Radeon 8060S.
- **Generation (24 configurations spawned at once):** 1,076 ms at p50 and 1,814 ms at p99. The native terrain stage averaged 408 ms (at most 890 ms).
- **The dressing:** 3,928 triangles at p50 (30,326 at p95), since the figure range's trees are the game's.
- **Walking inside one boundary:** 373,224 terrain triangles drawn, a frame at 0.15 ms CPU p95 and 0.28 ms GPU p95.
- **Inside two overlapping:** a frame at 0.3 ms p95.
- **Against the report before figures (070558Z):** generation p50 2% lower, the diversity frame's p95 21% lower.
- **The live plotter:** the happy glen with its boundary runs at 224 fps, drawing 250 terrain nodes (0.58 M triangles).

The generic quick plan doesn't fit a terrain: it spawns hundreds of overlapping worlds 10 m apart.

## Known limits

- The dressing doesn't sway, and the terrain casts no shadows.
- A build is heavy (about half a second natively, 30 MB), but there is one per region.
- The terrain's levels switch by distance (48, 32, 20 spacings by quality), not by measured sizes.
- Beyond the view distance there is only haze; there is no panorama.
- Inside a region the runtime draws nothing: the game's ground (a plotter's grass, or a terrain) covers it.
- Figures are placed in rows out from the rim, so a sharply concave rim gathers them closer together and a convex one spreads them apart.
