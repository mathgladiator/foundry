# CLAUDE.md

Guidance for agents working in this repository.

## What this project is

The foundry is an AI-driven harness for building game assets as versioned pipelines of C++ systems, with a human approving every step through play testing. Start with `README.md`, then `design/spine.md`, which is the core architecture. The design documents in `design/` are the source of truth.

## Scope

**`PLAN.md` sets scope.** Work only on the current milestone's TODOs. Designed features outside it (trees, humanoids, sound, shadows, and so on) wait, even when the design for them is complete.

- Don't build ahead. If something outside the milestone seems needed, write it as a suggestion under "Future milestones" in `PLAN.md` instead of building it.
- Make as much progress as you can, then write questions under "Questions from the agent" in the current milestone. The human answers inline there.
- When the human answers questions (in `PLAN.md` or in a design document), fold each answer into the document's text and remove the answered question.
- Don't change scope, rename milestones, or tick TODO boxes; the human validates the TODOs.

## Design documents

| Document | Read it when |
|---|---|
| `design/spine.md` | Always. Vocabulary, conformance, versioning, builds, caching. |
| `ORG.md` | Always, before adding or moving source files. The source tree, dependency rules, and build checks. |
| `design/flow.md` | Laying out files: systems, channels, packers, disk formats, manifests, Meshy items, fixed assets. |
| `design/transforms.md` | Working on the asset pipeline: transforms vs. runtime systems, mesh items, LODs, stamping. |
| `design/submit.md` | Working on the engine: update, sleep, rendering, the mega shader. |
| `design/api.md` | Working on the game-facing C API. |
| `design/playtest.md` | Working on play testing, the control pad, or approval. |
| `design/quality.perf.md` | Working on the flattened runtime's stress tests (diversity, forest caching, bulk events, soak leaks) and the committed reports in `perf/`. |
| `design/systems.md` | Building a specific pipeline. |
| `design/plotter.md` | Working on regions of hexes and the plotter rules that fill them (the design; its first steps are in `PLAN.md`). |
| `design/observatory.md` | Working on the sky: observatories, the sun, moons, stars, clouds, aurora, the light, and shadow maps. |
| `design/planner.md` | Working on settlements: planners that lay out villages, towns, castles, and cities as plots (buildings, civic buildings, centres, castle parts, road segments, waterfronts, water) in the settlement's own shape, walls and fences as tiles and lines, with open land for a plotter and a navigation mesh; and the systems that fill them (roads, civic buildings, castles, walls, fences, gates). A first version runs in the foundry (`book/planners.md`); the runtime API isn't built. |
| `design/boundary.md` | Working on boundaries: walling a playable region in while the world seems to go on (hulls, edges, the invented terrain, its quadtree and images, the walls). |
| `design/terrain.md` | Working on terrains: the ground of a playable region built inside a shape whose sides are given (aspects, openings for roads and rivers, flats for a planner), on the hexbound ground model (`design/import.hexbound.world.md`: columns of spans, cliffs by fusion, the 13-point fan), with rivers, roads planned on a navigation mesh, bridges, caves, and the hull a boundary takes. |
| `design/sounds.md` | Working on sound events. |
| `design/style.md` | Generating or judging how assets look. It is the current project's style and is opt-in. |
| `design/third.party.md` | Calling Meshy or other outside services. |
| `design/future.md` | Never as a source of work; these ideas are deliberately deferred. |
| `book/$pipeline.md` | Using or changing a pipeline: what it does, its parameters, systems, channels, genomes, flow, performance, and QA. |

The documents describe the foundry at maximum expression. Implement only what the current milestone needs.

## Documentation

Every pipeline has a book: `book/$pipeline.md` (`book/tree_v1.md` is the first). It explains the pipeline to a person who will use or change it: what the game asks for (its parameters and the generated C API), how each system builds its part, the channels between them, its genomes or other data, how information flows and where caching holds (`pipeline_flow`), its measured performance (`perf_stress`), how to QA it (sheets, the QA video), and its known limits. Writing the book is part of building a pipeline, not an afterthought:

- Create it with the pipeline, and update it in the same change as any system, channel, genome, or parameter it describes.
- Every number in it comes from a run (a stress report, a probe, a sheet), never from memory.
- Ask for a pipeline's approval only once its book is current.

