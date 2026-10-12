# Current FPS: a happy glen with 1,000 trees in view

**39.8 fps** with 1,377 trees in view (32,191 things drawn, 188.7 million triangles) at 1920 x 1080, in the native runtime, on an AMD Ryzen AI Max+ 395 with its Radeon 8060S (Vulkan). From a game camera standing in the same world, it runs at **73.6 fps**. A 1,000-tree view at 60 fps is a reasonable ask, and today we miss it by about 10 ms a frame. Most of that time goes to things nobody can see at that distance: flowers under a metre tall, each drawn with about 4,100 triangles and animated every frame, even off screen.

Every number below comes from one run: `demo/perf/report-plotter_happy_glen_v1-scale-20261012T224722Z.json`. To reproduce it, run `build/linux-release/stress/stress_plotter_happy_glen_v1 --plan scale`, call `plotter_perf` through MCP, or press **Run the scale plan** in the plotter's inspector. The inspector shows the newest report.

## How it was measured

The plotter's scale plan (`design/quality.perf.md`, "Scale") builds N x N grids of happy glens. Each glen is radius 12 (0.12 ha) with its own world seed, and each grid is grown to full with every placement spawned as an ordinary instance with its Jolt body. The plan then runs 240 frames from two cameras:
- **Overview:** sees everything.
- **Game:** 12 m up at the centre, looking ahead.

Each frame:
- **Update:** wind and animation on the CPU.
- **Prepare:** culling, sorting, and streaming bones to the GPU.
- **Draw:** instanced, skinned meshes, two frames in flight.

One frame in ten waits for its own fence, to time the GPU alone. The frame rate comes from the mean frame time. Nothing here runs WebAssembly: this is the flattened native runtime a game links.

| Grid | Hectares | Things | Overview fps | Trees seen | Triangles | Game fps | Trees seen |
|---|---|---|---|---|---|---|---|
| 1 x 1 | 0.12 | 134 | 4,972 | 5 | 0.8 M | 3,741 | 5 |
| 2 x 2 | 0.49 | 551 | 2,035 | 17 | 2.8 M | 2,664 | 11 |
| 4 x 4 | 1.95 | 2,177 | 538 | 82 | 11.6 M | 1,071 | 28 |
| 8 x 8 | 7.80 | 8,888 | 147 | 327 | 47.4 M | 319 | 93 |
| 12 x 12 | 17.6 | 20,094 | 68.6 | 764 | 106.7 M | 130 | 194 |
| 16 x 16 | 31.2 | 35,613 | **39.8** | **1,377** | 188.7 M | 73.6 | 338 |

Cost grows linearly with what is in the world. It does not grow with what is on screen.

## Where the 25 ms goes (16 x 16, overview)

| Part | Mean ms | What it is |
|---|---|---|
| Update | 17.5 | Every awake instance's `animate` on 24 of 32 threads: 35,297 instances, all of them every frame |
| Prepare | 7.2 | Culling 35,613 instances, sorting 32,191, and streaming every visible instance's bones to the GPU |
| Draw | 0.13 | 76 instanced draws |
| GPU, alone | 24.9 | 188.7 M triangles, measured on the synchronous frames; it overlaps the CPU on the others (0.17 ms waiting) |

The frame is CPU-bound: update and prepare run one after the other, adding up to about 25 ms. The GPU, taking about as long, hides behind them. Fixing only one side buys little. In the experiments below, low detail alone reaches 57.5 fps and no animation alone 49.1 fps, but the two together reach 98.6 fps.

### Triangles: 91 per pixel, mostly flowers

| Pipeline | Visible | Triangles each | Triangles | Share |
|---|---|---|---|---|
| `flower_v1` | 28,128 | 4,131 | 116.2 M | 62% |
| `bush_v1` | 2,395 | 16,843 | 40.3 M | 21% |
| `tree_v1` | 1,377 | 22,156 | 30.5 M | 16% |
| `rock_v1` | 290 | 5,748 | 1.7 M | 1% |

