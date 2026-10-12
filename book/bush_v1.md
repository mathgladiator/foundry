# The shrub pipeline (`bush_v1`)

`bush_v1` grows a shrub from a species, an age, and a season, with or without its berries, and sways it in the wind. It is the light version of the tree pipeline that `design/systems.md` proposes: several stems from the ground instead of a trunk, a smaller genome, no break, and one build system instead of three. Its wind is the trees' wind.

- **The pipeline:** `demo/pipelines/bush_v1.json` (draft)
- **Systems:** `bush_grow_v1`, `bush_animate_v1` (drafts, in `src/systems/`)
- **Channels:** the `bush_v1_*` family (drafts, in `src/channels/`)
- **Genomes:** 14 species in the config table `demo/config/bush_v1_genomes/` (drafts), each linked to public-domain or CC0 photos in `demo/reference/bushes/`
- **Design:** `design/systems.md`, "Shrubs and ground cover"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds the shrub? |
|---|---|---|---|---|
| `species` | config | 14 species | Everything that makes a hazel a hazel | yes |
| `age` | enum | young, mature, old | Height, stem count, crookedness | yes |
| `season` | enum | winter, spring, summer, fall | Leaves, flowers, berries ripening, snow | yes |
| `fruit` | bool | | The shrub still carries its berries. Clear it when the player picks them; the shrub rebuilds bare and shares that build with every other picked shrub like it. It applies only to the 7 species whose fruit can be picked (`pickable`, see "Fruit"); for the rest it is disabled and ignored | yes, where it applies |
| `seed` | u32 | 1 to 1000 | Which of the distinct shrubs | yes |
| `detail` | enum | high, medium, low | Level of detail | yes |
| `wind_speed`, `wind_heading`, `gust_damping`, `time` | | | The local wind and the clock, as for trees | no: the pose only |

```c
foundry_bush_v1_params p;
foundry_bush_v1_default_params(&p);
p.species = FOUNDRY_BUSH_V1_SPECIES_BLUEBERRY;
p.season = FOUNDRY_BUSH_V1_SEASON_SUMMER;
foundry_instance bush = foundry_bush_v1_spawn(rt, &p, transform);
if (foundry_bush_v1_fruit_applies(p.species))
  foundry_bush_v1_set_fruit(rt, bush, false);  /* picked */
```

## How it's built

```
species, age, season, fruit, seed, detail
        |
   bush_v1_spec ──> [bush_grow] ──> bush_v1_model   (drawn)
                          │
                          └─ bones ──> bush_v1_motion ──> [bush_animate] ──> bush_v1_pose   (every frame)
                                          ^
                    wind_speed, wind_heading, gust_damping, time, seed
```

### `bush_grow`: the shrub

1. **Stems.** `stems` stems (60% when young, 125% when old) from a small patch of ground, leaning out by up to `stem_spread` and arching by `stem_curve` (a bramble's canes arch over, a hazel's stand straight), crooked by `wobble`.
2. **Branches.** `branches` side branches per stem, sized by the outline (`shape`: round, upright, spreading), and `twigs` per branch on mature shrubs. A `fan` shrub (saw palmetto) has bare stalks ending in fan leaves instead.
3. **Wind bones.** A static root, two bones per stem, and one per side branch while the total stays under 128.
4. **Foliage.** Displaced icosphere clumps (or stretched tufts for `needles`) along the outer wood, capped at 220: spring leaves, summer, fall colour (two colours mixed) and `fall_drop`, an evergreen's `leaf_winter` or a deciduous shrub's last dead leaves (`winter_keep`). A share `bloom[season]` of the clumps carries a flower cluster (`blossom_color`, `blossom_size`).
5. **Berries.** `fruit` clusters per clump of `fruit_cluster` berries, ripening from `fruit_unripe` to `fruit_ripe` by `fruit_seasons[season]`; holly keeps its berries through winter, even on bare twigs. A cluster hangs on the clump's surface, facing out from the shrub's middle, and is left off if another clump would bury it (three tries), so the berries drawn are the ones you can see. A fan shrub (saw palmetto) hangs its clusters from partway up its stalks. Pickable berries show only while the request's `fruit` is set; seed balls and capsules always show. Each berry is an octahedron; at medium and low detail, clusters have half as many, slightly larger berries.
6. **Paint and snow.** Flat palette regions, and snow on upward surfaces in winter by `snow`.
7. **Detail.** Stem, branch, and twig sides; icosphere subdivision; clumps kept (all, two thirds, half, grown to cover); twigs dropped at low detail.

