# The gate pipeline (`gate`)

`gate` builds a gate in a wall's opening. A wall's network (`book/wall.md`) opens 3 m at each gate node (where the line runs straight through it) and at each gate link's middle, merging openings that touch. The wall's model reports each opening's middle, turn, and width (`draw.places`), and the gate stands there: a plan's gate tiles are where its roads cross its wall. A gate takes the **wall's own kind** as well as its type, so one type stands in any wall and matches it: its height, thickness, batter, footing, material, colours, battlements, and towers are the wall's.

- **The pipeline:** `demo/pipelines/gate.json` (draft)
- **Systems:** `gate_build` (draft), and `house_animate` for its doors, banners, and torches
- **Channels:** `gate_type`, `gate_spec` (which carries a `wall_kind`), `gate_model`, approved on 2026-10-13 (drafts)
- **Types:** 5 in the config table `demo/config/gate_types/` (drafts)
- **Design:** `design/planner.md`, "Walls, fences, and gates"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `type` | config | 5 types | How it's built, its doors, how far it rises, its roof, what it shows | yes |
| `wall` | config | 11 wall kinds | The wall it stands in | yes |
| `condition` | enum | intact, ruin | A ruin: its doors and portcullis gone, broken down, overgrown (`condition.h`) | yes |
| `width` | f32 | 0.8 to 12 m | The opening along the line (a wall model's `gate_widths`: 3 m, or 4.5 or 6 m where openings merge) | yes |
| `season`, `seed`, `detail` | | | Snow; which build; planks, straps, voussoirs | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | Banners and torches | no: the pose only |
| `door_time` | event | | Seconds since its doors opened | no: the pose only |

## How it fits the wall

In the line's frame: the line along +x, the opening from -width / 2 to width / 2, the inside toward +z. The wall's runs end at the opening's edges; the gate's piers or towers stand 3 cm into the opening and out over the wall's ends, deeper and taller than the wall, so no face of the gate lies on a face of the wall. Its passage is at least as wide as the opening less 6 cm (a postern's is a small door), and it reports the passage's clear width and height.

## How it's built

1. **By type:**
   - **a gate:** two piers of the wall's stone (great posts in a palisade, with a lintel and, on a walled palisade, a platform across), the wall's height, battlements on the piers if the wall has them;
   - **an arch:** a raised section of the wall's stone with an arched opening (its top filled above the arc in steps, the voussoirs picked out), battlements, a portcullis half raised;
   - **a gatehouse:** two towers of the wall's kind (round or square, as its towers are) either side of a vaulted passage, a battlemented floor over it, a portcullis half raised at its outer face, the type's roofs on its towers;
   - **a tower gate:** a tower over the passage, its arched window over the road, a gabled, pyramid, or battlemented top;
   - **a postern:** a small iron-strapped door in a thickened stretch of wall.
2. **Doors:** one or two leaves of planks with iron straps, hinged to swing inward by `door_time`.
3. **Banners** on its towers or piers that sway; **torches** either side of its outer face that flicker, each a fire point (fire config `torch`), and 3 blaze points.
4. **Collision:** piers, towers, and the section over the passage solid; the passage a sensor named `gateway`.
5. **Guides for the viewer:** the wall's line through it, the opening's ends, and the wall's top beyond either side.

## Types

| Type | Kind | Shows |
|---|---|---|
| wooden_gate | gate | two leaves, torches |
| town_gate | arch | a portcullis, banners, torches |
| gatehouse | gatehouse | two towers with cone roofs, a portcullis, banners, torches |
| tower_gate | tower_gate | a gabled tower, banners |
| postern | postern | one leaf |

## Gates in the planners

Gates the planner view stood in its walls' openings at the town tier (2026-10-14): `market_town` 7 (its palisade's wooden gates), `walled_town` 4 (town gates), `hill_keep` 5 (four in its palisade, the keep's gatehouse), `castle` 2 (the curtain's gatehouse and the keep's).

## Checked

Every type in every wall kind, 1.8, 3.5, and 6.5 m wide: 90 builds through `pipeline_set` with `verify_native` (2026-10-14). Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 110 to 1,188 triangles (median 468).

Ruins and the theme walls (2026-10-14): every type in every wall kind (11), 3.5 m wide, intact and as a ruin: 110 builds through `pipeline_set` with `verify_native`. Every build matches WebAssembly byte for byte, Jolt takes all collision, and it stays inside the conformance. 110 to 3,737 triangles (median 562); 3 to 5 fire points.

## Performance

`perf_stress` quick plan, `perf/report-gate-20261014T041822Z.json`, on the AMD Ryzen AI Max+ 395 with Radeon 8060S: generation 0.7 ms at p50 and 2.6 ms at p99; 100 of 100 configurations match WebAssembly; frames 0.16 ms at p95; 800 gates hold the 16.7 ms budget at 0.6 ms p95 with 98.8% of builds shared. The plan **fails** on its settle and events checks, as before.

## Known limits

- **Doors open briefly.** `door_time` opens them for a visit and closes them, as a house's; a gate held open, a portcullis raised or lowered, and a drawbridge need motions `house_bone` doesn't have (`PLAN.md` asks).
- **A palisade's tower gate** is posts and a roof rather than a timber tower.
- **It never settles** (above).
