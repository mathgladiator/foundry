# The planner: settlements as plots

A **planner** turns a description of a settlement ("a hamlet at a crossroads", "a street village along a stream", "a village round a lake", "a walled market town at a ford with a keep on the hill") into a **plan**: the settlement's own shape as hex cells, with the height of each, and its **plots**: buildings' lots (houses, civic buildings, a square's centre, a castle's parts), road segments, waterfronts' lots, and water. Walls and gates are **tiles**, and walls and fences are **lines**: the plan says where they go, never what they are. Everything else in its shape is **open**: unreserved land the planner leaves to a plotter (`plotter.md`), which fills it with nature. The game fills the plots. It decides what stands on each (a family's cottage, a smithy, a tent while it's being built), when, and at which stage; the planner never spawns anything.

The planner is the plotter's sibling and shares its spirit: a pure, versioned system bound to a config, deterministic from a seed, and checked byte for byte against its native build. It differs where a settlement differs from a forest:

| | Plotter | Planner |
|---|---|---|
| What it places | Many small things, loosely, by density and spacing | Few large things, rigidly, by rules: roads, plots that face them, squares, waterfronts |
| The ground | The game's: the plotter reads heights | **The planner's** inside its shape: it levels plots, grades roads, and eases the rest |
| How it works | Locally: each cell makes its own candidates, so an edit only changes things near it | Whole: a plan is laid out at once, because a road network only makes sense as a whole |
| Growth | `grow` hands out placements over time | The plan is laid out once, at its final size; **tiers** open it a part at a time, so growth never moves what exists |
| What it guarantees | No overlaps, spacing, smooth enough ground | Every plot reachable from every entrance, on a navigation mesh it exports; grades a cart can climb; flat pads for buildings |
| Output | Placements: a kind with parameters, at a position | Plots (buildings, road segments, waterfronts, water), the shape's cells, entrances, wall and fence lines, a navigation mesh, and the nature ground a plotter grows in |

The two work together: **a plan is a refinement of a plotter's region.** The plan's shape is stamped into a larger spot of ground, its open land and an apron round it go to a plotter, and the plotter grows nature in every gap. So a village gathers round an open body (a green, a pond, a lake) or strings along a road, and the woods, meadows, and gardens between and round its plots are the plotter's.

This document is a design, written on 2026-10-13 from the human's request and run on stated assumptions (see "Assumptions" at the end), then reshaped the same day by the human's feedback on the first version: no mounts, only open land; the village's own shape rather than a hex; roads as governed plots with their own system and config; water and waterfronts as plots. A second round the same day, and a third on 2026-10-14, made the plan stricter about being a plan: walls and gates are only tiles and lines (built by their own systems), a road only builds what stands at its edges and keeps its flow open (an edge piece takes at most half a cell), a square's centre is a building its road rings, civic buildings and castles are systems of their own, a castle is a keep's ward (a small village inside its own wall), buildings' doors are strict ways, and walls and fences run on lines through hexagons' opposite corners, as networks of segments, hugging what they enclose. A first version of all of it is built so it can be judged by eye: the plan system `plan_settle`, ten planners, the road, waterfront, civic, castle, wall, fence, and gate pipelines, and the planner view with nature from a plotter (`book/planners.md` and the pipelines' books). The runtime's API, compiled planners, a planner stress plan, merged navigation polygons, and the city are not built, and none of it is in the current milestone.

## Vocabulary

