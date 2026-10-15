# The wall pipeline (`wall`) and lattice networks (`line_network`)

A wall runs on the hex grid's **diagonal lattice**: along lines through a hexagon's opposite corners. Such a line runs vertex to vertex through one cell, then along the edge between the next two cells, then vertex to vertex through the next. So it stays straight however long it runs, and lines meet at 60 and 120 degrees with joins that fit. A wall is given as a **network** of lattice segments, not a list of points. `wall` builds a whole network (or one chunk of it) as one model, with its joins and its openings, and reports where its gates go (`book/gate.md`). Fences use the same networks (`book/fence.md`). A plan only says where a wall goes (its wall tiles and its network, `book/planners.md`); this is what builds it.

- **The pipeline:** `demo/pipelines/wall.json` (draft)
- **Systems:** `wall_build` (draft), and `house_animate` for banners
- **Channels:** `wall_kind`, `wall_spec`, `wall_model` (approved 2026-10-13, patched 2026-10-14: a whole network instead of a piece), and `line_network` (approved 2026-10-14) (drafts)
- **Kinds:** 11 in `demo/config/wall_kinds/`; **example networks:** 11 in `demo/config/line_networks/` (drafts)
- **Design:** `design/planner.md`, "Walls, fences, and gates"

## The lattice and a network (`line_network`)

- **Nodes** are cells on one of the grid's three diagonal sublattices: cells two apart across a corner, all with the same (q - r) mod 3. As lattice coordinates, a node is a (1, 1) + b (-1, 2) in axial terms.
- **A link** joins two nodes on one diagonal line: (q, r) + k (2, -1), (1, 1), or (-1, 2), in either direction. A unit link is one step, 3 m. A long link runs through the nodes between and is split into unit links, its heights interpolated.
- **Heights:** each node's ground height. A run rises with them.
- **Gates:** a **gate node** opens 3 m centred on it, where the line runs straight through it. A **gate link** opens the 3 m of its middle unit link. Openings that touch merge (a gate node beside a gate link opens 4.5 m).
- **Towers:** nodes the network asks to hold one, besides those its kind's rules add.
- **Inside:** a closed network laid counter-clockwise has its inside on each run's +z side, where a wall-walk and its parapet face.

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `kind` | config | 6 kinds | Material, height, thickness, crown, towers | yes |
| `network` | config | 11 examples, or the game's own | The network to build | yes |
| `chunks` | u32 | 0 to 64 | How many chunks to split it into, by angle round its middle (0 or 1: all of it; at most one per unit link) | yes |
| `chunk` | u32 | 0 to 63 | Which chunk to build (past the last it wraps round) | yes |
| `season`, `seed`, `detail` | | | Snow on its tops; which build; stones, merlons, sides | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | Towers' banners | no: the pose only |

In the foundry, choose a network from the examples to see a kind in straights, bends, junctions, rings, paddocks, slopes, and a small ward. A planner passes its own network inline, as a game would.

## How it's built

1. **Runs.** Each unit link is a run from its node to the next, rising with the ground, less what a gate opens. By material: a palisade of pointed logs with rails behind; a battered stone or brick wall with a coping (brick with a string course, stone with a few proud stones); or an earth bank with a timber crown. On top go battlements, or a wall-walk with a battlemented parapet on the inside (a palisade's: a fighting platform on posts). Buttresses stand out at each run's middle. Runs that meet straight through a node share their end faces, so a straight wall is seamless.
2. **Joins,** by each node's arms:
   - an end gets a square pier;
   - a bend gets a pier wide enough to cover its mitre, or a tower when it turns at least `tower_turn_deg` (a lattice bend turns 60 or 120 degrees);
   - a junction (three arms or more: a T, a Y, a cross, a star) gets a tower;
   - a straight node gets a tower every `tower_every_m` along its run (never on a gate or beside one), or where the network asks.
   Towers are square or round, with arrow slits, a crenellated top or a cone or pyramid roof, and a banner that sways.
3. **Gates.** The openings, merged where they touch. The model reports each one's middle, turn, and width (`gate_positions`, `gate_yaws`, `gate_widths`, read through `draw.places`), where a game (and the planner view) spawns `gate`.
4. **Chunks.** With `chunks` above one, unit links are sorted by angle round the network's middle and split evenly. Only `chunk`'s links, and the nodes it owns, are built, so a long wall draws as a few instances.
5. **Guides for the viewer:** each node's cell outlined, the links in white, and the openings in gold.

Collision: runs, piers, and towers solid. The model's positions are where the nodes are (its origin is the lattice's), so it spawns untransformed.

## Ruins and the theme materials

