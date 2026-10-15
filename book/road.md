# The road pipeline (`road`)

`road` builds one road cell: a hex of paving and what stands at its edges. A planner (`book/planners.md`) lays roads out as road plots, segments of cells, and gives each cell its neighbours, the half (if any) that an edge piece takes, and the edges a fence runs along. The game builds a `road` piece per cell. A road is a way through, governed like a building: its kind is a config entry, so a lane, a high street, and a market are data, and what stands at its edges (stalls, benches, planters, crates, lamp and sign posts) comes by rule.

A road never builds a fence or a centre. Fences are `fence`'s (`book/fence.md`), on the lattice; where a fence runs along a road's edge the road keeps it clear. A cell in front of a building's door is a **strict way**: the plan marks its door edges, and nothing stands in it. A square's centre (a well, a market cross, a fountain) is a civic building (`book/civic.md`) on a centre lot, which the square's road rings.

- **The pipeline:** `demo/pipelines/road.json` (draft)
- **Systems:** `road_build` (draft), and `house_animate` for what moves
- **Channels:** `road_kind`, `road_spec`, `road_model`, approved on 2026-10-13 and patched the same day (edges only, a half, fence edges, flow, viewer guides) and on 2026-10-14 (door edges) (drafts); bones are `house_bone`
- **Kinds:** 14 in the config table `demo/config/road_kinds/` (drafts)
- **Design:** `design/planner.md`, "Roads"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `kind` | config | 14 kinds | The paving, verge, and edge rules | yes |
| `condition` | enum | intact, ruin | A ruin: its stalls, lamps, and posts fallen, weeds through its paving, overgrown (`condition.h`) | yes |
| `mask` | u32 | 0 to 262,143 | What each of the six neighbours is, 3 bits per edge in the hex directions' order (axial (1,0), (1,-1), (0,-1), (-1,0), (-1,1), (0,1): east, north-east, north-west, west, south-west, south-east): 0 road, 1 a building's front, 2 open land, 3 water, 4 wall, 5 gate, 6 a waterfront, 7 a building's side or back | yes |
| `half` | enum | e0_1 to e5_0, none | Which half of the hex the kind's edge piece takes: the half holding edges h and h + 1 whole, cut through the middles of edges h + 2 and h + 5 | yes |
| `fences` | u32 | 0 to 63 | Which edges a fence runs along (bit d for edge d, where the neighbour is open land); the road keeps them clear | yes |
| `doors` | u32 | 0 to 63 | Which edges face a building's door (bit d for edge d): a strict way. Nothing stands against them, no post on their corners, and the cell takes no half | yes |
| `role` | enum | path, bridge | A path, or a bridge over water | yes |
| `season` | enum | winter, spring, summer, fall | Snow on what faces up (the paving half-whitened), planters' flowers | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct cells | yes |
| `detail` | enum | high, medium, low | How finely the paving breaks into stones, and round things' sides | yes |
| `paving` | enum | own, none | Who draws the way's surface: own (the paving, ruts, and skirt), or none, where a terrain draws it (a map's streets: `map_ground` gives the cell the surface's material). Boards (planks, a boardwalk, a bridge's deck) are always the cell's own | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The wind and the clock for awnings | no: the pose only |

In the foundry's inspector, `mask`, `fences`, and `doors` are edited edge by edge (six choices laid out as the edges face), and the viewer draws the cell's neighbours around it.

A plan's road plot gives every cell its mask, half, and fences. Turning a cell by sixths turns all three: a piece built with mask `m` and turned `k` sixths (yaw `k` times 60 degrees) has world edge `(d + k) % 6` of `m`'s edge `d`, its half `h` becomes world half `(h + k) % 6`, and its fence and door bits turn with the mask. The foundry turns each cell to the smallest of its six turns (mask, then fences, then half), so straight streets, bends, junctions, and their stalls share builds across a town.

## The half and the flow

A road cell's **flow** is the set of edges a walker or a cart passes through: its road and gate neighbours. An edge piece may take **at most half the cell**, and it must keep the flow open:

- The half `h` holds edges `h` and `h + 1` whole; the other half holds edges `h + 3` and `h + 4` whole and half of `h + 2` and `h + 5`. Every way through those four edges stays open across the free half.
- The cell's flow loses edges `h` and `h + 1`. On a street the planner only gives a half whose two edges aren't ways (a stall backing onto a shopfront, crates against a wall), so the flow doesn't change. On a square or a market it may take ways too, in rows, so a market builds up stall by stall, as long as every cell the entrances reached before is still reached on foot and by cart (`book/planners.md`).
- The model reports its `flow`, and a plan's navigation polygon for the cell is the free half (five corners) rather than the hex.

