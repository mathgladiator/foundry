# The civic pipeline (`civic_v1`)

`civic_v1` builds what a settlement holds in common beyond its homes and trades: a church or chapel, a temple, a court, a town hall, a guild hall, a library, a bank, a mage's tower, a market hall; and the **centres** a square's road rings round: a well, a market cross, a fountain, a maypole, a statue, a shrine. A planner (`book/planners.md`) places civic plots near its node and a centre lot at its square's middle; its fill picks a type for each by the plot's use.

- **The pipeline:** `demo/pipelines/civic_v1.json` (draft)
- **Systems:** `civic_build_v1` (draft), and `house_animate_v1` for what moves
- **Channels:** `civic_v1_type`, `civic_v1_spec`, `civic_v1_model`, approved on 2026-10-13, `civic_v1_model` patched 2026-10-14 for its doors' cells (drafts); bones are `house_v1_bone`
- **Types:** 16 in the config table `demo/config/civic_v1_types/` (drafts)
- **Design:** `design/planner.md`, "Civic buildings"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `type` | config | 16 types | What it is, its usual size, materials, colours, and what it shows | yes |
| `size` | enum | centre, small, large, great | Its lot: centre (flower7, a square's middle), small (rect6x5), large (rect9x7), great (rect12x9). A planner gives the type's own size | yes |
| `theme` | config | 8 themes | The village's architecture (`design/planner.md`, "Themes"): colours, walls' material, a hall's roof form (or a round great hall for a round-plan theme), a church tower's dome, ornaments, moss, and fires | yes |
| `stage` | enum | planning, half, finished, ruin | Its lot staked out with its trench dug and stone and timber piled; its walls half up in scaffolding; finished; or a ruin (`condition.h`) | yes |
| `grandeur` | enum | plain, fine, grand, wondrous | How grand it is: it scales the building within its lot and adds banners, lamps, a portico, a cupola (a planner gives it by wealth); wondrous adds the exotic (below) | yes |
| `season` | enum | winter, spring, summer, fall | Snow on what faces up; a shrine's and maypole's flowers | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct buildings | yes |
| `detail` | enum | high, medium, low | Round things' sides, window sills, planks and straps | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The wind and the clock for bells, banners, ribbons, and flames | no: the pose only |
| `door_time` | event | | Seconds since its door opened (it opens, stays, and closes) | no: the pose only |

## How it's built

1. **The lot.** Brick-laid hex rectangles (rows of cells, each row starting half a cell over) or a flower, anchored at the cell nearest its middle, its front (+z) toward its road. The building stands in a rectangle inside its cells, set back a step from the front.
2. **By kind:**
   - **a hall** (court, town hall, guild hall, library): a plinth, walls (timbered walls show their timbers), rows of windows on every face (two rows when tall), a gable roof along the front, a door up steps with the emblem over it, a portico of columns with a pediment (columns, or grand), a cupola on the ridge with a clock and a bell (clock, bell, or grand);
   - **a church:** a nave toward the back with tall arched windows, an apse, a tower at the front with a belfry (its bell swings) and a spire or battlements, a clock, the door at the tower's foot;
   - **a temple:** three steps, a cella, a portico of columns with an entablature and a pediment, a dome of metal (dome);
   - **a bank:** a stone block with a rusticated foot, a cornice, a parapet, barred windows, columns by an iron-strapped door, a dome (dome);
   - **a tower:** a round tower with a cone roof and a finial, windows climbing round it (the top one glows and flickers), an annex at its foot with the door;
   - **a market hall:** an upper hall on arcades of columns, stalls under it, a cupola with a clock;
   - **a centre**, in the lot's middle: a well under its roof (its bucket swings on its rope), a market cross on three octagonal steps, a fountain's basins with water falling from the bowl, a maypole whose ribbons stream, a bronze statue on its plinth, a shrine with a candle that flickers.
3. **The emblem** over the door: a sun, scales, coins, a book, a hammer, a star, a tree, a crown, or a wheel.
4. **Lamps** by the door (lamps, or grand) and **banners** on the front (banners, or grand), never on a centre lot.
5. **Its lot's cells, box, every lot as guides** (outlined on the ground at a fixed 30 m framing), **the doorway** (a sensor just outside the door; a centre's in front of it, inside its lot), **its doors' cells** (the front-row cell nearest the door, and its front neighbours beyond the lot: a planner keeps them a clear way; a centre has none, its road all round it), **the marker**, and **collision**: walls, columns, towers, and a centre's body solid.

