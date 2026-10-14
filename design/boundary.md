# Boundaries: walling a region in while the world goes on

A **boundary** keeps the player inside a playable region of hex cells while making it look as if the world goes on past it. The game gives a **hull**: the cells around the region, counterclockwise as seen from above, each with an **edge** that says what stops the player there and what lies beyond. The boundary invents everything outside: the ground near the rim, cliffs, trees, beaches, valleys and mountains out to the view distance, and the images that colour them. Invisible walls along the rim keep the player in.

Asked for on 2026-10-14. It is the pipeline `boundary_v1` (`book/boundary_v1.md`), built in the foundry and the runtime; nothing is approved yet.

## The hull and its edges

| Edge | Near the rim | Beyond |
|---|---|---|
| **ledge** | The ground drops away at the cell's outer edge, with a lip of stones: the player walks up to it and looks out | A valley floor far below (the biome's `ledge_drop`): fields and hedges, woods, a river; its far side climbs back toward the horizon |
| **ridge** | A cliff face rises in strata over scree (the biome's `cliff_height`) | A plateau, rising toward peaks by the rise |
| **mountain** | A steep slope of scree and boulders, pines below the tree line | Peaks |
| **forest** | Rows of trunks, crowns, and undergrowth too thick to pass, by the biome's form (broadleaf, conifer, mixed, birch, dead) | A canopy over rolling land |
| **jungle** | Palms, buttressed giants with vines, or mangroves, by the biome's form, over broad leaves | A taller canopy |
| **sea** | A beach, surf, rocks, and driftwood | Open water to the horizon, with islands where the rise allows |
| **marsh** | Reeds, cattails, snags, and lily pads on pools | Wet flats rising to hills |

Each hull cell also has a **height** (its ground) and a **rise**, 0 to 1: how high the land climbs behind it, from lowland to the biome's highest peaks. A mountain edge always climbs. "Forest with a mountain behind it" is a forest edge with a high rise.

The cells needn't touch: the gap between consecutive cells is the hex line between them, taking the earlier cell's values, so a hull can be given as its corners. Cells with edge **auto** take their edge by direction from the hull entry's **sectors** (equal sectors counterclockwise from east), so one preset ("a valley: a ledge to the east, cliffs north and south, forest west") fits any region's ring.

The hull's cells are the boundary's: their ground is drawn by it, and the region inside them is the game's.

### Hulls from shapes

A hull can name a **shape** (`terrain_v1_shape`; `design/terrain.md`, "The shape and its sides") instead of listing cells: a polygon of corners from a kind and its sizes, with each side's aspect (edge, height, rise) and its openings. The hull is the ring of cells just outside the shape, each taking its side's values, blended round the corners. Thinking of a hull this way, you pick a shape and swap aspects side by side. A terrain built in the same shape hands its own ring to the boundary (`foundry_ground_hull`), so the land beyond starts at the heights the terrain ends at.

### Openings

Where a road or river leaves the region, a hull cell carries an **opening**: a road, a lane, a river in, or a river out, with its width. The rim finds where it crosses and its way out. The boundary then carries it on through whatever the edge is:
- **A road:** its corridor is graded gently outward (no more than 7%), clear of trees, cliffs, and scree.
- **A river:** its channel falls away (out) or rises (in) at 2.5% between eased banks, with water at its level.

The wall stays closed just past the opening's cell, and the game puts its transition there.

## Seeds that spread

The rim is the smoothed line through the hull cells' centres, sampled every metre. Each sample carries each edge's share, blended along the rim over `blend` metres either way, so a cliff tapers into a forest instead of stopping. Each point outside takes its signed distance from the rim and blends the samples within a little more than that distance. The farther the point, the more of the rim it blends: near the rim from the finest samples, far away from coarser copies of the rim, each blended twice as wide as the last. So the cells are seeds whose influence spreads into the distance. A point between two parts of the rim (across a bay) takes from both, smoothly.

Every height is a pure function of the point, the rim, the biome, and the seed. Each edge has its near profile, and the far land climbs toward ridged peaks by the rise. The canopy sits over the ground beyond the trees the rim's dressing draws, and water lies at the sea's and the marsh's levels.

## Levels

The land is a **quadtree**: from the region's middle out past the view distance, each node a grid of 32 × 32 quads. A node is refined only where a player inside the region could ever be near enough to see the difference: nearer than 48 of its spacings, the runtime's distance at high quality. Nodes wholly inside the region are left out. The finest spacing is 2, 4, or 8 m by `detail`, and the root is the smallest power of two covering the view distance.

Every node carries:
- **Its heights**, with skirts down its sides deep enough to hide a coarser neighbour's step.
- **Its morph**: each vertex's rise to its parent's surface. The parent's vertices are the node's even ones, and between them its edges and its quads' diagonals.
- **Its own image** (64, 32, or 32 texels a side by `detail`). The image is painted texel by texel from the land, interpolated from the node's grid, plus patterns finer than the grid: crowns with dark gaps, strata, fields with furrows and hedges, rivers, beaches and surf, snow by height and season, and relief from a fine bump. Its alpha marks steep rock, which a top-down image smears down a cliff, so the shader lays strata there by height instead.

**The runtime** (`draw.terrain`) uploads each terrain build once: its vertices, its triangles, and every node's image in one atlas. Each frame it walks each terrain instance's tree from the eye, in the instance's space. A node is refined while the eye is nearer its box than the quality's multiple of its spacing (20, 32, or 48 for low, medium, and high). Otherwise it is drawn, one record per node.

**Morphing is the transition.** A drawn node's vertices rise toward its parent's surface from 60% to 95% of the parent's distance. By the time the parent takes over, the node already has the parent's shape, so a level never pops.

Under the classic light the terrain hazes with distance; under a sky, the sky's fog does it.

## Figures

Within the **figure range** (`figure_range`, 100 m by default) the boundary's trees and bushes are **figures**: control figures the game fills from its own systems, so the near world is the game's real trees while the boundary draws only what is far. A figure is a tree or a bush with a species, an age, a height, a turn, and a seed (`boundary_v1_figure`).
- **Species:** each biome lists its species by where they grow (forest, jungle, mountain, marsh, and bushes; `figure_forest` and the rest), as tree_v1 and bush_v1 genomes.
- **The forest's wall:** its rows and its slope pines become figures, and a third of its undergrowth becomes bushes.
- **Beyond the wall:** rows of figures fill out to the range wherever the land would carry woods.
- **The canopy:** none within the range, so the figures stand on bare ground. Past it, the canopy and the dressed trees carry on as before.

The model lists its figures (`draw.figures`, which also names the pipeline that fills each kind). The C API returns them (`foundry_instance_figures`, with each species' name). A plotter's boundary fills its own (`foundry_region_spawn_figures`, from the plotter's compiled figure spawner), and the foundry fills them in every view (`src/host/pipeline/compose.*`). A range of 0 draws every tree as before.

## The dressing and the walls

What stands along the rim, close enough that a height grid and an image can't carry it, is an ordinary vertex-coloured mesh drawn like any instance:
- cliff faces in strata;
- a ledge's lip and its face;
- rows of trees and undergrowth;
- scree;
- reeds, cattails, snags, and lily pads;
- surf and driftwood.

Every item stands on the land the terrain draws.

The walls are boxes along the rim, 40 m tall (at the lip for a ledge). Each hull cell has a floor as a hexagonal prism. The forest and jungle hold blaze points, so a game can set them burning.

## In the foundry

- **The pipeline view** shows a hull entry: a predefined shape (glade, valley, coast, plateau, canyon, bay, jungle clearing, marsh edge, mountain pass, L-shape), or a sector preset on a ring. The view takes one cut through the tree from the region's middle and retessellates each node at up to 32 texels a side, coloured by its image. A flat floor stands in for the region's ground. `pipeline_view` takes `eye`, `heading`, and `elevation` to look from inside, as a player does.
- **A plotter's `boundary`** (a pipeline and its parameters) puts one around its region:
  - The static picture builds it in WebAssembly around the region's ring of cells.
  - **Live** spawns it in the native runtime with `foundry_region_hull` and `foundry_region_spawn_boundary`, so Play walks inside it against its walls.
  - The inspector's Boundary section shows or hides it and sets its hull preset, biome, detail, and view distance.

## The API

- `foundry_boundary_v1_spawn_hull` takes the game's own cells (`foundry_hull_cell`: q, r, edge, height, rise). They replace the hull entry's cells, and its sectors still give auto edges.
- `foundry_region_hull` returns a region's ring, with each cell's height and auto edges, for the game to edit.
- `foundry_region_spawn_boundary` spawns the region's plotter's boundary, and `foundry_region_spawn_figures` fills its figures.
- `foundry_instance_figures` returns a boundary's figures, for a game to fill from its own systems; `foundry_tree_v1_species_find` turns a species' name into tree_v1's value.
- A hull cell's `opening` and `opening_width` carry a road or river out through it (a terrain's `foundry_ground_hull` sets them).

## Not built

- **Sway.** Trees and reeds in the dressing don't sway.
- **Shadows.** The terrain casts no shadows.
- **Measured LOD.** The terrain's levels are by distance, not by a measured switch size.
- **Far scenery.** Nothing beyond the view distance: the farthest land ends at the haze.