| Term | Meaning |
|---|---|
| **Plan** | A planner's output for one settlement: its shape's cells, its plots, entrances, navigation mesh, and the nature ground for a plotter. Immutable once made. |
| **Planner** | A workspace item (`planners/<name>.json`) that binds a **plan system** to a **settlement config**, with where its lots come from and how the foundry previews it. Approved like a pipeline or a plotter. |
| **Plan system** | A kind of system: pure, with a stable input channel (the config, the size, the seed, the tier) and a stable output channel (the plan). New ways of laying out a settlement are new plan systems. |
| **Settlement config** | The data a plan system is bound to: a **scenario** (a street village, a market town, a hill keep) and its ranges: plot counts, class shares, shapes, road widths and grades, the open body, waterfronts, walls, water, margins. |
| **Shape** | The settlement's own cells: what it built, its open body, the open land within its margin, the water beside it, and (walled) everything inside the wall. Not a hex: a street village is a strip along its road, a lake village a ring round its lake. |
| **Plot** | A part of the shape for the game to fill: a **building** plot (a lot for one building, turned by sixths to face its road), a **road** plot (a segment of road cells), a **waterfront** plot (a lot part on land and part on water), or a **water** plot (a river, a lake, a pond). |
| **Open** | Shape cells no plot holds: gardens, greens, backs, the margin. Unreserved: they pass to a plotter. |
| **Entrance** | Where the settlement meets the world: a **gate** tile, where a road leaves the shape (a gate in its wall, or where its plots end along a road). Every plot must be reachable from every entrance. |
| **Wall** | A wall tile: the cells of a ring round a walled settlement (or a keep's ward: the keep's wall), crossed only at gates. A wall has no shape in the plan: it is its tiles and its line. |
| **Line** | Where a wall, the keep's wall, or a fence would go, as a network on the hex grid's diagonal lattice (`line_network`): nodes (cells two apart across a corner) and links between them along lines through hexagons' opposite corners, with gate nodes, gate links, and towers. The game builds it with `wall` or `fence`, and `gate` in a wall's openings. |
| **Door** | A building's door cells: the cells just outside its door, fixed by its footprint. A road cell there is a strict way. |
| **Road** | Cells of the road network, each with a class (high, lane, alley, square, market, bridge, quay, gate), grouped into road plots. The road system (`road`) builds each cell from its class's kind, its neighbours, its half, and its fence edges. |
| **Flow** | A road cell's edges a walker or cart passes through. A **half** is the half of a cell an edge piece (a stall, a bench, crates) takes; it never closes the flow. |
| **Centre** | A building at a square's middle (a well, a market cross, a fountain), which the square's road rings. |
| **Keep's ward** | A circle inside the keep's own wall: the keep at its middle, a bailey round it, the castle's parts round the bailey, a gate facing the town. |
| **Node** | Where roads meet and people gather: a square, a crossroads, a green, a pond, a lake. |
| **Lot** | A shape a plot can take, measured from the pipeline that fills it: a house's footprint (`house`'s, by household and shape), a waterfront's (with its wet cells), a civic building's or a centre's (`civic`'s sizes), a castle part's (`castle`'s sizes). |
| **Pad** | A building's or waterfront's single ground height. Models stand flat at y = 0 (`style.md` §2). |
| **Tier** | A step of growth (hamlet, village, town). Every plot and road has the tier it opens at; a tier's plan is always complete and reachable on its own. |
| **Navigation mesh** | Walkable polygons over the plan's roads, with areas, flags, and links, which the planner exports and checks. |

## The key scenario

1. A designer writes: *"A walled market town of about 80 households at a ford, a keep on the hill to its north, a market square by the bridge, craftsmen by the east gate, a quay with warehouses along the river."*
2. The agent picks the closest scenario config, sets its size and site, and tunes its ranges. It plays the plan in the foundry with a preview fill (houses from `house`, roads from `road`, waterfronts from `waterfront`) and nature from a plotter on the open land, until it reads right from the gate, from the square, and from above.
3. The human play tests the planner: walks in from each gate, looks along the streets, steps through the tiers, and approves it or rejects it.
4. The approved planner is compiled into the runtime. The game creates a plan from a seed, stamps its shape into its world, hands its nature ground to the biome's plotter, and fills the plots it wants as the settlement grows.

## What a plan holds

