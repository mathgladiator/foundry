# The village building pipeline (`house_v1`)

`house_v1` builds a house, store, or workshop as a classic RPG village has them, seen from outside: a type (the inn, the smithy), a stage of its owner's upgrades, a season, and one of the type's two footprints. It moves on its own (chimney smoke, sails, a waterwheel, a forge's glow), answers the wind (signs, banners, laundry, awnings), and opens its door when the game says. The player meets it from the command view (its marker) or at its doorway.

- **The pipeline:** `demo/pipelines/house_v1.json` (draft)
- **Systems:** `house_build_v1`, `house_animate_v1` (drafts, in `src/systems/`)
- **Channels:** the `house_v1_*` family, six channels approved on 2026-10-13 (drafts, in `src/channels/`)
- **Types:** 10 in the config table `demo/config/house_v1_types/` (drafts)
- **Design:** `design/systems.md`, "Village houses"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `type` | config | 10 types | Everything that makes the smithy the smithy: kind, footprints, walls, roof, colours, sign, smoke, feature, yard things | yes |
| `stage` | enum | construction, basic, deluxe, premium | How far its owner has come. Every stage stands on the same footprint | yes |
| `season` | enum | winter, spring, summer, fall | Snow and icicles, flowers, the garden, leaves, the woodpile | yes |
| `footprint` | enum | usual, alternate | The type's usual footprint or its other one, for variety along a street | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct buildings: the door's place, the yard's arrangement, the phases of its moving parts | yes |
| `detail` | enum | high, medium, low | The finest level of detail the game allows | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The local wind and the clock, as for trees | no: the pose only |
| `door_time` | event | | Seconds since the door opened. The game starts it (`foundry_house_v1_start_door`) when someone comes to the door: it swings open in 0.6 s, stays open 3 s, and closes in 0.8 s | no |

```c
foundry_house_v1_params p;
foundry_house_v1_default_params(&p);
p.type = FOUNDRY_HOUSE_V1_TYPE_SMITHY;
p.stage = FOUNDRY_HOUSE_V1_STAGE_CONSTRUCTION;
foundry_instance smithy = foundry_house_v1_spawn(rt, &p, transform);
/* ...the owner saves up... */
foundry_house_v1_set_stage(rt, smithy, FOUNDRY_HOUSE_V1_STAGE_BASIC);
/* the player steps up to the door */
foundry_house_v1_start_door(rt, smithy);
```

## How it's built

```
type, stage, season, footprint, seed, detail
        |
  house_v1_spec ──> [house_build] ──> house_v1_model   (drawn; footprint, doorway, marker, collision)
                          │
                          └─ bones ──> house_v1_motion ──> [house_animate] ──> house_v1_pose   (every frame)
                                       (+ wind, time, door_time)
```

### `house_build`: the building

1. **The lot.** The footprint's cells, and the largest rectangle inside them (shrunk by 0.1 m from the edge, `style.md` §2), measured once and kept in a table. The lot also stays inside the round footprint that holds it (7, 19, or 37 cells), so a plotter that knows only round footprints reserves no more.

   | Footprint | Cells | Largest rectangle | Round footprint |
   |---|---|---|---|
   | `flower7` | a cell and its 6 neighbours | 3.3 × 3.3 m | 7 |
   | `rect3x2` | 2 rows of 3, brick-laid | 4.2 × 2.3 m | 19 |
   | `rect3x3` | 3 rows of 3 | 4.2 × 3.8 m | 19 |
   | `rect4x3` | 3 rows of 4 | 5.9 × 3.8 m | 19 |
   | `rect5x4` | 4 rows of 5 | 7.6 × 5.4 m | 37 |
   | `flower19` | radius 2 | 6.8 × 3.9 m | 19 |

2. **The stage.** The house stands at the back of the lot, its yard in front. Its share of the rectangle grows with the stage: 68% basic, 80% deluxe, 90% premium. Construction stakes out the premium house's 90% and shows:
   - the owner's A-frame tent (its flap swaying) and a campfire with smoke;
   - the plot marked with stakes and string, and the foundation 40 to 75% laid;
   - a scaffold, a lumber pile, a cart, and barrels.

   | Stage | What it has |
   |---|---|
   | Basic | Plank walls (log stays log) and thatch or plank shingles; a door, plain windows, a board sign; a stovepipe; up to two wall-side things |
   | Deluxe | The type's walls and roof, a chimney, framed and shuttered windows with window boxes, a porch or an awning, its real sign with its symbol, its feature, and its yard things |
   | Premium | A stone base, a second storey where the type has `stories: 2`, lanterns, banners, and a fence along the lot's edge with a gate at the path |

3. **Walls and roof.** Walls by material:
   - **timber:** plaster between corner posts, plates, studs, and braces;
   - **plank:** battens;
   - **log:** logs crossing at the corners;
   - **stone and brick:** quoins, with proud stones on stone.

   A gable roof runs along the long side, sloped by the type's pitch, covered by thatch (thick), shingle, slate, or tile (courses at high detail).
4. **The door and windows.** The door is on a hinge bone, placed by the seed. Windows go on either side of it where there is room, on the ends and the back, and upstairs.
5. **The sign.** A board over the door, or a sign hanging from a bracket (swaying in the wind), moved above or beside an awning. It carries the type's symbol on both faces: a mug, an anvil, a loaf, a needle and thread, a potion, coins, a hammer, wheat, or a bed.
6. **The feature** (deluxe and up):
   - windmill sails, turning faster in the wind;
   - a waterwheel with its sluice and pool;
   - a weathervane, which points where the wind blows;
   - a lean-to over a glowing forge or a workbench.

   A house with a lean-to or a waterwheel gives up some of its width to make room.
7. **Yard things**, placed on a 0.3 m grid of the lot in an order set by the seed, clear of the house, the door's path, the sign, the porch or lean-to, and each other:
   - **against the walls:** woodpile, barrels, crates, sacks, a bench;
   - **in the yard:** anvil, stall, well, cart, trough, hay, beehives, cauldron, laundry, garden.

   What doesn't fit is left out, so small lots have fewer.
8. **Seasons.**
   - **Winter:** snow on everything facing up, icicles along the eaves (high detail), and a woodpile twice the size.
   - **Spring and summer:** flowers in pots and window boxes, and the garden sprouting, then full.
   - **Fall:** orange and bronze flowers, pumpkins, and leaves in the yard.
9. **Randomness.** One stream for the layout, and a second for decoration that only some levels draw (proud stones, braces, icicles). Every level of detail therefore places the same things and gives its bones the same phases.

### `house_animate`: what moves

Every motion is closed form, so any time can be shown:

| Motion | Parts | How |
|---|---|---|
| sway | hanging sign, banners, laundry, awning valance, tent flap | Bends toward the wind as a damped oscillator driven by the trees' gusts, so it moves with the trees around it |
| spin | windmill sails, waterwheel, weathervane | Sails turn at 0.12 turns a second, times 0.15 + wind / 8; the wheel at its own rate; the vane turns to the wind's heading and wobbles in gusts |
| rise | chimney, stovepipe, campfire, and cauldron smoke | Three puffs loop, each rising 1.3 to 3.1 m by the type's `smoke`, swelling, drifting downwind, and fading |
| hinge | the door | Opens 1.7 radians outward with `door_time` |
| flicker | the forge's or oven's glow, the campfire, lanterns | Pulses by a few sines |

### Collision

| Body | Purpose |
|---|---|
| The walls (a box) and the roof (a convex hull) | solid |
| The lean-to, waterwheel, and tent | solid |
| Barrels, crates, woodpile, anvil, cart, well, stall, beehives, hay, trough, cauldron | solid |
| The fence, one thin box per edge | solid |
| A box in front of the door, named `doorway` | sensor |

## Types

| Type | Kind | Footprints | Walls, roof | Sign | Feature | Smoke, glow |
|---|---|---|---|---|---|---|
| cottage | home | flower7, rect3x2 | timber, thatch | none | none | 0.3 |
| farmhouse | home | rect4x3, rect5x4 | log, thatch | none | windmill | 0.3 |
| inn | store | flower19, rect5x4 | timber, tile, 2 storeys | hanging, mug | none (porch, banners) | 0.4 |
| general_store | store | rect3x3, rect4x3 | plank, shingle | board, coins | none (awning, stall) | 0.2 |
| bakery | store | flower7, rect3x3 | brick, tile | hanging, loaf | none (awning) | 0.5, oven glow |
| smithy | workshop | rect3x3, rect4x3 | stone, slate | hanging, anvil | lean-to forge | 1.0, forge glow |
| carpenter | workshop | rect4x3, rect3x2 | plank, shingle | board, hammer | waterwheel | 0.2 |
| tailor | store | rect3x2, flower7 | timber, tile, 2 storeys | hanging, needle | none (awning, laundry) | 0.2 |
| alchemist | store | flower7, rect3x3 | stone, slate, 2 storeys | hanging, potion | weathervane | 0.25, cauldron |
| mill | workshop | rect4x3, rect5x4 | stone, thatch | board, wheat | windmill | none |

Tune them through MCP (`config_get`, `config_set`), or in the config editor, where the fields are grouped as walls and roof, sign, moving, front and yard, and doodads.

## Flow and caching

- `pipeline_flow`: every request parameter (type 10, stage 4, season 4, footprint 2, seed, detail) reruns `build`. The wind, time, and door rerun only `animate`.
- The forest phase shared builds across instances with a cache hit rate of 98.8%, at 2.3 KB per instance.

## Checked

Every type × footprint × stage × season (summer and winter), 160 builds through `pipeline_set` with `verify_native`:

- every build matches its WebAssembly build byte for byte;
- Jolt takes all collision, and it stays inside the conformance;
- every footprint radius fits its round footprint;
- 1,050 to 5,162 triangles (median 1,822); 2 to 10 bones.

| Stage | Triangles |
|---|---|
| construction | 1,385 to 1,985 |
| basic | 1,050 to 2,072 |
| deluxe | 1,106 to 3,430 |
| premium | 1,710 to 5,162 |

## Levels of detail

Low detail drops window frames and shutters, battens, braces, quoins, icicles, roof courses, symbols, and two of three smoke puffs; round things lose sides. Bones are the same at every level. `stress_house_v1 --lod` (`perf/report-house_v1-detail-20261013T150331Z.json`, 32 configurations, median) measured the switch sizes:

| Quality | Medium | Low | Runtime's far level |
|---|---|---|---|
| high | 256 px | 256 px | 8 px |
| medium | 256 px | 256 px | 24 px |
| low | 256 px | 256 px | 256 px |

So medium and low look like high at any size the runtime measures. Coarser levels could drop more before they show.

## Performance

`perf_stress` quick plan, `perf/report-house_v1-20261013T150557Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S:

- **Generation:** 1.37 ms at p50, 2.45 ms at p95 (the worst, a premium inn on its alternate footprint, 2.9 ms).
- **WebAssembly reference:** 100 of 100 configurations match it; 20 of 20 are deterministic, the same on 10 threads.
- **Diversity:** frames 0.12 ms at p50 and 0.17 ms at p95; 1,101 triangles at p50 and 2,660 at p95.
- **Forest:** 800 houses (699,000 triangles, 3,040 collision bodies) hold the 16.7 ms budget at 0.30 ms p95, with the GPU at 0.46 ms p50 and 82 MB resident.
- **Events:** 100 doors opening at once spike the frame by 0.3 ms.
- **Result:** the plan **fails** on events: its settle check, and two of three "same as alone" checks (see the next item).

## Known limits

- **A house never settles.** Its smoke, sails, wheel, and glows always move, so its pose never reports settled, and the runtime never puts it to sleep. The stress plan's checks that an event settles, and that one fired alone ends the same as one fired in bulk, assume an instance stops moving after its event. A house's final pose depends on which frames the runtime chose to pose it (it skips unseen instances), so both fail. The door's own motion ends 4.4 s after `door_time` starts. The runtime's event-over flag never rises for it (`PLAN.md` asks how to handle this).
- **The game can't read the doorway, marker, or cells yet.** They are in the model, and `pipeline_set` reports them, but the C API's instance results carry only event flags. The doorway sensor is in the Jolt world, named `doorway`, for when the runtime reports sensor contacts (a later milestone).
- **Exteriors only.** No interiors, and the door opens onto a dark wall.
- **Small lots are sparse.** On `flower7` and `rect3x2`, few yard things fit beside a deluxe or premium house.
- **The waterwheel has no river.** It brings its own sluice and pool, whatever the ground.
- **Round footprints only in plotters.** A plotter reserves the round footprint (`footprint_cells`), not the house's exact cells.
