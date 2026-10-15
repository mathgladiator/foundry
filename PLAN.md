# The Plan

This file is how the project is managed. It holds one current milestone, with TODO items that I, the human, will validate. The agent works on the current milestone; when it's done, I rename it and set the next one.

- **I set scope.** The current milestone's TODOs define what is in scope, so we stay on the QA track and don't wander. Anything I add to the current milestone is in scope; anything else waits.
- **The agent can suggest.** Ideas beyond the current milestone go under "Future milestones" as suggestions. They are not in scope until I move them.
- **Maximum progress, then questions.** The agent makes as much progress as it can, then writes its questions at the end of the current milestone. I answer them inline, so questions and answers stay together.
- **Summaries on request.** As a separate step, I can ask the agent to summarize a finished milestone in a new file and suggest the next current milestone.

# Current milestone: a complex system

- [x] **rocks that break uniquely**
  - `rock` (`book/rock.md`): 8 materials (granite, basalt, sandstone, limestone, slate, marble, obsidian, river stone), each with its own fracture; 4 sizes, moss, and season.
  - Pieces are cut at a lower level of detail when the rock is built, so a break never rebuilds; a closed-form animator throws, bounces, and settles them.
  - To validate: each material whole, mid-break, and settled (Start `break_time` in the View tab), against its reference photos.
- [x] **bushes that pair with the trees**
  - `bush` (`book/bush.md`): 14 species across the trees' biomes, swamp included; several stems from the ground, the trees' wind, flowers and berries by season.
  - Fruit can be picked: the `fruit` parameter applies only to species whose config marks them `pickable`, and is disabled for the rest.
  - To validate: every species by age and season, fruit on and off, and the soak's drain check (RSS stays over tolerance while the heap is flat).
- [x] **flowers that pair with the trees**
  - `flower` (`book/flower.md`): 15 species in clumps, eight head forms, buds, bloom, seed heads, and winter stalks by season, swaying in the trees' wind.
  - To validate: every species by season, against its reference photos and beside the trees of its biome.
- [x] **trees have pickable fruit**

### Jolt, stumps, and the first plotter (`design/plotter.md`)

**Integrate Jolt**
- [ ] **Jolt in the build.** Jolt Physics pinned by tag and fetched by CMake (FetchContent, like the other source libraries), a static library linked beside the runtime and into the host. `runtime.h` names no Jolt type: the game's world arrives as a tagged pointer (tag and Jolt version, checked), with a map from purpose (solid, soft, query-only, sensor) to Jolt layer and mask.
- [ ] **Collision descriptions.** A channel of Jolt shapes that each build outputs, static for the build: a tree's trunk, a bush's soft body, a rock's convex hull, a flower's query-only shape. Native matches WebAssembly.
- [ ] **Bodies by handle.** The runtime adds each instance's body to the world when its build is ready, swaps it on a rebuild, and removes it on destroy. The stress tool's soak checks that bodies balance.
- [ ] **Seen in the foundry.** A toy Jolt world in the host; collision drawn over the model in the View tab; an automated check that collision stays inside the conformance.

**The tree's stump**
- [ ] **Trunks from nature.** `tree`'s genome gets a stump width by species, and the rig reports the footprint of the largest age (hex cells) and the stump's radius now; checked against the reference photos.
- [ ] **Stump mode.** A `stump` boolean on `tree`: the mesh draws only the stump, and its collision becomes the stump's.
- [ ] **Knowing the fall is over.** `break_over` in the tree's update result (a current value), and the changed list in the API, so a game can turn a felled tree into a stump.
- [ ] **Book and checks.** `book/tree.md` updated with numbers from runs; flow, stress, and the QA video cover the stump; native matches WebAssembly.

**Kick-start the plotter**
- [ ] **The primitive.** `plotters/` in the workspace, plot systems as a kind of system, and the `plot_regions` channel (prioritized hex regions with attributes).
- [ ] **`plot_scatter`.** Candidates from cells, spacing per layer (Matérn), footprints, keep-clear, smooth ground, caps per kind, seed and regrow modes, and the volume left. Native matches WebAssembly.
- [ ] **Regions in the runtime.** Create, restore, grow, vacate, set regions, find, destroy; placements spawn as ordinary instances.
- [ ] **The plotter view.** Configure the shape (a hex, a ring, a line, unions, painted cells and attributes), grow step by step or to full, clear and regrow, the shared season and wind, and the metrics; the same through MCP.
- [ ] **A happy glen.** `plotters/happy_glen.json`: a sunny, gentle green place of oaks and apple trees in loose groves, hazel, bramble, and elderberry along its edges, daisies, poppies, bluebells, and foxgloves in the open grass, and a few river stones. It reads as a happy place on every shape tried. It has a book with numbers from runs, and the stress tool's region phase.

  
**Questions from the agent** (Jolt, stumps, and the first plotter; everything above is built and in draft, nothing is committed)
1. **New channels to approve**, each a draft with its reason in its manifest: `physics_shape` and `physics_body` (collision descriptions), and 13 for the plotter: `plot_region`, `regions`, `config`, `layer`, `kind`, `clearance`, `preference`, `occupant`, `exclusion`, `request`, `placement`, `cell`, and `result`. The model channels of all four pipelines gained `collision`, and the tree's spec, rig, body, and model gained `stump`, `trunk_diameter`, and the footprint.
2. **Painted cells.** The plotter view shapes the ground with presets (glen, hex, ring, strip, union, pond, path, slope, ragged, single) and an editable list of prioritized hex regions, but you can't paint single cells with the mouse yet. Is the region list enough for this milestone?
3. **Hedges are sparse.** A shrub's footprint comes from its soft body at its oldest (radius half its reach), so an old elderberry reserves 7 cells and hedges stand about 5 m apart. Should shrubs get a tighter soft body (or a footprint smaller than their collision) so they can form a hedgerow?
4. **The bush soak's drift is still open.** The soak's heap growth today was Jolt keeping removed bodies' broad-phase nodes until the world updates; the stress tool now settles its toy world before measuring, and the drain check now judges only growth. Tree, rock, and flower pass. The bush still fails one way: the heap is flat, but RSS sits 11 to 14 MB above the first drain. A tree soak with the GPU off passes (drains within 5.6 MB), so this looks like the GPU driver's memory. Should the drain check measure RSS without GPU mappings, or should I keep digging?
5. **The QA video with `stump`** is 4,416 clips for every variation. I didn't export it; pinning `stump` to true gives a stump-only video. Do you want a full export before you approve?


### The runtime owns cost (`CURRENT.FPS.md`)

The game says what is in the world; the runtime says how much of it to compute and draw (`design/submit.md`, "Cost"; `design/api.md`, "Quality"). Asked for on 2026-10-12: a GPU renderer for the foundry, the most quality at the most performance, lighter flowers, all of `CURRENT.FPS.md`'s ideas ordered by gain for the least change in looks, and the most trees at 60 fps.

