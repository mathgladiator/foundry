# The crop pipeline (`crop_v1`)

`crop_v1` builds a crop field: a lot like a building's, sown in rows, its plants swaying in the wind as flowers do, but in bulk. A planner places field plots at a settlement's edge (`crop_fields`; `book/planners.md`) and its fill names the crops to sow.

- **The pipeline:** `demo/pipelines/crop_v1.json` (draft)
- **Systems:** `crop_build_v1` (draft), and `house_animate_v1` for the wind (its `drift` motion)
- **Channels:** `crop_v1_type`, `crop_v1_spec`, `crop_v1_model`, approved on 2026-10-14 (drafts); bones are `house_v1_bone`, patched the same day with `drift`
- **Crops:** 16 in the config table `demo/config/crop_v1_types/` (drafts)
- **Design:** `design/planner.md`, "Crops"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `type` | config | 16 crops | How its plants grow, how far apart and how tall, their colours, how much the wind moves them | yes |
| `size` | enum | small, medium, large | Its lot: small (rect6x4), medium (rect8x6), large (rect12x8) | yes |
| `growth` | enum | sown, sprouting, growing, ripe, harvested | Furrows only; a fifth, three fifths, or all of its height; stubble (and stooks of sheaves for grain) | yes |
| `season` | enum | winter, spring, summer, fall | Grain turns gold in the fall; snow on the furrows in winter | yes |
| `wind_groups` | u32 | 1 to 16 (7) | How many wind bones its plants share | yes |
| `seed`, `detail` | | | Which field; the level of detail (thins each row's plants, drops their finer parts) | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The wind and the clock | no: the pose only |

## How it's built

1. **The lot**, as `civic_v1`'s: brick-laid hex rectangles anchored at the cell nearest the middle, its front (+z) toward its road. The field fills the rectangle inside its cells, less a 0.35 m headland.
2. **The soil:** a bed and a ridged furrow per row along x, by the crop's row spacing.
3. **The plants** along each row by the crop's spacing, by how it grows:
   - **grain** (wheat, barley, oats): tufts of stalks, each with its head, gold when ripe;
   - **a tall stalk** (maize, sunflower): leaves up the stalk; a maize plant's cobs in their husks and its tassel; a sunflower's face and petals nodding south;
   - **a leafy head** (cabbage, lettuce): its heart and splayed outer leaves;
   - **root tops** (carrot, potato, onion): feathery fronds, a bushy haulm with flowers, or tubes; the root's shoulder at the ground when ripe;
   - **gourds** (pumpkin): a vine along the ground with broad leaves and its gourds;
   - **a pole vine** (beans, tomato): a teepee of canes, the vine climbing it, its pods or fruit;
   - **a trellis vine** (grapes): posts and two wires along the row, a twisted trunk, a curtain of leaves, its bunches;
   - **flowering bushes** (lavender, flax): mounds with flower spikes.
4. **Harvested:** stubble along grain's rows and stooks of sheaves (solid); bare furrows otherwise.
5. **The wind.** The field has `wind_groups` drift bones, each its own rate and phase. Each plant rides one (by its index's hash), and each of its vertices blends toward that bone by its height in the plant (smoothstep, 0 at the root), so every plant bends from its own root however far it stands from the bone's pivot. `house_animate_v1` moves a drift bone downwind in the trees' gusts, by up to its reach (35% of the crop's height), with a flutter across the wind. Canes, posts, wires, gourds, and roots' shoulders stay still. A field of hundreds of plants poses with that handful of computations and draws as one instance.
6. **Fire points:** 4, 7, or 10 blaze points across a grown or harvested field (fire config `blaze`), alight while the game burns it.
7. **Its lot's cells, box, every lot as guides** (a fixed 24 m framing), and **collision:** stooks solid; the field a query body named `field`, as tall as what grows on it (walked through).

A level of detail draws every second plant of a row at medium and every fourth at low, and at most about 48 a row at any level (a dense crop's rows draw as clumps); `plants` counts every plant either way.

## Crops

| Crop | Kind | Height | Spacing × rows |
|---|---|---|---|
| wheat | grain | 1.0 m | 0.30 × 0.45 m |
| barley | grain | 0.8 m | 0.28 × 0.42 m |
| oats | grain | 0.9 m | 0.30 × 0.45 m |
| maize | tall stalk | 2.2 m | 0.6 × 0.9 m |
| sunflower | tall stalk | 2.0 m | 0.6 × 0.9 m |
| cabbage | leafy head | 0.4 m | 0.5 × 0.6 m |
| lettuce | leafy head | 0.25 m | 0.32 × 0.45 m |
| carrot | root | 0.35 m | 0.12 × 0.4 m |
| potato | root | 0.55 m | 0.35 × 0.7 m |
| onion | root | 0.45 m | 0.15 × 0.4 m |
| pumpkin | gourd | 0.4 m | 1.2 × 1.4 m |
| beans | pole vine | 1.9 m | 0.8 × 1.2 m |
| tomato | pole vine | 1.4 m | 0.7 × 1.1 m |
| grapes | trellis vine | 1.6 m | 1.2 × 2.2 m |
| lavender | flowering | 0.6 m | 0.7 × 0.9 m |
| flax | flowering | 0.8 m | 0.35 × 0.5 m |

## Fields in the planners

Field plots the planners placed at the town tier (2026-10-14): `hamlet_v1` 3, `street_village_v1` 4, `green_village_v1` 3, `river_town_v1` 4, `stone_dome_v1` 3, `nordic_steading_v1` 4, `elven_glade_v1` 2, `ruined_keep_v1` 2 (harvested). The round and lake villages (and the themed planners built on them) placed none: a field needs at least half its front row on a road, and their curving lanes leave no frontage long enough.

## Checked

Every crop at every growth, small and large (160 builds), and every crop in winter and fall at medium and low detail (64): 224 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 18 to 38,296 triangles (median 294); the most is a large field of tomatoes at high detail.

## Performance

`perf_stress` quick plan, `perf/report-crop_v1-20261014T041659Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 1.2 ms at p50 and 15.5 ms at p99 (a large field of ripe oats in the fall at high detail); 100 of 100 configurations match WebAssembly; frames 0.33 ms at p95; 800 fields hold the 16.7 ms budget at 1.0 ms p95 with 98.8% of builds shared. The plan **passes** (it has no events).

## Known limits

- **A field never settles** in a wind: its drift bones always move.
- **Large fields are heavy at high detail** (up to about 38,000 triangles); the runtime's levels thin them, but no switch sizes are measured.
- **One field, one crop.** Mixed plots (a kitchen garden of several crops) would be several fields or a new kind.
- **No growth over time.** A game changes `growth` (and rebuilds) as the season turns.
