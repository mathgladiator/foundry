# The castle pipeline (`castle_v1`)

`castle_v1` builds the parts of a castle: the keep, a great hall, a chapel, a range of barracks, stables, a kitchen, or storehouses, and a tower standing alone. A castle is a plan's **keep ward**: a small village of these parts round a bailey inside the keep's own wall, with the keep at its middle. A castle (`castle_v1` planner) has an outer ward too, inside a curtain, with more parts beside its households. The walls and gates are `wall_v1` and `gate_v1`, laid on the plan's lines (`book/wall_v1.md`, `book/gate_v1.md`).

- **The pipeline:** `demo/pipelines/castle_v1.json` (draft)
- **Systems:** `castle_build_v1` (draft), and `house_animate_v1` for what moves
- **Channels:** `castle_v1_part`, `castle_v1_spec`, `castle_v1_model`, approved on 2026-10-13, `castle_v1_model` patched 2026-10-14 for its doors' cells (drafts); bones are `house_v1_bone`
- **Parts:** 8 in the config table `demo/config/castle_v1_parts/` (drafts)
- **Design:** `design/planner.md`, "Castles"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `part` | config | 8 parts | What it is, its usual size, stone, roof, storeys, and what it shows | yes |
| `size` | enum | small, medium, large, keep | Its lot: small (rect6x5), medium (rect8x6), large (rect10x7), or keep (rect8x8). A planner gives the part's own size | yes |
| `theme` | config | 8 themes | Colours; turrets' and towers' tops (a dome, a corbelled dome for flat themes, a flared cone for a sweep); roofs by the theme's form; ornaments by the door; moss; the fires | yes |
| `stage` | enum | planning, half, finished, ruin | Staked out; half built in scaffolding; finished; or a ruin (`condition.h`, as `book/civic_v1.md`'s) | yes |
| `season` | enum | winter, spring, summer, fall | Snow on what faces up | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct buildings | yes |
| `detail` | enum | high, medium, low | Windows, battlements, round things' sides | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The wind and the clock for banners, bells, and smoke | no: the pose only |
| `door_time` | event | | Seconds since its door opened | no: the pose only |

## How it's built

1. **The lot**, as `civic_v1`'s: brick-laid hex rectangles anchored at the cell nearest the middle, the front (+z) toward the bailey.
2. **By kind:**
   - **a keep:** a great square tower of storeys on a battered plinth with a string course, windows by storey (arrow slits on the lowest, the part's windows above), a parapet and battlements, corner turrets rising above it (cone roofs, or battlements), a roof inside the parapet if the part has one, a chimney smoking, a banner, and a forebuilding stair up its front to the first-floor door;
   - **a hall:** a tall stone hall along the lot, buttresses between its windows (tall traceried ones), a steep roof with a louvre on the ridge, a porch at its door, a chimney smoking, banners;
   - **a chapel:** a nave toward the back with arched windows, an apse, a bellcote on its front gable (the bell swings);
   - **a range:** a long, low building against the lot's back, its doors along the front (a stable's row of half doors, wide arches for carts, or doors), chimneys smoking;
   - **a tower:** round, standing alone, slits per storey, a cone roof or battlements, a banner.
3. **Its lot's cells, box, every lot as guides** (a fixed 26 m framing), **its doors' cells** (as `civic_v1`'s), **the doorway, the marker, and collision**: walls, towers, turrets, and stairs solid; the doorway a sensor.

## Fire

Torches on brackets either side of every door (the theme's torch fire); chimneys' smoke (the theme's hearth fire, smoke only), which was smoke puffs on bones; 4 to 10 blaze points on a finished or ruined part.

## Parts

| Part | Kind | Size | Shows |
|---|---|---|---|
| keep | keep | keep | four storeys, turrets with cone roofs, battlements, a banner, a chimney |
| great_hall | hall | large | two storeys, traceried windows, a slate roof, banners, a chimney |
| chapel | chapel | medium | arched windows, a bellcote |
| barracks | range | large | two storeys, doors, chimneys, a banner |
| stables | range | medium | half doors, a tiled roof |
| kitchen | range | small | big chimneys smoking |
| storehouse | range | medium | wide arched doors |
| tower | tower | small | three storeys, a cone roof, a banner |

## Checked

Every part at every size, summer and winter: 64 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 157 to 1,650 triangles (median 314).

Themes and stages (2026-10-14): every part at its own size in every theme (64), and at planning, half, and as a ruin (24): 88 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 236 to 3,615 triangles (median 502); 0 to 15 fire points (median 8).

## Performance

`perf_stress` quick plan, `perf/report-castle_v1-20261014T041737Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.9 ms at p50 and 3.8 ms at p99 (a stone-dome keep as a ruin); 100 of 100 configurations match WebAssembly; frames 0.17 ms at p95; 800 parts hold the 16.7 ms budget at 0.5 ms p95 with 98.8% of builds shared. The plan **fails** on its settle and events checks, as before.


## Known limits

- **Solid inside.** No interiors; a keep's floors are its windows.
- **The keep's lot is the keep's.** A planner never puts another part on it.
- **It never settles** (above).
- **No levels of detail measured.**