| Part | What it is |
|---|---|
| **Cells** | The shape's cells only: axial `(q, r)`, height (metres, in 5 cm steps), use (building, road, water, waterfront, wall, gate, open, keep's wall), road class, plot, tier. |
| **Plots** | Each plot's kind, cells (with each cell's height, its neighbours as road's mask, a road's role, half, flow, and fence edges), and for buildings and waterfronts its lot, anchor, turn, pad, size class (solo, family, extended, civic, castle, waterfront, centre), shape, suggested use, wealth, frontage, and how far below its pad the water lies. |
| **Entrances** | The gate tiles. |
| **Lines** | The wall's, the keep's wall's, and the fences': kind, closed or not, points, gate points, and the cells they run through or beside. |
| **Navigation** | Polygons over the roads (a hex, or a half's free half) with areas and flags, links between them only where both cells' flows cross, per tier; and the reachability report that proves it. |
| **Nature** | The ground for a plotter (`plot_regions`): the shape and an apron round it, open land available (with its heights and moisture, wetter by water), water as water, and everything built unavailable. |

The plan never names a pipeline. Plots carry kinds, classes, shapes, uses, and road classes; what goes there is the game's.

## Scenarios: ways settlements are laid out

Each scenario is a settlement config for `plan_settle`. The ranges below are the configs' starting points, assumptions to tune.

| Scenario | Pattern | Plots | Entrances | Node | Roads | Walls | Terrain, water |
|---|---|---|---|---|---|---|---|
| `hamlet` | A few farms at a crossroads | 4 to 12 | 3 road ends | A small square | Lanes from each end to the crossing | None | Gentle |
| `street_village` | Plots both sides of one long street, long lots behind (a *Straßendorf*) | 10 to 40 | 2, the street's ends | A widening halfway | One high street along the valley, back lanes | None | A valley floor |
| `green_village` | Plots around a long green with a pond (an *Angerdorf*) | 12 to 40 | 3 | The green and its pond (open: the plotter's) | A lane round the green | None | Flat |
| `round_village` | Plots like slices round a round green, one way in (a *Rundling*) | 8 to 20 | 1 | The round green (open) | A ring lane | A hedge ring | Gentle |
| `lake_village` | Waterfronts on a lake's shore, houses behind the ring lane | 10 to 30, 8 waterfronts | 1 | The lake | A ring lane a waterfront's depth back from the shore | None | Gentle |
| `market_town` | Burgage plots on the main streets, a market square where they cross, back lanes | 40 to 150 | 4 gates | The market square | Two high streets crossing, lanes behind the burgages, alleys | A palisade | Gentle |
| `walled_town` | A planned town in a round stone wall (a *bastide*) | 60 to 300 | 4 gates | A central market square | Straight high streets, a ring lane inside the wall, lanes and alleys | Stone | Flat |
| `hill_keep` | A keep's ward on the summit, a road climbing to its gate at a cart's grade, a town at the foot | 20 to 120 | 2, at the foot | The keep's bailey, the town's square | Switchbacks up, the town's streets | The keep's wall; the town's palisade | A hill |
| `castle` | A sprawling castle: a curtain round an outer ward (its parts, a chapel, a few households round a square), the keep's ward inside its own wall | 8 to 40 | 1 gatehouse | The outer square, the bailey | A high street from the gatehouse to the square and the keep's gate | The curtain; the keep's wall | Gentle |
| `river_town` | A quay with waterfronts along the river, streets up from it, a bridge | 40 to 200, 12 waterfronts | 3 roads | A square by the bridge | A quay a waterfront's depth back from the water, high streets, a bridge | None | A bank sloping to the river |
| `city` | Districts: a walled old town, markets, crafts, docks, suburbs | 300 to 3,000 | 4 to 8 gates | Several squares | Arterials, a ring road, each district's own pattern | Inner and outer walls | Any |

A scenario's rules are data, so variants are config entries. A layout the plan system can't express is a new plan system. `city` isn't built (see "Scale").

## Plots

### Building plots

Plot classes are ranges, and the first three match `house`'s households (`book/house.md`):

| Class | Cells | Lots (wide, deep, round, long, broad) | For |
|---|---|---|---|
| `solo` | 19 to 28 | `rect5x4`, `rect4x5`, `flower19`, `rect3x8`, `rect7x4` | One person: a cottage, a small shop |
| `family` | 30 to 45 | `rect6x5`, `rect5x6`, `flower37`, `rect4x9`, `rect9x5` | A family; most shops and workshops |
| `extended` | 48 to 61 | `rect8x6`, `rect6x8`, `flower61`, `rect5x11`, `rect10x6` | Several generations; the inn; a farmhouse |
| `civic` | 30 to 108 | `rect6x5`, `rect9x7`, `rect12x9` (civic's small, large, great) | A church, a court, a bank, a town hall |
| `centre` | 7 | `flower7` (civic's centre) | A well, a market cross, a fountain at a square's middle |
| `castle` | 30 to 70 | `rect6x5`, `rect8x6`, `rect10x7`, and the keep's `rect8x8` (castle's sizes) | The keep, a great hall, a chapel, ranges, a tower |

A planner's file measures its household lots from a building pipeline: for each household and shape, a config entry whose usual or alternate lot has that shape is built once, and its cells become the lot. Civic, centre, castle, and waterfront lots are measured by size the same way, each size built once from its pipeline. A building is placed by the plot's lot and turn: spawned at the plot's anchor, at its pad, turned the same, so its front (`+z`) faces the road.

### Road plots

Roads are governed plots. Each road cell has a class, and the plan groups them into segments (each square as one, the rest as runs of one class of at most 16 cells). Every cell carries its neighbours as six 3-bit edges (road, a building's front, open land, water, wall, gate, a waterfront, a building's side or back), its role (a path or a bridge), its **flow** (the edges a walker or cart passes through), its **half** (if an edge piece takes one), and its **fence edges**.

A road only builds what stands at its edges. The road system `road` builds each cell from its class's **kind** (a config entry: surface, verges, the half's piece, lamps, sign posts, planters and clutter against buildings, a quay) and the cell's mask, half, and fences:

- **The half rule.** An edge piece (a market stall, a bench, crates, a planter, a cart) takes at most half a cell: the half holding two neighbouring edges whole, cut through the middles of the edges either side. The other half keeps every way through the cell's remaining flow open. The orientation is the plan's: a half's two edges leave the cell's flow, and the plan gives a half only where it keeps every place reached that was reached before.
- **A plain road upgrades.** Every cell is a plain way first; a kind adds what stands at its edges, and the plan adds halves by each class's share. So a high street gets planters and benches against its fronts without changing its flow, and **a market is built up stall by stall**: on a square, halves may take ways too, in rows, as long as every cell is still reached from every entrance on foot and by cart.
- **Fences.** A road never builds a fence. The plan marks fence edges (runs of road edges to open land, by `fence_share`) and gives each run as a fence line; the road keeps those edges clear for `fence`.
- **Centres.** A square's centre is a building (a `civic` centre on a 7-cell lot), so the square's road rings it rather than a road cell carrying a focal.

A planner's fill maps road classes to kinds; a village's high street can be a lane, a town's a flagstone high street.

### Civic buildings, centres, and castles

- **Civic buildings** (`civic`): what a settlement holds in common: a church or chapel, a temple, a court, a town hall, a guild hall, a library, a bank, a mage's tower, a market hall. Civic plots go as near the node as they fit, the first on the largest civic lot. Types are config entries with a usual size; a planner's fill maps a plot's use to the types it allows, and its grandeur to wealth.
- **Centres** (`civic`'s centre size): a well, a market cross, a fountain, a maypole, a statue, a shrine, on a 7-cell lot at a square's middle, with a ring of square round it.
- **Castles** (`castle`): a keep's ward is a small village of its own: the keep (a great tower) at its middle, a bailey ring round it, and the castle's parts (a great hall, a chapel, barracks, stables, a kitchen, storehouses, a tower) round the bailey, backing onto the keep's wall. A castle's outer ward holds more parts beside its households, inside the curtain.

### Doors

A building's lot carries its door cells, measured from the pipeline that fills it: the cells just outside its door, fixed by the footprint so every type on the lot shares them (a house's door stands at its footprint's door cell). A lot is placed only where a door cell is a road, and a road cell in front of a door is a **strict way**: its edges toward the door are marked, nothing stands against them, and it takes no half.

### Walls, fences, and gates

A wall or fence is a network the game builds; the plan only says where.

- **The lattice.** Walls and fences run along lines through a hexagon's opposite corners: vertex to vertex through one cell, along the edge between the next two, vertex to vertex through the next. Such a line stays straight however long it runs, and lines meet at 60 and 120 degrees. Nodes are the cells on one diagonal sublattice (two apart across a corner); a link joins nodes along one line (`line_network`).
- **Networks, not lists.** A wall or fence is a set of links, so lines can branch and cross: T, Y, and X junctions, shared fences between paddocks. Gates are nodes (3 m centred on one) or links (their middle 3 m).
- **Walls hug the plan.** A metaball of what's built picks the lattice nodes a wall encloses. Because a lattice line can only run in three directions, the wall is the union of lattice hexagons round the metaball's lobes: straight sides and 120 degree corners, tight round each lobe. Where a road crosses, a gate.
- **Fences enclose yards.** A share of household plots get a yard behind them, each its lattice hexagon; neighbouring yards share their fence.
- **Walls** (`wall`): a whole network (or a chunk of it) as one model, its joins by each node's arms (an end's pier, a bend's pier or tower, a junction's tower, towers along straight runs), by kind (a palisade, a town wall, a castle's curtain, a keep's wall, an earth bank, a brick wall). It reports where its gates go.
- **Fences** (`fence`): a whole network with a post at every node and a swinging gate in each opening, by kind (pickets, rails, split rails, wattle, a hedge, a low stone wall, iron).
- **Gates** (`gate`): a gate, an arch, a gatehouse, a tower gate, or a postern, in each opening a wall reports. A gate takes the wall's own kind, so it matches any wall: its piers or towers stand just into the opening and over the wall's ends.

### Themes

A **village theme** (`village_theme`, a config table, `config/village_themes/`) sets the architecture a settlement's buildings share, so one planner can be a druids' grove or a dwarven hold without new systems: its roof form (the type's own, a gable, a hip, a cone, a dome, a flat roof with a parapet, a turf roof, or a sweeping roof with upturned eaves), its plan (rectangular, or round: a roundhouse, a beehive hut, a druid's great hall), its walls' material (adding wattle and daub, drystone, adobe, living wood, and marble to the types' own), its windows (square, round, arched, slits), what its buildings stand on (a stone base, stilts with steps, an earth mound), its ornaments (carved posts, standing stones, dragon heads on the gables, crystals, prayer flags, lanterns), its colours mixed over every type's, how much moss grows, whether smoke leaves by a smoke hole, the fires its hearths, torches, and blazes burn, and the road, wall, fence, and gate kinds that suit it.

- **Who takes it.** Houses, civic buildings, castles, and waterfronts take a `theme` parameter; a planner sets it once for everything it fills (its view's shared values apply to every pipeline that has them, by name). Roads, walls, fences, and gates are themed by their kinds: the planner's fill names them, as the theme suggests.
- **The themes:** heartland (the types' own), druidic, stone_dome, nordic, desert, elven, dwarven, marsh. The new kinds that suit them: moss stepping stones, sand, a boardwalk, and pale elven ways for roads; a henge of trilithons, drystone, living willow, adobe, and a dwarven rampart for walls (a dome on a tower); living willow, stone slabs, and reeds for fences.
- **Themed planners:** `druid_grove`, `stone_dome`, `nordic_steading`, `desert_oasis`, `elven_glade`, `dwarven_hold`, and `marsh_village`, each an existing scenario with its theme, its kinds, and its crops.

### Ruins and building

Every village system has a ruin: houses (a `ruin` stage), civic buildings and castles (a `stage`: planning, half, finished, or ruin), and waterfronts, roads, walls, fences, and gates (a `condition`: intact or ruin). A ruin is the whole building made and then broken down (each system's `condition.h`): its moving parts and lit fires gone, its roofs and floors fallen in, its walls broken off at a ragged height (a little over a tenth to three quarters of what stood), weathered and mossed, rubble about it (a wall's and a fence's beside its line, over its ground's slope). Civic buildings and castles are also built in steps: **planning** (stakes, string, the trench dug, a surveyor's table, stone and timber piled), **half** (its walls half up in scaffolding with a ladder and its materials), and **finished**. A planner makes a ruined settlement by sharing `stage: ruin` and `condition: ruin` (`ruined_keep`).

### Crops

A **crop field** (`crop`) is a plot like a building's: a lot (small rect6x4, medium rect8x6, large rect12x8) measured from the crop pipeline, fronting a road. A planner's `crop_fields` places up to that many at the settlement's edge, the farthest frontages first (use `field`), and its fill names the crops to sow (`fill.crops`). A field is one instance: its plants along its rows, each riding one of a few wind groups (`wind_groups`, 7 by default), so a field of hundreds of plants poses with a handful of wind computations and draws as one mesh. Crops are config entries (`config/crop_types/`, 16 of them) by how they grow: grain, a tall stalk, a leafy head, root tops, gourds, a pole vine, a trellis vine, or flowering bushes; a field is sown, sprouting, growing, ripe, or harvested (stubble and stooks).

## Planning, step by step

1. **The site.** A working hex sized from the plot count, the keep's ward, and a castle's outer parts (a walled town's: a circle inside its wall), natural heights by the terrain, and a river where the config says.
2. **The keep's ward,** where there is one: its circle and wall, its gate facing the town, the keep at its middle, and the bailey round it.
3. **The skeleton.** The node (a square, a green, a pond in a green, a lake with its ring lane), a centre at the square's middle, entrances on the site's rim, and least-cost roads from each entrance to the node, costing grade over the road's limit, turns, and water to bridge, and rewarding roads already laid so they merge. The keep's road runs from its gate down to the nearest road at a cart's grade, so it switches back. A river town's quay.
4. **Walls and lanes.** The wall ring, a ring lane inside it, back lanes on contours of the distance from the main roads, and alleys down to them (a lane piece's first way in is a lane, so carts reach it).
5. **Plots.** Waterfronts first, then the castle's parts round the bailey and in its outer ward, then civic plots by the node, then household plots along the roads and squares, the node's edge and high streets first, by class shares and shape weights.
6. **Pruning.** Roads stay only where something needs them: each plot's way to the node (a cart's way where there is one), the ring lanes, the quay, the keep's road, the entrance roads, and the width beside them. An unwalled settlement's entrance roads end where its plots do, at a gate tile.
7. **Zoning, wealth, and tiers,** as above.
8. **Levelling.** Squares flat; roads graded to their class's limit; each pad within 0.25 m of its road; the rest eased between what was set and its own ground.
9. **Doors, walls, fences, and a road's edges.** Strict ways at doors; each wall hugging what's built as lattice hexagons, with gates where roads cross it; fenced yards; flow; and halves, each checked against every entrance's reach.
10. **Navigation.** A polygon per road cell (a half's: its free half), linked where both flows cross and a walker steps, flagged for carts; every plot reached from every entrance at its tier, every trade by cart; a plot not reached is given up as open land (a repair).
11. **The shape, the plots, and the lines.** The shape's cells, the plots (with their cells' heights, edges, roles, halves, flows, and fences), the wall and fence lines, and the nature ground.

## The planner owns the ground

Inside its shape, a plan sets every cell's height, and the game takes those heights as given. Its open land is eased toward the natural ground, so the shape meets the world around it; the nature ground's apron carries those heights out for the plotter.

- **Heights in 5 cm steps,** like the plotter's terrain.
- **Pads are flat.** Buildings stand on them at y = 0; a waterfront's wet cells stay water.
- **Grades by road class:** high streets, bridges, gates, and quays 8% (a cart's), lanes 12 to 16%, alleys 25% (with steps).
- **Steps.** The report counts edges where levelling left neighbours more than 0.3 m apart beside something built. Retaining walls are the game's to build there (a line along them, laid like a wall, would do).

## Navigation

- **Two agents.** A walker (the hero's capsule: steps up to 0.45 m) and a cart (the cart roads only, steps within a cart's grade). Every plot is reachable by the walker from every entrance; every store, workshop, inn, civic, castle, and waterfront plot by a cart from at least one.
- **The mesh.** Each road cell is a polygon at its height (the hex, or for a cell with a half, the free half's five corners), with an area (street, square, bridge, gate) and flags (walk, cart), linked to its neighbours where both cells' flows cross and a walker steps. Merging cells into larger polygons, and off-mesh links, are for later.
- **Plots are the game's.** The guarantee ends at a plot's frontage; the game links a building's doorway once it stands.
- **The report:** plots unreached (must be 0), trades without a cart (must be 0), repairs, the longest walk, the steepest road, the highest step.

## Nature and the plotter

The plan's nature ground is `plot_regions`, so any plotter grows in it as in its own ground: trees, bushes, and flowers in the gardens, greens, and backs, reeds by the water, and the wild round the shape, and none on roads, plots, or walls. In the foundry the planner view grows a plotter of your choice on it. In a game, the biome's plotter grows the plan's ground inside its own region: the plan refines a spot of the plotter's region. A plan that crosses two biomes would be grown by both plotters, each in the part inside its own region (the game intersects each plotter's region with the plan's nature ground); this edge case isn't built.

## Determinism, versions, and saves

- **Pure.** The same config, size, seed, and tier always give the same plan, and the native build matches WebAssembly byte for byte.
- **Laid out once.** A plan is made once, when the game founds a settlement. Growth is tiers, and buildings come and go on plots.
- **Saves pin the plan.** The game saves the plan's seed and planner version, and what stands on each plot; or the plan's output whole (a town is tens of kilobytes), its choice.

## Scale: from a hamlet to a city

A hex cell is about 2.6 m². A village's shape is 800 to 2,700 cells; a market town's about 8,000; a city of 3,000 plots would be about 150,000, more than one plan should be. A `city` would be planned in two levels: a city plan lays out its walls, gates, arterials, and districts' borders; each district is then planned inside its border with the arterials as its entrances, independently and re-plannably. Planning runs once per settlement, off the frame: villages and the castle plan in about 9 to 12 ms and towns in 32 to 55 ms in WebAssembly (`book/planners.md`).

## The API

Planners would be generated into `runtime.h` like plotters (not built):

```c
foundry_plan_desc desc = {
    .planner   = FOUNDRY_PLANNER_MARKET_TOWN,
    .seed      = 0x5eed,
    .transform = grid_to_world,  /* float[16]: the hex grid in the world */
    .plots     = 80,             /* household plots at its final tier; within the scenario's range */
};
foundry_plan town;
foundry_plan_report report;     /* reachability, grades, repairs */
foundry_plan_create(rt, &desc, &town, &report);

foundry_plan_get_cells(rt, town, tier, cells, n);    /* the shape: q, r, height, use, road, plot */
foundry_plan_get_plots(rt, town, tier, plots, n);    /* buildings, road segments, waterfronts, water */
foundry_plan_get_lines(rt, town, tier, lines, n);    /* walls, the keep's wall, fences: points and gates */
foundry_plan_get_navmesh(rt, town, tier, &navmesh);
foundry_plan_get_nature(rt, town, tier, &regions);   /* plot_regions for the biome's plotter */

/* Fill a building plot, a road cell, a waterfront. */
foundry_house_spawn(rt, &house_params, plots[i].transform);
foundry_road_spawn(rt, &(foundry_road_params){.kind = FOUNDRY_ROAD_KIND_MARKET, .mask = canonical_mask, .seed = 1}, cell_transform);
foundry_waterfront_spawn(rt, &(foundry_waterfront_params){.type = FOUNDRY_WATERFRONT_TYPE_PIER, .drop = plots[j].drop}, plots[j].transform);

/* Build a wall's network in chunks, and a gate in each opening it reports. */
for (uint32_t c = 0; c < chunks; ++c) {
  foundry_instance w = foundry_wall_spawn_network(rt, &(foundry_wall_params){.kind = FOUNDRY_WALL_KIND_TOWN_WALL, .chunks = chunks, .chunk = c},
                                                     &lines[k].network, identity);
  for (gate in foundry_wall_gates(rt, w))  /* the model's gate_positions, gate_yaws, gate_widths */
    foundry_gate_spawn(rt, &(foundry_gate_params){.type = FOUNDRY_GATE_TYPE_TOWN_GATE, .wall = FOUNDRY_WALL_KIND_TOWN_WALL, .width = gate.width}, gate.transform);
}
```

The plan holds no instances, as a region holds none.

## In the foundry

Planners are a workspace primitive, `planners/<name>.json`: the plan system, the config, where the lots come from (`lots.from`: a building pipeline and its config table; `lots.waterfront`, `lots.civic`, `lots.castle`: a pipeline and its sizes), the preview fill (`fill.buildings`, `fill.civics`, `fill.castles`, `fill.roads` with its class-to-kind map, `fill.waterfronts`, and `fill.lines`: the line system and each line kind's pipeline, kind, and gates), and the view's start (seed, plots, tier, the plotter for nature). The host names no pipeline: all of these are data in the file.

The planner view shows the shape filled (or coloured by use, height, tier, or wealth) with the systems a game would use, and nothing else built: walls and gates as flat, outlined tiles, or laid on their lines with walls, fences, and gates; nature from a plotter on its open land and apron, plot outlines (gold; waterfronts blue; red where unreached), gate tiles, and the navigation mesh; the inspector sets the seed, plot count, tier, and every config field, and shows the report. Play walks it. MCP: `planner_get`, `planner_set`, `planner_plan`, `planner_view`.

## What this adds

- **Channels** (approved 2026-10-13): `plan_config`, `plan_request`, `plan_plan`, `plan_cell`, `plan_plot`, `plan_line`; `road_kind`, `road_spec`, `road_model`; `waterfront_type`, `waterfront_spec`, `waterfront_model`; `civic_type`, `civic_spec`, `civic_model`; `castle_part`, `castle_spec`, `castle_model`; `wall_kind`, `wall_spec`, `wall_model`; `fence_kind`, `fence_spec`, `fence_model`; `gate_type`, `gate_spec`, `gate_model`; `line_network` (2026-10-14, replacing the first version's line request, piece, and layout).
- **Systems:** `plan_settle`, and the builders `road_build`, `waterfront_build`, `civic_build`, `castle_build`, `wall_build`, `fence_build`, `gate_build` (all animated by `house_animate`).
- **Pipelines:** `road`, `waterfront`, `civic`, `castle`, `wall`, `fence`, `gate`, with config tables of road kinds, waterfront types, civic types, castle parts, wall kinds, fence kinds, and gate types.
- **Runtime (not built):** plans beside regions, compiled planners, the API above (with a network as an array parameter and a model's places read back), drawing a road plot as one instance.
- **The foundry:** planners as a workspace primitive, the planner view with nature, MCP tools, and the books.

## Assumptions

Run with on 2026-10-13, to be confirmed or corrected in review:

1. **The plan is the whole settlement, laid out once,** and growth is by tiers that open parts of it.
2. **The planner owns the heights inside its shape;** the plotter and the game's terrain meet it at its apron.
3. **Plots are flat pads,** and a road cell is flat at its own height.
4. **Roads run on hex lines,** cell to cell; no road cuts across cells at an arbitrary angle.
5. **Plot sizes come from the buildings that fill them:** `house`'s households, and `waterfront`'s, `civic`'s, and `castle`'s sizes.
6. **The navigation guarantee ends at the frontage.**
7. **Only plots are reserved.** Buildings, roads, waterfronts, and water are plots; walls and gates are tiles and lines; everything else is open and the plotter's. What stands at a road's edges comes with the road system's kinds (in at most half a cell); fences, walls, and gates with their own systems on the plan's lines.
8. **The walker and the cart are the agents.**
9. **One plan system first.** `plan_settle` covers every scenario but the city by config.
