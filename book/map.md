# Maps: the regional executor (`map_plan`, `map_chunk`, `map_far`)

A map is the region a player plays in (`design/map.md`): a plan the human and the agent talk through (its shape, parcels, sites, links, and rivers), turned into a macro plan, chunks of hexbound ground, a far land, and the nature, towns, and points of interest round the player. The first map is `realm`, about 3.4 km across.

- **Workspace:** `demo/maps/<name>.json` (realm, pocket, scratch).
- **Systems:** `map_plan` (the macro plan), `map_ground` (a chunk's ground), `map_far` (the far land), plus `terrain_mesh` (drawing a chunk).
- **Pipelines:**
  - `map_chunk`: a chunk, from a `map_chunks` sample, or packed from a plan (`foundry_map_chunk_spawn_packed`);
  - `map_far`: the far land, from a `map_fars` sample, or packed (`foundry_map_far_spawn_packed`).
- **Channels:** `map_spec`, `map_parcel`, `map_site`, `map_link`, `map_river`, `map_plan`, `map_segment`, `map_flat`, `map_place`, `map_chunk`, and `map_far_spec`. `terrain_ground` gained each nature region's parcel and an apron flag.
- **Plotters for parcels:** `oakwood`, `pine_woods`, `moor`, `fen`, and `shore`, beside `happy_glen`.
- **Host:** `src/host/map/` holds the file and its operations, running the plan, cutting chunks, the atlas, the view, the live map, and the tools. `src/host/plan/fill.*` turns a planner's settlement into a list of placements.

## Talking a map through

The agent edits the plan with `map_set` operations and shows the atlas (`map_view`). The human reads the atlas or flies the live map, and asks for changes:
- **Parcels** partition the map: each is a seed point with a reach, a kind (plains, hills, highlands, mountains, valley, lake, marsh, coast), a plotter, its relief, and its woods. Moving a seed or changing its reach moves the borders, which wander by noise. A lake or the sea fills its whole parcel, so a small lake needs land parcels round it.
- **Sites** stand on flats:
  - a settlement by its planner (town, village, hamlet, keep, castle, walled town...);
  - or one thing by a pipeline with its parameters (a lone farmhouse, a ruined tower, a camp).
- **Links** join sites, or a site and an exit (`exit:<n>`, the shape's openings), as a road, a lane, or a path. They route themselves over the land, bridge rivers, and cross marshes on causeways.
- **Rivers** run from a source to an exit, a lake, or wherever the land leads.

## The realm

Its plan (`map_get realm`):
- **Edges:** mountains along the north, forests to the north-west and south-west, the sea to the south, and a ledge to the south-east.
- **Exits:** the west road and the east road.
- **Parcels:** northern peaks, high moor, greenvale, oakwood, eastern hills, the fen, mirror lake, south shore, and south downs.
- **Sites:**
  - Ashford, a market town by the lake;
  - Brookhollow (a green village) and Saltmere (a river town on the shore);
  - Fenwick, a marsh hamlet;
  - Greyspire Keep, a hill keep on the eastern hills: the castle to raid;
  - Old Mill Farm, Hermit's Cottage, and the Bandit Camp (houses);
  - the Ruined Watchtower (a castle part in ruin).
- **Rivers:** the Coldwater, down from the peaks to the lake; the Lakeflow, from the lake to the sea.

## Checked

- **`map_plan`** (2026-10-15): the realm plans in 725 ms in WebAssembly (21.3 billion fuel) and matches its native build.
  - 786 chunks, 12.2 km of road, 4 bridges, the steepest grade 18% (a path's limit), and no link unrouted.
  - Its five settlements are settled first (each planner on flat ground), cached by planner and seed: Saltmere is 1,949 pieces, Greyspire Keep 1,175.
- **Native matches** after the ground changes (2026-10-15): `map_chunk`'s sample, grass with `bed` own and none (blades, the mat level, and a plate), and road with `paving` own and none.
- **The hull** (2026-10-15): every map's ring is whole (`map_plan` `hull`): scratch 8,106 cells, realm 8,679, pocket 1,049, with no gaps; all three match native. Before the fix, corners keyed by rounded floats broke the ring far from the origin, so only a piece of it reached the boundary (scratch's was 298 cells in one corner), and the boundary's invented hills covered the region.
- **`map_chunk`:** the sample chunk (`realm_m64_0`, holding Ashford's flat and a road) matches native. Its ground takes 3 to 5 ms in WebAssembly (38 ms on a cold first run); its mesh 49,152 triangles.
- **`map_far`:** the sample (`pocket`, a small map) matches native, 30 ms.
- **Seams:** `check_seams` on 12 pairs of neighbouring chunks: 729 apron cells, none differing from their neighbour's own.

## Performance, live

Live in the foundry (the native runtime, on the AMD Ryzen AI Max+ 395 with Radeon 8060S), 2026-10-15:
- **Walking near Ashford's road with grass:** 122 fps. Update 1.3 ms, prepare 0.6 ms, GPU 5.2 ms; 6,336 of 36,886 instances drawn, 20.9 M triangles, 8,640 grass tiles.
- **Flying at eye height without grass:** 146 fps, GPU 4.4 ms.
- **Loading:** the first minute of streaming builds chunks and their nature, at about 10 fps while building.

Two fixes came out of measuring:
- **Grass:** fields spread over every filled chunk made the runtime's update 54 ms, so grass now covers only the nearest chunks.
- **The broadphase:** a long tour streamed so many bodies in and out that Jolt's broadphase ran out of nodes and aborted; the live map now rebuilds it whenever bodies change.
- **The walker:** its rays used the world's box, which one unbounded body made endless, so Play never found ground. It now clamps its rays (`src/host/physics/walker.cpp`).

## Videos

`map_video` with the `sites` tour (7 s orbits, 110 m/s between sites, 1280 × 720 at 30 fps, reach 1.8), 2026-10-15: 4,556 frames (152 s) recorded in 279 s. One frame was kept before everything round it was built (it waited its 4 s), and the file is 496 MB (H.264, CRF 20: dense foliage costs bits). A two-site test at 960 × 540 took 17.5 s for 388 frames.

## Known limits

- **No game API yet:** the host does the streaming; there is no `foundry_map_*` for a game.
- **Towns:** their walls and gates aren't spawned live (a wall's network can't be given by name yet), and the map's roads stop at a town's edge rather than meeting its gates.
- **Distant forests:** painted on the far land only, so a wood's edge shows where the nature fill ends (210 m).
- **The unbounded body:** some body in the walker's world has an endless box; it isn't found yet.
- **Town buildings on stilts:** some civic buildings stand on posts above the flat (their own foundations); not looked into yet.
- **Navigation:** each chunk's mesh stops at its own cells.
