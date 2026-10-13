# The Plan

This file is how the project is managed. It holds one current milestone, with TODO items that I, the human, will validate. The agent works on the current milestone; when it's done, I rename it and set the next one.

- **I set scope.** The current milestone's TODOs define what is in scope, so we stay on the QA track and don't wander. Anything I add to the current milestone is in scope; anything else waits.
- **The agent can suggest.** Ideas beyond the current milestone go under "Future milestones" as suggestions. They are not in scope until I move them.
- **Maximum progress, then questions.** The agent makes as much progress as it can, then writes its questions at the end of the current milestone. I answer them inline, so questions and answers stay together.
- **Summaries on request.** As a separate step, I can ask the agent to summarize a finished milestone in a new file and suggest the next current milestone.

# Current milestone: a complex system

- [x] **rocks that break uniquely**
  - `rock_v1` (`book/rock_v1.md`): 8 materials (granite, basalt, sandstone, limestone, slate, marble, obsidian, river stone), each with its own fracture; 4 sizes, moss, and season.
  - Pieces are cut at a lower level of detail when the rock is built, so a break never rebuilds; a closed-form animator throws, bounces, and settles them.
  - To validate: each material whole, mid-break, and settled (Start `break_time` in the View tab), against its reference photos.
- [x] **bushes that pair with the trees**
  - `bush_v1` (`book/bush_v1.md`): 14 species across the trees' biomes, swamp included; several stems from the ground, the trees' wind, flowers and berries by season.
  - Fruit can be picked: the `fruit` parameter applies only to species whose config marks them `pickable`, and is disabled for the rest.
  - To validate: every species by age and season, fruit on and off, and the soak's drain check (RSS stays over tolerance while the heap is flat).
- [x] **flowers that pair with the trees**
  - `flower_v1` (`book/flower_v1.md`): 15 species in clumps, eight head forms, buds, bloom, seed heads, and winter stalks by season, swaying in the trees' wind.
  - To validate: every species by season, against its reference photos and beside the trees of its biome.
- [x] **trees have pickable fruit**

### Jolt, stumps, and the first plotter (`design/plotter.md`)

**Integrate Jolt**
- [ ] **Jolt in the build.** Jolt Physics pinned by tag and fetched by CMake (FetchContent, like the other source libraries), a static library linked beside the runtime and into the host. `runtime.h` names no Jolt type: the game's world arrives as a tagged pointer (tag and Jolt version, checked), with a map from purpose (solid, soft, query-only, sensor) to Jolt layer and mask.
- [ ] **Collision descriptions.** A channel of Jolt shapes that each build outputs, static for the build: a tree's trunk, a bush's soft body, a rock's convex hull, a flower's query-only shape. Native matches WebAssembly.
- [ ] **Bodies by handle.** The runtime adds each instance's body to the world when its build is ready, swaps it on a rebuild, and removes it on destroy. The stress tool's soak checks that bodies balance.
- [ ] **Seen in the foundry.** A toy Jolt world in the host; collision drawn over the model in the View tab; an automated check that collision stays inside the conformance.

**The tree's stump**
- [ ] **Trunks from nature.** `tree_v1`'s genome gets a stump width by species, and the rig reports the footprint of the largest age (hex cells) and the stump's radius now; checked against the reference photos.
- [ ] **Stump mode.** A `stump` boolean on `tree_v1`: the mesh draws only the stump, and its collision becomes the stump's.
- [ ] **Knowing the fall is over.** `break_over` in the tree's update result (a current value), and the changed list in the API, so a game can turn a felled tree into a stump.
- [ ] **Book and checks.** `book/tree_v1.md` updated with numbers from runs; flow, stress, and the QA video cover the stump; native matches WebAssembly.