## Rules from the design

These are invariants. Breaking one is a design change, which needs the human's approval.

- **Never edit a final system.** A system version marked final in its manifest is immutable. To change it, fork: create `src/systems/$name_$newversion/`. A git check fails any change inside a final system's directory (`ORG.md`).
- **Channels are contracts.** Creating or upgrading a channel needs the human's approval, with a written reason. An upgrade invalidates everything that depends on the old channel.
- **Don't hand-write generated code.** Packers and disk formats are generated by the host into `src/generated/`; never edit them.
- **No system escapes the runtime.** Systems are private. The C API exposes pipelines, their parameters, and macro-level configuration only; never a system, system version, or channel type.
- **Never edit `runtime.h` by hand.** It is assembled from the fragment headers in `src/library/`.
- **Respect the dependency rules** in `ORG.md`: the runtime never depends on the host, and systems depend only on channels and `src/common/`.
- **Systems are pure.** A system takes one channel in and puts one channel out. It never reads a clock, a global random generator, or any other hidden input; time and seeds arrive through its input channel. The same inputs must always give the same output.
- **Meshy and other services run at build time only.** Nothing in a runtime system calls an outside service.
- **Record provenance.** Every generated file gets a JSON sidecar with the service, model version, prompt, inputs, cost, date, and license.
- **Download Meshy results immediately.** Non-Enterprise API results are deleted after 3 days.
- **Spend carefully.** Meshy calls cost credits. Stay within the budget under "Decisions" below, and ask before generating in bulk.

## Technology

- **Languages:** C++ for the host, runtime, and systems; a plain C API for games. Shaders in HLSL, compiled for each backend with `SDL_shadercross`.
- **Platform:** SDL3, with SDL3's GPU API (Vulkan, Metal, Direct3D 12). The web is not a target.
- **Two builds:** systems compile to WebAssembly for development (one sandbox per system, inside the host), and the same sources compile natively into one merged runtime for games.
- **Math types:** plain `float[16]` (column-major) and `float[3]` at boundaries. Don't introduce wrapper math types in the API.
- **Data:** disk formats are little-endian, laid out like memory, with a header naming the channel type and version. Pipelines and manifests are JSON.
- **The agent's interface:** the foundry runs an MCP server, and everything a human can do in a play test, the agent can do through it.
- **Tools:** Dear ImGui (SDL3 GPU backend) for the workspace UI, CMake with `wasi-sdk` for WebAssembly builds, Wasmtime (C API) to host systems, and MCP over HTTP on localhost. See "Decisions" below.

## Decisions

Settled by the human (recorded in `reports/2026-10-10-1.md`):

