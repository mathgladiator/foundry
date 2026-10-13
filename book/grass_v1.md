# grass_v1

`grass_v1` is the first **field** (`design/plotter.md`, "Fields"). It covers a region's cells rather than being placed in them: a meadow of several hectares is one instance, drawn from a few shared tiles. It is a draft.

**Three ideas make it cheap:**
- **A per-cell functor.** Whether a cell shows grass, and which tile, is a pure function of the cell's integers, its ground, and the world's seed.
- **Shared tiles.** A tile is one hex cell's worth of blades, shared by every cell that picks it.
- **Sway on the GPU.** The wind moves the blades in the vertex shader, so nothing is posed on the CPU.

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds? |
|---|---|---|---|---|
| `species` | config | 5 species | Everything that makes a meadow a meadow | yes |
| `season` | enum | winter, spring, summer, fall | Green, gold, straw; how tall | yes |
| `variant` | u32 | 0 to 12 | Which tile: the grass variants, then plates for water, rock, sand, path, and bare soil (it wraps past them). In a field, each cell's choice sets it | yes |
| `detail` | enum | high, medium, low, mat | The finest level the game allows; the runtime picks coarser levels by size on screen | yes |
| `cell_q`, `cell_r` | u32 | 0 to 1000 | The cell's axial coordinates (in a field, each cell's own, negative ones as their bits) | yes |
| `ground` | enum | soil, water, rock, sand, path | The cell's ground | yes |
| `moisture` | f32 | 0 to 1 | Wetter grass grows taller and greener | yes |
| `world_seed` | u32 | 1 to 1,000,000 | The same cell in another world chooses differently | yes |
| `wind_speed` | f32 | 0 to 25 m/s | The field's wind | no: the shader only |
| `wind_heading` | f32 | 0 to 360° | Toward which it blows | no |

A field is spawned over cells, not placed:

```c
foundry_grass_v1_params p;
foundry_grass_v1_default_params(&p);
p.species = FOUNDRY_GRASS_V1_SPECIES_MEADOW;
p.world_seed = 7;
foundry_instance meadow = foundry_grass_v1_spawn_field(rt, &p, transform, cells, count);  /* foundry_field_cell[] */
foundry_grass_v1_set_wind_speed(rt, meadow, 6.0f);
```

A plotter lists it under `fields`, and `foundry_region_spawn_fields` spawns it over the region's cells: the happy glen's is `{"pipeline": "grass_v1", "params": {"species": "meadow"}}`. `foundry_grass_v1_spawn` makes one cell as an ordinary instance. The foundry's views and the stress tool's phases use that, with no sway.

## How it's built

```
species, season, cell_q, cell_r, ground, moisture, world_seed
        |
   grass_v1_cell ──> [grass_cell] ──> grass_v1_choice   (per cell: present, variant, turn, scale, tint)

species, season, variant, detail
        |
   grass_v1_spec ──> [grass_tile] ──> grass_v1_model    (drawn; shared by every cell that picks it)
```

`draw.field` names the cell stage and the parameters that are the cell's own. The runtime runs `grass_cell` natively over a field's cells, once, when the field is spawned, and builds only the tiles the choices name.

### `grass_cell`: the functor

Every step is pure integer hashing or smooth value noise over the cells' centres, so neighbouring cells vary smoothly without knowing each other:
1. **Ground.** Water, rock, sand, and path cells show their plate, slightly shaded; soil shows grass.
2. **Patches.** Value noise at `patch_cells` leaves a `bald` share of soil cells as bare soil.
3. **The tile.** A hash of the cell picks one of `variants` grass tiles, and a turn by sixths (a hex looks the same turned).
4. **Lushness.** Wetter cells grow taller and greener, by `lush`, with a second noise so a meadow isn't flat.

### `grass_tile`: one cell of grass

