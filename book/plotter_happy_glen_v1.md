# The happy glen (`plotter happy_glen_v1`)

`happy_glen_v1` is the first plotter (`design/plotter.md`): it fills any region of hex cells with a happy, gentle green place. Oaks, apple trees, and the odd willow stand in loose groves that thicken toward the border, so the middle stays open and sunny. Hazel, bramble, and elderberry gather under the trees and along the edges, bluebells and foxgloves grow in the shade, daisies and poppies fill the open grass, and a few river stones lie about.

- **The plotter:** `demo/plotters/happy_glen_v1.json` (draft)
- **Plot system:** `plot_scatter_v1` (draft, `src/systems/plot_scatter_v1/`), on the `plot_v1_*` channels (drafts)
- **Pipelines it places:** `tree_v1`, `bush_v1`, `flower_v1`, `rock_v1` (their books describe each species)
- **Design:** `design/plotter.md`

## What the game asks for

The game describes the ground as prioritized hex regions, and asks for growth:

```c
foundry_hex_region ground[] = {
    {.shape = FOUNDRY_HEX_HEX, .radius = 12, .set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE,
     .ground = FOUNDRY_GROUND_SOIL, .available = true},
    {.shape = FOUNDRY_HEX_HEX, .q = 4, .r = -3, .radius = 3, .priority = 1,
     .set = FOUNDRY_SET_GROUND, .ground = FOUNDRY_GROUND_WATER},                     /* a pond */
    {.shape = FOUNDRY_HEX_LINE, .q = -12, .r = 6, .q2 = 12, .r2 = -6, .priority = 2,
     .set = FOUNDRY_SET_GROUND | FOUNDRY_SET_AVAILABLE | FOUNDRY_SET_KEEP_CLEAR,
     .ground = FOUNDRY_GROUND_PATH, .available = false, .keep_clear_m = 1.2f},       /* a path, kept clear */
};
foundry_region_desc desc = {.plotter = FOUNDRY_PLOTTER_HAPPY_GLEN_V1, .world_seed = 7,
                            .regions = ground, .region_count = 3};
foundry_region glen = foundry_region_create(rt, &desc);
foundry_placement batch[64];
uint32_t volume;
do {
  uint32_t n = foundry_region_grow(rt, glen, FOUNDRY_GROW_SEED, NULL, 0, NULL, batch, 64, &volume);
  for (uint32_t i = 0; i < n; ++i) keep(batch[i], foundry_placement_spawn(rt, glen, &batch[i]));
} while (volume > 0);  /* one batch a frame, in a game */
```

The game owns every instance spawned; it sets the season and the wind on them, and tells the region when they go (`foundry_region_vacate`). A later `grow` in `FOUNDRY_GROW_REGROW` mode fills cleared ground, starting things young.

## How it places

`plot_scatter_v1` resolves the ground's cells (the highest priority covering a cell wins), then places five layers in order. Every candidate comes from a hash of its cell, so editing a region changes things only near the edit, and spacing is Matérn type II within each layer (`design/plotter.md`, "Placing deterministically").

| Layer | Spacing | Density (per 100 cells) | Where | Kinds |
|---|---|---|---|---|
| `canopy` | 7.0 m | 1.5; groves 0.25x to 1.7x; 1.4x at the border | soil | oak 0.55 (young, mature, ancient), apple 0.33 (young, mature), willow 0.12 (mature, at most 20%) |
| `hedges` | 2.2 m | 7; 3x at the border; 2.5x within 6 m of a tree | soil, 2.6 m clear of trees | hazel 0.36, blackberry 0.34, elderberry 0.30 |
| `shade_flowers` | 1.3 m | 9; 3x within 5 m of a tree | soil, clear of trees and hedges | bluebell 0.6, foxglove 0.4 |
| `meadow_flowers` | 1.1 m | 22; 1.6x in clearings, 0.35x in groves | soil, clear of trees, hedges, and shade flowers | daisy 0.55, poppy 0.45 |
| `stones` | 4.0 m | 0.9; never regrows | soil, 2 m clear of trees | river stone, small 0.75 or medium 0.25, lightly mossed |

