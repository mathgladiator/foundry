# Quality and performance

This document describes how the foundry stress-tests a pipeline in the flattened native runtime. Each pipeline gets a stress binary that measures its speed, checks its output against what was approved, and watches its memory over time. The agent runs it through MCP and gets a short report back, and the same binary can later run for days on a cloud machine.

Play testing (`playtest.md`) answers "does it look right?" in the WebAssembly build. Stress testing answers "is it still right, fast, and leak-free once it is native, at scale, for a long time?" WebAssembly is the development box and native is the shipping one (`spine.md`, "The native build"). The stress binary is how we learn that the shipping box keeps the promises the development box made.

## The shape: compiled bindings, JSON plans

The stress tool has three layers:

| Layer | Where | What it does |
|---|---|---|
| The platform | `src/tools/stress/` | Everything that isn't specific to a pipeline: the phases, sampling, scene layout, timing, OS memory, reports, crash capture, and the command line. Handwritten, shared by every pipeline. |
| The binding | `src/generated/stress/stress_$pipeline.c` | Generated from the pipeline JSON: a table of the pipeline's parameters, plus thunks that turn generic values into calls on the pipeline's typed C API. It has no logic. |
| The plan | JSON, passed on the command line | Which phases to run, for how long, at what sizes, with which parameters pinned or narrowed, and the pass thresholds. |

**Why not a single tool driven purely by JSON?** The game-facing API is typed per pipeline (`api.md`): `foundry_tree_spawn` takes a `foundry_tree_params`, and each parameter has its own setter. A generic tool can't call typed functions without a table that says how, and that table is exactly what the generated binding is. So the binding is compiled and mechanical, and everything else is data:

- **Each pipeline gets a stress binary.** It is generated, never written by hand.
- **The plan says what to do.** The binary is fed a JSON plan, so the agent or a cloud runner can vary the test without rebuilding anything.

**The binding goes through `runtime.h`, exactly like a game.** A stress binary is a game whose game loop is a test. It uses only the public C API, plus a diagnostics fragment for counters (see "Diagnostics"). This makes it the first real consumer of the API, and it proves mechanically that the pipeline is drivable without any system escaping the runtime.

A binary can hold several bindings. A later scene might mix forests with rocks, and the platform takes a list of pipelines from the start. The first binaries each bind one pipeline.

### The generated binding

For `pipelines/tree.json`, the generator writes:

```c
/* Generated from pipelines/tree.json by tools/gen_pipelines.py. Do not edit. */
#include "runtime.h"
#include "runtime_diag.h"
#include "stress.h"

static const char *const species_values[] = {"pine", "cherry", "oak", "apple", "hickory",
                                             "osage", "olive", "ebony", "dragonwood", "lignumvitae"};
static const char *const age_values[] = {"sapling", "young", "mature", "ancient"};
/* ... */
static const stress_param params[] = {
    {"species",      STRESS_ENUM,  STRESS_BUILD,  .values = species_values, .count = 10, .fallback = 2},
    {"age",          STRESS_ENUM,  STRESS_BUILD,  .values = age_values, .count = 4, .fallback = 2},
    {"fruit",        STRESS_ENUM,  STRESS_BUILD,  .values = fruit_values, .count = 2, .fallback = 1,
     .when = 0, .when_on = fruit_when},  /* gated: applies where fruit_when[species] is 1 */
    {"seed",         STRESS_U32,   STRESS_BUILD,  .min = 1, .max = 50, .fallback = 1},
    {"wind_speed",   STRESS_F32,   STRESS_UPDATE, .min = 0, .max = 25, .fallback = 6},
    {"time",         STRESS_CLOCK, STRESS_UPDATE},
    {"break_time",   STRESS_EVENT, STRESS_UPDATE},
    /* ... */
};

static foundry_instance spawn(foundry_runtime *rt, const stress_value *v, const float transform[16]) {
    foundry_tree_params p = {
        .species = (foundry_tree_species)v[0].u,
        .age = (foundry_tree_age)v[1].u,
        .seed = v[2].u,
        .wind_speed = v[3].f,
        /* ... */
    };
    return foundry_tree_spawn(rt, &p, transform);
}

static foundry_status set(foundry_runtime *rt, foundry_instance i, int param, const stress_value *v) {
    switch (param) {
    case 3: return foundry_tree_set_wind_speed(rt, i, v->f);
    /* ... one case per settable parameter ... */
    }
    return FOUNDRY_STATUS_BAD_PARAMETER;
}

static foundry_status trigger(foundry_runtime *rt, foundry_instance i, int param) {
    switch (param) {
    case 5: return foundry_tree_start_break(rt, i);
    }
    return FOUNDRY_STATUS_BAD_PARAMETER;
}

static const stress_pipeline tree = {"tree", params, 11, spawn, set, trigger};

int main(int argc, char **argv) {
    return stress_main(argc, argv, &tree, 1);
}
```

