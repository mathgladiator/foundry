# Maps: the regional executor

A **map** is the region a player plays in: several kilometres of land with its biomes, towns, roads, rivers, and points of interest, walled in by a boundary that makes the world seem to go on. The **regional executor** assembles a map from the foundry's pieces:
- terrains (`design/terrain.md`): the ground;
- plotters (`design/plotter.md`): nature;
- planners (`design/planner.md`): towns, villages, and keeps;
- single pipelines: a lone farmhouse, a castle to raid, a ruin;
- the boundary (`design/boundary.md`): the edge.

Asked for on 2026-10-14: the executor holds a high-level plan, gives the agent ways to partition the map, and assembles it from the human's feedback. The human talks the map through ("a fishing town on the south coast, a road over the pass to a hill keep, a lone house in the marsh"), and the agent places and partitions through MCP while the human watches. The map is what a player engages with, and it will keep evolving.

## The plan

A map is a workspace primitive, `maps/<name>.json`: the high-level plan, which the agent edits through `map_set` and the human reads in the atlas. It holds:
- **The shape:** a terrain shape (`terrain_shape`) about 3 km across. Its sides give the boundary its edges, and its openings are where roads and rivers leave the map.
- **The biome** (`boundary_biome`) and a seed.
- **Parcels:** the partition. Each parcel is a seed point with a reach and a character:
  - **Kind:** plains, hills, highlands, mountains, valley, lake, marsh, or coast.
  - **A plotter** for its nature.
  - **Its relief:** height, hills, roughness, ridges, cliffs, caves, and moisture.

  Every cell belongs to the parcel nearest it, by a weighted distance warped by noise, so borders wander naturally. Relief blends smoothly across borders. Partitioning is adding, moving, splitting, and reweighting parcels.
- **Sites:** what stands somewhere. Each site has a name, a place, a flat to stand on, and what fills it:
  - a planner (a town, a village, a hamlet, a keep, a walled town);
  - or a pipeline with its parameters (a lone farmhouse, a castle part, a ruin, a camp, a shrine).