On regrowth the canopy comes back as oak 0.6 and birch 0.4 (a cleared glen's first trees), every tree as a sapling, and every shrub young. Flowers come back as they were; stones don't.

### Footprints

Measured from each kind's pipeline at its largest age, when the foundry loaded the plotter:

| Kind | Footprint | Radius |
|---|---|---|
| oak (and regrown oak) | 7 cells | 1.71 m |
| apple | 1 cell | 0.49 m |
| willow | 7 cells | 1.16 m |
| birch (regrown) | 1 cell | 0.32 m |
| hazel | 7 cells | 1.50 m |
| blackberry | 7 cells | 1.57 m |
| elderberry | 7 cells | 1.88 m |
| river stone (medium at its largest) | 7 cells | 0.82 m |
| bluebell, foxglove, daisy, poppy | none (query only) | |

A shrub's footprint follows its soft collision at its largest (old) age, half its spread, so an old elderberry reserves seven cells. That keeps hedges a few metres apart; see "Known limits".

## The glen on its own ground

The foundry's `glen` ground at radius 12 (469 cells, 0.12 ha: a meadow with a pond, a path across it, and a gentle knoll), world seed 7, grown to full:

| Layer | Count | Per hectare | Nearest neighbour (spacing) |
|---|---|---|---|
| canopy | 5 (2 oaks, 2 apples, 1 willow) | 41 | 8.52 m (7.0) |
| hedges | 11 (6 blackberry, 3 elderberry, 2 hazel) | 90 | 4.99 m (2.2) |
| shade flowers | 44 (22 bluebell, 22 foxglove) | 361 | 1.41 m (1.3) |
| meadow flowers | 72 (37 daisy, 35 poppy) | 591 | 1.11 m (1.1) |
| stones | 2 | 16 | |

134 things, no footprint overlaps, 32 distinct builds. The plot system ran in 2.3 ms in WebAssembly, and its native output matched WebAssembly byte for byte (`plotter_grow` with `verify_native`). The native runtime grows the same 134 placements, kind for kind, in 5 calls of 32 (5.2 ms in all, 1.2 ms the worst), and every one of them gets its collision body (`tests/region_test.cpp`).

The same test clears everything within 6 m of the middle (9 things), regrows it in regrow mode (9 back, all young, the region full again), restores a save into a fresh region (refusing one with two things on one footprint), and shrinks the ground to radius 6, which reports 86 of the 134 as lost.

## Performance

The stress tool's region run (`stress_plotter_happy_glen_v1`, `design/quality.perf.md`, "Regions") in the native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan). Each region is the glen's ground at radius 12, with its own world seed, grown at 64 placements per frame, every placement spawned with its collision body in a toy Jolt world:

| | 16 glens (1.95 ha) | 64 glens (7.80 ha) |
|---|---|---|
| Placements | 2,177 | 8,888 |
| Growing | 35 frames; a grow call 0.84 ms p50, 1.17 ms max; frame 3.83 ms p95 | 139 frames; a grow call 1.24 ms p50, 2.11 ms max; frame 5.89 ms p95 |
| Distinct builds | 74 | 76 |
| Bodies | 2,177 | 8,888 |
| Steady frame (swaying in the wind) | 2.00 ms p50, 5.14 ms p95 | 7.38 ms p50, 14.87 ms p95 |
| Bombing (everything within 8 m of a random point in every glen, three times) | 311 to 342 cleared, regrown in 5 or 6 frames; 2.6 to 4.2 ms at the blast | 1,287 to 1,349 cleared, regrown in 21 to 23 frames; 10.5 to 13.3 ms at the blast |
| Drain | 0 instances, 0 builds, 0 bodies | 0 instances, 0 builds, 0 bodies |

Reports: `demo/perf/report-plotter_happy_glen_v1-20261012T195521Z.json` (16) and `-20261012T195524Z.json` (64). Every build was ready by the time growth ended, because 64 glens share only 76 builds (`distinct_seeds` 4). Regrowth can place a few more than were cleared (1,438 for 1,349 in the first bombing of 64): regrown canopy is part birch, whose one-cell footprint packs closer than an oak's seven.

Run it with `build/linux-release/stress/stress_plotter_happy_glen_v1 [--regions N] [--radius R] [--blast m]`.

### Scale

The scale plan (`plotter_perf`, or `--plan scale`; `design/quality.perf.md`, "Scale") grows grids of glens and measures the frame rate at 1920 x 1080 (`demo/perf/report-plotter_happy_glen_v1-scale-20261012T224722Z.json`):

| Grid | Hectares | Things | Overview: fps (trees seen, triangles) | Game camera: fps (trees seen) |
|---|---|---|---|---|
| 1 x 1 | 0.12 | 134 | 4,972 (5, 0.8 M) | 3,741 (5) |
| 4 x 4 | 1.95 | 2,177 | 538 (82, 11.6 M) | 1,071 (28) |
| 8 x 8 | 7.80 | 8,888 | 147 (327, 47.4 M) | 319 (93) |
| 12 x 12 | 17.6 | 20,094 | 69 (764, 106.7 M) | 130 (194) |
| 16 x 16 | 31.2 | 35,613 | 39.8 (1,377, 188.7 M) | 73.6 (338) |

With 1,377 trees in view (32,191 things drawn), the glen runs at 39.8 fps. Flowers are most of the cost: 62% of the triangles and 72% of the animation time. `CURRENT.FPS.md` breaks it down and proposes what to change.

## QA

- **The plotter view.** Select `plotters / happy / glen / v1` in the workspace. The inspector picks a ground preset (glen, hex, ring, strip, union, pond, path, slope, ragged, single) and radius, edits the regions, sets the world seed and the shared season and wind, and grows, regrows, clears, fells a share of the canopy (leaving stumps), and clears stumps. The metrics below it show counts per layer, per hectare, the nearest neighbours against the spacing, footprint overlaps, and distinct builds. "Show collision and footprints" draws every thing's collision and the footprint ring it reserves.
- **MCP.** `plotter_get`, `plotter_set`, `plotter_grow` (with `verify_native`), `plotter_clear`, and `plotter_view` do the same.
- **What to look at.** Several shapes (a big hex, a strip, a ragged union, a single cell), a path and a pond, a slope, a clearing and its regrowth, every season, and the game camera's distance (`plotter_view` with `distance`).

## Known limits

- **Hedges are sparse.** A shrub reserves the footprint of its oldest self (seven cells for an old elderberry), so hedges stand 4 to 5 m apart, not in a tight row. A shrub's soft collision (and with it the footprint) at half its spread may be too generous; a hedgerow would want a smaller one.
- **No oaks near the border.** An oak's seven-cell footprint must lie wholly inside the region, so oaks keep a cell in from the edge, while the edge field asks for more trees there; apples and willows take those places.
- **Regions plot alone.** Two glens side by side show an edge where their spacing tests didn't see each other (`design/plotter.md`, "Size and borders").
- **Flowers are small at a distance.** A flower clump is under a metre tall, so a radius-12 glen seen whole reads as grass with specks of colour.
