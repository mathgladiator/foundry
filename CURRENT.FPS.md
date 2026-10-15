# Current FPS: the runtime owns cost

**At 60 fps, 8,227 trees in view.** A vista across a 44 x 44 grid of happy glens (269,743 things on 236 hectares), from a corner of the world at game height, holds 61.1 fps at 1920 x 1080 with 8,227 trees in view and 21,420 things drawn. Above the same world, the overview holds 90.7 fps with 7,312 trees in view. At 48 x 48 the vista drops to 57.0 fps. The machine is an AMD Ryzen AI Max+ 395 with its Radeon 8060S (Vulkan).

**With grass on every cell,** the same vista holds 4,030 trees at 68.8 fps; grass alone holds 1.76 million grass cells in view at 76.6 fps (see "Grass" below).

**The 1,000-tree view.** The scene from the first report (16 x 16 glens, 1,377 trees in view from the overview) went from **39.8 fps to 295.4 fps**. Its game camera went from 73.6 to 349.7 fps.

Every number here comes from `demo/perf/report-plotter_happy_glen-scale-20261013T005027Z.json`, except where another report is named. To reproduce it, run `build/linux-release/stress/stress_plotter_happy_glen --plan scale --find 60`, call `plotter_perf` with `find: 60`, or press **Run the scale plan** in the plotter's inspector.

## Grass: a field, and the most of it at 60 fps

The glen now has grass on every cell: one **field** instance per region, not an instance per blade (`design/plotter.md`, "Fields"; `book/grass.md`).
- **The functor:** a pure function of each cell's integers, ground and the world's seed picks one of a few shared tiles.
- **Drawing:** chunks of 8 x 8 cells pick their level by size on screen, and the wind sways the blades in the vertex shader.

From `demo/perf/report-plotter_happy_glen-scale-20261013T023041Z.json` (the glen with grass) and `demo/perf/report-plotter_happy_glen-scale-20261013T023137Z.json` (grass alone, `--only-fields`):

| | Grid | Grass cells in view | Trees in view | fps |
|---|---|---|---|---|
| **Grass alone, vista** | 64 x 64 (499 ha) | **1,755,284** | | **76.6** |
| Grass alone, vista | 72 x 72 (632 ha) | 2,219,613 | | 59.8 |
| **Glen with grass, vista** | 28 x 28 (95.5 ha) | 339,414 | **4,030** | **68.8** |
| Glen with grass, vista | 32 x 32 | 442,292 | 5,103 | 57.0 |
| Glen with grass, overview | 16 x 16 | 110,079 | 1,377 | 207.9 |

- **Grass costs a third of the frame.** Grass on every cell of the 1,377-tree world takes the overview from 295 to 208 fps, and the game camera from 350 to 294 fps. The 60 fps vista goes from 8,227 trees on bare sky to 4,030 trees on a meadow.
- **Without measured levels, grass would cost 7 times as much.** The ladder on the glen with grass, with every decision off, draws grass at its finest everywhere: 204.6M triangles and 40.5 fps from the overview. With every decision on, it is 27.5M triangles at 194 fps.
- **Grass changes more of the picture when its level changes.** From the game camera, the decisions visibly change 7.8% of what shows (1.1% without grass). Almost all of it is grass levels drawing different blades in different places: the same texture, which a pixel-by-pixel comparison counts as change.
- **A grass cell is 32 bytes.** A field keeps each cell as instance data, never as an instance, so grass alone at 64 x 64 holds 1.9M cells in 308 MB RSS.
- **The far limit is the GPU, then copying.** At 72 x 72, grass alone draws 88.8M triangles: every far cell at its low level, 37 triangles. Preparing copies every visible cell's 32 bytes each frame: 8.6 ms there, on the update threads. At 64 x 64 it is 6.6 ms; on one thread it was 15.1 ms (a run without a report, before this was spread over the threads). The next steps are drawing far chunks as one mesh each, and keeping tiles in GPU memory with indirect draws.

The foundry's plotter view now shows this simulation live: the region in the native runtime, with its grass, at its real frame rate. At radius 150 (22,384 things and 68,000 grass cells), it grows in under a second and runs at 95 fps inside the foundry's window.

## The thesis change

**The game says what; the runtime says how much.** The game still sets every parameter of everything in its world. The runtime now decides how much of it to compute and draw, from what the camera sees and the quality setting (`design/submit.md`, "Cost"; `design/api.md`, "Quality"):
- **Detail level:** each instance's level of detail.
- **Posing:** whether the instance is posed this frame at all, and how often.
- **Drawing:** whether it is too small to draw.