- [ ] **Detail by size on screen.** The runtime picks each instance's level from its projected size, at switch sizes each pipeline measures (`stress_<pipeline> --lod`, the foundry's 10% rule at high quality) and compiles in (`draw.detail_pixels`), with hysteresis; coarser levels share the instance's pose once checked. A far level made by the runtime (the coarsest mesh, clustered). Too small not drawn.
- [ ] **Levels that look alike.** Flower, bush, and tree levels keep every element and simplify shapes; flowers' leaves and petals are one sheet (half their triangles, no change on screen). Native matches WebAssembly.
- [ ] **Posed when seen, as often as it shows.** Nothing out of view or too small is posed (events still play); small things are posed every 2nd to 16th frame.
- [ ] **Retained poses.** A persistent bone slot per instance on the GPU; only changed poses go up, scattered by a compute pass.
- [ ] **Measured.** The scale plan's ladder (each decision on in turn), its looks (what each costs on screen), and `--find 60` (the most trees at 60 fps); the plotter's inspector and `plotter_perf` show them. `CURRENT.FPS.md` reports it.
- [ ] **The foundry draws on the GPU.** The viewport, screenshots, sheets, and `plotter_view` render through SDL's GPU API with the same look; the CPU renderer stays for headless runs.

**Questions from the agent** (the runtime owns cost; built and in draft, nothing is committed)
1. **How strict is "high quality"?** A coarser level switches in where the median configuration differs from the finest on at most 10% of its pixels (20% at medium, 30% at low). From the game camera this changes 1% of what shows; from a far overview, about 16%. Is the 10% rule (the one fixed assets use) the right bar for high?
2. **Rocks keep the game's level.** Their pieces' break depends on the level, so the runtime can't share a pose across levels. Should the pieces' physics come from the finest level at every level (a change to `rock_body`), so rocks can switch too?
3. **Not built, measured as not needed yet:** impostors, wind on the GPU, and overlapping update with prepare. The next limit is the per-frame scan of every instance in the world (it grows with the world, not the view); culling and classifying by region would remove it. Should that be next?

### Fields, grass, and the live plotter (`design/plotter.md`, "Fields")

Asked for on 2026-10-13: the plotter's view should show the full simulation with its frame rate; radius up to 150; and grass as a lean, state-free functor over a region's cells that a plotter iterates without an instance per blade, stress-tested hard.

- [ ] **The live plotter.** The plotter's viewport runs the region in the native runtime (grown to full, with its fields), uncapped, with its frame rate and what it drew and posed; orbit and fly. The radius goes to 150.
- [ ] **Fields.** A pipeline with `draw.field` covers cells: its cell stage is a pure functor of a cell's integers, ground, moisture, and seed; its tile stage grows one shared cell's worth. The runtime spawns a field as one instance, runs the functor natively, draws by chunk at measured levels, and sways in the vertex shader. Plotters list `fields`; `foundry_region_spawn_fields` spawns them.
- [ ] **`grass`.** Five species (meadow, lawn, prairie, sedge, dune), plates for the other grounds, skirts for steps, four levels measured as a field. Native matches WebAssembly. Its book.
- [ ] **Terrain and wind.** The plotter's ground takes a terrain (flat, hills, ridges, terraces, dunes, mesas, wild; height, hill size, roughness, jitter). The inspector's season and wind reach the live view. Wind headings are the world's, so turned trees bend alike. Grass doesn't colour the ground.
- [ ] **Stressed.** The scale plan spawns fields, and `--only-fields` measures grass alone; `CURRENT.FPS.md` reports the most grass at 60 fps.

**Questions from the agent** (fields, grass, and the live plotter; built and in draft, nothing is committed)
1. **New channels to approve:** `grass_genome`, `grass_spec`, `grass_model` (with `sway`, a new draw convention: how far a vertex moves in a 10 m/s wind), `grass_cell`, and `grass_choice` (the field convention: present, variant, turn, scale, tint). Each has its reason in its manifest.
2. **Cell coordinates as unsigned parameters.** Pipelines have no signed parameter type, so `cell_q` and `cell_r` are u32 and a field passes a negative coordinate's bits (the cell stage reads them as i32). Should parameters get an i32 type instead?
3. **Reference photos** for the five grasses aren't gathered yet. Should I find CC0 photos before you judge them?
4. **Next, by the measurements:** a cheaper far tier for trees and shrubs (impostors), and drawing far grass chunks as one mesh each. See `CURRENT.FPS.md`.

### Terrain smoothing and the observatory (`design/observatory.md`)

Asked for on 2026-10-13: a smooth setting for terrain with no cliffs (for a game that blends between cells); and a new kind of workspace item, the **observatory**, one per planet and not versioned: a spherical sky whose state is a time of day, a time of year, and a latitude. It has a star map of the major constellations (a map says which are lit and which are connected), dynamic clouds that shadow the ground through the mega shader, a northern-lights parameter that animates, the sun and a visible moon in the right places, and light coloured by the time of day. It is also the first test of shadow maps, with a sundial, and every pipeline's viewer gets a checkbox and a dropdown to show it under an observatory.