**Kick-start the plotter**
- [ ] **The primitive.** `plotters/` in the workspace, plot systems as a kind of system, and the `plot_v1_regions` channel (prioritized hex regions with attributes).
- [ ] **`plot_scatter_v1`.** Candidates from cells, spacing per layer (Matérn), footprints, keep-clear, smooth ground, caps per kind, seed and regrow modes, and the volume left. Native matches WebAssembly.
- [ ] **Regions in the runtime.** Create, restore, grow, vacate, set regions, find, destroy; placements spawn as ordinary instances.
- [ ] **The plotter view.** Configure the shape (a hex, a ring, a line, unions, painted cells and attributes), grow step by step or to full, clear and regrow, the shared season and wind, and the metrics; the same through MCP.
- [ ] **A happy glen.** `plotters/happy_glen_v1.json`: a sunny, gentle green place of oaks and apple trees in loose groves, hazel, bramble, and elderberry along its edges, daisies, poppies, bluebells, and foxgloves in the open grass, and a few river stones. It reads as a happy place on every shape tried. It has a book with numbers from runs, and the stress tool's region phase.

  
**Questions from the agent** (Jolt, stumps, and the first plotter; everything above is built and in draft, nothing is committed)
1. **New channels to approve**, each a draft with its reason in its manifest: `physics_v1_shape` and `physics_v1_body` (collision descriptions), and 13 for the plotter: `plot_v1_region`, `regions`, `config`, `layer`, `kind`, `clearance`, `preference`, `occupant`, `exclusion`, `request`, `placement`, `cell`, and `result`. The model channels of all four pipelines gained `collision`, and the tree's spec, rig, body, and model gained `stump`, `trunk_diameter`, and the footprint.
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
2. **Rocks keep the game's level.** Their pieces' break depends on the level, so the runtime can't share a pose across levels. Should the pieces' physics come from the finest level at every level (a change to `rock_body_v1`), so rocks can switch too?
3. **Not built, measured as not needed yet:** impostors, wind on the GPU, and overlapping update with prepare. The next limit is the per-frame scan of every instance in the world (it grows with the world, not the view); culling and classifying by region would remove it. Should that be next?

### Fields, grass, and the live plotter (`design/plotter.md`, "Fields")

Asked for on 2026-10-13: the plotter's view should show the full simulation with its frame rate; radius up to 150; and grass as a lean, state-free functor over a region's cells that a plotter iterates without an instance per blade, stress-tested hard.

- [ ] **The live plotter.** The plotter's viewport runs the region in the native runtime (grown to full, with its fields), uncapped, with its frame rate and what it drew and posed; orbit and fly. The radius goes to 150.
- [ ] **Fields.** A pipeline with `draw.field` covers cells: its cell stage is a pure functor of a cell's integers, ground, moisture, and seed; its tile stage grows one shared cell's worth. The runtime spawns a field as one instance, runs the functor natively, draws by chunk at measured levels, and sways in the vertex shader. Plotters list `fields`; `foundry_region_spawn_fields` spawns them.
- [ ] **`grass_v1`.** Five species (meadow, lawn, prairie, sedge, dune), plates for the other grounds, skirts for steps, four levels measured as a field. Native matches WebAssembly. Its book.
- [ ] **Terrain and wind.** The plotter's ground takes a terrain (flat, hills, ridges, terraces, dunes, mesas, wild; height, hill size, roughness, jitter). The inspector's season and wind reach the live view. Wind headings are the world's, so turned trees bend alike. Grass doesn't colour the ground.
- [ ] **Stressed.** The scale plan spawns fields, and `--only-fields` measures grass alone; `CURRENT.FPS.md` reports the most grass at 60 fps.

**Questions from the agent** (fields, grass, and the live plotter; built and in draft, nothing is committed)
1. **New channels to approve:** `grass_v1_genome`, `grass_v1_spec`, `grass_v1_model` (with `sway`, a new draw convention: how far a vertex moves in a 10 m/s wind), `grass_v1_cell`, and `grass_v1_choice` (the field convention: present, variant, turn, scale, tint). Each has its reason in its manifest.
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

## Not in this milestone

To keep the milestone focused, these wait even though they are designed: trees and other nature, crafted things, buildings, humanoids, creatures, magic, auras, sound, shadows, the sleep and wake engine beyond what static meshes need, and recording and replay.

# Future milestones


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