A pipeline's `detail` parameter is now the finest level the game allows, not the level that gets drawn.

## The ideas, in the order predicted, and what each bought

I ranked the ideas by predicted gain for the least change in looks, then built them in that order. The scale plan's ladder turns them on one at a time on the 16 x 16 scene. The looks column comes from comparing pictures: it is the share of pixels that show something and that visibly change, against the scene drawn in full at the same moment.

| # | Idea, as ranked | Predicted cost in looks | Overview fps | Game fps | Looks: overview / game |
|---|---|---|---|---|---|
| | None: the game's detail everywhere, everything posed and uploaded every frame | | 58.1 | 91.7 | |
| 1 | Poses kept on the GPU: a slot per instance, only changed poses uploaded | none | 58.1 | 91.2 | none |
| 2 | Nothing out of view or too small is posed (events still play) | none | 59.2 | 194.8 | none |
| 3 | Small things are posed less often (every 2nd to 16th frame by size) | barely any | 74.9 | 227.2 | 2.2% / 0.4% |
| 4 | Detail by size on screen, at sizes each pipeline measured | small | 139.2 | 295.3 | 12.9% / 0.9% |
| 5 | Too small not drawn (under 4 px) | small | 288.8 | 334.2 | 4.6% / 0.0% |
| | All of them | | **288.8** | **334.2** | **16.0% / 1.1%** |

The "none" row is already faster than the first report's 39.8 fps, because of two changes with no cost in looks:
- **Flowers drew every leaf and petal twice,** once per side, though the renderer draws both faces anyway. They now draw one sheet: 188.7M triangles became 126.4M with no change on screen.
- **Prepare runs in parallel,** with draws bucketed by build.

### Reading the ladder

- **Keeping poses on the GPU gains nothing alone, and is what makes 3 cheap.** With everything posed every frame, every pose still uploads. Once small things are posed less often, uploads fall from 718,014 bones a frame to 61,390.
- **Skipping what can't be seen is worth most from inside the world.** The game camera sees 8,074 of 35,297 things, and posing only those halved its frame (91.7 to 194.8 fps). From the overview, almost everything is in view, so it changed little there.
- **The posing rate fixed the CPU.** The overview's update went from 13.4 ms to 1.9 ms. Poses are pure functions of time, so a skipped pose is never wrong, only late: 2.2% of what shows moves by a few pixels in the far view.
- **Detail by size halved the GPU's work** (126.4M to 51.8M triangles from the overview).
- **Too small finished the job.** 28,262 flowers and shrubs under 4 px stopped being drawn (51.8M to 23.1M triangles).
- **Most of the overview's 16% change is detail, on things a few pixels tall.** From the game camera, the scene changes on 1.1% of what shows.

`--snapshot` writes each comparison and its differences as PNGs.

### How detail is chosen: measured, not guessed

A coarser level switches in only below a measured size: where the median configuration differs from the finest on at most 10% of its pixels. That is the foundry's existing switch rule for fixed assets. Medium quality allows 20%, and low 30%.

`stress_<pipeline> --lod` measures these sizes (`design/quality.perf.md`, "Detail sizes"):
- It draws each level at sizes from 8 to 256 px, supersampled so a pixel holds what it covers.
- It writes the sizes into the pipeline's `draw.detail_pixels`, which the generator compiles into the runtime.

The measurement showed the old levels were poor. Flowers dropped up to 55% of their leaves and florets. Shrubs and trees dropped half their leaf clumps and grew the rest to cover the gaps. Every level now keeps every element and simplifies its shape: fewer petal segments, and 20-face or 8-face leaf clumps instead of 80. This version is closer to the finest level and costs fewer triangles. A shrub's medium level at 64 px went from 21.9% to 14.1% of its pixels differing, at 3,699 triangles instead of 7,973.

Mean triangles per level, from the detail reports (`demo/perf/report-*-detail-20261013T002122Z.json`):

