# The rock pipeline (`rock_v1`)

`rock_v1` makes a rock from a material, a size, moss, and a season, and breaks it when the game says so. Each material breaks its own way: granite into chunks, slate into plates, basalt into columns, obsidian into shards. That is the current milestone's "rocks that break uniquely". The pieces are cut when the rock is built, at a lower level of detail, so a break never rebuilds anything; a faux-physics animator throws them, in closed form.

- **The pipeline:** `demo/pipelines/rock_v1.json` (draft)
- **Systems:** `rock_body_v1`, `rock_animate_v1` (drafts, in `src/systems/`)
- **Channels:** the `rock_v1_*` family (drafts, in `src/channels/`)
- **Materials:** 8 in the config table `demo/config/rock_v1_materials/` (drafts; `design/flow.md`, "Config"), each linked to public-domain or CC0 photos in `demo/reference/rocks/`
- **Design:** `design/systems.md`, "Rocks"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds the rock? |
|---|---|---|---|---|
| `material` | config | 8 materials | Shape, surface, colour, and how it breaks (see "Materials") | yes |
| `size` | enum | small, medium, large, huge | About 0.6, 1.2, 2.2, and 4 m across; bigger rocks break into more pieces | yes |
| `moss` | enum | none, light, heavy | Moss and lichen on the upper faces, scaled by the material's `lichen` | yes |
| `season` | enum | winter, spring, summer, fall | Snow on the upper faces in winter | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct rocks of that material and size, and how it breaks | yes |
| `detail` | enum | high, medium, low | Level of detail | yes |
| `break_time` | event | | Seconds since the rock broke, negative while it is whole. The game starts it with `foundry_rock_v1_start_break` | no |
| `break_heading` | f32 | 0 to 360° | Where the blow sends the pieces | no |

```c
foundry_rock_v1_params p;
foundry_rock_v1_default_params(&p);
p.material = FOUNDRY_ROCK_V1_MATERIAL_SLATE;
p.size = FOUNDRY_ROCK_V1_SIZE_LARGE;
p.moss = FOUNDRY_ROCK_V1_MOSS_LIGHT;
foundry_instance rock = foundry_rock_v1_spawn(rt, &p, transform);
foundry_rock_v1_set_break_heading(rt, rock, 45.0f);
foundry_rock_v1_start_break(rt, rock);  /* a hammer landed */
```

## How it's built

Two systems:

```
material, size, moss, season, seed, detail
        |
   rock_v1_spec ──> [rock_body] ──> rock_v1_model   (drawn: the whole rock on bone 0, piece n on bone 1 + n)
                          │
                          └─ pieces ──> rock_v1_motion ──> [rock_animate] ──> rock_v1_pose   (every frame; poses the model)
                                           ^
                                break_time, break_heading, seed
```

### `rock_body`: the rock and its pieces

1. **The block.** A cube is chiselled by random planes into a convex block about 1 unit across: `facets` planes for blocky stone, more for a worn one (`roundness` adds up to 14), with a flatter base to sit on.
2. **One transform for every point.** Each vertex goes through the same function of its position:
   - worn round toward a sphere near the skin by `roundness` (the inside stays put);
   - roughened by a vector noise field (`roughness`, `relief_scale`);
   - stretched (`elongation`), flattened (`flatness`), turned, scaled to the size, and sunk a tenth of its height into the ground.
   Because the transform depends only on the point, faces that meet stay joined however they were cut, so every piece is closed and the pieces fit the whole.
3. **The pieces.** The same block is cut into cells by the material's `fracture`, each cell the block clipped by half-spaces:

   | Fracture | Cells | Materials |
   |---|---|---|
   | `voronoi` | random sites, a little flatter across the grain | granite |
   | `slab` | layers across the grain, each split in two | slate |
   | `columnar` | prisms along the grain, cut once across | basalt |
   | `splinter` | voronoi stretched along the grain: long shards | obsidian |
   | `crumble` | twice as many sites: many small pieces | sandstone |
   | `impact` | sites crowded toward the front: fine near the blow, coarse behind | limestone, marble |
   | `split` | one or two cuts near the middle: halves or quarters | river stone |

   The grain is the material's `grain` direction, turned a little at random per rock, and `grain_strength` says how far pieces follow it. A medium rock breaks into `pieces`; small ones into half as many, large into 1.6 times, huge into 2.3 times, up to 40. Slivers under 0.2% of the volume are dropped.
4. **Detail.** The whole rock's faces are split twice at high detail (three times for a worn rock), once at medium and low; the pieces once at high and not at all below.
5. **Paint.** One colour per triangle, for crisp palette facets (`style.md` §5), by `pattern`: speckled (granite's crystals), banded (sandstone's strata), veined (marble), layered (slate's cleavage), jointed (basalt's columns, with rust), glassy (obsidian's sheen), or pitted (limestone). Broken faces take the `fresh` colour. Moss grows on the upper faces of the skin, never on a break; snow settles on them in winter.
6. **Pieces for the animator.** Each piece's centre of mass and mass (from its volume and the material's `density`), its radius, and 14 support points (its extremes in 14 directions).

### `rock_animate`: the faux physics