## Wondrous

| Kind | What wondrous adds |
|---|---|
| hall, market hall, bank | Twin round towers at the front corners, domed in the type's metal, crowned with crystals in the theme's accent |
| church | Flying buttresses down the nave, a rose window on the tower, a gilded finial |
| temple | Braziers burning either side of the steps (fire config `brazier`), a gilded figure on the pediment |
| mage's tower | A ring of crystals circling its crown on a spin bone, an orb over its point, a spirit fire burning there |
| well | A gilded dome on four columns |
| market cross, fountain, maypole, statue, shrine | A crystal finial; a gilded figure on the bowl; a gilded crown; the figure's torch burning; an eternal flame and crystals |

## Themes, fire, and stages

- **Themes** (`theme.h`): the theme's colours over the type's; its walls' material over a hall's, church's, and bank's (wattle, drystone, adobe, living wood, marble); a hall's roof by the theme's form, or for a round plan a **great roundhouse** (round walls, the theme's cone or dome, arched windows round it); a church's spire becomes a dome for dome and flat themes; ornaments by the door (standing stones, lanterns, crystals on top, dragon heads on a hall's gable ends, prayer flags, carved posts) from fine up; moss.
- **Fire points:** lamps (the theme's torch fire), a shrine's candle, a wondrous temple's braziers, a statue's torch, a shrine's eternal flame, a mage's tower's spirit fire, and 4 to 10 blaze points on a finished or ruined building (not a centre).
- **Stages** (`condition.h`): planning clears the building and stakes its outline (stakes, string, the trench, a surveyor's table, a ranging rod, stone and timber inside the plot); half cuts it to 40% of its height under scaffolding with a ladder and materials; a ruin breaks it down (as `book/house_v1.md`'s). The doorway's sensor is never taller than what stands.

## Types

| Type | Kind | Size | Shows |
|---|---|---|---|
| church | church | large | stone, a spire, a bell, a clock, a sun |
| chapel | church | small | a squat battlemented tower, a bell |
| temple | temple | great | marble, columns, a gilded dome, lamps |
| court | hall | large | a portico, a clock, scales |
| town_hall | hall | great | brick, a clock and a bell, banners, a crown |
| guild_hall | hall | large | timbered, banners, a hammer |
| library | hall | large | stone, lamps, a book |
| bank | bank | small | pale stone, columns, coins |
| mage_tower | tower | small | violet glowing windows, a star |
| market_hall | market_hall | large | timbered over stone arcades, a clock, a wheel |
| well, market_cross, fountain, maypole, statue, shrine | centres | centre | |

## Checked

Every type at every size, plain and grand, summer and winter: 256 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte and Jolt takes all collision; all but 2 stay inside the conformance and the lot's box. The two that don't are a grand market hall forced onto a centre lot (a hall's eaves overrun a 7-cell flower), which a planner never asks for: it builds every type at the type's own size. 63 to 1,584 triangles (median 404).

Themes, stages, and wondrous (2026-10-14): every type at its own size, wondrous and finished, in every theme (128), and grand at planning, half, and as a ruin (48): 176 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance and the lot's box. 100 to 13,201 triangles (median 737); 0 to 16 fire points (median 6).

## Performance

`perf_stress` quick plan, `perf/report-civic_v1-20261014T041726Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 1.0 ms at p50 and 3.7 ms at p99 (a large stone-dome library, grand, as a ruin); 100 of 100 configurations match WebAssembly; frames 0.14 ms at p95; 800 buildings hold the 16.7 ms budget at 0.3 ms p95 with 98.8% of builds shared. The plan **fails** on its settle and events checks, as before.


## Known limits

- **A type on another type's size.** Every type builds at every size, but a hall on a centre lot or a centre on a great lot looks wrong; a planner builds each type at its own size.
- **Plain shapes.** Arches are pointed triangles, domes four rings; interiors are solid.
- **It never settles** (above).
- **No levels of detail measured.**
