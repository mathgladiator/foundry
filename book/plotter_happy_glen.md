# The happy glen (`plotter happy_glen`)

`happy_glen` is the first plotter (`design/plotter.md`): it fills any region of hex cells with a happy, gentle green place. Oaks, apple trees, and the odd willow stand in loose groves that thicken toward the border, so the middle stays open and sunny. Hazel, bramble, and elderberry gather under the trees and along the edges, bluebells and foxgloves grow in the shade, daisies and poppies fill the open grass, and a few river stones lie about. Under it all, a field of meadow grass covers every cell (`fields`, `book/grass.md`): grass on the soil, and plates for the pond and the path.

- **The plotter:** `demo/plotters/happy_glen.json` (draft)
- **Plot system:** `plot_scatter` (draft, `src/systems/plot_scatter/`), on the `plot_*` channels (drafts)
- **Pipelines it places:** `tree`, `bush`, `flower`, `rock` (their books describe each species)
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
foundry_region_desc desc = {.plotter = FOUNDRY_PLOTTER_HAPPY_GLEN, .world_seed = 7,
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

`plot_scatter` resolves the ground's cells (the highest priority covering a cell wins), then places five layers in order. Every candidate comes from a hash of its cell, so editing a region changes things only near the edit, and spacing is Matérn type II within each layer (`design/plotter.md`, "Placing deterministically").

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

The stress tool's region run (`stress_plotter_happy_glen`, `design/quality.perf.md`, "Regions") in the native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan). Each region is the glen's ground at radius 12, with its own world seed, grown at 64 placements per frame, every placement spawned with its collision body in a toy Jolt world:

| | 16 glens (1.95 ha) | 64 glens (7.80 ha) |
|---|---|---|
| Placements | 2,177 | 8,888 |
| Growing | 35 frames; a grow call 0.84 ms p50, 1.17 ms max; frame 3.83 ms p95 | 139 frames; a grow call 1.24 ms p50, 2.11 ms max; frame 5.89 ms p95 |
| Distinct builds | 74 | 76 |
| Bodies | 2,177 | 8,888 |
| Steady frame (swaying in the wind) | 2.00 ms p50, 5.14 ms p95 | 7.38 ms p50, 14.87 ms p95 |
| Bombing (everything within 8 m of a random point in every glen, three times) | 311 to 342 cleared, regrown in 5 or 6 frames; 2.6 to 4.2 ms at the blast | 1,287 to 1,349 cleared, regrown in 21 to 23 frames; 10.5 to 13.3 ms at the blast |
| Drain | 0 instances, 0 builds, 0 bodies | 0 instances, 0 builds, 0 bodies |

Reports: `demo/perf/report-plotter_happy_glen-20261012T195521Z.json` (16) and `-20261012T195524Z.json` (64). Every build was ready by the time growth ended, because 64 glens share only 76 builds (`distinct_seeds` 4). Regrowth can place a few more than were cleared (1,438 for 1,349 in the first bombing of 64): regrown canopy is part birch, whose one-cell footprint packs closer than an oak's seven.

Run it with `build/linux-release/stress/stress_plotter_happy_glen [--regions N] [--radius R] [--blast m]`.

### Scale

The scale plan (`plotter_perf`, or `--plan scale --find 60`; `design/quality.perf.md`, "Scale") grows grids of glens and measures the frame rate at 1920 x 1080 from three cameras: the overview from far above, a vista across the world from a corner at game height, and a game camera inside it (`demo/perf/report-plotter_happy_glen-scale-20261013T005027Z.json`, high quality, with the runtime's cost decisions on):

| Grid | Hectares | Things | Overview: fps (trees seen, triangles) | Vista: fps (trees seen) | Game camera: fps (trees seen) |
|---|---|---|---|---|---|
| 1 x 1 | 0.12 | 134 | 5,896.2 (5, 0.5 M) | 6,052.5 (5) | 4,426.3 (5) |
| 4 x 4 | 1.95 | 2,177 | 923.2 (82, 5.9 M) | 858.7 (83) | 1,659.0 (28) |
| 8 x 8 | 7.80 | 8,888 | 447.5 (327, 14.0 M) | 353.7 (332) | 703.6 (93) |
| 12 x 12 | 17.55 | 20,094 | 372.6 (764, 17.6 M) | 242.8 (775) | 446.6 (194) |
| 16 x 16 | 31.19 | 35,613 | 295.4 (1,377, 23.1 M) | 183.5 (1,392) | 349.7 (338) |
| 32 x 32 | 124.77 | 142,780 | 109.3 (4,829, 66.2 M) | 80.8 (5,103) | 232.7 (718) |
| 44 x 44 | 235.90 | 269,743 | 90.7 (7,312, 82.9 M) | 61.1 (8,227) | 219.2 (690) |
| 48 x 48 | 280.74 | 321,243 | 89.6 (8,102, 84.4 M) | 57.0 (9,188) | 197.5 (713) |

At 60 fps the vista holds 8,227 trees in view (44 x 44 glens, 269,743 things); it is limited by the GPU (110.6 M triangles, half of them trees and half shrubs).

**With its grass** (the glen's field of meadow grass on every cell; `book/grass.md`), from `demo/perf/report-plotter_happy_glen-scale-20261013T023041Z.json`:

| Grid | Things | Field cells (all of them) | Overview: fps (trees seen) | Vista: fps (trees seen) | Game camera: fps |
|---|---|---|---|---|---|
| 4 x 4 | 2,177 | 7,504 | 769.0 (82) | 707.2 (83) | 1,413.5 |
| 16 x 16 | 35,613 | 120,064 | 207.9 (1,377) | 147.1 (1,392) | 293.9 |
| 28 x 28 | 109,213 | 367,696 | 83.1 (3,872) | 68.8 (4,030) | 190.8 |
| 32 x 32 | 142,780 | 480,256 | 68.0 (4,829) | 57.0 (5,103) | 179.2 |

With grass, 60 fps holds to 28 x 28 glens: 4,030 trees in the vista over 339,414 grass cells in view. The 16 x 16 scene, 1,377 trees from the overview, went from 39.8 fps (`demo/perf/report-plotter_happy_glen-scale-20261012T224722Z.json`, before the runtime made its cost decisions) to 295.4. `CURRENT.FPS.md` breaks down each decision's gain and what it costs in looks.

## QA

- **The plotter view.** Select `plotters / happy / glen / v1` in the workspace. The inspector picks a ground preset (glen, hex, ring, strip, union, pond, path, slope, ragged, single) and radius, edits the regions, sets the world seed and the shared season and wind, and grows, regrows, clears, fells a share of the canopy (leaving stumps), and clears stumps. The metrics below it show counts per layer, per hectare, the nearest neighbours against the spacing, footprint overlaps, and distinct builds. "Show collision and footprints" draws every thing's collision and the footprint ring it reserves.
- **MCP.** `plotter_get`, `plotter_set`, `plotter_grow` (with `verify_native`), `plotter_clear`, and `plotter_view` do the same.
- **What to look at.** Several shapes (a big hex, a strip, a ragged union, a single cell), a path and a pond, a slope, a clearing and its regrowth, every season, and the game camera's distance (`plotter_view` with `distance`).

## Known limits

- **Hedges are sparse.** A shrub reserves the footprint of its oldest self (seven cells for an old elderberry), so hedges stand 4 to 5 m apart, not in a tight row. A shrub's soft collision (and with it the footprint) at half its spread may be too generous; a hedgerow would want a smaller one.
- **No oaks near the border.** An oak's seven-cell footprint must lie wholly inside the region, so oaks keep a cell in from the edge, while the edge field asks for more trees there; apples and willows take those places.
- **Regions plot alone.** Two glens side by side show an edge where their spacing tests didn't see each other (`design/plotter.md`, "Size and borders").
- **Flowers are small at a distance.** A flower clump is under a metre tall, so a radius-12 glen seen whole reads as grass with specks of colour.
- **Boundary.** It has a boundary (`boundary`, around_valley in the temperate biome; `book/boundary.md`): a ledge to the east, cliffs north and south, forest west, around whatever ground the view gives it. Live, it runs at 224 fps drawing 250 terrain nodes (2026-10-14). Its forest within 100 m is figures, which the glen fills natively once the boundary is built (`foundry_region_spawn_figures`): live, the glen draws 261 instances where it drew 136 things and the boundary before, and the picture fills them with tree and bush builds.