- **Links:** the network. Each link joins two sites, or a site and an exit (one of the shape's openings), as a road, a lane, or a path, with optional waypoints.
- **Rivers:** from a source (a cell, or an inlet) to a mouth (an outlet, a lake parcel, or wherever the land leads), with a width.

## Executing it

Three stages, each a pure system, wasm and native alike:
1. **`map_plan`:** the macro plan, from the map's plan.
   - **A grid over the map** every 8 m: each node's parcel and blended relief, giving the macro height.
   - **Water and flats:** lakes at their level, rivers routed and carved, and the sites' flats levelled into it.
   - **The network:** each link routed as the cheapest path over the grid (grade, water, and existing roads weighed), then smoothed and graded (8% for a road, 12% for a lane, 18% for a path). Bridges stand where a road crosses a river.
   - **The partition into chunks:** 64 × 64 cells each, in axial parallelograms. Each chunk record carries everything its ground needs: the grid's window, the parcels it touches, the roads, rivers, and flats that cross it, and the seed. So a chunk builds alone.
2. **`map_ground`:** a chunk's ground, as hexbound columns (`terrain_ground`), plus an apron of one cell, so the mesh fuses across chunk seams.
   - **Every height is a function of the cell's world place:** the macro height interpolated, fine relief by the parcel's character, roads and rivers along their segments, and flats. Neighbouring chunks therefore agree exactly at their seam.
   - **Also per chunk:** materials, cliffs, caves, the navigation mesh, and the nature ground grouped by parcel, for each parcel's plotter.
3. **`terrain_mesh`:** draws the chunk by the hexbound rules, leaving the apron undrawn.

**The far land:** `map_far` builds the whole map as a terrain quadtree (`draw.terrain`, as a boundary's) from the macro plan, painted by parcel, road, river, and town, and sunk a little below the ground. It is what a player sees beyond the chunks near them.

## The pieces round it

- **The boundary** walls the map in: its hull is the map's shape, and its openings carry the exits on outward. The trees near its rim are figures (`design/boundary.md`, "Figures").
- **Nature:** each parcel's plotter grows over the chunk's nature ground (a region per parcel), as a game's region does.
- **Towns:** a site with a planner plans its settlement (the planner's own system) on the site's flat. Its fill (houses, roads, walls, gates, civic buildings, castle parts, waterfronts, crops) spawns as ordinary instances.
- **Points of interest:** a site with a pipeline spawns it on its flat, turned to face its road.

## In the foundry and the runtime

- **The atlas** (`map_view` with `atlas`, and the map's viewport) draws the plan from above:
  - parcels coloured by kind and outlined;
  - roads by kind, rivers, and lakes;
  - sites with their names, chunk lines, and the scale.

  The human and the agent talk over this picture.
- **Live** runs the map in the native runtime as a game would:
  - the boundary and the far land;
  - the chunks within a few hundred metres of the camera, with their nature, towns, and points of interest, spawned as the camera moves and destroyed behind it;
  - Play walks it.
- **MCP:**
  - `map_list` and `map_get` read maps;
  - `map_set` takes operations (parcels, sites, links, rivers, the shape, the seed) and saves; `create` makes a new map;
  - `map_plan` runs the macro plan and reports, with `verify_native`. `check_seams` builds pairs of neighbouring chunks and checks every apron cell matches its neighbour's own; `export_chunk` and `export_far` write samples for the `map_chunk` and `map_far` pipelines;
  - `map_view` draws the atlas;
  - `map_look` flies the live view's camera to a place (with `play` to walk from there, `grass`, and `probe` to report what the walker's world holds there), for `ui_screenshot`.
- **Settlements:** before planning, each settlement site's planner plans its town on flat ground (its config's terrain flat, no river: the map carries the rivers). Its working radius sizes the site's flat, and links to it end at its edge. Its fill is a list of placements (`src/host/plan/fill.*`, the same decisions as the planner view's preview fill) that the live map spawns by name (`foundry_diag_spawn`).
- **Who draws the ground:** the chunk's terrain alone. Plotters grow only what stands on a cell: the live map's grass fields have no bed (grass `bed: none`: blades only, no earth hexes or plates to fight the terrain). A settlement's streets are the ground's: its flat carries the cells it built on and its streets' materials (`map_flat.built`, `streets`, `street_materials`), so `map_ground` draws each street as dirt, sand, or cobble, its road pieces build only what stands at their edges (road `paving: none`; boards stay their own), and nothing grows on what it built. Its open ground, and any site's flat, grows grass but nothing planted.
- **Moving in it:** flying (WASD, Space and C or E and Q, Shift faster, the right button to look, the wheel with it held for the **fly speed**), or walking (**Walk here**, at a **walk speed**); F switches between them where the camera or the walker is (`design/playtest.md`, "Moving around").
- **Videos** (`map_video`, or the viewport's **Record**), into `qa/videos/` as H.264 MP4:
  - **Record** keeps what the view shows, at 30 frames a second of real time, until it stops.
  - `map_video` flies a tour (`src/host/map/tour.*`): `sites` (an orbit over each site and a flight between them, turning toward the next), `road` (along one link at a rider's eye), or `keys` (through camera keys). A tour is a function of time, stepped one video frame at a time; each frame waits until everything streamed round it is built (at most `max_wait_s`), and the runtime steps one frame's time per frame kept, so the video is smooth however slowly it renders. While it records, the streaming radii grow (`reach`, 1.8 by default), so forests and towns stand farther out under an aerial camera.
- **Streaming radii in the live view:**

  | What | Within | Gone beyond |
  |---|---|---|
  | Chunks' ground | 330 m (nearest first, six a frame) | 460 m |
  | Nature (two chunks a frame) | 210 m | 280 m |
  | Grass fields | 90 m | 130 m |
  | Sites | 650 m | 820 m |

  The runtime keeps every body in the walker's Jolt world, so the live map rebuilds the world's broadphase whenever bodies came or went (`Walker::tidy`): streaming adds and removes them one by one, which otherwise runs the broadphase out of nodes.

## Not built yet

- **The game's API for maps** (`foundry_map_*`): the map compiled into the runtime as plotters are, streaming round a focus point the game moves, with queries for sites, roads, and the ground. Today the foundry's host does the streaming through the runtime's public API (and its diagnostics' spawn by name).
- **Towns' walls and gates in the live map:** a wall is a whole network given inline, which spawning by name can't carry yet.
- **Town entrances:** a town plans its own streets; the map's roads stop at its edge rather than meeting its gates.
- **Distant forests:** only painted on the far land; no distant canopy or figures across the map.
- **Navigation across chunks:** each chunk's mesh stops at its own cells.