- [ ] **Smooth terrain.** The ground's terrain takes `smooth` (0 to 1): blur passes, then no step between neighbouring cells over 0.25 m at 1 (0.3 m with the 5 cm height steps, measured).
- [ ] **The sky in the runtime.** `src/runtime/sky/`: the sun from the year, the time of day, and the latitude; stars on a planet's own pole; moons with phases; single-scattering air that gives the sky's colour and the light (the key light's colour at dawn, noon, dusk, and moonlight; the ambient; exposure); clouds; the aurora. `runtime.h` gains `sky.h` (observatory as data, the state, the light). A test checks the sun, Polaris, and the moon against known positions.
- [ ] **The mega shader's light.** `light.hlsli` shared by skinned, fields, and the foundry's viewport: the cel bands between ambient and key, the key's shadow map, the clouds' shadows, fog, and exposure; the classic light unchanged without a sky. The sky pass draws last, at the far plane.
- [ ] **Shadow maps.** The key light's depth map, snapped and biased, soft at its edge; tested on a sundial whose shadow tells the local time.
- [ ] **Observatories in the foundry.** `observatories/earth.json` (17 constellations, the Moon, the Milky Way, northern lights) and `mars.json` (dusty air with a blue sunset, Phobos and Deimos, a pole near Deneb). Selecting one shows its sundial (orbit, or stand and look up); its inspector sets and plays the time, day, latitude, clouds, aurora, shadows, and constellations. Pipelines, config entries, and plotters (static and live) have a Sky checkbox and dropdown. `observatory_get`, `observatory_set`, `observatory_view`; `pipeline_view` and `plotter_view` take `observatory`.

Asked for next, on 2026-10-13: a denser, adjustable star field; panning and setting the height of 3D views; a play test as a capsule that walks and jumps; and a gamepad, including looking up at an observatory's sky.

- [ ] **Star density.** The state's `star_density` (0 to 1) shows the background stars brightest first, live (each star's rank is baked into the chart), from the constellations alone to 60,000 stars down to magnitude 7.6; the Milky Way fades below 0.5.
- [ ] **Moon distance, more stars, and seeds** (asked for on 2026-10-13, after the above). Each moon has a usual distance, and the state a distance per moon (`moon_distance_km`): nearer is bigger and lights the ground more. Star density goes to 4 (240,000 stars baked, to magnitude 8.8). The sky's seeds (`stars.seed`, `clouds.seed`) are inputs in the inspector and `observatory_set`, and each star comes from (seed, its index) alone, so a seed always gives the same sky. The moon's light no longer waits for the sun to be 3° down (dusk had a dark gap), and the eye adapts less, so the light seen falls from noon to night except while a moon climbs.
- [ ] **Pan and height.** Every 3D view (static and live) pans along the ground (shift-drag or right-drag) and raises its target (ctrl-drag, or the height field), with a Centre button.
- [ ] **Play.** A Play button walks the hero's capsule (1.8 m) through the scene with Jolt's character controller: walk, run, jump, steps up to 0.45 m, slopes up to 50 degrees, solid bodies stop it and soft ones part; the camera behind it or at its eyes. In the live plotter it walks in the Jolt world the runtime keeps every trunk in (handed over as a game would). `tests/walker_test.cpp` checks it.
- [ ] **A gamepad.** Its sticks orbit and pan (shoulders raise, triggers zoom), walk and look in a play test (A jumps, Y swaps the camera, Back stops), and look around an observatory's sky; ImGui's gamepad navigation steps aside while a 3D view shows.

**Questions from the agent** (terrain smoothing and the observatory; built and in draft, nothing is committed)
1. **Shadows were listed under "Not in this milestone".** This brings the first shadow map in (one sun or moon map, no cascades, skinned instances as casters, fields receiving only). Should cascades, fields casting, and casters outside the view wait for a later milestone?
2. **Observatories are data, not compiled in.** A game hands the runtime an observatory through `foundry_sky_set_observatory` (as the live plotter does), while plotters are compiled in by `tools/gen_pipelines.py`. Should observatories be compiled in too, by name?
3. **Stylized, not exact:** the Moon is drawn at twice its size, moonlight is blue and brighter than real, faint stars are brighter than real, and the eye opens up to 14 times at night so nights stay readable. A full-moon midnight is now about a quarter as bright as noon (0.41 against 1.45), darker than sunset. Is that the right balance for the game, or should nights be darker still?
4. **The night sky's star data** (positions, magnitudes, colours of the constellations' 119 stars) is from the standard catalogue values as I know them, not from a downloaded catalogue. Should I check them against a CC0 catalogue (such as the Yale Bright Star Catalogue)?
5. **The play test isn't on MCP yet.** The agent can't press Play, walk, or jump through MCP, so I tested the walker with a unit test, not by playing. Should there be a `view_play` tool (start, walk for a time along a heading, jump, then screenshot)? `CLAUDE.md` says the agent can do everything a human does in a play test.
### Village buildings (`book/house.md`; `design/systems.md`, "Village houses")

Asked for on 2026-10-13: a new pipeline for classic RPG village houses, stores, and workshops, exteriors only, with:
- a type that brings its own signs and doodads;
- the season, a base level of animation, and parts that answer the wind;
- four stages mirroring the owner's upgrades: construction with a tent, basic, deluxe, and premium;
- the player meeting it from a command view or at its doorway;
- reasonable but diverse footprints.

Its six channels were approved on 2026-10-13.

- [ ] **The pipeline.** `house`, with:
  - `house_build`: the lot, stages, walls and roofs by material, door, windows, signs and symbols, features, yard things, seasons, collision, the doorway, and the marker;
  - `house_animate`: sway in the trees' gusts, spin, smoke, the door, and glows;
  - the `house_*` channels.
- [ ] **Ten types** in `config/house_types/`: cottage, farmhouse, inn, general store, bakery, smithy, carpenter, tailor, alchemist, and mill. Each has a usual and an alternate lot shape (wide, deep, or round).
- [ ] **Checked and measured.**
  - All 480 type × household × lot shape × stage × season (summer and winter) builds match their WebAssembly builds and stay inside their round footprints, with collision Jolt takes inside the conformance.
  - Detail sizes are measured.
  - The quick stress plan holds 800 houses at 0.37 ms p95. It fails its event checks (question 2 below).
  - The book is written.

Asked for next, on 2026-10-13: houses about twice as big, so a hero could believably live, work, and lie down in one; sizes for a solo NPC, a family, and a multi-generation family; cloth (the tent's flap, the awning) that stays attached; and a design for a planner of settlements, from small villages to sprawling keeps and cities.

- [ ] **Households.** `house`'s spec gains `household` (solo, family, extended), which sizes the lot: 19 to 20 cells for one person, 30 to 37 for a family, 48 to 61 for several generations, each wide (`rect5x4`, `rect6x5`, `rect8x6`), deep (`rect4x5`, `rect5x6`, `rect6x8`), or round (`flower19`, `flower37`, `flower61`). A deep house turns its gable to the street; an extended household's house has a second storey from deluxe on; construction pitches a tent per generation, each long enough to lie down in.
- [ ] **Cloth stays on.** Sway turns a part about the line it hangs from (a bracket, a pole, a washing line, an awning's edge, a tent flap's seam), pushed by the wind across that line, so nothing comes away.
- [ ] **The planner's design.** `design/planner.md`: a planner lays out a settlement as a plan map (its shape and every cell's height, streets, squares, plots for buildings with their size classes and frontages, mounts for fixtures and decorations, and a navigation mesh proving every plot is reachable from every entrance) for the game to fill; scenarios from a hamlet to a city, with ranges for plot sizes and the game's rules. A design only; nothing is built.

