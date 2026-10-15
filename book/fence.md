# The fence pipeline (`fence`)

`fence` builds a fence network on the hex grid's diagonal lattice, like a wall (`book/wall.md`, "The lattice and a network"). Its lines run through a hexagon's opposite corners, straight however long they run, and meet at 60 and 120 degrees. It builds a whole network (or one chunk of it) as one model, with a post at every node and a swinging gate in each opening. A plan fences yards behind plots as networks (`book/planners.md`), and a round village's hedge is its wall's network built with the hedge kind.

- **The pipeline:** `demo/pipelines/fence.json` (draft)
- **Systems:** `fence_build` (draft), and `house_animate` for its gates and hedges
- **Channels:** `fence_kind`, `fence_spec`, `fence_model` (approved 2026-10-13, patched 2026-10-14: a whole network instead of a piece), and `line_network` (drafts)
- **Kinds:** 10 in `demo/config/fence_kinds/`; it shares the example networks in `demo/config/line_networks/` (drafts)
- **Design:** `design/planner.md`, "Walls, fences, and gates"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `kind` | config | 10 kinds | Style, size, colours, post spacing | yes |
| `condition` | enum | intact, ruin | A ruin: broken and leaning to a ragged height over its line's ground, its gates gone, rubble beside it (`condition.h`) | yes |
| `network` | config | 11 examples, or the game's own | The network to build | yes |
| `chunks`, `chunk` | u32 | | As a wall's | yes |
| `season`, `seed`, `detail` | | | A hedge's leaves, snow; which build; pickets, bars, courses | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | A hedge's sway | no: the pose only |
| `door_time` | event | | Seconds since its gates opened | no: the pose only |

## How it's built

1. **Runs.** Each unit link (3 m) runs from its node to the next, rising with the ground, less what a gate opens: pickets between two rails, three rails, split rails zigzagging, a woven wattle hurdle, a hedge (leafy by season, swaying a little), a low dry-stone wall with coping stones, or iron railings with finials. Posts stand along it every `module_m` (1.5 m: one at each run's middle).
2. **Nodes.** A post stands at every node the fence reaches, so posts never double where links meet. A split rail gets crossed stakes. A hedge or a low wall runs straight on through a straight node, and gets a rounded clump or a pier where it ends, bends, or branches.
3. **Gates.** Each opening (a gate node's 3 m, a gate link's 3 m, merged where they touch) gets a gate hinged on a post at one end and latching on one at the other (a node's own post where there is one). It swings open by `door_time`.
4. **Chunks** and **guides**, as a wall's.

Collision: runs, posts, a hedge or wall, and the gates' leaves, solid.

## Kinds

| Kind | Style | Height | Post spacing |
|---|---|---|---|
| picket | white pickets | 0.95 m | 1.5 m |
| rail | three rails | 1.15 m | 1.5 m |
| split_rail | zigzag rails | 1.1 m | 1.5 m |
| wattle | woven hurdles | 1.0 m | 1.5 m |
| hedge | clipped hedge | 1.3 m | 1.5 m |
| low_wall | dry stone | 0.9 m | 1.5 m |
| iron | railings | 1.3 m | 1.5 m |
| living_willow | willow wands crossing in diamonds, tufts of leaf | 1.2 m | runs on through |
| stone_slab | upright slabs edge to edge | 1.0 m | runs on through |
| reed | bundled reeds lashed to rails | 1.3 m | 1.5 m |

## Fences in the planners

What the planner view built at the town tier (2026-10-14): each planner's fenced yards (`fence_share` of its household plots, `fence_cells` deep), as separate networks where they don't touch.

| Planner (kind) | Networks | Links | Length |
|---|---|---|---|
| `hamlet` (split rail) | 4 | 44 | 132 m |
| `street_village` (rail) | 16 | 133 | 399 m |
| `green_village` (picket) | 13 | 146 | 438 m |
| `lake_village` (rail) | 8 | 79 | 237 m |
| `round_village` (wattle) | 6 | 55 | 165 m |
| `market_town` (low wall) | 23 | 125 | 375 m |
| `river_town` (rail) | 27 | 118 | 354 m |
| `hill_keep` (low wall) | 18 | 92 | 276 m |

The round village's hedge, its wall, is one more network of 69 links.

## Checked

Every kind with every example network, summer and winter: 154 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 320 to 11,888 triangles (median 2,056); the most is the star network in iron railings at high detail.

Ruins and the theme kinds (2026-10-14): every kind (10) with the branch and ring networks, intact and as a ruin: 40 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 400 to 22,617 triangles (median 3,364).

## Performance

`perf_stress` quick plan, `perf/report-fence-20261014T042937Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.5 ms at p50 and 7.3 ms at p99 (a ruined iron paddock at low detail); 100 of 100 configurations match WebAssembly; frames 0.12 ms at p95; 800 hold the 16.7 ms budget at 0.3 ms p95 with 98.8% of builds shared. The plan **fails** only its settle checks, on hedges, as before.

## Known limits

- **Three directions,** as walls'. A yard's fence is its lattice hexagon.
- **A hedge never settles** (above).
- **Iron and pickets are many triangles** at high detail in a big network; a far level drops them to a panel.