| | high | medium | low | far (the runtime's) | Switches to medium / low / far at high quality |
|---|---|---|---|---|---|
| tree | 6,927 | 3,076 | 1,696 | 661 | 32 / 16 px / never |
| shrub | 9,599 | 3,699 | 1,774 | 1,021 | 32 / 16 px / never |
| flower | 1,554 | 1,225 | 860 | 115 | 64 / 24 px / never |
| rock | 3,103 | (game's level) | | | never: see below |

- **The far level is new and generic.** The runtime clusters a build's coarsest mesh on a grid, so every pipeline gets one without a level of its own. At high quality it never passes the 10% rule. Medium and low quality use it.
- **Coarser levels share the instance's pose.** The runtime checks each pair of levels first, and refuses a pair whose poses differ. A rock's pieces fly differently at each level, so rocks always keep the game's level.

## Where 60 fps runs out

| Grid | Things | Vista fps | Trees in view (vista) | Triangles (vista) | Overview fps | Game fps |
|---|---|---|---|---|---|---|
| 16 x 16 | 35,613 | 183.5 | 1,392 | 31.4M | 295.4 | 349.7 |
| 24 x 24 | 80,164 | 113.5 | 3,052 | 54.0M | 171.0 | 248.3 |
| 32 x 32 | 142,780 | 80.8 | 5,103 | 80.6M | 109.3 | 232.7 |
| 40 x 40 | 222,612 | 64.5 | 7,210 | 103.1M | 89.4 | 214.6 |
| **44 x 44** | **269,743** | **61.1** | **8,227** | **110.6M** | **90.7** | **219.2** |
| 48 x 48 | 321,243 | 57.0 | 9,188 | 116.9M | 89.6 | 197.5 |


- **The vista is limited by the GPU.** At 44 x 44 the CPU spends 5.0 ms updating and 1.9 ms preparing. The GPU has 110.6M triangles to draw: 52.0M of trees and 53.1M of shrubs, mostly at their low level, since a tree 20 px tall is still 1,700 to 6,000 triangles.
- **The overview flattens out at about 90 fps.** From 2.6 km up, most trees are about 5 px tall and every flower and most shrubs are under the 4 px cutoff.
- **The game camera loses frames as the world grows,** though it always sees about 700 trees. Classifying every instance and deciding who is posed are per-frame scans of the whole world: from the game camera, 2.8 ms of its update at 44 x 44 (classification 1.3 ms, choosing and posing 1.5 ms, with only about 770 posed), and 8.4 ms at 88 x 88 (a run of `--sizes 88` without a report). That is the next limit for big worlds.
- **Memory:** 1,302 MB RSS at 44 x 44, collision bodies included.

## The foundry draws on the GPU

The workspace's viewport, screenshots, contact sheets and `plotter_view` now render through SDL's GPU API (`src/host/viewport/gpu.h`).
- **The look is the same:** cel bands, rim band, ink on silhouettes and depth edges, overlays that fade behind the mesh, and 2x supersampling.
- **Region meshes upload once and are kept while shown,** so orbiting a region only draws.
- **Speed:** a radius-30 glen of 3.5M triangles at 900 px renders in 17 to 19 ms. The CPU renderer took 350 ms for the same picture, and the first GPU render (with the upload) took 42 ms. These timings come from the foundry's log.
- **The two renderers agree:** the GPU and CPU pictures differ on 1.7% of pixels, along edges.
- **Headless runs keep the CPU renderer.** `plotter_view` reports which renderer drew, and takes `cpu: true` to compare.
- **Composing a region still runs its pipelines in WebAssembly** (about 0.85 s for 871 things), once per change.

## What's next, ranked the same way

These were ranked before grass. Grass adds two of its own: drawing far grass chunks as one mesh each, and keeping field tiles in GPU memory, drawn indirectly, so preparing doesn't copy every visible cell each frame.


1. **Cull and classify by region.** Each plotter region gets bounds; a region out of view, or far beyond the posing sizes, is skipped whole. This removes the per-frame scan that grows with the world. No cost in looks.
2. **A cheaper far tier for trees and shrubs.** Impostors, or a stricter cluster, so a 20 px tree costs hundreds of triangles, not thousands. This is the vista's limit. At high quality the far level doesn't pass the 10% rule yet; a better far level would.
3. **Rocks that switch levels.** The pieces' physics would come from the finest level at every level (a change to `rock_body`).
4. **A standing gate.** "1,000 trees at 60 fps" is now passed six times over; the scale plan could fail any run below it.
5. **Built ideas not needed yet:** wind on the GPU for flowers, and overlapping update with prepare. Posing is now about 1 ms.

## What this doesn't cover

- **One machine, one resolution, one plotter.** The machine is an APU, whose CPU and GPU share power and memory. GPU times measured alone (one frame in ten) overstate the GPU's share of a steady frame on it.
- **No ground, shadows or occlusion.** All three would change the balance. Flowers culled under 4 px would read as ground colour if there were a ground.
- **Standard stress plans not rerun.** The quick plans were rerun after the level and flower changes, and all four pipelines pass with native matching WebAssembly 100/100. The standard plans weren't rerun.