Asked for next, later on 2026-10-13: planners in the workspace to look at before reviewing the design; more footprints, every one outlined on the ground, a camera that frames the largest so sizes compare, and a box conformance for each lot; palettes for houses as config (players' colours, village themes); and a game-wide look for accessibility and effects (colour vision, contrast, lines between colours, greyscale, a tint).

- [ ] **Planners in the workspace.** `plan_settle` and eight planners (hamlet, street village, green village, round village, market town, walled town, hill keep, river town) under **Planners**, filled with `house` and stand-ins, with the tier, seed, plot count, rules, report, navigation mesh, and Play; `planner_get`, `planner_set`, `planner_plan`, `planner_view` (`book/planners.md`).
- [ ] **More lots, shown to scale.** Long (a burgage: `rect3x8`, `rect4x9`, `rect5x11`) and broad (a shopfront: `rect7x4`, `rect9x5`, `rect10x6`) lots beside wide, deep, and round; every lot outlined on the ground; a fixed 20 m framing (`draw.frame_m`); the lot's box as a second conformance, checked against the collision.
- [ ] **House palettes.** `house_palette` and the `palette` parameter: per part, a colour and a mix over the type's; natural, eight players' colours, and three village themes in `config/house_palettes/`.
- [ ] **The look.** `foundry_set_look`: colour vision (daltonizing), contrast, saturation, edges, and an effect (greyscale, or a duotone tint) at a strength, applied last by every runtime shader; the viewport's **Look** button and `look_set` apply it to every view (`design/submit.md`, "The look").

Asked for next, later still on 2026-10-13 (feedback on the first planners): no mounts, only open land that passes to a plotter, which fills the gaps with nature; the settlement's own shape rather than a hex, so villages can string along a road or ring an open body; entrances and walls as special tiles; roads as governed plots with their own system and config of road kinds (market squares, stalls along a road, fences); and water, rivers, and waterfronts as plots, with a system for waterfront buildings.

- [ ] **Plans as plots.** A plan is the settlement's shape (no hex) and its plots: buildings, road segments (each cell with its neighbours and role), waterfronts, and water; walls and gates are tiles; everything else is open and goes to a plotter as the plan's nature ground. Mounts are gone (`plan_mount` deleted).
- [ ] **Roads.** `road` and `config/road_kinds/` (lane, street, high street, market, square, alley, bridge, quay, gate): one cell from its kind and its neighbour mask, with verges, fences, stalls, planters, crates, lamps, a market cross or well, quays, and bridges by rule (`book/road.md`).
- [ ] **Waterfronts.** `waterfront` and `config/waterfront_types/` (pier, boathouse, warehouse, fishery) on part-land, part-water lots; the planner places them along rivers and lakes, behind a quay or ring lane, and a lake village gathers round its lake (`book/waterfront.md`).
- [ ] **Nature in the planner view.** A plotter (`happy_glen` by default) grows the plan's open land and an apron round it.

Asked for next, late on 2026-10-13 (feedback on roads and the plan): fix the road viewer; walls and gates only as marked cells in a plan (a visual detail in the view, no geometry); no stand-ins or other details that don't speak to the plan; a road's flow, so a plain road can be upgraded and every road piece keeps movement open (an edge piece in at most half a cell, its orientation in the plan), so a market is built up that way; a square's centre as a building its road rings; roads only for what stands at their edges, with room for fences; fence and wall systems that lay a line of points straight across hexes, chunked and optimized; civic buildings as their own system; a keep's wall inside the town's, so a keep is a small village and a castle can sprawl; a castle system for its parts; and a gate system that works with every wall.

- [ ] **The road viewer.** A road cell is shown among its six neighbours (each outlined by what it is), with arrows along its flow, its half, and its fence edges (`draw.guides.lines`); the inspector edits a mask edge by edge; each item opens at its own framing.
- [ ] **Roads keep their flow.** `road` builds only what stands at its edges: a half's edge piece (a stall, a bench, crates, a planter, a cart) in at most half a cell, never closing its flow; planters and crates in strips along buildings; lamp and sign posts; fence edges kept clear. No fences, no centres (`book/road.md`).
- [ ] **Plans as a draft.** Walls and gates are tiles and lines; a square's centre is a building plot; road cells carry their halves, flows, and fence edges; a market is built up in halves checked against every entrance's reach; navigation follows the flow. The view builds nothing of its own: no stand-ins, flat outlined wall tiles (`book/planners.md`).
- [ ] **Civic buildings.** `civic` and `config/civic_types/`: church, chapel, temple, court, town hall, guild hall, library, bank, mage's tower, market hall, and the centres (well, market cross, fountain, maypole, statue, shrine) (`book/civic.md`).
- [ ] **Castles.** `castle` and `config/castle_parts/` (keep, great hall, chapel, barracks, stables, kitchen, storehouse, tower); a keep's ward in a hill keep and a new `castle` planner, the keep at the ward's middle with a bailey round it and the parts round the bailey (`book/castle.md`).
- [ ] **Walls, fences, and gates on lines.** `line_chunk` lays a line of points (straightening hex zigzags, openings for gates, towers spaced, standard modules shared, steps down slopes, chunks); `wall`, `fence`, and `gate` build the pieces, a gate with the wall's own kind; the plan gives its walls, the keep's wall, and road fences as lines (`book/wall.md`, `book/fence.md`, `book/gate.md`).

Asked for next, on 2026-10-14 (feedback on doors, fences, and walls): mark where the doors are as part of house and civic footprints, so the planner keeps the road in front of a door a strict path (no stall in front of a door); fences and walls that follow lines through a hexagon's opposite vertices and fill the gaps as one continuous system, given as line segments so they form topologies with joins that fit, not zigzags; example lines to see the wall and fence systems by; and walls that fit tightly round the plan, like a metaball of it.

- [ ] **Doors.** House, civic, and castle footprints report their doors' cells (a house's door stands at its footprint's door cell); a lot is placed only where a door meets a road; that road cell is a strict way (road's `doors`: no half, nothing against the door).
- [ ] **Walls and fences on the lattice.** `line_network` (nodes on a diagonal sublattice, links through opposite corners, gate nodes and links, towers) replaces the line request, pieces, and layout; `wall` and `fence` build a whole network (or a chunk) as one model with joins for ends, bends, T, Y, X, and stars; a wall reports its openings, where `gate` stands; 11 example networks in `config/line_networks/` (`book/wall.md`, `book/fence.md`).
- [ ] **Walls hug the plan; fences enclose yards.** A wall is the union of lattice hexagons round the lobes of a metaball of what's built, with gates where roads cross it; a share of plots get fenced yards, each its lattice hexagon, sharing fences with their neighbours (`book/planners.md`).

Asked for next, later on 2026-10-14 (themes, ruins, fire, and crops): a ruin version of every village system (castle, fence, gate, house, road, waterfront); civic buildings in three construction steps (planning, half, finished) and castles in those and a ruin; more exotic features at the top grandeur; a theme that controls the architecture of houses, castles, civic buildings, and waterfronts, with wall, fence, and road materials to suit the themes (a druidic village, a stone-dome village); fire control points on every building (x, y, z, fire config id) and a runtime fire system with particles, flames, and smoke that respects the wind, its configs controlling colour and the rest, with what it means for the mega shader; and a crop system: a footprint like a building's, behaving like a bulk flower, about 12 crops, wind computed a configurable number of times and the plants instanced on it.

