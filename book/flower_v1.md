# The flower pipeline (`flower_v1`)

`flower_v1` grows a clump of flowers from a species and a season and sways it in the wind. A clump of stems rather than one flower, so it reads at battle distance (`style.md` §3); ground cover with no conformance, purely visual. Its wind is the trees' wind, so a meadow and its trees move together.

- **The pipeline:** `demo/pipelines/flower_v1.json` (draft)
- **Systems:** `flower_grow_v1`, `flower_animate_v1` (drafts, in `src/systems/`)
- **Channels:** the `flower_v1_*` family (drafts, in `src/channels/`)
- **Genomes:** 15 species in the config table `demo/config/flower_v1_genomes/` (drafts), each linked to public-domain or CC0 photos in `demo/reference/flowers/`
- **Design:** `design/systems.md`, "Mushrooms and flowers"

## What the game asks for

| Parameter | Type | Range | What it does | Rebuilds the clump? |
|---|---|---|---|---|
| `species` | config | 15 species | Everything that makes a daisy a daisy | yes |
| `season` | enum | winter, spring, summer, fall | Buds, bloom, seed heads, dry stalks | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct clumps of that species and season | yes |
| `detail` | enum | high, medium, low | Level of detail | yes |
| `wind_speed` | f32 | 0 to 25 m/s | The local wind; 0 lets the clump sleep | no: the pose only |
| `wind_heading` | f32 | 0 to 360° | Toward which the wind blows | no |
| `gust_damping` | f32 | 0 to 1 | 0 is raw gusts, 1 a steady wind | no |
| `time` | time | | The instance's clock | no |

Flowers have no age (a clump is always mature) and no break.

```c
foundry_flower_v1_params p;
foundry_flower_v1_default_params(&p);
p.species = FOUNDRY_FLOWER_V1_SPECIES_POPPY;
p.season = FOUNDRY_FLOWER_V1_SEASON_SUMMER;
foundry_instance poppies = foundry_flower_v1_spawn(rt, &p, transform);
foundry_flower_v1_set_wind_speed(rt, poppies, 9.0f);
```

## How it's built

```
species, season, seed, detail
        |
   flower_v1_spec ──> [flower_grow] ──> flower_v1_model   (drawn)
                            │
                            └─ bones ──> flower_v1_motion ──> [flower_animate] ──> flower_v1_pose   (every frame)
                                            ^
                      wind_speed, wind_heading, gust_damping, time, seed
```

### `flower_grow`: the clump

1. **Stems.** `stems` stems spread over a disc of `clump_radius`, the first in the middle. Outer stems lean out by up to `stem_lean`, every stem bends by `stem_curve`, and heights vary by about ±15%. Each stem is a tapered tube on two wind bones (lower and upper half) under one root bone.
2. **The season, per stem.** A share `bloom[season]` of the stems is in flower; the rest are buds when the next season blooms more, or spent seed heads after the peak. A `dormant` species dies back in winter to two in five stems, as dry stalks at 60% height, with seed heads for the forms that keep one.
3. **Heads, by form** (`head`). Petals are two-sided ribbons that run from `petal` at the base to `petal_tip` at the tip, raised by `petal_cup` and curling by `petal_curl`; the head bows by `nod`.
   - `ray`: a disc or cone (`center_size`, `center_height`) ringed by rays in 1 to 3 layers: daisy, sunflower, coneflower, desert marigold, edelweiss.
   - `cup`: broad petals cupped up: tulip, poppy.
   - `trumpet`: flat tepals around a flared tube: daffodil.
   - `bell`: a raceme of hanging bells, all to one side when `one_sided`: bluebell, foxglove.
   - `spike`: a raceme of small florets: lupine, lavender.
   - `iris`: three drooping falls (from the `center` signal colour to `petal`) and three upright standards.
   - `cattail`: a velvet sausage under a thin spike, fluffing to `seed_head` in winter.
   - `crest`: a level beak (`center`) under a fan of orange petals and a blue tongue (`petal_tip`): bird of paradise.