188.7 M triangles in a 2.07-megapixel frame is 91 triangles per pixel. From the overview, a 0.6 m daisy clump covers less than a pixel and still draws 4,131 triangles. The trees, the thing the scene is about, are only a sixth of the work.

### Animation: everything, always

| Update stage | Calls per frame | CPU ms per frame (summed over threads) | µs per call |
|---|---|---|---|
| `flower_v1.animate` | 31,146 | 185.6 | 5.96 |
| `bush_v1.animate` | 2,640 | 45.4 | 17.2 |
| `tree_v1.animate` | 1,511 | 27.2 | 18.0 |

That is 258 ms of CPU per frame, spread over 24 threads into 17.5 ms of wall time, which is about 60% parallel efficiency. The game camera sees 8,074 things, yet it still animates all 35,297: its update is 11.6 ms for a view with a quarter as much in it. A call costs less from the game camera (3.56 µs per flower) than from the overview (5.96 µs). The CPU and GPU share one power budget and one memory on this machine, so a busier GPU may slow the CPU, but I haven't verified that.

### What changing one thing does

The largest scene, from the overview:

| Experiment | fps | Update | Prepare | GPU alone | Triangles |
|---|---|---|---|---|---|
| As placed (high detail, wind 3 m/s) | 39.6 | 17.6 | 7.3 | 24.7 | 188.7 M |
| No drawing (CPU only) | 71.2 | 11.1 | 2.9 | none | none |
| Medium detail everywhere | 51.0 | 13.9 | 5.5 | 16.6 | 118.0 M |
| Low detail everywhere | 57.5 | 12.4 | 4.8 | 11.5 | 63.7 M |
| Low detail, no wind (all asleep) | **98.6** | 1.6 | 8.2 | 9.3 | 63.7 M |
| High detail, no wind (all asleep) | 49.1 | 1.5 | 8.7 | 37.8 | 188.7 M |

- **The two levers combined clear 60 fps with room to spare.** Low detail plus no animation runs at 98.6 fps. Each alone stays under 60.
- **Low detail is not low.** A tree at `low` still has 4,209 triangles, and a flower clump 1,755. That's fine up close, but nothing like what something 300 m away needs.
- **Sleeping costs nothing to update but still costs to prepare.** With everything asleep, prepare grew from 7.2 to 8.2 ms. Every visible instance's bones are copied and streamed every frame, whether they moved or not.
- **Even without drawing, update is 11.1 ms.** This is the CPU floor of animating 35,000 things.

### Not the problem

- **Draw calls:** 76, because instancing works.
- **Plotting:** 35,613 placements took 206 ms of grow calls, with every build ready 0.48 s after the grow started (76 distinct builds, shared by `distinct_seeds`).
- **Memory:** 349 MB RSS for the largest scene, Jolt bodies included.

## Is it the WebAssembly boundary?

Not in the game. The native runtime contains no WebAssembly. In the foundry, WebAssembly is a fixed cost: composing the plotter view ran pipelines for about 0.45 s whether the region held 156, 950, or 3,900 things, because builds are cached.

What makes the foundry feel slow is its viewport, a software renderer on the CPU (`src/host/viewport/raster.h`). It redraws the whole region on every camera move:

| Region (foundry, `plotter_view` at 900 px) | Things | Triangles | One picture on the CPU |
|---|---|---|---|
| Hex radius 12 | 156 | 0.9 M | 175 ms |
| Hex radius 30 | 950 | 5.7 M | about 0.5 s |
| Hex radius 60 | 3,900 | 22.6 M | about 1.4 s |

The same machine's GPU draws 188.7 M triangles in about 25 ms.

## The thesis change

Today the game decides every input, detail included (`design/api.md`). A game that fills its world from plotters can't sensibly do that: it doesn't know what is near, what is on screen, or what anything costs. The runtime does know.