| Decision | Choice |
|---|---|
| UI toolkit | Dear ImGui, on its SDL3 GPU backend |
| Build system | CMake, with two toolchains: the native compiler for the host and static libraries, and `wasi-sdk` (Clang targeting WebAssembly) for systems |
| WebAssembly host runtime | Wasmtime, through its C API |
| MCP transport | HTTP on localhost; Claude Code connects with `claude mcp add --transport http` |
| Physics engine | Jolt Physics, a static library linked beside the runtime (and into the host). There is one Jolt world, owned by the game, which hands the runtime a blind pointer to it (tagged with Jolt's version, which the runtime checks), with a purpose-to-layer-and-mask map; the runtime keeps every instance's body and sensors in it, and the game moves its players and creatures through it, registering them with the runtime (by handle, with an external ID and a collision mask) so contacts are reported and attributed. The host and tools build their own toy worlds. It is small-p physics, a baseline rather than a sandbox. Each build outputs a static collision description (a channel of Jolt's shapes), the boundary between a pipeline and physics (decided in the `design/plotter.md` review, 2026-10-12). Jolt v5.6.0 comes through CMake's FetchContent like the other source libraries |
| Meshy budget | 250 credits without asking. At current prices that is roughly 6 to 8 static meshes taken from concept image to remeshed 3D, so start with one or two and ask before generating in bulk. The ledger (`demo/meshy/ledger.json`) tracks the spend. |

## Building and running

```sh
tools/bootstrap.sh                      # once: wasi-sdk, Wasmtime, and (if missing) X11 headers into .deps/
cmake --preset linux-release            # configure into build/linux-release
cmake --build --preset linux-release    # build the host, every system (WebAssembly + native), and tests
(cd build/linux-release && ctest)       # run the tests
build/linux-release/foundry             # the workspace UI on demo/, with MCP at http://127.0.0.1:7878/mcp
build/linux-release/foundry --headless  # MCP only, no window
```

- Project data lives in the workspace root, `demo/` (`--workspace` picks another): `foundry.json`, `pipelines/`, `conformance/`, `meshy/`, and `fixed/`. Source code stays at the repo root.
- After changing a channel schema in `src/channels/`, run `tools/gen_channels.py`; the build fails if `src/generated/` is stale.
- Channels are named `$family_v$version_$feature` (`tree_v1_rig`, `tree_v1_genome`): the schema gives `family`, `version`, and `feature`, never a `name` (`design/flow.md`, "Naming"). Systems keep `$name_v$version` (`tree_rig_v1`).
- Connect Claude Code to a running foundry with `claude mcp add --transport http foundry http://127.0.0.1:7878/mcp`.
- Prefer the MCP tools for system work (`system_create`, `system_build`, `system_run`, `view_screenshot`, `ui_screenshot`): they keep manifests consistent and show the human what you're doing in the activity log.
- Static things go through Meshy mesh items (`design/transforms.md`): `meshy_text_to_image` makes `<name>_concept`; once the human approves it (`approval_wait`), `meshy_image_to_3d` makes `<name>_mesh`. Size it with `mesh_transform` (fit against a conformance shape) and check `mesh_info` until it's grounded, centred, and sized right; then `mesh_recommend` builds the progressive mesh, recommends LOD0/1/2 by image error, and benchmarks each LOD. Use `mesh_lod_preview` for snippets, and `mesh_regenerate_lod` to change one LOD's error limit (limits always stack: the others are pushed out of its way, and only the LODs that changed are rebaked and re-benchmarked). Each LOD also gets a switch distance: the first distance at which fewer than the item's switch threshold (default 10%; `mesh_switch_threshold` sets it) of its pixels visibly disagree with the full mesh. `view_screenshot` takes `distance` to view an asset from that far with the game camera. Approving the mesh item stamps its LODs into `fixed/<name>/`.
- While a system or channel is a draft and not committed, patch it in place; don't create a `_v2` for work in progress. Fork only from a final or committed version.
- Pipelines run in the foundry (`design/flow.md`, "Pipelines"): `pipeline_get` describes one, `pipeline_set` drives the live instance the human sees (and `verify_native` compares every stage with its native build), `pipeline_view` renders one configuration, and `pipeline_sheet` renders a contact sheet across two parameters at one scale. Use sheets to check every species, age, and season before asking for approval.
- Systems must draw random numbers one per statement: function argument order is unspecified, and GCC (native) and Clang (WebAssembly) differ, so `v3(rng(), rng(), rng())` breaks the native match.
- The native runtime is generated: after changing a pipeline (or its genomes), run `tools/gen_pipelines.py`; the build fails if `src/library/` or `src/generated/stress/` is stale. `perf_stress` stress-tests a pipeline in it (`design/quality.perf.md`): `quick` after a change, `standard` before asking for approval. Each run writes `perf/report-$pipeline-$timestamp.json` (committed); pass `no_report` for scratch runs. Run the binary directly (`build/linux-release/stress/stress_tree_v1 --plan quick --no-report --snapshot run`) to see PNGs of what it measured.
- Pipeline data such as species lives in config tables (`config/$table/`, e.g. `tree_v1_genomes`; `design/flow.md`, "Config"), not in the host: the host knows no pipeline, and its config editor is generic (the channel schema gives the fields, `table.json` the groups and colours). Tune entries through MCP: `config_get` (with `schema` for every field's range) and `config_set` (`from` to create an entry from the closest one). Each change is checked against the channel, rebuilds the live instance, and regenerates the native runtime's compiled-in config. Photograph the result with `pipeline_sheet` and `pipeline_view` (including from below and the side), and compare it with the entry's reference photos (`reference/`, linked with `config_set` `reference`; only public-domain or CC0 photos, each with a provenance sidecar), before calling it done. `workspace_select` with `reference: true` shows them in the viewport's corner.
- `pipeline_flow` shows how parameters reach each stage, each stage's cache key, and how many distinct outputs it can have; with `probe` it shows which declared dependencies actually change an output. The viewport's Flow tab draws it (`workspace_select` takes `tab`: view, flow, or video).
- `pipeline_video` exports a QA video of every variation of a pipeline into `qa/videos/` (H.264 MP4, through the system's `ffmpeg`); the viewport's Video tab does the same with pins and settings.
- `build/linux-release/stress/stress_<pipeline> --plan explore --window` opens a forest to fly through (Tab or the right mouse button flies, B fires events where you look).
- Plotters (`design/plotter.md`) live in the workspace's `plotters/`: a plot system bound to a biome config. `plotter_get` loads one (measuring any kind's missing footprint from its pipeline and writing it back), `plotter_set` gives its region a ground (a preset such as `glen`, `hex`, `pond`, `path`, or prioritized hex regions) and a seed, `plotter_grow` grows it (seed or regrow mode, `verify_native`), `plotter_clear` clears or fells, and `plotter_view` renders it (`collision` shows collision and footprints). Selecting one in the workspace shows the same region. `build/linux-release/stress/stress_plotter_<name>` is its region run; `plotter_perf` (or `--plan scale`) is its scale plan, frame rates at several sizes in the native runtime with a ladder of the runtime's cost decisions, what they cost in looks, and (`find`, `--find 60`) the most trees at a frame rate, which the plotter's inspector summarizes (`design/quality.perf.md`, "Scale"; `CURRENT.FPS.md`).
- The runtime decides what to compute and draw (`design/submit.md`, "Cost"): each instance's level of detail by its size on screen, posing only what is seen, as often as it shows. A pipeline's `detail` is the finest the game allows. Its switch sizes are measured, not guessed: after changing a pipeline's levels of detail, run `build/linux-release/stress/stress_<pipeline> --lod demo/perf/report-<pipeline>-detail-<stamp>.json` (`--snapshot DIR` for the differences as PNGs), put its `detail_pixels` in the pipeline's `draw.detail_pixels`, and regenerate. Coarser levels must keep the same bones (the runtime checks, and won't draw a level whose pose differs).
- Grass is a field (`design/plotter.md`, "Fields"; `book/grass_v1.md`): a pipeline whose per-cell stage (a pure functor of `q`, `r`, ground, moisture, and seed) picks one of a few shared tiles per hex cell; the runtime draws a region's grass as one instance, by chunk, swayed in the vertex shader. A plotter lists its fields under `fields`. A plotter's viewport is **Live** by default: the region in the native runtime (as last built), with its fields, frame rate, and fly camera.
- Village buildings are `house_v1` (`book/house_v1.md`): a type from `config/house_v1_types/` (cottage, farmhouse, inn, general_store, bakery, smithy, carpenter, tailor, alchemist, mill), a stage (construction, basic, deluxe, premium) on one footprint, a season, a household (solo, family, extended), which sizes the lot, the type's usual or alternate lot shape (wide, deep, round, long, broad: `rect5x4` and `flower19` for one person up to `rect10x6` for several generations), and a palette from `config/house_v1_palettes/` mixed over the type's colours. Its model carries the footprint's cells, the lot's box (a second conformance), every lot as guides (outlined on the ground at a fixed 20 m framing, `draw.frame_m`), the doorway, and the command view's marker. Check footprints and native matches across every combination before calling a change done, as the book's "Checked" does.
- Planners (`design/planner.md`; `book/planners.md`) live in the workspace's `planners/`: a plan system (`plan_settle_v1`) bound to a scenario's config, with lots measured from pipelines (houses by household and shape; waterfronts, civic buildings and centres, castle parts by size) and a preview fill (buildings, civics, castles, roads, waterfronts, and lines: the line system and each line kind's wall, fence, and gate pipelines), all named in the file. A plan is the settlement's own shape and its plots (buildings, road segments with each cell's mask, half, flow, fence edges, and door edges, waterfronts, water), its wall and gate tiles, and its lines (walls hugging what's built as lattice hexagons, the keep's wall, fenced yards, as networks); the rest is open land, which the planner view grows with a plotter (`nature`). A plan is a draft: the view builds only what those systems build (no stand-ins). `planner_get`, `planner_set` (seed, plots, tier, fill, lines, nature, config fields to try, `save`), `planner_plan` (`verify_native`; its report: every plot reached from every entrance, every trade by cart), and `planner_view`. Check every planner's report and native match after changing the plan system or the lots.
- Roads are `road_v1` (`book/road_v1.md`): one cell, a kind from `config/road_v1_kinds/`, a 6-edge neighbour mask, a half (an edge piece in at most half the cell, never closing its flow), and fence edges (turn cells to their smallest turn to share builds). A road builds only what stands at its edges: never a fence or a centre. Its viewer draws its neighbours, flow, and half (`draw.guides.lines`), and the inspector edits a mask edge by edge (a param's `edges`). Waterfronts are `waterfront_v1` (`book/waterfront_v1.md`): a type from `config/waterfront_v1_types/` on a part-land, part-water lot (`draw.footprint.wet`). Civic buildings and centres are `civic_v1` (`book/civic_v1.md`); a keep's ward and a castle's parts are `castle_v1` (`book/castle_v1.md`). Walls and fences are networks on the diagonal lattice (`line_v1_network`: links through hexagons' opposite corners, so lines stay straight and meet at 60 and 120 degrees); `wall_v1` and `fence_v1` build a whole network (or a chunk) as one model, and the example networks in `config/line_v1_networks/` show them (`book/wall_v1.md`, `book/fence_v1.md`). A wall reports its openings (`draw.places`), where `gate_v1` stands, built with the wall's own kind (`book/gate_v1.md`). A config parameter may be given inline as an object (a planner's own network). Houses, civic buildings, and castle parts report their doors' cells (`draw.footprint.doors`); a planner keeps a door's road cell a strict way (road_v1's `doors`). All animate with `house_animate_v1`.
- Village themes (`design/planner.md`, "Themes"): `config/village_v1_themes/` (heartland, druidic, stone_dome, nordic, desert, elven, dwarven, marsh) set roof form, plan, walls, windows, foundation, ornaments, colours, and fires for `house_v1`, `civic_v1`, `castle_v1`, and `waterfront_v1` (their `theme` parameter; a planner shares it through `view.shared`). Each of those systems carries `theme.h`, and every village system carries `condition.h` (ruins, construction steps, fire points); edit the copies in step. Ruins: house `stage: ruin`; civic and castle `stage` (planning, half, finished, ruin); the rest `condition: ruin`.
- Fire (`design/submit.md`, "Fire"): buildings' models carry fire points (`draw.fires`: lit 1, smoke only 2, blaze 0), each naming a `config/fire_v1_configs/` id (compiled in; ids are stable). The runtime's fire system (`src/runtime/fire/`) simulates and draws them; `foundry_set_wind`, `foundry_set_burning`. `pipeline_view` and `pipeline_sheet` take `burning`; every foundry view shows fires as a still.
- Boundaries are `boundary_v1` (`design/boundary.md`; `book/boundary_v1.md`): a hull of cells around a playable region (each with an edge: ledge, ridge, mountain, forest, jungle, sea, marsh; a height; and a rise) walled in by invisible walls while a quadtree of terrain with generated images invents the world out to the view distance (`draw.terrain`; the runtime picks a cut by distance and morphs between levels). Hulls (`config/boundary_v1_hulls/`: predefined shapes, and sector presets for any region's ring) and biomes (`config/boundary_v1_biomes/`). `pipeline_view` takes `eye`, `heading`, `elevation` to stand inside the region. A plotter's `boundary` puts one around its region (`foundry_region_hull`, `foundry_region_spawn_boundary`; the inspector's Boundary section; `plotter_view` takes `boundary` and `eye`); a game gives its own hull with `foundry_boundary_v1_spawn_hull` (a cell may carry an opening, which the boundary carries outward). A hull may name a shape (`terrain_v1_shape`) instead of cells. Within `figure_range` (100 m) its trees are figures for the game to fill (`foundry_instance_figures`, `foundry_region_spawn_figures`); the foundry fills them with tree_v1 and bush_v1. Stress it with `--plan terrain`, not quick. `FOUNDRY_DUMP_MISMATCH=<dir>` keeps a mismatching system's input and both outputs (decode them by the channel's schema).
- Terrains are `terrain_v1` (`design/terrain.md`; `book/terrain_v1.md`): a shape from `config/terrain_v1_shapes/` (corners from a kind, each side's aspect: edge, height, rise; openings: roads, lanes, rivers in and out), optional flats (`config/terrain_v1_flats/`; a town is optional), and relief parameters, built as hexbound columns of spans in 1/8 m (`design/import.hexbound.world.md`), with rivers, graded roads proved on a navigation mesh, bridge mounts, cliffs, and caves. Check the report (`ground.*` facts: nothing unreached, flats within tolerance, the steepest road step) and the native match across shapes and seeds. Its view composes a boundary round its hull, a plotter over its open land, and the boundary's figures (`draw.surround`, `draw.nature`, `draw.figures`; `src/host/pipeline/compose.*`; the inspector's Around it). A config parameter's **edit** button edits its entry in place (a shape's sides, one by one). `shape.h` is shared by `terrain_relief_v1` and `boundary_rim_v1`: edit the copies in step. The C API reads a built ground and a boundary's figures (`ground.h`: `foundry_ground_*`, `foundry_instance_figures`; `foundry_<pipeline>_<param>_find` turns a name into a value).
- Crops are `crop_v1` (`book/crop_v1.md`): a field on a lot, its plants on `wind_groups` drift bones. A planner's `config.crop_fields`, `lots.field`, and `fill.crops` place and sow field plots.
- The look (`design/submit.md`, "The look"): the game's per-pixel grade (colour vision, contrast, saturation, edges, greyscale or tint), `foundry_set_look` in the runtime and `look_set` / the viewport's **Look** button in the foundry; every view and screenshot uses it. Reset it (`look_set` with `reset`) after judging under a look.
- Observatories (`design/observatory.md`) live in the workspace's `observatories/`: a planet's sky as data, not versioned (`earth`, `mars`). Selecting one shows its sundial under its sky; its inspector sets the time of day, day of year, latitude, clouds, aurora, shadows, and which constellations are lit or connected. `observatory_get`, `observatory_set` (`view` puts an item's viewer under it; the viewer's Sky checkbox does the same), and `observatory_view` (orbit, or `look` up). `pipeline_view` and `plotter_view` take `observatory`. The light every mesh shader shares is `src/runtime/shaders/light.hlsli`; without a sky it is the classic fixed light.
- 3D views pan (shift-drag or right-drag, ctrl-drag for height) and take a gamepad. **Play** walks the hero's capsule through the scene on Jolt's character controller (`src/host/physics/walker.h`; `design/playtest.md`, "Moving around"). In the live plotter it walks in the Jolt world the runtime keeps the trunks in. It isn't on MCP yet.
- The viewport draws on the GPU when the foundry has a window (`src/host/viewport/gpu.h`), and on the CPU headless; `plotter_view` says which (`renderer`) and takes `cpu: true` to compare.
- Models carry their collision (`physics_v1_body`: Jolt shapes with a purpose). `pipeline_set` reports it under the model's facts (`collision`: whether Jolt took it, and whether it stays inside the conformance), and the View tab draws it.
- Fixed assets break through fractures (`design/transforms.md`, "Fracture"): `fracture_preview` cuts the asset's LOD1 with every algorithm (voronoi, impact, radial, slab, splinter) and recommends one; `fracture_generate` cuts and bakes the break into `fixed/<name>/fracture.bin` as a draft `fracture:<name>` item; `fracture_view` shows it at moments of the break. Look at the cut and the break before asking for approval.
- When you need the human to approve something, call `approval_wait` on it instead of asking them to tell you; it returns as soon as they approve or reject, with any reason they gave.
- Systems use `foundry::det` math from `boundary.h` (not `<cmath>` transcendentals), so the native build stays byte-identical to WebAssembly. `system_run` reports whether they match.
- To keep the foundry running while you rebuild, run a copy (`cp build/linux-release/foundry build/linux-release/foundry-run`): relinking the binary under a running process crashes it.
- Never run `pkill -f` with a pattern that appears in your own command line; it kills your shell. Use `pkill -x foundry-run`. Process names are cut to 15 characters, so keep copies' names short.

## Working style

- Prefer the simplest thing that satisfies the current milestone.
- Play test your own work through MCP before asking the human to approve it.
- Report honestly: if a build or test fails, say so with its output.