- [ ] **Themes.** `village_theme` (8 themes in `config/village_themes/`) sets roof form, plan, walls, windows, foundation, ornaments, colours, moss, and fires for `house`, `civic`, `castle`, and `waterfront` (`theme.h` in each); new road (moss stones, sand, boardwalk, elven way), wall (a henge of trilithons, drystone, living willow, adobe, a dwarven rampart; dome towers), and fence (living willow, stone slabs, reeds) kinds; seven themed planners (`design/planner.md`, "Themes").
- [ ] **Ruins and construction steps.** Houses' `ruin` stage; civic buildings' and castles' `stage` (planning, half, finished, ruin); waterfronts', roads', walls', fences', and gates' `condition` (`condition.h` in each); `ruined_keep` (`design/planner.md`, "Ruins and building").
- [ ] **Wondrous.** Civic buildings' fourth grandeur: twin domed towers crowned with crystals, flying buttresses and a rose window, braziers and a gilded figure, a ring of crystals circling a mage's tower, a gilded dome over a well (`book/civic.md`).
- [ ] **Fire.** Fire points on building models (`draw.fires`); `fire_config` (12 fires in `config/fire_configs/`, compiled in); the runtime's fire system (`src/runtime/fire/`: a CPU particle system, smoke drifting with the world's wind, sprites drawn after everything solid) and the nearest fires' light in the mega shader; `foundry_set_wind`, `foundry_set_burning`; fires in every foundry view (`design/submit.md`, "Fire").
- [ ] **Crops.** `crop` (16 crops in `config/crop_types/`): a field on a lot, its plants on a few drift bones; field plots in the planners (`book/crop.md`).

