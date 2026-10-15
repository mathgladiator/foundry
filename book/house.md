# The village building pipeline (`house`)

`house` builds a house, store, or workshop as a classic RPG village has them, seen from outside: a type (the inn, the smithy), a stage of its owner's upgrades, a season, a household (one person, a family, or several generations), which sizes its lot, one of the type's two lot shapes, and a palette (a player's colour or a village's theme) mixed over the type's colours. It moves on its own (chimney smoke, sails, a waterwheel, a forge's glow), answers the wind (signs, banners, laundry, awnings, tent flaps), and opens its door when the game says. The player meets it from the command view (its marker) or at its doorway.

- **The pipeline:** `demo/pipelines/house.json` (draft)
- **Systems:** `house_build`, `house_animate` (drafts, in `src/systems/`)
- **Channels:** the `house_*` family, six channels approved on 2026-10-13, and `house_palette`, approved the same day; patched on 2026-10-14 with a theme (`village_theme`) and a ruin stage on the spec, fire points on the model, and a drift motion on the bone (drafts, in `src/channels/`)
- **Types:** 10 in the config table `demo/config/house_types/` (drafts)
- **Palettes:** 12 in the config table `demo/config/house_palettes/` (drafts)
- **Design:** `design/systems.md`, "Village houses"; settlements that place them, `design/planner.md`

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `type` | config | 10 types | Everything that makes the smithy the smithy: kind, lot shapes, walls, roof, colours, sign, smoke, feature, yard things | yes |
| `palette` | config | 12 palettes | A colour scheme mixed over the type's colours, part by part: natural (the type's own), a player's colour (red, green, blue, purple, orange, yellow, black, white), or a village's theme (whitewash, autumn, northern) | yes |
| `theme` | config | 8 themes | The village's architecture (`config/village_themes/`; `design/planner.md`, "Themes"): roof form, plan (rectangular or round), walls' material, windows, foundation, ornaments, colours, moss, smoke hole, and fires. Heartland keeps the type's own | yes |
| `stage` | enum | construction, basic, deluxe, premium, ruin | How far its owner has come, or a ruin (a premium house fallen to wreck). Every stage stands on the same footprint | yes |
| `season` | enum | winter, spring, summer, fall | Snow and icicles, flowers, the garden, leaves, the woodpile | yes |
| `household` | enum | solo, family, extended | Who lives or works there, which sets the lot's size: one person, a couple and their children, or several generations | yes |
| `footprint` | enum | usual, alternate | The type's usual lot shape (wide, deep, round, long, or broad) or its other one, for variety along a street | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct buildings: the yard's arrangement, the phases of its moving parts | yes |
| `detail` | enum | high, medium, low | The finest level of detail the game allows | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The local wind and the clock, as for trees | no: the pose only |
| `door_time` | event | | Seconds since the door opened. The game starts it (`foundry_house_start_door`) when someone comes to the door: it swings open in 0.6 s, stays open 3 s, and closes in 0.8 s | no |

```c
foundry_house_params p;
foundry_house_default_params(&p);
p.type = FOUNDRY_HOUSE_TYPE_SMITHY;
p.household = FOUNDRY_HOUSE_HOUSEHOLD_FAMILY;
p.stage = FOUNDRY_HOUSE_STAGE_CONSTRUCTION;
foundry_instance smithy = foundry_house_spawn(rt, &p, transform);
/* ...the owner saves up... */
foundry_house_set_stage(rt, smithy, FOUNDRY_HOUSE_STAGE_BASIC);
/* the player steps up to the door */
foundry_house_start_door(rt, smithy);
```

## How it's built

```
type, palette, stage, season, household, footprint, seed, detail
        |
  house_spec ──> [house_build] ──> house_model   (drawn; footprint, doorway, marker, collision)
                          │
                          └─ bones ──> house_motion ──> [house_animate] ──> house_pose   (every frame)
                                       (+ wind, time, door_time)
```

### `house_build`: the building