## How it's built

1. **The paving**, by the kind's surface: dirt (with cart ruts along the flow), gravel, cobble, flagstone (each of its six triangles broken into stones of their own shade: cobble into 16, 4, or 1 by detail, flagstone into at most 4), or planks across the way. A skirt runs down its sides, so a step between cells never shows a gap.
2. **The half's edge piece**, by the kind: a market stall (a table of goods, two posts, a striped awning over the table and the cut, swaying), a bench, crates and a barrel, a planter (flowering by season), or a hand cart. It faces the free half.
3. **Each edge**, by its neighbour, within a strip along the edge, never across the way:
   - **open land:** a verge (grass, a kerb, or a ditch) as deep as the paving leaves; a fenced edge stays clear (the fence stands on the edge itself);
   - **a building's front:** a planter at its foot, by the kind's rate;
   - **a building's side or back:** a barrel and a crate against it, by the kind's rate;
   - **water:** a stone quay edge with a bollard (a kind with `quay`), or a grassy bank.
4. **Posts:** a lamp post on a corner by a building or open land, by the kind's rate per cell (its lantern flickers), never on a corner of the half or a fenced edge; a sign post at one junction cell in four (three or four ways) for kinds with `signs`, its arms pointing along the flow.
5. **A bridge** is a plank deck on posts with rails along the water.
6. **Guides for the viewer:** each neighbour's outline coloured by what it is (road grey-brown, front gold, open green, water blue, wall grey, gate cyan, waterfront teal, side orange), arrows along the flow, the half's outline and cut in orange, dashes along fenced edges, and a red double line on door edges. The game ignores them.

Every choice hashes the seed, the kind, the mask, the half, and the fences, so a cell's look doesn't change when another cell does. Collision: the half's piece, posts, crates, and a bridge's deck and rails. The paving itself isn't collision: the ground is the game's.

## Kinds

| Kind | Surface | Half | Edges and posts |
|---|---|---|---|
| lane | dirt, ruts | a hand cart | grass verge |
| street | cobble | a bench | kerb; lamps (6% of cells), crates by walls, sign posts |
| high_street | flagstone | a planter | kerb; lamps (8%), planters by fronts, sign posts |
| market | flagstone | a stall | kerb; lamps (8%) |
| square | gravel | a bench | grass verge; lamps (6%), planters |
| alley | rough cobble | crates | ditch; crates and barrels by walls |
| bridge | planks | none | rails on the water |
| quay | flagstone | crates | a stone quay and bollards on the water; lamps (8%) |
| gate | cobble | none | kerb; lamps (30%) |
| moss_path | moss stones: stepping stones in moss out along each way | a planter | grass; green lamps (4%) |
| sand_lane | sand, wind ripples, ruts | a stall | grass; oil lamps (5%) |
| boardwalk | planks on piles over mud | crates | ditch; marsh lamps (6%) |
| drystone_way | flagstone | a bench | kerb; lamps (3%) |
| elven_way | pale flagstone | a planter | grass; blue lamps (10%), planters |

Make more with `config_set` (`from` an existing kind) or in the config editor; a planner's fill maps its road classes to kinds by name.

## Checked

Every kind with seven combinations of mask, half, fences, and doors (a straight street with a stall against two fronts and a fenced open edge, a square's middle with a half across two ways, open land on four sides with four fenced edges, a gate by a wall with a half, along water and a waterfront with a half, a T with two door edges, and a stall asked for against a door, which the cell drops), as a path and as a bridge: 126 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 22 to 252 triangles (median 132).

Ruins and the theme kinds (2026-10-14): every kind with three combinations of mask, half, and fences, intact and as a ruin: 84 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 30 to 929 triangles (median 207).

## Performance

`perf_stress` quick plan, `perf/report-road-20261014T041833Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.4 ms at p50 and 2.9 ms at p99; 100 of 100 configurations match WebAssembly; frames 0.10 ms at p95; 800 cells hold the 16.7 ms budget at 0.2 ms p95 with 98.8% of builds shared. The plan **passes**.

## Known limits

- **Flat cells.** A cell is flat at its own height; a graded road steps a few centimetres from cell to cell (the planner keeps steps within the road's grade).
- **One piece per cell.** The runtime holds a market town's road as about a thousand instances; drawing a road plot as one instance (as grass fields do) would be cheaper.
- **A sign's arms are plain boards.** They point along the flow, but don't name where a way goes; that would be the game's.
- **No levels of detail measured.** `detail` drops stones and sides, but no switch sizes are measured yet.