**Questions from the agent** (village buildings; built and in draft, nothing is committed)
1. **Buildings were under "Not in this milestone"**, and `design/systems.md` designs a kit of pieces (walls, floors, stairs) for houses, forts, and dungeons. `house` is a different, whole-house pipeline for village exteriors, beside that kit rather than replacing it. Is that the split you want?
2. **A house never settles.** Its smoke, sails, and glows always move, so the runtime never sleeps it. The runtime's event-over flag and the stress plan's event checks treat "settled" as "the event is over", so `house` fails them by design:
   - the door's event never reads as over;
   - "same as alone" differs, because the runtime skips posing unseen houses.

   Should events get their own end, apart from sleeping? For example, an event parameter could declare how long it lasts (the door's 4.4 s), the runtime could raise its over flag then, and the stress plan could check that instead.
3. **The game can't read a house's doorway, marker, or cells.** They are in the model, but the C API's instance results carry only event flags. Should instance results (`api.md`, "Instance results") carry such model facts, by name, for any pipeline?
4. **Draft channels patched for households and lots.** `house_spec` gained `household` and `palette`; `house_type`'s footprints became lot shapes (wide, deep, round, long, broad); `house_model`'s footprint names changed (15 lots), its round footprints now go to 127 cells, and it gained `lot_min`, `lot_max`, `guide_cells`, and `guide_tones`; `house_bone`'s sway axis now means the line a part hangs from. They are uncommitted drafts, so I patched them in place under the approvals of 2026-10-13. Is that right, or do you want to approve the changes again?
5. **Bigger houses cost more.** The largest premium house is 11,854 triangles (it was 5,162). With the long and broad lots, generation is 2.0 ms at p50 (it was 1.37), and 800 houses take 0.6 ms a frame at p95 (`perf/report-house-20261013T211537Z.json`). Are extended households' premium houses within budget, or should their high level drop detail?
6. **The planner runs on assumptions** (listed at the end of `design/planner.md`, reshaped by your feedback: only plots are reserved, the rest is open for a plotter). The ones that most shape it now:
   - a plan is laid out once at its final size, and growth opens it by tiers rather than planning more later;
   - the planner sets the heights inside its shape, and its open land eases toward the natural ground for the plotter;
   - plots are flat pads, and a road cell is flat at its own height;
   - roads follow hex lines, cell to cell;
   - the navigation guarantee ends at each plot's frontage.

   Which of them would you change before it moves into a milestone? You can try them in the workspace.
7. **Planners are outside the milestone.** I built them because you asked to see them; they aren't in the runtime or the C API, and have no stress plan. Should they become a milestone of their own (the API, compiled planners, a stress plan, merged navigation polygons, the city), or stay a foundry preview for now?
8. **The hill keep terraces hard:** its 16 m hill cuts steps into every pad and road, and its alleys climb at up to 26%. Should hill towns terrace whole blocks (a few big steps) instead of each plot and road?
9. **Lines between colours in the game.** The look's edges need a pass that sees neighbouring pixels. The foundry's viewport has one (its ink pass); the runtime draws into the game's render pass and has none. Should the runtime offer a finishing pass the game can run (ink, edges, and the look), or leave edges to the game?
10. **A system that wants the look** (a model built with more contrast for a high-contrast setting) would take it as a pipeline parameter the game sets, like the wind. None needs it yet; is that the mechanism you want?
11. **Everything borrows the house's motion.** Roads, waterfronts, civic buildings, castles, fences, and gates use `house_bone` and `house_animate`, as approved. Gates now want motions it doesn't have: doors held open, a portcullis rising and falling, a drawbridge lowering; and boats want to bob on a swell. Should that become a shared motion family (`motion_bone`, with a lift and a held open-or-shut), rather than more house motions?
12. **One instance per road cell.** A market town's roads are about a thousand `road` instances. They draw within budget (800 at 0.17 ms p95), and walls and fences are already one instance per chunk of about 40 links. Drawing a road plot as one instance, as grass fields are, would be cheaper. Worth doing before planners reach the runtime?
13. **A plan across two biomes.** In the foundry one plotter grows the whole plan. In a game, each biome's plotter would grow the part of the plan's nature ground inside its own region. Is that the split you want, or should the plan say which biome each open cell belongs to?

14. **A game's own network.** `wall` and `fence` take their network as a config parameter: the foundry passes a planner's network inline, but the C API can only name a compiled example. A game giving its own network (a player's fence, a ruined wall) needs an array parameter type in the generated API (`design/planner.md`, "The API"). Should pipelines get array parameters, or should networks go through a separate call?
15. **Signs and lamps are thin.** A sign post stands at one junction cell in four (a two-wide high street's every cell has three ways or more, and signs everywhere were noise), and lamps are one roll per cell (6 to 8% on streets). Should the plan mark junctions and lamp places instead, so they read as planned?
16. **Halves in a bailey.** A keep's bailey is a square, so it gets benches in halves like a village square (`halves_square`, 0.15 in the keep planners). Should a bailey have its own road class and kind (a castle's yard: hitching posts, a well, practice dummies)?
17. **The hill keep's walk.** Its keep's ward on the hilltop is reached by one road switching back at a cart's grade, so its longest walk is 689 m. Should a keep on a hill have a steeper walkers' stair as well (an alley class that only walkers use)?

18. **Three directions only.** A line through opposite vertices runs at 30, 90, or 150 degrees, so a wall or fence along any other direction zigzags. I made walls and yards lattice hexagons (straight sides, 120 degree corners), which follow a town's lobes but not every bay in its edge. Lines through opposite edge midpoints (0, 60, 120 degrees, cell centre to cell centre) would give twelve directions, so a fence could also run straight along a street. Do you want that second family, or keep walls and fences to the vertex lines?
19. **Yards have no gates.** A yard's fence stops at its lot and at roads, so it is open toward its street. Should a yard's fence close across its front with a gate at the door?
20. **Doors on waterfronts.** Waterfronts' doors aren't marked yet; their backs face a cart road. Should they get door cells too?

21. **Draft channels patched beyond the list you approved.** Within this request I also added `dome` to `wall_kind`'s tower roofs (the stone-dome wall), a `drift` motion to `house_bone` (crops' wind groups), and a third meaning to the models' `fire_lit` (2: smoke only, for chimneys). They are uncommitted drafts, patched in place. Is that right, or do you want to approve them again?
22. **Fire lights are banded, unshadowed, and eight.** The mega shader takes the eight strongest, nearest fires each frame, in the cel bands, without shadows (`design/submit.md`'s lights budget says four per object and three shadowed). Is eight a frame, unshadowed, what you want, or should fires take part in the per-object light budget?
23. **Fire is CPU particles.** At most 60,000 particles; the runtime only emits from instances near the view. A burning town of a few hundred blazes stays within that, but the stress plans don't burn anything yet. Should the stress plan gain a fire phase (set every instance burning), and should particles move to the GPU (analytic flames, simulated smoke) once a game burns cities?
24. **Chimneys' smoke moved to the fire system.** Houses, castles, and civic buildings no longer animate smoke puffs as bones; their chimneys are smoke-only fire points. That is lighter on bones but means a game without the fire system shows no smoke. Is that the split you want?
25. **Crop fields and curving lanes.** A field needs half its front row on a road, so round and lake villages (and the themes built on them) place none. Should fields front a lane with less frontage, or sit behind a farm's lot with only a cart track?
26. **Wondrous at the centre.** On a centre lot a wondrous centre adds a gilded dome, a crystal finial, a gilded figure, or an eternal flame; a wondrous hall's twin towers only on its own lot. Should the planners give any building wondrous (only `elven_glade` does, by wealth)?

### Boundaries (`design/boundary.md`; `book/boundary.md`)

Asked for on 2026-10-14: a boundary system that walls a region of hexes in so the player can't escape while it looks like a natural, larger world. The game gives a counterclockwise hull of cells, each with its type (a ledge to look out from, a ridge's cliff, forest or jungle too dense to pass, a mountain behind); quadtrees and invented geometry and images give a sense of the world beyond; the engine interpolates transitions smoothly; the cells seed the world, spreading into the view distance to make mountains; and the inspector takes predefined shapes to see how it plays out.

- [ ] **Hulls and edges.** `boundary_hull`: cells with edges (ledge, ridge, mountain, forest, jungle, sea, marsh, or auto by direction), heights, and rises; gaps filled with hex lines; 10 predefined shapes and 7 sector presets. `boundary_rim` blends edges along the rim.
- [ ] **The invented world.** `boundary_terrain`: each edge's land near the rim, the far land climbing toward peaks by the rise, seeds that spread farther the farther from the rim; a quadtree of terrain nodes with skirts, morph targets, and painted images; the rim's dressing; walls and floors. 7 biomes. Native matches WebAssembly.
- [ ] **The runtime draws it.** `draw.terrain`: a cut through the quadtree by distance each frame, vertices morphing to their parent's surface, images in one atlas, strata on steep rock, haze under the classic light. `foundry_boundary_spawn_hull`, `foundry_region_hull`, `foundry_region_spawn_boundary`.
- [ ] **In the foundry.** The pipeline view of every shape, and standing inside one (`pipeline_view`'s `eye`); a plotter's `boundary`, in its picture and live (Play walks against its walls), with the inspector's Boundary section.
- [ ] **Measured.** `--plan terrain`; the book with numbers from runs.
- [ ] **Play starts in the middle** (asked for on 2026-10-14). On a map (a plotter's region, live or static; a plan; a boundary's world), Play starts at the middle in the first place the capsule fits: open ground with nothing overhead, flat enough to stand on, touching nothing (`Walker::find_spawn`; `tests/walker_test.cpp`).

**Questions from the agent** (boundaries; built and in draft, nothing is committed)
1. **Host and tool changes outside the pipeline.** To build and see it I raised the WebAssembly fuel guard from 20 to 60 billion (a boundary at a 12 km view distance takes 23.7 billion), made the stress tool's waits hold on the wall clock as well as in simulated time (a boundary build takes a good part of a second, and frames take microseconds), added `perf/plans/terrain.json`, split `light.hlsli`'s `shade` into `shade_lit` and the look, and added `FOUNDRY_DUMP_MISMATCH` (it found that `face()` emitted vertices in argument order, which GCC and Clang evaluate differently). Are those all right?
2. **One draft channel patched after approval:** `boundary_model`'s images now say their alpha marks steep rock (the runtime lays strata there by height). Is that right?
3. **The foundry's picture isn't the runtime's.** The foundry draws one cut from the region's middle, retessellated and vertex-coloured, with no morph; the runtime textures and morphs per frame. Live in a plotter shows the real thing. Should the foundry's viewport learn textured meshes, so a pipeline view of a boundary is the runtime's own?
4. **Static dressing.** Its trees and reeds don't sway, and the terrain casts no shadows. Should the dressing take wind bones (as crops do) or the field's GPU sway, and should the terrain cast into the shadow map?
5. **Beyond the view distance** is only haze. Should a panorama (a ring of distant ranges as one image) close the horizon, so the view distance can be shorter and builds lighter?
6. **A game's hull from a region.** `foundry_region_hull` gives the ring around a region with auto edges; a game sets edges cell by cell or lets the boundary's sectors choose by direction. Is that the split you want, or should a region's cells carry their edge as an attribute (`plot_region`)?

### Terrains, and the boundary's figures (`design/terrain.md`; `book/terrain.md`)

Asked for on 2026-10-14:
- **The boundary's near trees** (its first 100 metres or so) are control figures the game fills from its own systems, so the boundary owns what is far away and the game fills in the details.
- **Parametric hulls:** a shape whose sides take aspects you can swap.
- **A terrain builder:**
  - **Its inputs:** a shape with its edges given, the hexbound document as its ground model, entrances and exits on the edges, and water flowing in and out.
  - **What it builds:** roads planned and checked on a navigation mesh, flat-ish cells a city planner can use (optional), and bridge mount points.
  - **Its view:** a plotter to fill it, which can be turned off.
  - **The boundary:** given a matching shape, so the two connect.

- [ ] **Figures.** Within `figure_range` (100 m) the boundary's trees and bushes are figures (`boundary_figure`: kind, species, age, height, turn, seed; species by biome) with no canopy there, and rows of figures out to the range. `foundry_instance_figures`, `foundry_region_spawn_figures` (the live plotter fills them natively), and the foundry's views fill them with tree and bush (`draw.figures`).
- [ ] **Shapes and openings.** `terrain_shape` (hexagon, rectangle, triangle, L, polygon; turned and wobbled; each side's aspect; openings for roads, lanes, and rivers in and out), shared by the terrain and the boundary (`shape.h`); a hull may name a shape; a hull cell may carry an opening, which the boundary carries outward (a graded road or a river channel through a cut). The inspector's **edit** edits a config entry in place, side by side.
- [ ] **The terrain builder.** `terrain` (`terrain_relief`, `terrain_mesh`):
  - **The ground:** hexbound columns of spans in 1/8 m. Relief from the sides inward; optional flats levelled within a tolerance; rivers from inlet to outlet (or to a lake); outcrops and cave mouths.
  - **Roads:** joining every road opening and flat, graded to 3/8 m a cell. Bridges with their mounts.
  - **Navigation:** a navigation mesh, with its report that every opening and flat is reached.
  - **Hand-offs:** the boundary's hull, and the plotter's nature ground.
  - **Drawing:** the 13-point fans, fused corners, walls, and water.
  - **Presets:** 7 shapes and 4 flats. Native matches WebAssembly.
- [ ] **Composed views.** A pipeline's view composes what its draw asks for (`draw.nature`, `draw.surround`, `draw.figures`): the terrain inside its boundary with a plotter's fill and the boundary's figures, each turned off in the inspector's Around it; `pipeline_view` takes `compose` and `on_ground`.
- [ ] **In the runtime.** The terrain spawns natively; `ground.h` reads its cells, heights, roads, bridges, navigation mesh, and hull (`foundry_ground_*`); `foundry_<pipeline>_<param>_find` names enum values; `tests/ground_test.cpp` spawns the river valley, hands its hull to a boundary, and fills its figures.
- [ ] **Measured.** `--plan terrain` for `terrain`; both books with numbers from runs.

**Questions from the agent** (terrains and figures; built and in draft, nothing is committed)
1. **A channel patched after approval, and a C struct changed.**
   - `terrain_mesh_spec`'s `season` gained its enum names, as `boundary_terrain_spec`'s has.
   - `foundry_hull_cell` gained `opening` and `opening_width`, so a game's hull can carry a road or river out.

   Are those all right?
2. **Changes outside the pipelines.**
   - **Composer:** the generic composer (`draw.nature`, `draw.surround`, `draw.figures`). Its plotter fill grows through `plot::grow`, moved out of the planner view; it now builds each distinct placement once.
   - **Facts:** a nested channel's scalars become facts (`ground.openings_unreached`).
   - **`pipeline_view`:** gains `compose` and `on_ground`.
   - **The config editor:** edits nested structs and lists of structs, and a pipeline's config parameter in place.
   - **The generator:** binds a list of config parameters (the two flats) and writes enum lookups by name.

   Are those all right?
3. **Collision is convex columns.** `physics_shape` has no triangle mesh, and it is committed, so the ground collides as one convex prism per span (4,016 shapes for the river valley). Should `physics_shape` fork to add a triangle mesh (every pipeline's collision depends on it), or are columns right for hex ground?
4. **Bridges are mounts only.** The terrain reports each bridge's two mounts and width, but nothing stands a bridge on them. Should `road`'s bridge kind take a span, or should a bridge pipeline (spans of 2 to 20 m, by the road's kind) come next?
5. **Flats at different levels.** Where two flats meet at different levels, the road between them can't keep its grade (0.36 a cell in the river valley with its town and village), and the report says so. Should the builder ramp the later flat toward the earlier one, refuse overlapping flats, or leave it to the planner?
6. **How many figures.** At 100 m the river valley's boundary leaves 736 figures, which the live plotter spawns as real trees, and the foundry builds them at low detail (85 distinct builds). Is 100 m the right default, or should figures thin with distance, with the boundary drawing its own trees from, say, 50 m?

### Maps: the regional executor (`design/map.md`; `book/map.md`)

Asked for on 2026-10-15: a regional executor that assembles the pieces:
- **The plan:** it holds a high-level plan and gives the agent ways to partition the map.
- **The conversation:** it assembles a region from the human's feedback, talking through a game map with several towns, biomes by plotters, and a network joining towns to each other and to points of interest (a lone house in the middle of nowhere, a castle to raid).
- **Purpose:** it is the map a player engages with, and it will keep evolving.
- **Scale and workflow:** about 3 km across, talked through in chat with the agent driving.

Also decided that day:
- no `v1` in names anywhere;
- no approval gates: changes are reported here and in the summaries, and only Meshy spending and commits ask (`design/spine.md`, "Changing in place").

- [ ] **No versions.** Pipelines, systems, channels, plotters, planners, config tables, books, and the C API renamed without `_v1` (generated pipeline code as `src/library/pipeline_<name>.*`). Systems and channels change in place; the forge refuses nothing as final.
- [ ] **The plan and its atlas.** `maps/<name>.json` with its shape, biome, seed, parcels, sites, links, and rivers. `map_plan`: an 8 m grid of parcels and blended relief, lakes and marshes, flats, rivers carved, links routed and graded (bridges, causeways), chunks, and the boundary's hull; native matches WebAssembly. The atlas, and `map_list`, `map_get`, `map_set`, `map_plan`, and `map_view`.
- [ ] **Chunks of ground.** `map_ground`: a chunk's hexbound columns as a function of world place (seams checked), its nature by parcel, its navigation mesh; `terrain_mesh` draws it, leaving its apron. The `map_chunk` and `map_far` pipelines, each spawnable from packed bytes.
- [ ] **Live.** The native runtime runs a map as a game would: the far land and boundary, chunks streamed round the camera, each parcel's plotter on them, settlements' fills and points of interest spawned near; Play walks it. `map_look` for the agent's eye.
- [ ] **Biomes.** The plotters `oakwood`, `pine_woods`, `moor`, `fen`, and `shore`.
- [ ] **The realm.** A first map: a market town, two villages, and a hamlet, a keep, a farm, a hermit's cottage, a ruined watchtower, and a bandit camp, joined by roads, lanes, and paths, under a northern range with a lake, a fen, and the sea.

- [ ] **Moving round a map.** Asked for on 2026-10-15 ("better camera controls... a fly enable button while playing"): a fly speed, a walk speed, and F to switch between flying and walking where the camera or the walker is.
- [ ] **Videos of a map.** Asked for on 2026-10-15 ("an automated video to be able to record"): `map_video` tours (sites, a road, camera keys) and the viewport's Record, into `qa/videos/`.
- [ ] **Only the terrain draws the ground.** Asked for on 2026-10-15 ("only the terrain controls the dirt while plotters only control what is on the hex"): on a map, grass has no bed, a town's streets are the ground's materials, and nothing grows on what a town built.
- [ ] **Seeing a map and a terrain clearly.** Asked for on 2026-10-15 ("the camera is a bit messed up, as I'm not seeing stuff"; "I don't want the boundary show on the terrain pipeline"; "visually separate out parameters that go into the system versus parameters that feed the visualization"; "increase the max zoom"; "the plotter maps onto the terrain"):
  - each map's boundary walls in its whole region, so a parcel's plotter grows on the map's own ground;
  - the terrain view leaves the boundary off by default;
  - the inspector frames **Into the systems** (each parameter marked with the stages it feeds) apart from **Only the view**;
  - views zoom out 50 times farther than they did (orbit views to 0.02, live views to 0.01), and Play's camera stands up to 60 m behind.

**Changes, reported** (made without asking, as decided on 2026-10-15)
- **New channels:** `map_spec`, `map_parcel`, `map_site`, `map_link`, `map_river`, `map_plan`, `map_segment`, `map_flat`, `map_place`, `map_chunk`, and `map_far_spec`: the executor's plan, its macro plan, a chunk's spec, and the far land's input.
- **Changed channels:**
  - `terrain_ground` gained `nature_parcels` (which plotter grows each region) and an apron flag (512).
  - `terrain_shape` allows map-sized shapes (radius to 2,000 cells, sides to 4,000, wobble to 60).
- **Changed for the ground's ownership** (2026-10-15):
  - `grass_spec.bed` (own, none) and the grass pipeline's `bed`;
  - `road_spec.paving` (own, none) and the road pipeline's `paving`;
  - `map_flat.built`, `streets`, and `street_materials`, filled by the host from each settlement's plan;
  - `terrain_ground`'s flag 1024 (built).
- **The walker:** `Input.speed`, and `tidy()`, which the live map calls every frame.
- **The runtime's API:**
  - `foundry_ground_nature` (a built ground's regions by parcel);
  - `foundry_diag_spawn` (a pipeline by name, with parameters as text);
  - `foundry_<pipeline>_spawn_packed` for pipelines with `draw.payload`.
- **The generator:**
  - payloads;
  - a stage bound to a whole config parameter;
  - a registry of pipelines by name;
  - nature regions copied into a build's ground.
- **The host:**
  - `shape.h` fills by scanline, finding sides only for border cells (the same cells, much faster);
  - `shape.h` keys the hull's corners on an integer lattice (2026-10-15). Corners rounded from floats broke the ring far from the origin, so a 3 km map's hull was a fragment (298 of 8,106 cells on scratch) and its boundary covered the region. All three copies changed; `terrain`, `boundary`, and `map_plan` match native;
  - `map_plan` (the tool) takes `hull` to return the boundary's hull;
  - a pipeline's `draw.surround` takes `on` (false: composed only when the inspector turns it on);
  - `plot::hand_sky` is shared;
  - the walker clamps its rays and re-optimizes Jolt's broadphase when bodies change.

**Questions from the agent** (maps; built and in draft, nothing is committed)
1. **The game's API.** The host streams the map today, through the runtime's API and its diagnostics' spawn by name. Should maps be compiled into the runtime as plotters are, with `foundry_map_spawn` streaming round a focus the game moves and queries for sites, roads, and the ground?
2. **Towns meeting their roads.** A town plans its own streets on flat ground, and the map's roads stop at its edge. Should the planner take the directions of the map's links as its entrances, so roads run into its gates?
3. **Walls in the live map.** Walls and fences are whole networks given inline, so the live map leaves them out. Should spawning by name take an inline entry (packed, as chunks are), or should walls be cut into pieces a name can carry?
4. **Distant forests.** Past 210 m a wood is only painted on the far land, so its edge shows. Should the far land carry a canopy (cut away where the chunks are), or should woods' figures stand farther out as a boundary's do?
5. **Partitioning.** Parcels are seeds with a reach, and lakes fill their whole parcel. Is that a good way to talk a map through, or do you want parcels drawn as outlines (or painted cells)?

## Not in this milestone

To keep the milestone focused, these wait even though they are designed: trees and other nature, crafted things, buildings, humanoids, creatures, magic, auras, sound, shadows, the sleep and wake engine beyond what static meshes need, and recording and replay.

# Future milestones


- **Settlements: the planner** (`design/planner.md`). Planners as a workspace primitive and `plan_settle` with the `hamlet`, `street_village`, and `market_town` scenarios first. Plans in the runtime with plots, mounts, streets, and the navigation mesh checked for every tier; the planner view with a preview fill from `house`; and a book with numbers from runs. Then the keep, the walled town, the river town, and cities planned by district.
- **The rest of the plotter design** (`design/plotter.md`), after the first steps above: active entities (players and creatures, by handle, with collision masks) and sensors with contacts and distances; full instance results (event flags, sounds playing, sleep, ready) for every pipeline; footprints and collision descriptions on bushes, rocks, and flowers and on the conformance files; regrow kinds (succession); and more plotters, one per biome (a cypress swamp first, since its species exist).


# Suggestions are great
Suggestions from the agent, in a proposed order. Each builds on the one before and ends with something the human can play test and approve. They are not in scope until moved into the current milestone.

1. **Static things, for real.** The full static-thing pipeline (`design/systems.md`): concept images, image to 3D, remesh, resize to a conformance shape, bake to a disk format. The fixed asset library with its two-way conformance index (`design/flow.md`). Provenance sidecars. Play test views: explore and battle cameras, the silhouette thumbnail, and the lineup against the hero and the hex (`design/playtest.md`).
2. **The engine core.** The submit queue, shared buffers, instancing, and the first version of the mega shader with `style.md` §6 (cel bands, rim, ink), with its parameters live on the control pad. Sleep and wake, with the active set visible in the workspace (`design/submit.md`).
3. **Interactive things.** The chest (a hinge driven by game-owned progress) and the rock (a break interaction path), proving the per-instance state machine, the gamepad control pad, and interaction paths. Recording and replay, and the wall of recordings for bulk review.
4. **The native build and the C API.** The merged native runtime, `runtime.h` assembled from fragments, the build checks from `ORG.md` (namespaces, private systems, final-system isolation), and a minimal sample game that spawns and drives instances through the API alone. Replay recordings against both builds and compare.
5. **Nature.** Trees (skeleton, body, coloring, wind simulator, break) and the quality-slider caching that collapses variants.
6. **Buildings.** Box conformance pieces with damage stages, doodad mounts, and the room-combination tests.
7. **Light, shadow, and transparency.** Sun and marked-light shadow maps with the shadow test scene, the transparent and additive passes, and the first magic effects and auras.
8. **Sound.** Sound events, per-pipeline tables, and the sound manager on SDL3 audio.
9. **Humanoids.** Likely several milestones: skeleton and animation, body mesh, skin, face, armor and mounts, hands, sensors and head rings.
10. **Creatures and bosses.** Locomotion mixtures, then bosses assembled from the pipelines above.