1. **The lot.** The household picks the size and the type's shape picks among wide, deep, round, long (a burgage: a narrow front and a long lot), and broad (a shopfront: a very wide front on a shallow lot). `rectWxH` is H rows of W cells laid like bricks; `flowerN` is a hex of hexes. The largest rectangle inside each lot (shrunk by 0.1 m from the edge, `style.md` §2) was measured by rasterizing its cells and is kept in a table. The lot also stays inside the round footprint that holds it, so a plotter that knows only round footprints reserves no more.

   | Household | Shape | Footprint | Cells | Largest rectangle | Round footprint |
   |---|---|---|---|---|---|
   | solo | wide | `rect5x4` | 20 | 7.6 × 5.3 m | 37 |
   | solo | deep | `rect4x5` | 20 | 5.9 × 6.8 m | 37 |
   | solo | round | `flower19` | 19 | 6.6 × 4.0 m | 19 |
   | family | wide | `rect6x5` | 30 | 9.3 × 6.8 m | 61 |
   | family | deep | `rect5x6` | 30 | 7.6 × 8.3 m | 61 |
   | family | round | `flower37` | 37 | 8.3 × 7.0 m | 37 |
   | extended | wide | `rect8x6` | 48 | 12.8 × 8.3 m | 91 |
   | extended | deep | `rect6x8` | 48 | 9.3 × 11.3 m | 91 |
   | extended | round | `flower61` | 61 | 10.2 × 9.9 m | 61 |
   | solo | long | `rect3x8` | 24 | 4.1 × 11.3 m | 61 |
   | solo | broad | `rect7x4` | 28 | 11.0 × 5.3 m | 61 |
   | family | long | `rect4x9` | 36 | 5.8 × 12.8 m | 61 |
   | family | broad | `rect9x5` | 45 | 14.5 × 6.8 m | 91 |
   | extended | long | `rect5x11` | 55 | 7.5 × 15.8 m | 91 |
   | extended | broad | `rect10x6` | 60 | 16.2 × 8.3 m | 127 |

   The model carries the lot's cells, its box (`lot_min` and `lot_max`: the cells' bounds, up to the building's top), and every lot's cells as guides (`guide_cells`, `guide_tones`). The viewer draws the box as a second conformance (the box a planner's plot matches) and outlines every lot on the ground, the house's own in gold, the same shape for the other households in white, and the rest faintly, so their sizes compare; it frames 20 m for every build (`draw.frame_m`), so changing the household or shape shows the change in size.

2. **The house on its lot.** The house stands against the back of the rectangle. Its width is 72% of the rectangle's when basic, 84% deluxe, and 92% premium. Its depth runs to a front yard of 2.0, 1.8, and 1.6 m before the lot's edge, which on a flower lies past the rectangle, so a round lot's house uses the whole rectangle's depth. A house deeper than it is wide turns its gable to the street: its ridge runs front to back, and its gable gets a window in its peak. A house is never deeper than 1.25 times its width: on a long lot it keeps to the front, and the rest is its back garden.
3. **The stage.** Construction stakes out the premium house and shows:
   - a tent for each generation of the household (one, two, or three, as fit), side by side along the front, each 2.1 m wide and 2.6 m long (room to lie down), opening toward the street, with one flap rolled and tied back and the other hanging from its seam;
   - a campfire with smoke in front of them;
   - the plot marked with stakes and string, and the foundation 40 to 75% laid;
   - a scaffold, a lumber pile, a cart, and barrels.

   | Stage | What it has |
   |---|---|
   | Basic | Plank walls (log stays log) and thatch or plank shingles; a door, plain windows, a board sign; a stovepipe; up to two wall-side things |
   | Deluxe | The type's walls and roof, a chimney, framed and shuttered windows with window boxes, a porch or an awning, its real sign with its symbol, its feature, and its yard things; an extended household's second storey |
   | Premium | A stone base, a second storey where the type has `stories: 2`, lanterns, banners, and a fence along the lot's edge with a gate at the path |

4. **Walls and roof.** Walls by material:
   - **timber:** plaster between corner posts, plates, studs, and braces;
   - **plank:** battens;
   - **log:** logs crossing at the corners;
   - **stone and brick:** quoins, with proud stones on stone.

   A gable roof runs along the house's longer side (to the street on a wide house, front to back on a deep one), sloped by the type's pitch, covered by thatch (thick), shingle, slate, or tile (courses at high detail).
5. **The door and windows.** The door (2.05 m high where the storey allows) is on a hinge bone at the footprint's **door cell**: the front-row cell nearest the lot's middle (held within the house's front). So every type, stage, and seed on a lot has its door in one place, and the model reports the cells just outside it (`doors`, read through `draw.footprint.doors`), which a planner keeps a clear way and only places a lot where one of them is a road. Windows go on either side of it, one every 1.7 m or so where there is room, on the ends and the back every 2.4 to 2.8 m, and upstairs.
6. **The sign.** A board over the door, or a sign hanging from a bracket (swinging in the wind), moved above or beside an awning. It carries the type's symbol on both faces: a mug, an anvil, a loaf, a needle and thread, a potion, coins, a hammer, wheat, or a bed.
7. **The feature** (deluxe and up):
   - windmill sails on a gable end (the right end of a wide house, the back of a deep one), turning faster in the wind, bigger for a bigger household;
   - a waterwheel with its sluice and pool;
   - a weathervane on the ridge, which points where the wind blows;
   - a lean-to over a glowing forge or a workbench.

   A house with a lean-to or a waterwheel gives up some of its width to make room.
8. **Yard things**, placed on a 0.3 m grid of the lot in an order set by the seed, clear of the house, the door's path, the sign, the porch or lean-to, and each other:
   - **against the walls:** woodpile, barrels, crates, sacks, a bench; a family places another round of these, and an extended household two more;
   - **in the yard:** anvil, stall, well, cart, trough, hay, beehives, cauldron, laundry, garden.

   What doesn't fit is left out.
9. **Seasons.**
   - **Winter:** snow on everything facing up, icicles along the eaves (high detail; both sides of a deep house), and a woodpile twice the size.
   - **Spring and summer:** flowers in pots and window boxes, and the garden sprouting, then full.
   - **Fall:** orange and bronze flowers, pumpkins, and leaves in the yard.
10. **Randomness.** One stream for the layout, and a second for decoration that only some levels draw (proud stones, braces, icicles). Every level of detail therefore places the same things and gives its bones the same phases.

11. **The theme** (`theme.h`, shared with civic buildings, castles, and waterfronts): its colours mixed over the type's by its `mix`; the walls in its material (wattle and daub with a woven skirt, drystone courses and proud stones, adobe with rounded buttresses and vigas, living trunks crowned in leaf, marble with pilasters, or the types' own); a round plan as a roundhouse at the door, as wide as the body is deep, with smaller round huts either side where the lot is wide; its roof form (a gable keeps today's roof; a hip; a cone, a pyramid on a rectangle; a corbelled dome, or a dome over a flat roof; a flat roof with a parapet; turf with a log along the eaves and tufts; a sweep with a knee, flared eaves, upturned corners, and horned ridge ends); its windows (round, arched, slits) for the type's square ones; its foundation (stilts with a deck and steps lift the house 0.8 m; a mound 0.25 m); its ornaments (carved posts at the door, two or more standing stones in the yard, dragon heads on the gable peaks, crystals on the ridge, a string of prayer flags from the apex to a pole, lanterns at the door); and moss on what faces up.
12. **Fire points** (`draw.fires`; `design/submit.md`, "Fire"): the chimney's or smoke hole's smoke (the theme's hearth fire, smoke only), the forge's and oven's fire (a forge's coals in the heartland, the theme's hearth fire otherwise), lanterns' flames (the theme's torch fire), the campfire at construction, and 3 to 7 blaze points over the roof and walls that burn only while the game burns the house. The smoke is the fire system's now, drifting with the world's wind; the geometric smoke puffs are gone.
13. **A ruin:** the premium house built, then broken down (`condition.h`): its door, sign, banners, sails, and other moving parts gone, its lit fires out, its roof and floors fallen in, its walls subdivided and broken off at a ragged height from a tenth to three quarters of what stood, weathered and mossed by the theme, rubble on its lot, its collision cut to what stands.

### `house_animate`: what moves

Every motion is closed form, so any time can be shown:

| Motion | Parts | How |
|---|---|---|
| sway | hanging sign, banners, laundry, awning valance, tent flaps | Swings about the line it hangs from (the bone's axis: a bracket, a pole, a washing line, an awning's front edge, a flap's seam), so it never comes away from it. The wind across that line pushes it as a damped oscillator driven by the trees' gusts, and it flutters at its own frequency even when the wind runs along the line |
| spin | windmill sails, waterwheel, weathervane | Sails turn at 0.12 turns a second, times 0.15 + wind / 8; the wheel at its own rate; the vane turns to the wind's heading and wobbles in gusts |
| rise | a cauldron's steam | Three puffs loop, each rising, swelling, drifting downwind, and fading (chimneys' smoke is now the fire system's) |
| drift | (crops' wind groups; `book/crop.md`) | Moves downwind in the gusts, by up to its reach |
| hinge | the door | Opens 1.7 radians outward with `door_time` |
| flicker | the forge's or oven's glow, the campfire, lanterns | Pulses by a few sines |

### Collision

| Body | Purpose |
|---|---|
| The walls (a box) and the roof (a convex hull, along the ridge) | solid |
| The lean-to, waterwheel, and each tent | solid |
| Barrels, crates, woodpile, anvil, cart, well, stall, beehives, hay, trough, cauldron | solid |
| The fence, one thin box per edge | solid |
| A box in front of the door (the first tent's, during construction), named `doorway` | sensor |

## Palettes

A palette gives each part a colour and a mix: 0 keeps the type's own colour, 1 is the palette's. The parts are the roof, walls, trim (timbers and frames), door, shutters (and window boxes), accent (awnings, banners, the tent's cloth), and the sign's board. Mixing happens before anything is built, so a palette costs nothing at runtime.

| Palette | What it changes |
|---|---|
| natural | Nothing: every type in its own colours |
| red, green, blue, purple, orange, yellow, black, white | A player's colour: the roof (0.85), cloth (1), door (0.9, darker), shutters (0.9), and sign (0.6); walls and timbers stay the type's |
| whitewash | Whitewashed walls, blue doors, shutters, and cloth, pale roofs |
| autumn | Ochre walls, russet roofs, deep green doors |
| northern | Dark tarred walls, red trim and doors, turf-dark roofs |

Make more with `config_set` (`from` an existing palette) or in the config editor; every type wears every palette.

## Types

| Type | Kind | Lot shapes | Walls, roof | Sign | Feature | Smoke, glow |
|---|---|---|---|---|---|---|
| cottage | home | round, wide | timber, thatch | none | none | 0.3 |
| farmhouse | home | wide, round | log, thatch | none | windmill | 0.3 |
| inn | store | wide, deep | timber, tile, 2 storeys | hanging, mug | none (porch, banners) | 0.4 |
| general_store | store | deep, broad | plank, shingle | board, coins | none (awning, stall) | 0.2 |
| bakery | store | deep, long | brick, tile | hanging, loaf | none (awning) | 0.5, oven glow |
| smithy | workshop | wide, broad | stone, slate | hanging, anvil | lean-to forge | 1.0, forge glow |
| carpenter | workshop | wide, round | plank, shingle | board, hammer | waterwheel | 0.2 |
| tailor | store | long, deep | timber, tile, 2 storeys | hanging, needle | none (awning, laundry) | 0.2 |
| alchemist | store | round, long | stone, slate, 2 storeys | hanging, potion | weathervane | 0.25, cauldron |
| mill | workshop | wide, round | stone, thatch | board, wheat | windmill | none |

Tune them through MCP (`config_get`, `config_set`), or in the config editor, where the fields are grouped as walls and roof, sign, moving, front and yard, and doodads.

## Flow and caching

- `pipeline_flow`: every request parameter (type 10, palette 12, stage 4, season 4, household 3, footprint 2, seed, detail) reruns `build`. The wind, time, and door rerun only `animate`.
- The forest phase shared builds across instances with a cache hit rate of 98.8%, at 1.7 KB per instance.

## Checked

Every type × household × lot shape × stage × season (summer and winter), 480 builds through `pipeline_set` with `verify_native`:

- every build matches its WebAssembly build byte for byte;
- Jolt takes all collision, and it stays inside the conformance and inside the lot's box;
- every footprint radius fits its round footprint;
- 1,086 to 11,854 triangles (median 2,449); 2 to 12 bones.

The long and broad lots are covered through the five types that have them (tailor, bakery, and alchemist long; general store and smithy broad). Three palettes (red, northern, whitewash) on an extended premium inn also match natively.

| Stage | Triangles (median) |
|---|---|
| construction | 1,567 to 2,717 (2,124) |
| basic | 1,086 to 3,110 (1,628) |
| deluxe | 2,016 to 9,958 (3,437) |
| premium | 2,122 to 11,854 (4,522) |

| Household | Triangles (median) |
|---|---|
| solo | 1,086 to 6,208 (2,092) |
| family | 1,158 to 7,492 (2,590) |
| extended | 1,158 to 11,854 (3,592) |

Sheets of household by stage, and frames of the tents and an awning in a 16 m/s wind, show every cloth swinging from its seam or edge.

After the door moved to the footprint's door cell (2026-10-14), every type × household × lot shape at construction and premium, summer, 120 builds, matched natively again, with Jolt taking all collision inside the conformance and the lot's box: 1,567 to 11,854 triangles (median 2,533).

Themes, ruins, and fire (2026-10-14): every type in every theme as premium and as a ruin (a family's), and the cottage and inn in every theme at construction, basic, and deluxe for a solo and an extended household: 256 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 870 to 31,989 triangles (median 3,040); 1 to 10 fire points (median 5).

## Levels of detail

The switch sizes below were measured before the long and broad lots and palettes; palettes only recolour, and the new lots use the same parts.

Low detail drops window frames and shutters, battens, braces, quoins, icicles, roof courses, symbols, and two of three smoke puffs; round things lose sides. Bones are the same at every level. `stress_house --lod` (`perf/report-house-detail-20261013T165341Z.json`, 32 configurations, median) measured the switch sizes:

| Quality | Medium | Low | Runtime's far level |
|---|---|---|---|
| high | 256 px | 256 px | 8 px |
| medium | 256 px | 256 px | 256 px |
| low | 256 px | 256 px | 256 px |

So medium and low look like high at any size the runtime measures. Coarser levels could drop more before they show.

## Performance

`perf_stress` quick plan, `perf/report-house-20261014T041714Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S, with themes, the ruin stage, and fire points:

- **Generation:** 2.1 ms at p50, 9.0 ms at p99 (the worst, an extended household's nordic smithy as a ruin: a ruin builds the whole house, then breaks it).
- **WebAssembly reference:** 100 of 100 configurations match it.
- **Diversity:** frames 0.26 ms at p95.
- **Forest:** 800 houses hold the 16.7 ms budget at 1.0 ms p95, with 98.8% of builds shared.
- **Events:** 100 doors opening at once spike the frame by 4.5 ms over 0.6 ms.
- **Against the last report** (`report-house-20261014T025123Z.json`): generation +29%, the forest's p95 at 800 +103%, the events' spike +1388% (themes and ruins add triangles, and every house now feeds the fire system).
- **Result:** the plan **fails** on its settle checks, as before (Known limits).

## Known limits

- **A house never settles.** Its smoke, sails, wheel, and glows always move, so its pose never reports settled, and the runtime never puts it to sleep. The stress plan's checks that an event settles, and that one fired alone ends the same as one fired in bulk, assume an instance stops moving after its event. A house's final pose depends on which frames the runtime chose to pose it (it skips unseen instances), so both fail. The door's own motion ends 4.4 s after `door_time` starts. The runtime's event-over flag never rises for it (`PLAN.md` asks how to handle this).
- **The game can't read the doorway, marker, or cells yet.** They are in the model, and `pipeline_set` reports them, but the C API's instance results carry only event flags. The doorway sensor is in the Jolt world, named `doorway`, for when the runtime reports sensor contacts (a later milestone).
- **Exteriors only.** No interiors, and the door opens onto a dark wall.
- **The waterwheel has no river.** It brings its own sluice and pool, whatever the ground.
- **Round footprints only in plotters.** A plotter reserves the round footprint (`footprint_cells`, up to 127 cells for an extended household's broad lot), not the house's exact cells; a planner (`design/planner.md`) places it by its cells, turned by sixths.
