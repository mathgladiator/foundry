# The waterfront pipeline (`waterfront_v1`)

`waterfront_v1` builds a building that comes off land into water: a pier with boats tied along it, a boathouse over the shore, a quay warehouse with a crane, or a fishery with its nets. It stands on a lot whose back two rows are land and the rest water, and a planner (`book/planners.md`) places such lots along a river or a lake with a road behind them.

- **The pipeline:** `demo/pipelines/waterfront_v1.json` (draft)
- **Systems:** `waterfront_build_v1` (draft), and `house_animate_v1` for what moves
- **Channels:** `waterfront_v1_type`, `waterfront_v1_spec`, `waterfront_v1_model`, approved on 2026-10-13 (drafts); bones are `house_v1_bone`
- **Types:** 4 in the config table `demo/config/waterfront_v1_types/` (drafts)
- **Design:** `design/planner.md`, "Waterfronts"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `theme` | config | 8 themes | Colours (walls, roof, timbers), moss, and fires | yes |
| `condition` | enum | intact, ruin | A ruin: broken down, its boats and lamp gone, overgrown (`condition.h`) | yes |
| `type` | config | 4 types | What it is, its colours, and what it keeps (boats, crane, nets, barrels, a lamp) | yes |
| `size` | enum | small, medium, large | Its lot: `rect3x4`, `rect4x5`, `rect5x6`; the back two rows land, the rest water | yes |
| `drop` | f32 | 0 to 4 m | How far below the lot's ground the water lies (a plan gives each waterfront plot's) | yes |
| `season` | enum | winter, spring, summer, fall | Snow on what faces up | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct buildings | yes |
| `detail` | enum | high, medium, low | Round things' sides, and the second net rack | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | Boats rocking, nets and the crane's jib swaying | no: the pose only |

The model carries its lot's cells (`cells`), which of them are water (`wet`), and its box (`lot_min`, `lot_max`); `draw.footprint.wet` lets a planner read the wet cells, so it places the lot where those are water and the rest land.

## How it's built

+z is the water; the shore runs between the second land row and the first water row.

1. **The shore:** a quay wall along it, stone for a warehouse or a boathouse, timber otherwise.
2. **By type:**
   - **pier:** a plank deck on posts out over the water, boats (up to four, fewer on a smaller lot) tied along it, a lamp at its end;
   - **boathouse:** a shed across the shore on posts, its gable open to the water, walkways either side of the slip, a boat inside;
   - **warehouse:** a big shed on the land with doors to the quay, a crane whose jib swings over the water, and a cargo boat with a mast alongside;
   - **fishery:** a hut on the land, a short jetty, racks of nets drying, and boats along the shore.
3. **Barrels and crates** on the quay, as the type says.
4. **What moves:** boats rock about their keels (a `house_v1_bone` sway along the keel), nets and the crane's jib sway, the lamp flickers.

Collision: the quay wall, decks (walkable), sheds, and the crane's post.

## Checked

Every type at every size, summer and winter, with the water 0.6 and 1.5 m down: 48 builds through `pipeline_set` with `verify_native`, all matching WebAssembly byte for byte, Jolt taking all collision, inside the conformance and the lot's box. 229 to 420 triangles (median 309), 2 to 5 bones.

Themes and ruins (2026-10-14): every type in every theme, intact and as a ruin: 64 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 269 to 971 triangles (median 462); 0 to 6 fire points (its lamp, and 3 to 7 blaze points).

## Performance

`perf_stress` quick plan, `perf/report-waterfront_v1-20261014T041748Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.6 ms at p50 and 2.0 ms at p99 (a dwarven warehouse as a ruin); 100 of 100 configurations match WebAssembly; frames 0.15 ms at p95; 800 hold the 16.7 ms budget at 0.4 ms p95 with 98.8% of builds shared. The plan **passes**.

## Known limits

- **The water isn't drawn.** The model is the building; the water is the game's (or the plan's water plot). In the pipeline's own view, the posts and boats hang over nothing.
- **Simple shapes.** Sheds, decks, and boats are boxes: a first cut to judge placement by, not the final look.
- **No levels of detail measured.**