This is the only stage that runs every frame, and it is closed form: a frame depends only on its inputs, so any moment can be shown.
- **Whole.** Bone 0 at rest and every piece hidden (a zero matrix). A whole rock is `settled`, so it sleeps.
- **Flight.** Each piece leaves from the rock's centre, along `break_heading`, and a little up. The higher a piece sat, the faster it goes, so the base slumps where it was and the top scatters; lighter pieces go faster. It tumbles about a mostly level axis on a ballistic arc.
- **Landing.** From then on the piece rests on its lowest support point, so it never sinks into the ground however it has turned. It makes one small bounce and slides until friction stops it, and its roll eases on to the nearest whole turn, so it comes to rest lying the way it lay in the rock (a stable face down), turned only about the vertical.
- **Sleep.** The rock settles once every piece is still.

Pieces don't collide with each other, so a piece can come to rest partly inside another.

### Collision

Each build carries one **solid** body: the convex hull of the rock's support points in 26 directions, so it hugs the rock's shape without its every facet. Measured on a medium granite rock: reach 0.81 m, top 0.74 m, accepted by Jolt and inside the conformance. The body is the whole rock's; its broken pieces don't collide (the break is a closed-form animation).

## Materials

| Material | Kind | Breaks | Pattern | Biome |
|---|---|---|---|---|
| Granite | igneous, coarse-grained | voronoi chunks | speckled | mountains, old shields |
| Basalt | volcanic, fine-grained | columnar prisms | jointed, rusting | volcanic |
| Sandstone | sedimentary, sand | crumbles | banded | desert, canyon |
| Limestone | sedimentary, shells | impact | pitted, quick to moss | temperate hills, karst |
| Slate | metamorphic, cleaved mudstone | slab plates | layered | mountains, moor |
| Marble | metamorphic, limestone | impact | veined | mountains |
| Obsidian | volcanic glass | splinter shards | glassy | volcanic |
| River stone | water-worn cobble | split in halves | plain, smooth | rivers, beaches, swamp edges |

Materials are entries of `config/rock_v1_materials/`, edited through the generic config editor or `config_set`. Each links to 2 or 3 reference photos in `demo/reference/rocks/<material>/`, all public domain or CC0 (no attribution needed; sidecars record the source). Compared against them on 2026-10-12, granite lost its pink cast, sandstone went from orange to golden with wider bands, limestone from cream to grey, and slate from brown-flecked to blue-grey.

## Flow and caching

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `body` | material, size, moss, season, seed, detail | 8 × 4 × 3 × 4 × 1000 × 3 = 1,152,000 |
| `animate` | every parameter | every frame, per broken instance |

The probe (`pipeline_flow` with `probe`, on a whole rock) shows that `moss`, `season`, `seed`, and `detail` never change the animator's output there, and that `break_heading` changes nothing until the rock breaks. Moss and season only paint, so the pieces' physics could be cached without them.

## Performance

Native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan), from the standard stress plan (`demo/perf/report-rock_v1-20261012T*.json`):

| | |
|---|---|
| Making a rock (`body`, rock and pieces together) | 0.9 ms mean; 1.6 ms from spawn to drawable (p50), 6.8 ms p99 (a huge sandstone, which crumbles into the most pieces) |
| Posing it (`animate`) | about 1 µs |
| Triangles | 728 (p50) to 6,896 (max) at high detail, pieces included |

- **Field of rocks.** 25,600 rocks run at 7.1 ms p95 on 10 shared builds, a 99.96% cache hit rate; the budget held at every count tried, so capacity is higher still.
- **Bulk breaks.** Breaking 100 rocks in one frame raises the frame time from 0.1 to 0.4 ms; they settle in 1.8 s, and memory comes back exactly. 1,600 breaking at once still fit the frame budget.
- **Soak.** Five drains all came back within 2.3 MB.
- **With collision** (`demo/perf/report-rock_v1-20261012T194430Z.json`, every rock's hull in a toy Jolt world): 25,600 rocks at 7.0 ms p95; 100 broken in one frame cost 0.6 ms over 0.1; the soak's five drains within 1.0 MB, bodies balanced.
- **Matches WebAssembly.** 100 sampled rocks and their poses, byte for byte.

Run `perf_stress` with plan `quick` after a change and `standard` before asking for approval (`design/quality.perf.md`). `build/linux-release/stress/stress_rock_v1 --plan explore --window` opens a field to fly through; B breaks the rocks you look at.

## QA

- **Contact sheets.** `pipeline_sheet` with rows `material` and cols `break_time` (say -1, 0.35, 3) shows each material whole, mid-break, and settled; cols `size` or `moss` show the rest.
- **The QA video.** `pipeline_video` (or the **Video** tab) enumerates material, size, moss, season, and detail.
- **The live instance.** Select `pipeline:rock_v1` or a material under Config; the **View** tab's Start `break_time` breaks the rock.

## Known limits

- **No piece-to-piece collision.** Pieces can rest partly inside each other.
- **Convex rocks.** Every rock starts as a convex block; overhangs and holes come only from the surface noise.
- **One impact side.** `impact` fractures crowd their small pieces toward the rock's front, wherever the blow lands; the heading only steers where they fly.
- **No dust.** A break has no particles yet.
