# Planners: settlements as plots (`plan_settle_v1`)

A planner lays out a settlement (a hamlet, a street village, a village round a green or a lake, a market town, a walled town, a castle town on a hill, a river town, a castle) as a **plan**: the settlement's own shape as hex cells with their heights, and its **plots**: buildings' lots (houses, civic buildings, a square's centre, castle parts), road segments, waterfronts' lots, and water. Walls and gates are **tiles** and **lines**: the plan marks the cells a wall stands on and the cells of its gates, and gives each wall and fence as a network on the hex grid's diagonal lattice (`book/wall_v1.md`). A building's door is a **strict way**: the road cell in front of it is kept clear. Everything else is **open** and passes to a plotter, which grows nature in it.

A plan is a draft for the game to fill. The foundry previews the fill with the same systems a game would use: `house_v1`, `civic_v1`, `castle_v1`, `road_v1`, and `waterfront_v1` on plots, and `wall_v1` and `fence_v1` on lines, with `gate_v1` where a wall's model asks for its gates. A plot no fill names is only outlined; nothing in the view is built that isn't one of those.

- **The plan system:** `plan_settle_v1` (draft, `src/systems/plan_settle_v1/`)
- **Channels:** `plan_v1_config`, `plan_v1_request`, `plan_v1_plan`, `plan_v1_cell`, `plan_v1_plot`, and `plan_v1_line`, approved on 2026-10-13 and patched the same day (no mounts; shapes, open land, roads, waterfronts, water, nature; then centres, keep wards and castle parts, halves, flows, fences, lines, and a castle scenario) and on 2026-10-14 (lots' doors and road cells' door edges; lines as lattice networks; walls' reach and yards' depth) (drafts)
- **Planners:** ten, in `demo/planners/` (drafts)
- **Design:** `design/planner.md`; what fills them, `book/house_v1.md`, `book/civic_v1.md`, `book/castle_v1.md`, `book/road_v1.md`, `book/waterfront_v1.md`, `book/wall_v1.md`, `book/fence_v1.md`, `book/gate_v1.md`

Planners aren't in the native runtime or the C API yet: the plan system builds natively (and matches WebAssembly byte for byte), but `foundry_plan_*` is still a design.

## A planner's file

| Part | What it holds |
|---|---|
| `config` | The scenario and its rules, as `plan_v1_config`: plot range and density, terrain, entrances, road widths and grades, block depth, alleys, the node's size and whether a centre stands in it, the open body, the margin of open land, class shares, shape weights, civic plots, the keep's ward (its radius, its parts) and a castle's outer parts, waterfronts, walls and how far they reach round what's built, the river, each road class's share of halves, and the share of plots with a fenced yard and its depth |
| `lots` | Where plots' shapes come from. `from` measures household lots from a building pipeline (each household and shape built once, its `draw.footprint.lot` cells read). `waterfront`, `civic`, and `castle` measure lots by size from their pipelines (each size built once, with the entries of that size as its builds; `civic`'s centre size is the centre class). Every lot's door cells come from its build too (`draw.footprint.doors`) |
| `fill` | The preview: `buildings`, `civics`, `castles`, `roads`, `waterfronts` (each its pipeline and which types suit each use), and `lines`: for each line kind (`wall`, `keep_wall`, `fence`) a pipeline and kind (a round village's hedge is `fence_v1`), the gates' pipeline and type for each wall, and `chunk_links` (a long line is built in chunks of about that many links) |
| `view` | The seed, plot count, tier, shared values (season, wind), and the plotter that grows nature (`nature`) |

The host names no pipeline: the lots' sources and the fill are data in the file. A line's network goes to its pipeline inline as the `network` config parameter, as a game's own would.

## How a plan is made

`plan_settle_v1` takes a `plan_v1_request` (the config with its lots, a seed, a plot count, a tier) and returns a `plan_v1_plan`:

1. **The site.** A working hex sized from the plot count, the keep's ward, and a castle's outer parts (a walled settlement's: a circle inside its wall), natural heights by the terrain (flat, gentle, hill, valley, bank), and a river band where the config says, a little below its banks.
2. **The keep's ward** (a hill keep, a castle): a circle round the summit (a castle's: the north of its site), its rim the keep's wall, a gate on the side facing the town. The keep (the largest castle lot) stands at its middle, its front toward the gate, and a ring of square two cells wide round it is the bailey, so the keep stands as a square's centre does.
3. **The skeleton.** The node: a square, a green (long in a green village, round in a round village) with a pond in its lowest spot in the green village, or a lake. A ring lane goes right round a green, and a waterfront's depth (three cells) back from a lake's shore. With `centre`, the largest centre lot that leaves a ring of square round it stands at the square's middle, so the square's road rings it. Entrances on the site's rim, and least-cost roads from each to the node, costing grade over the road's limit, turns (a lot, in a walled town, so its streets run straight), and water (a bridge), and rewarding roads already laid so they merge. The keep's road runs from its gate down to the nearest road at a cart's grade, and inside from the gate to the bailey. A river town's quay runs three cells back from the water.
4. **Walls and lanes.** A wall ring round a walled settlement's circle, with gates where roads cross it, and a ring lane inside a palisade or stone wall. Back lanes on contours of the distance from the main roads, every `block_cells`, with alleys down to them every `alley_every`; the first way into each piece of a lane is a lane, so carts reach it.
5. **Plots.**
   - **Waterfronts** first, nearest the node: a waterfront lot (small, medium, or large) where its wet cells all lie on water, its dry cells on open land, and its back on a road a cart can use.
   - **Castle parts** round the bailey, inside the ward only, nearest the keep first, each preferring the next size down (never the keep's lot); on a hilltop their ground may span 6 m, which levelling terraces.
   - **A castle's outer parts** along its roads, outside the ward.
   - **Civic plots** as near the node as they fit: the first the largest civic lot, then smaller.
   - **Household plots** along the roads and squares (the node's edge and high streets first), never in the ward: the class furthest behind its share, a shape by the weights, every lot, turn, and anchor that puts the lot's front (past its front row) against a road or square along at least two cells and half its front, with one of its door cells on a road, scored by frontage, snugness, backs off roads, and even ground. Every placed lot (civic, castle, keep) has a door on a road the same way.
6. **Pruning.** Roads stay only where something needs them: each plot's way to the node (by cart roads where there is one), every road a plot fronts, the ring lanes, the quay, the keep's road, the entrance roads, and the width beside them. An unwalled settlement's entrance roads are cut back to the first cell within three of a plot, which becomes the gate tile.
7. **Zoning, wealth, and tiers.** The inn (two in a town) on the square or high street; stores on the square and near high street; workshops by the entrances; farms at a village's edge; homes elsewhere; a trade always fronts a road a cart can use; waterfronts are harbours. Wealth falls with distance along the roads (and rises with height on a hill). Tiers by distance: the nearest 30% (at least three, and every civic plot, centre, keep, and castle part) at the hamlet tier, to 65% at the village tier, the rest at the town tier.
8. **Levelling.** Squares flat; roads graded to their class's grade (high streets, bridges, gates, and quays 8%, lanes 12 to 16%, alleys 25%); each pad within 0.25 m of its road; the rest eased between; heights in 5 cm steps.
9. **A road's edges and the lines.** Every road cell's **flow** is its edges to road and gate neighbours open at the tier.
   - **Doors.** A road cell in front of a plot's door (its lot's door cells) is a strict way: its edges toward the plot are marked, and it takes no half.
   - **Walls.** The ring laid round a walled site (to bring roads to gates while planning) and a keep's ward's ring are cleared, and each wall is laid to hug what's built. A metaball of the plots, squares, and roads near plots (each adds (1 - (d / R)^2)^2 within `wall_reach_cells`), at least three cells round every plot, picks the lattice nodes inside. A lattice boundary can only run at 30, 90, and 150 degrees, so a tight trace would zigzag. The metaball is split into lobes (what survives eroding it two lattice steps, in connected pieces), each lobe becomes its lattice hexagon (grown back by the erosion), and hexagons that touch merge when the merged one is at most 35% bigger. The wall is the boundary of their union, counter-clockwise: straight sides and 120 degree corners. Its nodes on open land become wall tiles. Where a road crosses it (a road node, or a link whose two gap cells are road), a gate tile. Entrance roads run on outward until they are past the wall, so they pass through a gate.
   - **Fences.** A share of household plots (`fence_share`) get a yard: the open land within `fence_cells` of the plot, each cell the nearest plot's. Each yard becomes its lattice hexagon; where yards overlap, the earlier plot's wins. The boundary of each plot's region is its fence, so neighbours share the fence between them and meet in T junctions. A fence never runs through a lot or a road, nor across one: there it stops, so a yard's front stays open to its street. Each connected piece is one fence line.
   - **Halves.** By its class's share, a road cell's edge piece takes half the cell (`book/road_v1.md`, "The half and the flow"), never on a door cell. Off a square it takes only edges the flow doesn't use, preferring halves that back onto buildings or water. On a square or market it may also take ways through, in rows facing one way, if every cell each entrance reached before (on foot and by cart, within the walker's and the cart's step) is still reached; the two edges leave the flow.
10. **Navigation.** A polygon per road cell at the tier (a half's cell: the free half, five corners), linked where both cells' flows cross and a walker steps (0.45 m), flagged for carts. From every entrance, the walk to every plot's frontage, and for carts from any entrance. A plot not reached from every entrance is given up as open land (a repair).
11. **The plan.** The shape (what it built at the tier, its open body, open land within `margin_cells`, the water within two cells, and a walled settlement's whole inside); the plots (buildings, centres, castle parts, and waterfronts, then road segments: each square as one, the rest as runs of one class of at most 16 cells, then water bodies), each road cell with its height, edges (road_v1's mask), role, half, flow, fences, and door edges; the lines (each wall, the keep's wall, and each fence as a lattice network with its gates); and the nature ground: the shape and a six-cell apron, open land available (moister nearer water), water as water, everything built a path.

Everything is deterministic: every walk over cells is in index order, every sort has a total order, and each step draws from its own random stream.

## The planners

Each planner as it opens, at the town tier (`planner_plan` with `verify_native`, 2026-10-14; the plan's time is the middle of three runs):

| Planner | Household plots | Civic | Centre | Keep + castle parts | Waterfronts | Fields | Road cells | Halves | Shape, cells | Area, ha | Entrances | Longest walk, m | Steepest road | Unreached, no cart, repairs | Plan, ms | Native |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `hamlet_v1` | 8 | 0 | 0 | 0 + 0 | 0 | 3 | 163 | 9 | 1,199 | 0.31 | 3 | 42 | 9% | 0, 0, 0 | 8.2 | matches |
| `street_village_v1` | 24 | 1 | 1 | 0 + 0 | 0 | 4 | 323 | 24 | 2,338 | 0.61 | 2 | 83 | 12% | 0, 0, 0 | 12.6 | matches |
| `green_village_v1` | 24 | 1 | 0 | 0 + 0 | 0 | 3 | 294 | 19 | 2,097 | 0.54 | 3 | 87 | 12% | 0, 0, 0 | 14.6 | matches |
| `round_village_v1` | 12 | 0 | 0 | 0 + 0 | 0 | 0 | 80 | 4 | 1,562 | 0.41 | 1 | 66 | 6% | 0, 0, 0 | 11.6 | matches |
| `lake_village_v1` | 18 | 0 | 0 | 0 + 0 | 8 | 0 | 101 | 8 | 1,296 | 0.34 | 1 | 80 | 3% | 0, 0, 0 | 9.6 | matches |
| `market_town_v1` | 70 | 2 | 1 | 0 + 0 | 0 | 0 | 982 | 57 | 6,886 | 1.79 | 4 | 120 | 9% | 0, 0, 0 | 39.2 | matches |
| `walled_town_v1` | 100 | 3 | 1 | 0 + 0 | 0 | 0 | 1,471 | 76 | 10,163 | 2.64 | 4 | 165 | 3% | 0, 0, 0 | 52.3 | matches |
| `hill_keep_v1` | 50 | 1 | 1 | 1 + 4 | 0 | 0 | 921 | 55 | 7,239 | 1.88 | 2 | 689 | 26% | 0, 0, 0 | 69.9 | matches |
| `river_town_v1` | 70 | 2 | 1 | 0 + 0 | 12 | 4 | 1,165 | 55 | 5,220 | 1.36 | 3 | 184 | 12% | 0, 0, 0 | 38.2 | matches |
| `castle_v1` | 16 | 1 | 1 | 1 + 8 | 0 | 0 | 315 | 38 | 3,644 | 0.95 | 1 | 114 | 14% | 0, 0, 0 | 18.5 | matches |
| `druid_grove_v1` | 12 | 0 | 0 | 0 + 0 | 0 | 0 | 80 | 4 | 1,562 | 0.41 | 1 | 66 | 6% | 0, 0, 0 | 13.2 | matches |
| `stone_dome_v1` | 8 | 0 | 0 | 0 + 0 | 0 | 3 | 163 | 9 | 1,199 | 0.31 | 3 | 42 | 9% | 0, 0, 0 | 7.9 | matches |
| `nordic_steading_v1` | 24 | 1 | 1 | 0 + 0 | 0 | 4 | 323 | 24 | 2,338 | 0.61 | 2 | 83 | 12% | 0, 0, 0 | 12.3 | matches |
| `desert_oasis_v1` | 18 | 0 | 0 | 0 + 0 | 8 | 0 | 101 | 8 | 1,296 | 0.34 | 1 | 80 | 3% | 0, 0, 0 | 12.6 | matches |
| `elven_glade_v1` | 24 | 1 | 0 | 0 + 0 | 0 | 2 | 270 | 19 | 1,983 | 0.52 | 3 | 85 | 12% | 0, 0, 0 | 14.9 | matches |
| `dwarven_hold_v1` | 70 | 2 | 1 | 0 + 0 | 0 | 0 | 982 | 57 | 6,886 | 1.79 | 4 | 120 | 9% | 0, 0, 0 | 39.5 | matches |
| `marsh_village_v1` | 17 | 0 | 0 | 0 + 0 | 8 | 0 | 98 | 8 | 1,217 | 0.32 | 1 | 78 | 3% | 0, 0, 0 | 10.9 | matches |
| `ruined_keep_v1` | 50 | 1 | 1 | 1 + 4 | 0 | 2 | 1,050 | 52 | 8,709 | 2.26 | 2 | 289 | 26% | 0, 0, 0 | 73.9 | matches |

Halves and lines in the same runs (what the planner view built; a round village's wall is its hedge, a fence network, so its gates are the fence's own; the druids' grove's is a henge, `wall_v1`):

| Planner | Halves by class | Lines |
|---|---|---|
| `hamlet_v1` | lane 9 | 3 fence networks (44 links) |
| `street_village_v1` | high 13, lane 8, square 3 | 15 fence networks (111 links) |
| `green_village_v1` | high 15, lane 4 | 16 fence networks (131 links) |
| `round_village_v1` | high 4 | its wall (71 links, 6 joins, 0 gates); 6 fence networks (57 links) |
| `lake_village_v1` | high 5, lane 3 | 8 fence networks (100 links) |
| `market_town_v1` | high 25, market 21, lane 11 | its wall (166 links, 6 joins, 7 gates); 23 fence networks (125 links) |
| `walled_town_v1` | high 38, market 21, lane 17 | its wall (199 links, 25 joins, 4 gates) |
| `hill_keep_v1` | high 22, square 20, market 13 | its wall (155 links, 6 joins, 4 gates); the keep's wall (49 links, 6 joins, 1 gates); 18 fence networks (92 links) |
| `river_town_v1` | high 24, quay 20, market 11 | 29 fence networks (136 links) |
| `castle_v1` | square 21, market 11, high 6 | its wall (114 links, 17 joins, 1 gates); the keep's wall (48 links, 6 joins, 1 gates) |
| `druid_grove_v1` | high 4 | its wall (71 links, 11 joins, 1 gates); 6 fence networks (57 links) |
| `stone_dome_v1` | lane 9 | 3 fence networks (44 links) |
| `nordic_steading_v1` | high 13, lane 8, square 3 | 15 fence networks (111 links) |
| `desert_oasis_v1` | high 5, lane 3 | 8 fence networks (100 links) |
| `elven_glade_v1` | high 15, lane 4 | 15 fence networks (140 links) |
| `dwarven_hold_v1` | high 25, market 21, lane 11 | its wall (166 links, 18 joins, 7 gates); 23 fence networks (125 links) |
| `marsh_village_v1` | high 5, lane 3 | 8 fence networks (74 links) |
| `ruined_keep_v1` | high 23, square 18, market 11 | its wall (175 links, 6 joins, 7 gates); the keep's wall (49 links, 6 joins, 1 gates); 20 fence networks (110 links) |

A walled settlement's shape is the inside of its wall; an unwalled one's is its plots and roads with a margin, so a street village is a strip along its street and a lake village a ring round its lake.

## Themes, fields, and ruins

- **A theme** is a shared value: `view.shared.theme` names a `config/village_v1_themes/` entry, and the planner view sets it on every fill pipeline that has a `theme` parameter (houses, civic buildings, castles, waterfronts). Shared values now go only to the pipelines that have them, by name. The fill names the roads', walls', fences', and gates' kinds to suit it. The themed planners (`druid_grove_v1`, `stone_dome_v1`, `nordic_steading_v1`, `desert_oasis_v1`, `elven_glade_v1`, `dwarven_hold_v1`, `marsh_village_v1`) are existing scenarios with a theme, its kinds, and its crops; the elven glade's civic buildings are wondrous by wealth.
- **Crop fields:** `config.crop_fields` (at most that many), `lots.field` (measured from `crop_v1`'s sizes, class `field`), and `fill.crops` (the pipeline, the crops to sow, and their growth). The plan places them after its households at the farthest frontages, use `field` (`book/crop_v1.md`).
- **Ruins:** `ruined_keep_v1` shares `stage: ruin`, `condition: ruin`, and `growth: harvested`, so every house, civic building, castle part, wall, gate, fence, road cell, and field is a ruin or stubble.
- **Fire:** the planner view gathers every placed build's fire points, so a planner's hearths' smoke, torches, and lamps show (the wind is the view's shared `wind_speed` and `wind_heading`).

## In the foundry

Select a planner under **Planners**. The viewport shows the plan filled; the inspector sets:

- **Plan:** the seed, the plot count (within the scenario's range), and the tier.
- **Show:** the fill (houses, civic buildings, castle parts, road pieces, waterfronts); walls, fences, and gates (built on the plan's lattice networks; off, walls and gates are only their tiles, flat and outlined); nature, and which plotter grows it; plot outlines (gold, waterfronts blue, red where unreached); the navigation mesh; collision; colour by use, height, tier, or wealth; the season and wind the fill shares.
- **Report:** whether every plot is reached and every trade by cart, the counts above, halves and fenced edges, each line built (nodes, links, joins, gates, chunks), what was drawn, and the plan's time. **Plan and check native** runs the plan system natively too and compares.
- **Rules:** every field of the config, from the `plan_v1_config` schema, to try in the view; **Save rules and view to the file**, **Revert rules**.
- **Lots:** each lot, its cells, and the types that stand on it.

Through MCP: `planner_get`, `planner_set` (settings, `lines`, `nature`, config fields to try, `save`), `planner_plan` (`verify_native`, and `plan` for the whole plan), and `planner_view`.

## Known limits

- **Not in the runtime yet.** No `foundry_plan_*`, no compiled planners, and no planner stress plan; numbers here come from `planner_plan` in the foundry.
- **A navigation polygon per road cell,** not merged into larger polygons, and no off-mesh links.
- **A city is not planned.** It needs a second level (districts planned inside a city plan).
- **The hill keep's climb.** Its hill rises 16 m; the keep's road switches back up it, so the longest walk is 689 m and its alleys climb at up to 26%.
- **Tiers by distance only;** a scenario's own growth order isn't modelled.
- **Many road instances.** Each road cell is its own instance; drawing a road plot as one would be cheaper. Walls and fences are one instance per chunk of about 40 links.
- **Two biomes.** A plan crossing two plotters' regions would be grown by each in its own part; the foundry grows one plotter over the whole plan.
- **Walls are convex per lobe.** A wall is the union of lattice hexagons round what's built, so it follows a town's lobes, not every bay in its edge.
- **Yards have no gates.** A yard's fence stops where it meets its lot or a road, so it is open at the front.