4. **Leaves.** Basal leaves around the foot, and leaves up each stem, by shape (`strap`, `lance`, `broad`, `needle`, `sword`), arching by `leaf_droop`; green, then `leaf_fall` unless `evergreen`. Basal leaves blend from the root bone to a stem's bone along their length, so they sway too.
5. **Detail.** Petal and leaf segments 3, 2, 1; stem sides 5, 4, 3; florets and leaves kept 100%, 70%, 45%.

### `flower_animate`: the wind

The trees' simulator, for stems: each bone bends toward the wind as a damped oscillator driven by the same three gusts as `tree_animate_v1`, plus a resonant sway with a per-stem phase, in closed form. The clump settles (sleeps) when there is no wind.

## Species

| Species | Latin | Height (m) | Form | Blooms | Biome |
|---|---|---|---|---|---|
| Oxeye daisy | *Leucanthemum vulgare* | 0.6 | ray | early summer | meadow |
| Common poppy | *Papaver rhoeas* | 0.6 | cup | early summer | meadow, fields |
| Garden lupine | *Lupinus polyphyllus* | 1.0 | spike | early summer | mountain meadow |
| Sunflower | *Helianthus annuus* | 2.0 | ray, nodding | late summer | prairie |
| Purple coneflower | *Echinacea purpurea* | 0.9 | ray, swept back | summer | prairie |
| English lavender | *Lavandula angustifolia* | 0.6 | spike | summer, evergreen | Mediterranean |
| Bluebell | *Hyacinthoides non-scripta* | 0.45 | bell, one-sided | spring | woodland floor |
| Foxglove | *Digitalis purpurea* | 1.4 | bell, one-sided | early summer, evergreen rosette | woodland clearing |
| Tulip | *Tulipa gesneriana* | 0.5 | cup | spring | steppe, gardens |
| Wild daffodil | *Narcissus pseudonarcissus* | 0.45 | trumpet | early spring | woods, meadows |
| Blue flag iris | *Iris versicolor* | 0.8 | iris | late spring | swamp, marsh |
| Cattail | *Typha latifolia* | 1.8 | cattail | summer into winter | swamp, pond edge |
| Desert marigold | *Baileya multiradiata* | 0.4 | ray | most of the year | desert |
| Edelweiss | *Leontopodium nivale* | 0.3 | ray (woolly bracts) | summer | alpine |
| Bird of paradise | *Strelitzia reginae* | 1.1 | crest | all year | tropical |

Heights sit a little above wild ones, and heads are enlarged (`head_size`), so small flowers still read next to the 1.8 m hero. After comparing every species with its reference photos on 2026-10-12, heads grew 1.3 to 1.6 times, coneflower's rays turned pink and swept back, lavender became a denser clump, foxglove and iris got bigger flowers, bird of paradise's leaves stood up, and sunflower's head turned to face sideways.

## Flow and caching

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `grow` | species, season, seed, detail | 15 × 4 × 1000 × 3 = 180,000 |
| `animate` | every parameter | every frame, per instance |

The probe shows `detail` reaches `animate` (through the bones) but never changes its output: the bones are the same at every level of detail, so poses could be shared across them.

## Performance

Native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan), from the standard stress plan (`demo/perf/report-flower_v1-20261012T*.json`):

| | |
|---|---|
| Growing a clump (`grow`) | 0.19 ms mean; 1.1 ms from spawn to drawable (p50), 4.8 ms p99 (a lupine) |
| Posing it (`animate`) | about 3 µs |
| Triangles | 760 (p50) to 10,244 (max) at high detail |

- **Meadow.** 25,600 clumps run at 9.9 ms p95 on 10 shared builds; the budget held at every count tried.
- **Soak.** Five drains all came back within 1.2 MB.
- **Matches WebAssembly.** 100 sampled clumps and their poses, byte for byte.

## QA

- **Contact sheets.** `pipeline_sheet` with one species per row and cols `season` frames each species to its own height (a sheet of every species at one scale makes small flowers specks next to the sunflower).
- **The QA video.** `pipeline_video` enumerates species, season, and detail.

## Known limits

- **Stylised forms.** Eight head forms cover the fifteen species; a poppy's crumpled petals, a lupine's palmate leaves, and edelweiss's felt are approximations.
- **Sunflowers don't track the sun.** Heads face a random way.
- **No conformance.** Clumps are ground cover: nothing collides with them.