### `bush_animate`: the wind

The flowers' and trees' simulator for stems and branches, with the same gusts, in closed form. A shrub in no wind settles.

### Collision

Each build carries one **soft** body (`physics_v1_body`): a vertical cylinder of radius half the shrub's reach (at least 5 cm) up to 0.8 of its top, which a player can push a little way into. The runtime adds it to the game's Jolt world when the build is ready, and a plotter reads its footprint from it. Measured on an old elderberry: one cylinder of radius 1.88 m reaching 2.86 m up, which Jolt accepts and which stays inside the conformance (`pipeline_set`'s `collision` fact). The View tab's Collision toggle draws it.

## Species

| Species | Latin | Height (m) | Habit | Leaves | Flowers | Berries | Biome |
|---|---|---|---|---|---|---|---|
| Hazel | *Corylus avellana* | 4 | upright canes | deciduous, yellow in fall | catkins, late winter | nuts | temperate woodland |
| Elderberry | *Sambucus nigra* | 3 | arching | deciduous | cream umbels | black clusters | woodland edge |
| Blackberry | *Rubus fruticosus* | 1.6 | sprawling bramble | semi-evergreen | white-pink | red to black | hedgerow, clearing |
| Holly | *Ilex aquifolium* | 3 | upright | evergreen, glossy | small white | scarlet, through winter | oak and beech woods |
| Rhododendron | *Rhododendron ponticum* | 2.5 | broad dome | evergreen | purple-pink trusses | | damp woods, mountains |
| Boxwood | *Buxus sempervirens* | 1.4 | dense round | evergreen, bronzing | | | chalk woods, gardens |
| Common juniper | *Juniperus communis* | 1.8 | upright, needles | evergreen | | blue cones | heath, boreal, alpine |
| Blueberry | *Vaccinium corymbosum* | 1.5 | upright, red twigs | deciduous, scarlet in fall | white bells | blue, summer | bog, boreal clearing |
| Creosote bush | *Larrea tridentata* | 1.8 | open vase | evergreen, olive | yellow | fuzzy white | hot desert |
| Big sagebrush | *Artemisia tridentata* | 1.2 | round, crooked | evergreen, silver | tiny yellow, fall | | cold steppe |
| Hibiscus | *Hibiscus rosa-sinensis* | 2.2 | round | evergreen | scarlet, all year | | tropical |
| Saw palmetto | *Serenoa repens* | 1.5 | fans from the ground | evergreen | | | swamp, pine flatwoods |
| Buttonbush | *Cephalanthus occidentalis* | 2.5 | crooked round | deciduous | white balls | brown seed balls | swamp, pond margin |
| Oleander | *Nerium oleander* | 3 | upright | evergreen | pink | | Mediterranean |

Compared with their reference photos on 2026-10-12: elderberry, blackberry, and holly berries were enlarged to read; rhododendron, hibiscus, and oleander flowers grew; hazel and juniper were drawn in tighter; palmetto's stalks were shortened under bigger, upright fans; blackberry's canes arch less and its fall leaves stay greener.

### Fruit

Each genome says whether its fruit can be picked (`pickable`) and how ripe it is per season (`fruit_seasons`). Where it isn't pickable, the `fruit` switch is disabled in the workspace and the QA video, and the shrub keeps whatever it bears.

| Species | Bears | Pickable | Ripeness (W, Sp, Su, F) |
|---|---|---|---|
| Hazel | hazelnuts in husks | yes | 0, 0, 0.3, 1 |
| Elderberry | black berries in umbels | yes | 0, 0, 0.5, 1 |
| Blackberry | blackberries | yes | 0, 0, 0.6, 1 |
| Holly | scarlet berries | yes | 1, 0.8, 0.1, 0.8 |
| Common juniper | blue berry-like cones (the gin spice) | yes | 0.8, 0.4, 0.3, 0.9 |
| Blueberry | blueberries | yes | 0, 0, 1, 0.3 |
| Saw palmetto | yellow to black drupes, ripening August to October ([UF/IFAS](https://hort.ifas.ufl.edu/shrubs/SERREPA.PDF)) | yes | 0, 0, 0.4, 1 (new) |
| Buttonbush | brown seed balls | no | 0.6, 0, 0.2, 1 |
| Creosote bush | fuzzy white capsules | no | 0, 0.2, 0.8, 0.5 |
| Boxwood, hibiscus, oleander, rhododendron, sagebrush | dry capsules or none worth drawing | no | none |

Elderberry was the case that started this: with fruit on, a mature elderberry in summer had 18,432 more triangles but looked the same, because every berry sat inside a leaf clump. Berries now hang outside, elderberry's are larger (`fruit_size` 0.04, 9 to a cluster) and lighter green while unripe, and hazel (0.05), blackberry (0.035), blueberry (0.03), juniper (0.026), and saw palmetto (0.04) grew so they read at a shrub's distance.

## Flow and caching

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `grow` | species, age, season, fruit, seed, detail | (7 × 2 + 7) × 3 × 4 × 1000 × 3 = 756,000: `fruit` counts only where it applies |
| `animate` | every parameter | every frame, per instance |

The probe shows `fruit` and `detail` reach `animate` (through the bones) but never change its output: picking the berries doesn't move the wood, and the bones are the same at every level of detail.

## Performance

Native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan), from the standard stress plan (`demo/perf/report-bush_v1-20261012T152753Z.json`, after the fruit research):

| | |
|---|---|
| Growing a shrub (`grow`) | 0.62 ms mean; 1.8 ms from spawn to drawable (p50), 7.2 ms p99 (an old elderberry in fall, in fruit) |
| Posing it (`animate`) | about 8 µs |
| Triangles | 4,013 (p50) to 30,344 (max) at high detail |

- **Thicket.** About 16,000 shrubs fit a 60 fps frame (16.4 ms p95), sharing 10 builds at a 99.96% hit rate; 25,600 take 26 ms. Before the fruit work it was 14,400.
- **Soak: drains fail on RSS, not the heap.** After each drain the heap in use is back within 0.2 MB of the first, and the runtime's counters balance (no instances, cache entries, or GPU buffers left), but the process's RSS sits above the first drain, past the 8 MB tolerance on two of four: +8.4 and +10.7 MB on the third and fourth (3 of 5 drains pass). The run before the fruit work was worse (+8.8, 12.2, 16.3, and 16.3 MB), and a scratch soak then passed four drains of five. That reads as memory the allocator or GPU driver keeps rather than a leak, but it is unexplained, and the check stays failing until it is.
- **With collision** (`demo/perf/report-bush_v1-20261012T193827Z.json`, every shrub's soft body in a toy Jolt world): about 14,400 shrubs fit the frame (16.0 ms p95), and native still matched WebAssembly 100/100. The soak's drains fail the same way: the heap is back within 0.2 MB at every drain and the bodies balance, but RSS sits 11 to 14 MB above the first drain. The same drift appeared in the tree's soak today and went away with the GPU off (`--gpu none`: drains within 5.6 MB), so it looks like the GPU driver's memory, still unexplained.
- **Matches WebAssembly.** 100 sampled shrubs and their poses, byte for byte (through the foundry's `perf_stress`); `pipeline_set` with `verify_native` also matched all 14 species in winter, summer, and fall, fruit on and off (84 configurations).

## QA

- **Contact sheets.** One species per row with cols `season` (or `age`), so each frames to its own size.
- **The QA video.** `pipeline_video` enumerates species, age, season, fruit, and detail: 756 variations, because the 7 species whose fruit isn't pickable get one clip with `fruit` marked disabled instead of two (a 3-clip export of hazel and boxwood in fall confirmed it). Every discrete parameter's value is shown under its name at the top of each clip.

## Known limits

- **One mesh per shrub.** Berries are geometry, so picking rebuilds the shrub (from the cache, after the first).
- **No collision detail.** The conformance is the shrub's whole footprint and height.
- **Soak drains over tolerance.** RSS after a drain sits up to 11 MB above the first drain while the heap stays flat (see "Performance"); not yet explained.