1. **The ground.** The cell is a pointy-top hex of circumradius 1 m. Its soil is earth, the ground's colour: grass never colours the ground it grows on, so a meadow over earth reads as green blades with earth between them. It has a skirt 4 m down its sides, so steps between cells of different heights (steep terrain included) show earth, not a gap. Only grass takes the cell's tint: each vertex's alpha says whether it is grass (255) or ground (0), and the shader tints by it.
2. **Blades.** There are `blades` per cell, spread over the hex and gathered into tufts by `clump`. Each leans out by up to `lean`, bends further by `bend`, and tapers to a point. A blade is one sheet; the renderer draws both faces. Colour runs from `base_color` to the tip, which dries toward `dry_color` by season. In summer and fall, `seed_heads` of them carry a small diamond head.
3. **Sway.** Each vertex carries how far it moves in a 10 m/s wind: `sway` times the square of its height along the blade.
4. **Levels.** Every level covers the cell with the same colours and height:

   | Level | What it draws |
   |---|---|
   | high | every blade in three segments |
   | medium | every 2nd blade, twice as wide, in two segments |
   | low | every 6th, five times as wide, as one tapering triangle |
   | mat | a hex lid at 42% of the blades' height, coloured as the finest blades read from above: their colours weighted by the area each covers, over the soil they leave showing |

5. **Plates.** After the grass variants come water (a little below the ground), rock, sand, path, and bare soil: each a flat hex with its skirt.

## Species

Genomes are config entries in `config/grass_v1_genomes/`:

| Species | Height | Blades per cell | What it is |
|---|---|---|---|
| `meadow` | 0.42 m | 260 | A hay meadow: soft, leaning and arching, a little clumped, gold by late summer |
| `lawn` | 0.09 m | 320 | A cut lawn: short, dense, even, green all year |
| `prairie` | 1.3 m | 150 | Tallgrass: big bluestem and switchgrass, tufted, bronze seed heads in fall |
| `sedge` | 0.7 m | 180 | Tussock sedge: dense arching clumps for swamps and wet meadows |
| `dune` | 0.75 m | 80 | Marram: sparse stiff tufts on sand |

None has reference photos yet.

## Flow and caching

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `cell` | every parameter but `variant` and `detail` | one per cell: kept as 32 bytes of instance data, never built |
| `tile` | species, season, variant, detail | 5 × 4 × 9 × 4 = 720 |

A field's whole meadow is at most the species' variants plus plates, times the levels: 36 builds for the meadow in one season.

## Levels of detail

From the detail sizes (`demo/perf/report-grass_v1-detail-20261013T031129Z.json`, 32 sampled configurations, each measured as a patch of 37 cells; `design/quality.perf.md`, "Detail sizes"):

| Level | Triangles per cell (mean) | Drawn below, at high quality | medium | low |
|---|---|---|---|---|
| high | 998 | | | |
| medium | 307 | 96 px | 256 px or more | 256 px or more |
| low | 50 | 64 px | 96 px | 256 px or more |
| mat | 18 | never | never | 64 px |

A size is one tile's (1.5 m) on screen, in a 1080-pixel-tall view. At high quality a level switches in where the median patch differs from the finest on at most 10% of its pixels. With the game camera's 50° view, that puts high within about 36 m, medium out to about 54 m, and low beyond.

The mat differs on 20 to 33% of the pixels whatever the size, even with its measured colour. It reads as a flat edge where blades stand up, so only low quality uses it.

## Performance

From the quick stress plan (`demo/perf/report-grass_v1-20261013T031208Z.json`), one cell per instance:
- **Matches WebAssembly:** 100 of 100 sampled configurations, both stages.
- **Growing:** a tile takes 0.3 ms (p50) and 2.6 ms (p99).
- **Forest:** 800 cells as instances hold 0.2 ms p95, with 10 generations for 10 sets.

As fields, see the happy glen's book and `CURRENT.FPS.md`. Those scale numbers were measured before the earth ground and the denser, leaning blades. The finest level now has about 400 more triangles per cell, but it is drawn only within about 36 m. They haven't been rerun.

## QA

- **Contact sheets:** `pipeline_sheet` with rows `species` and cols `detail` (or `season`), pitched down about 0.45, shows every level side by side.
- **Live:** a plotter with a field shows it in the viewport's live view, swaying.
- **Levels of detail:** `stress_grass_v1 --lod REPORT.json --snapshot DIR` measures as a field and writes each comparison's differences.

## Known limits

- **No reference photos** for the five species yet.
- **One wind per field.** Every cell of a field sways in the same wind, with gusts by position; a local gust is the game's to model with several fields.
- **Fields don't move.** A field's tiles are placed when it is spawned, so `foundry_set_transform` doesn't move them.
- **No picture in WebAssembly.** The plotter's composed picture in the foundry leaves fields out; the live view shows them.
- **Nothing walks on it.** Grass has no collision and doesn't bend under feet.