- **A ruin** (`condition`): the network built, then broken down to a ragged height over its line's ground (each vertex's cut from the nearest link's height, so a ruin on a slope breaks evenly), its banners gone, weathered and overgrown, rubble beside the line in scale with the wall (`condition.h`, `ruin_over`).
- **Megalith:** standing stones every couple of metres on a low grassed bank, each leaning a little, lintels across pairs (trilithons); a pier is a great stone; a tower a capped block.
- **Drystone:** unmortared courses and a row of upright cap stones along its top.
- **Living:** willow and hawthorn trunks, a woven core with diagonals, leafy crowns along its top (bare in winter); a pier is a trunk and its crown.
- **A dome** tops a tower in corbelled courses.
- **Chunks:** only links with something to build count toward chunks, so a link a gate opens whole never leaves a chunk empty.

## Kinds

| Kind | Material | Height, thickness | Crown | Towers |
|---|---|---|---|---|
| palisade | logs | 3.8 m, 0.34 m | a fighting platform | square timber, pyramid roofs, at bends of 35 degrees or more |
| town_wall | stone | 5.5 m, 1.8 m | wall-walk, battlements | round, cone roofs, every 30 m and at bends of 25 degrees or more |
| curtain | stone | 8 m, 2.6 m | wall-walk, battlements, buttresses | square, battlemented, every 24 m and at bends |
| keep_wall | stone | 7 m, 2.2 m | wall-walk, battlements | round, cone roofs, every 26 m and at bends of 35 degrees or more |
| earth_bank | earth | 3.2 m, 1.2 m | a timber crown | none |
| brick_wall | brick | 4.5 m, 1.0 m | battlements | square, pyramid roofs, every 26 m |
| stone_circle | megalith | 3.6 m, 1.0 m | lintels across pairs (a henge) | square capped stones, every 24 m |
| drystone_wall | drystone | 2.4 m, 1.2 m | upright cap stones | round, corbelled domes, every 30 m |
| living_wall | living willow and hawthorn | 3.0 m, 1.2 m | leafy crowns | round, green cone roofs, banners |
| adobe_wall | brick (mud brick colours) | 4.5 m, 1.6 m | wall-walk, battlements | square, battlemented, banners |
| dwarf_rampart | stone | 6 m, 2.2 m | wall-walk, battlements, buttresses | square, battlemented, banners |

## Example networks

`straight` (eight links given as one), `bend_sharp` (arms 60 degrees apart), `bend_gentle` (120), `branch` (a T), `wye` (a Y), `cross` (two lines at 60 degrees), `star` (six arms), `ring` (a hexagonal enclosure of side 4 with a gate node and a gate link), `paddocks` (a ring with a wall down its middle, meeting it in T junctions), `slope` (climbing 1 m a link, then bending along the top), and `compound` (a ring with a gate and a spur out to a tower).

## Walls in the planners

What the planner view built at the town tier (2026-10-14). Each wall is the plan's lattice hexagon round each lobe of what's built (`book/planners.md`), so it has six corners where one lobe holds everything.

| Planner and wall | Nodes and links | Joins | Gates | Chunks |
|---|---|---|---|---|
| `market_town` palisade | 166 | 6 | 7 | 5 |
| `walled_town` town wall | 199 | 25 | 4 | 5 |
| `hill_keep` palisade | 155 | 6 | 4 | 4 |
| `hill_keep` keep's wall | 49 | 6 | 1 | 2 |
| `castle` curtain | 114 | 17 | 1 | 3 |
| `castle` keep's wall | 48 | 6 | 1 | 2 |

## Checked

Every kind with every example network, at high and low detail, whole and as the second of two chunks: 264 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 84 to 5,972 triangles (median 654).

Ruins and the theme kinds (2026-10-14): every kind (11) with the straight, ring, and compound networks, intact and as a ruin: 66 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 180 to 45,816 triangles (median 4,312).

## Performance

`perf_stress` quick plan, `perf/report-wall-20261014T042925Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.4 ms at p50 and 9.0 ms at p99 (a ruined living wall round the ring at low detail); 100 of 100 configurations match WebAssembly; frames 0.09 ms at p95; 800 hold the 16.7 ms budget at 2.0 ms p95 with 98.8% of builds shared. The plan **passes**. (The run before it, `report-wall-20261014T041759Z.json`, failed on a chunk left empty by a link a gate opens whole; fixed.)

## Known limits

- **Three directions.** A lattice line runs at 30, 90, or 150 degrees. A wall along any other direction has to zigzag, which is why the planner makes its walls lattice hexagons rather than tracing a curve.
- **Not in the runtime as a call.** The C API takes a config entry for `network`; a game giving its own network needs an array parameter (`PLAN.md` asks).
- **No stairs or doors.** Wall-walks have no stairs up to them, towers no doors.