The binding needs the following from the pipeline format and the generated API:

- **Parameter kinds.** Each kind maps one-to-one onto a pipeline parameter type:
  - enums, genomes, and numbers are values;
  - the `time` parameter is the instance's clock (`STRESS_CLOCK`), which the runtime advances on its own;
  - an `event` parameter is an event clock (`STRESS_EVENT`), negative until the game starts it, such as `break_time`. The API names its trigger after the parameter without `_time`: `foundry_tree_start_break`.
- **Build or update.** Each parameter is marked as one or the other, using the same rule as the live instance in the foundry. A parameter is "update" when only `"phase": "update"` stages read it. The forest phase depends on this split.
- **Genomes become enums.** A flattened runtime compiles each pipeline's approved genomes in, and the API names them as an enum (`FOUNDRY_TREE_SPECIES_OAK`). So the stress tool treats a genome exactly like any other enum.

## The binary

```sh
stress_tree --describe                       # the parameter table and defaults, as JSON
stress_tree --plan quick                     # run a preset; writes perf/report-tree-<timestamp>.json
stress_tree --plan quick --phase forest --seconds 20   # narrow a plan from the command line
stress_tree --plan quick --no-report --snapshot run    # keep it out of the timeline; save a PNG per step
stress_tree --plan quick --sample configs.json         # the configs diversity draws, for the reference
stress_tree --plan quick --reference reference.json    # compare them with WebAssembly
stress_tree --replay perf/runs/<run>/journal.jsonl --until 18342   # rerun a recorded run up to one action
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Every phase passed. |
| 1 | A threshold failed. |
| 2 | The plan is invalid. |
| 3 | The run crashed. The journal names the last action. |

**Runs are reproducible.** Every random choice comes from the plan's seed, using the PCG32 generator the systems use. Simulated time moves by a fixed `dt` rather than the wall clock. So a run is a pure function of the binary and the plan, timings aside, and any failure can be replayed exactly.

**The journal records a crash.** The binary appends every action to `journal.jsonl` as it goes, flushed at each phase boundary and every second: each spawn with its parameters, each set, trigger, destroy, and quality change. Native code can't contain a crash inside a system, so the journal is the evidence. The tail of the journal is the reproduction, and `--replay` replays it under a debugger. The foundry always runs the binary as a child process, so a crash never takes the foundry down with it.

**It can run with or without a GPU.**
- `"gpu": "auto"` renders off screen at the plan's resolution, on SDL3's GPU API, with no window and no vsync.
- `"gpu": "none"` creates the runtime with no device. Everything runs except submission: generation, caching, update, culling, and drawable building. This is the mode for cloud machines without a GPU.

**GPU time** comes from fences. SDL3's GPU API has no timestamp queries, so the binary measures it two ways. Normal frames keep at most two frames in flight, as a game with a swapchain would, and record how long the CPU waits for the older one (`gpu_wait_ms`). Every tenth frame in the forest and events phases is synchronous instead: everything ahead of it drains, it is submitted, and the binary waits for its fence (`gpu_frame_ms`, the GPU's cost for that frame alone).

**Snapshots.** `--snapshot DIR` (or `run`, for the run directory) writes the off-screen frame as a PNG at the end of each diversity run, each forest step, and 1.5 seconds into each events step, so a person or the agent can see that what was measured was really drawn.

## The window

`--window` draws the run into a window instead of off screen, to feel how a pipeline behaves at scale rather than measure it. Any plan runs in it, and the `explore` plan is made for it: a forest of 2000 instances from 10 sets, running until the window closes.

| Control | Does |
|---|---|
| Tab, or hold the right mouse button | Fly; the mouse looks while the button is held. Tab again (or Escape) returns to the test's own camera |
| W A S D, arrows | Move |
| Space, C | Rise, fall |
| Shift, mouse wheel | Faster; set the speed |
| B | Fire every event on the instances around the point you're looking at (fell the trees) |
| R, N | Regrow the scene; draw new sets |
| - and = | Turn the first continuous update parameter down and up (the wind, for trees) |

The window's title shows the instance count, how many are awake, the update and prepare times, and the last action. A window waits on the display (vsync), so its runs never write to the timeline. It still renders off screen and blits to the window, so `--snapshot` works in it. There is no ground yet: the runtime draws pipelines, and terrain is the game's.

## Regions

A plotter gets a region run instead of the phases above: `stress_plotter_<name>` (generated per plotter; `src/tools/stress/region.cpp`) plays a game that fills its world from regions:
1. **Grow.** A grid of regions (the glen's ground, each with its own world seed; `--regions N` for N x N, `--radius`), grown round-robin at `--batch` placements per frame, every placement spawned into a toy Jolt world. It reports the grow calls' times, the frames, and how long the builds take.
2. **Steady.** The full scene, swaying, for a few seconds: frame times, instances, distinct builds, and bodies (one per thing that collides).
3. **Bomb and regrow.** Per cycle, everything within `--blast` metres of a random point in each region is destroyed and vacated, then regrown in regrow mode over frames: the frame at the blast and the frames to regrow.
4. **Drain.** Every instance and region destroyed: instances, builds, and bodies must all come back to zero, or the run fails.

Reports go to `perf/report-plotter_<name>-<timestamp>.json`.

### Scale

The same binary's scale plan (`--plan scale`, `src/tools/stress/scale.cpp`; `plotter_perf` through MCP, or **Run the scale plan** in the plotter's inspector) asks what frame rate a plotter's world runs at as it grows. Each size is an N x N grid of regions (`--sizes`, default 1, 2, 4, 8, 12, 16), grown to full and spawned with collision bodies, then simulated and drawn for `--frames` frames (240) from two cameras: the overview, which sees the whole scene, and a game camera 12 m up at the scene's centre. Each camera first settles (the coarser levels it asks for are built). Every frame updates, prepares, and draws at 1920 x 1080 with two frames in flight; one frame in ten waits for its own fence to time the GPU alone (on this APU, a GPU waited on alone runs slower than one kept busy, so these times overstate the GPU's share of a steady frame). It records frames per second (from the mean frame), the p95 frame, the update, prepare, and draw times, the update's parts (finished builds, classification, posing, results), the GPU frame, what the runtime drew (instances, trees, triangles, draws, by pipeline and by level of detail, as it reports per instance), how many it posed and why it skipped the rest, and each update stage's CPU time per frame. `--quality` sets the runtime's quality (high by default). The plotter's fields (grass, `plotter.md`, "Fields") are spawned over every region's cells and counted as grass tiles drawn; `--no-fields` leaves them out, and `--only-fields` grows nothing else, to stress grass alone. Three cameras look at each size: the overview from far above, a vista from a corner of the world at game height across all of it, and the game camera.

The largest listed size is then measured as a **ladder**, from both cameras: the runtime's cost decisions (`submit.md`, "Cost") turned on one at a time through `foundry_diag_set_cost`, in the order of the most gain for the least change in looks: none (the game's detail everywhere, everything posed every frame, every pose uploaded every frame), then poses kept on the GPU, then nothing posed out of view, then small things posed less often, then detail by size, then too small not drawn. A last step draws nothing, for the CPU alone. Then **looks**: what the decisions cost on screen, as the share of the pixels that show something that visibly change (a tenth of the colour cube's diagonal) against the scene drawn in full at the same moment. Time stands still for these (a frame with no time step), so poses are exact and only the decision differs; the rate's cost is its stale poses, against the same moment posed fresh. With `--snapshot`, each comparison's picture and its differences are written as PNGs.

With `--find FPS`, the grid then keeps growing (`--find-step`, 4 sides, up to `--max-side`) until the overview drops below that frame rate: the report's `most_trees` is the largest grid that held it, with its trees in view. The headline is the first overview with at least 1,000 trees visible. The plotter's inspector summarizes the newest report; `CURRENT.FPS.md` reads them.

Reports go to `perf/report-plotter_<name>-scale-<timestamp>.json`.

### Detail sizes

`stress_<pipeline> --lod REPORT.json` (`src/tools/stress/lod.cpp`) measures where each coarser level of detail looks like the finest, the way the foundry measures a fixed asset's switch distances (`transforms.md`). For `--lod-configs` sampled configurations (32) at rest, it draws the finest level and each coarser one, then the runtime's far level (`submit.md`, "Cost"), each forced through `foundry_diag_force_level`, at sizes from 8 to 256 pixels (the projected diameter of the bounding sphere in a 1080-pixel-tall view), from a low side view and a raised three-quarter view. Each picture is drawn at four times the size and box-filtered down, so a pixel holds the colour of what it covers, as the eye averages it (one sample per pixel would count every sub-pixel shift of a thin stem as a change). A pixel disagrees when its colour moves by more than a tenth of the colour cube's diagonal. A level's switch size for an allowance is the largest size at which it, and every smaller size, keeps the disagreeing share of the pixels that show the thing within it; across configurations, the median. Each quality has its allowance: high 10% (the foundry's default switch threshold for a fixed asset's levels), medium 20%, low 30%. The sizes go into the pipeline's `draw.detail_pixels` (with the report's name), and `tools/gen_pipelines.py` compiles them into the runtime. A level the runtime can't draw with the instance's own pose (a rock's pieces) is never drawn, and measures as no change. A field pipeline is measured as a field: a patch of 37 cells (a hex of radius 3), each showing the tile its cell chooses, at sizes of one tile (1.5 m, the size the runtime judges a field's chunks by), so a single tile's ground and skirt don't swamp the blades.

## The QA video

`--video OUT.mp4` turns the stress binary into a QA camera: a bulk way to look at a pipeline's whole space. The foundry's Video tab and `pipeline_video` drive it. One video exports at a time, since exports share the GPU and ffmpeg. Each pipeline's Video tab shows its own latest export (progress, then the file or the error), and the others show that they are blocked on the running one, with its progress; `pipeline_video` refuses with the same reason. Pins are kept per pipeline.

- **A title slide.** "Foundry QA", with the subtitle "Pipeline '$pipeline' Demo", for 5 seconds (`--title-seconds`). It also lists the number of variations, the pipeline's state, the date, and its systems.
- **One clip per variation.** About a second each (`--seconds-per`). Every combination of the discrete parameters (enums and genomes) is enumerated, in parameter order. Each combination gets `--floats` random samples of the continuous parameters (numbers and seeds), drawn from the plan's seed. `--fixed` pins parameters and `--ranges` narrows enums, to keep a video short. A gated parameter (`flow.md`, `when`) gets one clip, not one per value, wherever its gate is off, and shows as disabled.
- **What each clip shows.** The camera frames the instance's bounding sphere and orbits a quarter turn while its clocks run (a tree sways). The title shows each discrete value under its parameter's name (so `true` reads as `fruit: true`, and a gated-off value as a grey `disabled`), a panel lists every parameter with the instance's height and triangles, and a counter shows its place. The clear colour is replaced with a sky and a ground at the camera's horizon, so things stand on something.
- **An index.** `OUT.mp4.json` lists every variation's start time and values, with the codec and settings, so any moment in the video can be traced back to its parameters.

**The codec is H.264 in MP4** (libx264, CRF 20, yuv420p, `+faststart`), encoded by piping raw frames to the system's `ffmpeg`:
- **It plays everywhere.** Browsers, every desktop player, and chat and review tools all take it.
- **It's small.** The tree's full 864-variation, 14.5-minute video is 82 MB at 720p, where MJPEG would be several gigabytes.
- **It stays out of the build.** `ffmpeg` runs as its own process, so no encoder (and no GPL code) links into the foundry. If `ffmpeg` lacks libx264, MPEG-4 Part 2 stands in; without `ffmpeg` the export says so and stops.
- **It doesn't slow the export.** Rendering is native and on the GPU, so the full tree video exports in about 2 minutes.

Text is drawn with `stb_truetype` in Roboto, from Dear ImGui's fonts. Videos land in the workspace's `qa/videos/` and are not committed.

## The plan

```json
{
  "pipeline": "tree",
  "seed": 7,
  "quality": "high",
  "gpu": "auto",
  "resolution": [1920, 1080],
  "dt": 0.016667,
  "fixed":  {"season": "summer"},
  "ranges": {"wind_speed": [0, 12]},
  "phases": [
    {"kind": "diversity", "configs": 100, "seconds": 30},
    {"kind": "forest", "sets": 10, "counts": [100, 200, 400, 800, 1600, 3200], "seconds": 10,
     "camera": "overview", "budget_ms": 16.67},
    {"kind": "events", "sets": 10, "counts": [100, 400, 1600], "stagger_seconds": 0, "seconds": 15},
    {"kind": "soak", "minutes": 60, "population": 300, "churn_per_second": 20, "sample_seconds": 5}
  ],
  "thresholds": {"soak_rss_slope_mb_per_hour": 4, "drain_tolerance_mb": 8}
}
```

Pinning and narrowing parameters lets one plan cover a slice of the space:
- **`fixed`** pins a parameter, such as an all-summer forest.
- **`ranges`** narrows a number. For an enum, it takes a list of values.

Anything not mentioned is sampled over the pipeline's whole declared range. Every field has a default, so `{"phases": [{"kind": "diversity"}]}` is a valid plan.

**Preset plans live in the workspace**, under `perf/plans/`:

| Plan | Phases | Use |
|---|---|---|
| `quick` | about 1 minute: diversity (30 s), forest up to 800, events at 100 | The agent's default after a change |
| `standard` | about 10 minutes: diversity (2 min), forest up to 3200 or the budget, events up to 1600, then a 5-minute soak | Before asking for approval |
| `soak` | hours | Overnight, and later in the cloud |

A plan that leaves out `pipeline` works for any pipeline.

## Phases

There are four phases, and none of them knows what a tree is: each works from the binding's parameter table alone, so every pipeline gets all four for free. The platform runs the phases of a plan in order, in one runtime, so later phases also test the state the earlier ones left behind. A plan that wants a clean start can set `"fresh": true` on a phase.

### Diversity: is every corner of the space right?

The diversity phase takes `configs` random parameter sets (100 by default) and runs them for `seconds`.

**Sampling.** Sampling is stratified rather than purely random:
- Every enum value and genome appears at least `floor(configs / count)` times.
- Numbers are drawn across their full range.
- The two ends of every range, and each enum's first and last values, are always included.

With 10 species and 4 ages, 100 configs touch every species-and-age pair at least twice.

**Spawning and running.**
1. **Spawning.** The configs are spawned in waves of 10 onto a grid laid out from the pipeline's conformance.
2. **Running.** Clock parameters run on their own. Every few simulated seconds, the update parameters are re-randomized (the wind changes), and a random fifth of the instances fire each event (some trees break).
3. **Overlap.** Waves overlap, so new instances generate while earlier ones are updating.

**What diversity checks.** Each instance must pass:

| Check | How |
|---|---|
| No errors | No status errors from the API, and no instance in the diagnostics error list |
| Matches what was approved | The hash of each config's draw output, and of its pose at fixed times, equals the WebAssembly reference (see "Reference hashes") |
| Deterministic | A second instance of the same config, spawned later, hashes the same. The same holds for update with 1 thread and with N threads (with the cost decisions off, so both pose every instance every frame) |
| Sane geometry | Every vertex finite and inside the conformance bounds; no degenerate runs of triangles; triangle count within the detail level's budget |
| Sane motion | Every pose matrix finite; no bone leaves the instance's bounds at the highest wind; an event settles (the pipeline reports settled) within its declared time |

**What diversity measures.**
- **Generation.** Latency from spawn to first drawable, with the 50th, 95th, and 99th percentiles and the maximum, plus time per stage.
- **Size.** Triangles and bytes per config.
- **Frame time.** Frame time while generation runs alongside.

Every percentile comes with the config that produced its worst case, so a slow outlier can be reproduced directly.

### Forest: does caching make large scenes cheap?

The forest phase draws `sets` random parameter sets (10 by default) and assigns them round-robin across a scene. It then grows the scene through `counts` (by default 100, 200, 400, and so on, doubling until the frame budget breaks or the list ends), holding each count for `seconds`.

- **Shared and per-instance parameters.** Within a set, the build parameters are identical, so all those instances should share one generated mesh per detail level. Each instance gets its own update parameters: a wind heading that varies smoothly across the scene, and a clock offset. Every instance therefore moves on its own while drawing a shared mesh.
- **Layout.** A hex scatter, spaced from the conformance footprint, with jitter.
- **Camera.** Either `overview` (a high view of the whole scene, the worst case for drawing) or `walk` (a ground-level path through it, which exercises culling, distance LODs, and sleep by distance).
- **Quality collapse.** `"collapse": true` swaps the 10 sets for every instance having its own `seed`. The runtime's quality cap should then collapse them onto the allowed number of distinct trees (`systems.md`, Trees). The phase checks that the generation count equals the cap, not the number of instances.
- **Cache pressure.** A `sets` larger than the cache can hold measures eviction and thrash, rather than the happy path.

**What forest reports, at each count:**
- **Frame time.** The 50th, 95th, and 99th percentiles and the maximum, split into update, drawable building, culling and submission, and the GPU fence wait.
- **Generations.** The number of generations of the game's builds, against the expected `sets`. Anything above that is a caching bug, and the phase fails on it. The runtime's coarser levels of detail are builds of their own, counted apart (`detail_generations`).
- **Cache.** Hits, misses, evictions, and bytes on the CPU and GPU.
- **Drawing.** Draw calls and batches.
- **Instances.** How many are active and how many asleep.
- **Memory per instance.** The change in RSS divided by the change in instances, which should be small and flat once the meshes are shared.

**The headline numbers.**
- **Capacity.** The largest count whose 95th-percentile frame time fits `budget_ms`, refined by bisection, like the mesh benchmark in the `perf` slice.
- **Scaling.** The slope of frame time against instance count. A slope that rises with the count is flagged, because that means something is worse than linear.

### Events: what does it cost when everything happens at once?

An interaction is the expensive moment: a tree breaking runs its toppling simulation, flings its limbs, and has to settle and go back to sleep. The events phase measures that in bulk, for every `event` parameter in the binding (or only the one named by `"event"`). It is generic: it knows an event only as a trigger and a clock, so 100 trees breaking, 100 rocks shattering, and 100 chests opening are the same test.

For each event and each entry in `counts` (100 by default, then larger), it:
1. **Spawns** the instances from `sets` random parameter sets, as the forest does, and lets them run idle until the frame time is steady. That steady frame is the baseline for the spike.
2. **Fires** the event on all of them, either in one frame (`"stagger_seconds": 0`, the worst case) or spread evenly over `stagger_seconds` (a battle).
3. **Runs** until every instance reports that the event has settled, or `seconds` pass.
4. **Destroys** them and checks that memory returns to where it was before the spawn.

**What events checks.**
- **Every instance settles.** An instance that never settles, or never goes back to sleep afterwards, fails the phase and is named with its parameters.
- **Same as alone.** A sample of the instances is hashed at fixed times after the event and compared with the same config fired alone. Firing in bulk must not change any one result.
- **Memory comes back.** Whatever the event allocates (break pieces, the simulation's state) is released once it settles and the instance is destroyed.

**What events reports, at each count:**
- **The spike.** The frame time when the event fires, against the steady frame before it: the first frame, then the 95th percentile and the maximum while events run.
- **The cost while running.** Update time per instance with the event active, against idle.
- **Settling.** The time until the last instance settles, and until the scene's frame time is back to its baseline.
- **Memory.** The peak above the baseline during the event, and the remainder after destroying.

The headline number is the largest count whose spike stays inside `budget_ms`: how many trees the game can fell in the same frame.

### Soak: does it leak?

The soak phase keeps a population (300 by default) alive for `minutes` and churns it continuously:
- destroys and respawns `churn_per_second` instances;
- changes build parameters on some of them, forcing regeneration and eventual cache release;
- changes update parameters on most of them;
- fires events;
- lets instances fall asleep and wakes them;
- changes the quality setting now and then;
- uses stale handles on purpose, expecting an error status and never a crash (`api.md`, "Principles").

It runs at `speed` times real time (unthrottled with no GPU, or rendering every Nth frame with one), so an hour of soak covers days of game time. `spine.md` asks whether a pipeline can "run at 100x speed without crashing or leaking memory", and this is that test.

**Memory is measured at the OS level**, because that is what a player's machine sees:

| Signal | Linux | Windows (later) |
|---|---|---|
| Resident memory | `VmRSS`, `RssAnon` from `/proc/self/status` | `WorkingSetSize` |
| Peak | `VmHWM` | `PeakWorkingSetSize` |
| Committed private | `Private_Dirty` from `/proc/self/smaps_rollup` | `PrivateUsage` |

Every `sample_seconds`, the binary records these alongside the heap in use (glibc's `mallinfo2`) and the runtime's counters: cache entries and bytes, live and awake instances, generations, evictions, and GPU buffers and bytes (see "Diagnostics"). It applies two leak checks:

1. **Drain.** Every `drain_minutes`, the binary destroys every instance, waits out the cache's release timer (`spine.md`, "Caching and GPU resources"), and releases what is left. It then settles the toy Jolt world (rebuilding its broad phase, as a game's physics update does every step; until then Jolt keeps removed bodies' tree nodes), returns freed memory to the OS (`malloc_trim(0)` on glibc), and samples. No drain may grow more than `drain_tolerance_mb` above the first, in either the heap in use or RSS; memory given back is not a failure. The runtime's counters must balance exactly: no instances, cache entries, or bodies remain (in the runtime and in the world), and the live GPU buffers are the same at every drain.
2. **Plateau.** After a warmup (the first 10% of the soak), it fits a line to the RSS samples. The slope must stay under `soak_rss_slope_mb_per_hour`. A bounded workload with a bounded cache has to plateau. Churn makes RSS swing by tens of megabytes from one sample to the next, so a short soak's slope is mostly noise: the slope is always reported, but it only fails a soak of `plateau_min_minutes` (20 by default) or longer. Short soaks rely on the drains, which compare the same state each time.

The OS numbers are the ground truth. The heap in use is exact but process-wide, and the runtime's counters name what the runtime holds. Together they say both whether memory leaks and whose it is.

## Diagnostics

The stress tool needs counters that a game doesn't, and some of them name things a game must never see, such as stage labels. They live in a second header, `runtime_diag.h`. It is compiled only into stress and development builds (`FOUNDRY_DIAGNOSTICS`), and it is never part of `runtime.h`.

| Function | Returns |
|---|---|
| `foundry_diag_counters_get` | Instances (all, awake, waiting for a build, failed); cache entries, pending builds, CPU and GPU bytes, hits, misses, evictions; generations and failures; GPU buffers created and released, and GPU bytes; the last frame's draws, drawn and culled instances, and triangles |
| `foundry_diag_frame_get` | The last frame's CPU time in update, prepare (culling, gathering bones, uploads), and draw |
| `foundry_diag_stages` | Per pipeline stage (`tree.rig`): calls, total and worst time |
| `foundry_diag_instance_get` | One instance: ready, asleep, settled, failed and why (a system's error, or a failed geometry check), triangles, bones, bounds |
| `foundry_diag_hash` | Hashes of an instance's draw output and of its pose (live, or at given clocks), packed with the generated packers so they compare directly with WebAssembly |
| `foundry_diag_bad_poses` | Instances whose pose has a non-finite matrix or a bone far outside its bounds |
| `foundry_diag_flush` | Releases every unused build now, for the drain check |

The runtime doesn't count its own allocations: the heap in use is read from the allocator instead, which also catches leaks outside the runtime.

Stage labels such as `tree.rig` appear only in diagnostics. That keeps the API rule (no system escapes the runtime) intact for games.

## Reference hashes

"Matches what was approved" needs the WebAssembly answer for the same configs:
1. **Sample.** `stress_$pipeline --plan p.json --sample` writes the configs that the diversity phase would draw.
2. **Reference.** The foundry runs them through the WebAssembly pipeline (the same `Runner` as `pipeline_view`) and writes `reference.json`. It holds, for each config, the draw output's hash and the pose hash at the plan's fixed times.
3. **Compare.** The diversity phase compares against that file.

The foundry's stage cache keeps a rerun cheap when only the host changed. `perf_stress` builds the reference on every run unless told not to (`reference: false`); for `tree`, hashing 100 configs at three clocks each takes a few seconds.

This is the pipeline-wide version of the per-system `verify_native` check. It runs on the merged runtime, with its own scheduler, threads, and cache in the path, rather than on a single system's runner.

## Drafts and finals

Stress binaries are built for draft and final pipelines alike. Drafts are where the numbers matter most, before approval; finals are where they show what changed underneath them. A final pipeline's systems never change, so any movement in its numbers comes from the runtime around it: the renderer, the cache, the scheduler. Measuring old finals against every new runtime is how a rendering change that helps today's pipeline but hurts last month's gets caught.

The shipping runtime contains only final pipelines. Stress and development builds contain every pipeline, and each report records the pipeline's state and the version of every system in it.

## Reports

Each run writes a report to the workspace, at `perf/report-$pipeline-$timestamp.json` (the timestamp in UTC, `20261011T153000Z`), and prints a short summary that an agent can read at a glance:

```
stress_tree (draft)  plan quick  seed 7  linux  AMD Ryzen 9 7950X  RTX 4070 (Vulkan)  build 3f2a91c
diversity  100 configs  30 s     PASS  native = wasm 100/100  gen p50 4.1 ms p99 11.8 ms (ebony ancient high)
forest     10 sets  100..1600    PASS  budget 16.7 ms holds to 1600 (p95 14.2 ms)  generations 30/30  hits 99.8%
events     break_time  100       PASS  spike 3.9 ms over 2.1 ms  all settled in 7.4 s  memory back within 0.4 MB
soak       5 min  x100           PASS  RSS 212 -> 214 MB  slope +0.6 MB/h (limit 4)  drains 5/5 within 1.1 MB
vs report-tree-20261010T221500Z: forest p95 at 1600 -12%, events spike +4%, generation p50 -30%
```

A report holds:
- the plan and the machine: CPU, GPU, driver, OS, memory, and a machine fingerprint;
- the build: the commit, the runtime's version, the pipeline's state, and its system versions;
- every phase's samples, with their percentiles and outliers;
- each failure, with the journal index that reproduces it.

The journal and the raw frame samples stay in a run directory beside it (`perf/runs/`), which is not committed.

**The reports are the timeline.** Reports are committed, so `perf/` becomes the history of every pipeline's performance as the runtime and the renderer change, readable by the agent, by tools, and in review. There is no separate baseline: each run is compared with the latest earlier report for the same pipeline and plan on the same machine fingerprint, and the summary's last line gives the deltas. Numbers are only compared within one machine, since a different GPU is not a regression. The workspace can chart a pipeline's reports over time, and a report that names a regression links to the commit range between it and the one before.

The `perf` slice's mesh benchmarks can write into the same timeline later, so fixed assets and pipelines share one history.

## The agent's interface

The MCP tool `perf_stress` belongs to the `perf` slice, beside the mesh benchmark. It takes the following arguments:

| Argument | Meaning |
|---|---|
| `pipeline` | The pipeline to stress. |
| `plan` | A preset name or an inline plan. |
| `wait` | Defaults to on for `quick`. |

It runs these steps:
1. Regenerates the binding if the pipeline changed, and builds the `stress_$pipeline` target.
2. Prepares reference hashes if they are missing.
3. Runs the binary as a background job, which appears on the status bar and in the activity log.
4. Returns the summary, the report's path, the failures, and the deltas from the previous report.

Long plans return a job ID immediately, and `perf_stress_status` reports on the job. `perf_stress_all` runs a plan across every pipeline, draft and final, which is the check to run after changing the runtime or the renderer. The workspace shows a pipeline's reports beside it: frame time against instance count, the event spikes, RSS over the soak, and its timeline across reports.

The intended loop: the agent changes a system, plays it in the foundry, runs `quick`, and runs `standard` before calling `approval_wait`. The report lets it say "native matches, holds 60 fps to 1600 trees, flat memory" with evidence, rather than as an impression.

## Layout and build

| Path | What it is |
|---|---|
| `src/tools/stress/stress.h` | The C header a binding includes: parameter kinds, `stress_value`, `stress_pipeline`, `stress_main` |
| `src/tools/stress/*.cpp` | The platform: plan, sampler, scene, phases, measurement, OS memory, journal, report |
| `src/generated/stress/stress_$pipeline.c` | A pipeline's generated binding, committed like other generated code |
| `src/library/$pipeline.{h,cpp}` | A pipeline's generated API fragment and its native executor |
| `src/library/runtime.h` | Every fragment assembled: `foundry.h`, then the pipelines by name |
| `tools/gen_pipelines.py` | Generates all of the above from the workspace's `pipelines/*.json` and `config/`; `--check` fails the build when they are stale |
| `build/*/stress/stress_$pipeline` | The binaries: CMake builds one for each binding in `src/generated/stress/` |
| `perf/plans/*.json` (workspace) | Preset plans |
| `perf/report-$pipeline-$timestamp.json` (workspace) | Reports, committed: the performance timeline |
| `perf/runs/` (workspace) | Journals and raw samples, not committed |
| `src/tools/stress/video.cpp`, `text.cpp` | The QA video and its titles |
| `qa/videos/` (workspace) | QA videos and their indexes, not committed |

`src/tools/` depends on `src/library/` only, through `runtime.h` and `runtime_diag.h`, just as a game does, plus its own third-party libraries (a JSON parser). It never includes the host, a system, or a channel.

## What it needs first: the flattened runtime

The stress binary drives the native runtime, so the runtime has to exist. In order:

1. **The native executor.** For each pipeline, generated C++ that calls each stage's system directly (`tree_rig::run`) with channel structures, binding inputs exactly as `stages` says. This is the same binding logic as the foundry's `Runner`, compiled instead of interpreted. There are no packers on this path.
2. **The runtime core** (`src/runtime/core/`). Instances with handles, their timelines, the cache (keyed by the build parameters, reference counted, released on a timer, with a budget for unused builds), generation on background threads with the old build drawn until the new one is ready, the update broadcast across a thread pool, sleep when every update stage votes for it, frustum culling, and submission through a skinned, instanced renderer (`src/runtime/render/skinned_renderer`, `skinned.hlsl`): one draw per distinct build, bones in a storage buffer where each instance keeps a slot and only changed poses are uploaded (`scatter.hlsl`), with the cost decisions of `submit.md`, "Cost". The stress tool needs only what a tree needs: no shadows or transparency.
3. **The API.** The general fragment (`foundry.h`: runtime, instances, update, prepare, draw, camera, quality), plus each pipeline's generated fragment, assembled into `runtime.h`. A frame is three calls rather than `api.md`'s single `foundry_frame`, because uploads must be recorded before the game's render pass begins.
4. **Diagnostics.** `runtime_diag.h`.
5. **The platform, the generator, and `perf_stress`.**

All five exist now. What the runtime doesn't do yet: pick the level of detail from distance and quality (a tree's `detail` is still a parameter the game sets), cap distinct seeds by quality (so the forest phase's `collapse` option isn't implemented), the "too far" and "too small" culling, and lifecycle and sound events.

Steps 1 to 4 are the native build and the generated C API, brought into the current milestone in `PLAN.md` because the stress tool needs them. The stress tool is what makes them measurable. It is also where the performance work starts: the tree's animate stage costs about 15 ms per frame in WebAssembly, almost all of it instance setup. Native, it should be microseconds per tree, and the forest phase is what will show it.

## Later: the cloud

The binary and the plan are already everything a cloud run needs: one self-contained executable, a JSON plan, and `"gpu": "none"` where there is no GPU. The later pieces are:
- a runner that ships both to a machine and runs a plan for days;
- streaming the journal and samples back as the run goes, so a crash at hour 30 still leaves its reproduction;
- a farm of machines, for spread across hardware.

Nothing in the binary needs to change for that, only the runner around it.