**The proposed thesis: the runtime owns cost.** The game sets a budget: a frame time, or the quality level that `foundry_set_quality` already accepts and nothing reads. The runtime then chooses each instance's detail and animation rate from its size on screen. Parameters stay the game's; *how much of it to compute and draw* becomes the runtime's.

It needs the human's approval as a design change, and it changes `design/api.md` ("the game decides every input"), `design/submit.md` (sleep and wake), and every pipeline's notion of `detail`.

## Proposals, in order of what they buy

1. **Screen-size detail, chosen by the runtime.** Each frame, the runtime picks every instance's detail from its projected height, and the build cache holds the levels the way it holds species. Measured: going from high to low everywhere cut the GPU from 24.7 to 11.5 ms. With distance doing it, only what's near stays high. *Needs:* a projected-size rule per pipeline (in its conformance), `detail` moved from the game's parameters to the runtime's, and hysteresis so things don't flicker between levels.
2. **A far tier below `low`.** Trees and bushes become impostors (a camera-facing card baked from the build, a few triangles) beyond a distance their conformance sets. Flowers beyond about 30 m become a colour in the ground's shading, or fade out by density. Flowers are 116 M of the 189 M triangles, and from the overview they are sub-pixel. *Needs:* a baked impostor per build (the foundry already renders builds), and a ground the runtime draws, which it doesn't yet (the native scenes have no terrain).
3. **Animate what is seen, at the rate it is seen.** Sleep what is off screen and wake it when it enters view. Animate far things every Nth frame, and freeze the farthest. Measured: with no animation, update fell from 17.6 to 1.6 ms. From the game camera, 77% of the animated instances aren't visible at all. *Needs:* visibility and distance fed back from prepare to update, and the poses stay pure (time is an input, so skipping frames is exact).
4. **Wind on the GPU for small things.** A flower's or shrub's sway can be a vertex-shader function of time, wind, and a per-instance phase, with no bones on the CPU. Flowers alone are 72% of the animation CPU. *Needs:* a "procedural sway" mode in the skinned shader. The pipeline declares it, and its `animate` becomes the sway's parameters, built once.
5. **Persistent poses on the GPU.** Give each instance a stable slot in the bone buffer and upload only the poses that changed. Sleeping and far-frozen things then cost nothing in prepare (8.2 ms today with everything asleep).
6. **Overlap update and prepare.** They run back to back today, at about 60% parallel efficiency. Pipelining them, preparing frame N while updating frame N + 1, and larger work chunks would use more of the 32 threads. *Needs:* double-buffered poses.
7. **Triangle budgets as conformance.** Cap each pipeline's high detail (a flower clump at 4,131 and a bush at 16,843 are generous) and check the cap in the stress plan and the book, as the conformance capsule is checked.
8. **Draw the foundry's viewport on the GPU.** The host already links SDL3's GPU API and the native runtime. Rendering the plotter view through the runtime's renderer would make orbiting a 4,000-thing region real time instead of 1.4 s a picture. *Needs:* a GPU viewport in the host (`design/submit.md` already plans for this) and screenshot readback for MCP.
9. **A standing gate.** Make "1,000 trees in view at 60 fps on this machine" a threshold in the scale plan, so the headline passes or fails like the soak does.

**What I'd do first:** 1, 3, and 5. Together they are what the "low detail, no wind" experiment measured (98.6 fps), with distance deciding instead of a global switch. They don't touch any pipeline's look up close. Then 2 and 4, for scenes larger than this one.

## What this doesn't cover

- **Scope of the measurement:** one machine (an APU with shared memory), one plotter, and one resolution. There are no shadows, no ground, and no occlusion culling; all three would change the balance.
- **Wind:** a steady 3 m/s, the glen's shared value. A calm world sleeps and is much cheaper (above).
- **The foundry figures:** the CPU render times are from the foundry's log, one run each (`plotter_view` at 900 px); they are not in a committed report.
