# The tree pipeline (`tree_v1`)

`tree_v1` grows a tree from a species, an age, a season, and a seed, and then animates it: it sways in the local wind or, once broken, topples and throws its limbs. It is the first pipeline the foundry built end to end, and the first to run in the flattened native runtime.

- **The pipeline:** `demo/pipelines/tree_v1.json` (draft)
- **Systems:** `tree_rig_v1`, `tree_mesh_v1`, `tree_colorize_v1`, `tree_animate_v1` (drafts, in `src/systems/`)
- **Channels:** the `tree_v1_*` family (drafts, in `src/channels/`)
- **Genomes:** 28 species in the config table `demo/config/tree_v1_genomes/` (drafts; `design/flow.md`, "Config"), each linked to public-domain or CC0 photos in `demo/reference/trees/`
- **Design:** `design/systems.md`, "Trees"

## What the game asks for

The pipeline's parameters are its whole interface. The generated C API (`src/library/tree_v1.h`, part of `runtime.h`) gives each one a field of `foundry_tree_v1_params` and a setter, and each event a trigger.

| Parameter | Type | Range | What it does | Rebuilds the tree? |
|---|---|---|---|---|
| `species` | config | 28 species | Everything that makes an oak an oak (see "Species") | yes |
| `age` | enum | sapling, young, mature, ancient | Height and girth, branching, dead limbs and moss on ancient trees | yes |
| `season` | enum | winter, spring, summer, fall | Leaves, blossom, fruit, fall colour, bare winters, snow | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct trees of that species, age, and season | yes |
| `detail` | enum | high, medium, low | Level of detail: ring sides, foliage density, sprigs | yes |
| `wind_speed` | f32 | 0 to 25 m/s | The local wind; 0 lets a standing tree sleep | no: the pose only |
| `wind_heading` | f32 | 0 to 360° | The direction the wind blows toward, from +Z toward +X | no |
| `gust_damping` | f32 | 0 to 1 | 0 is raw gusts, 1 is a steady wind | no |
| `time` | time | | The instance's clock; the runtime runs it | no |
| `break_time` | event | | Seconds since the tree broke, negative while it stands. The game starts it with `foundry_tree_v1_start_break` | no |
| `break_heading` | f32 | 0 to 360° | The direction the tree falls | no |

```c
foundry_tree_v1_params p;
foundry_tree_v1_default_params(&p);
p.species = FOUNDRY_TREE_V1_SPECIES_BIRCH;
p.season = FOUNDRY_TREE_V1_SEASON_FALL;
p.seed = 42;
foundry_instance tree = foundry_tree_v1_spawn(rt, &p, transform);
foundry_tree_v1_set_wind_speed(rt, tree, 9.0f);
foundry_tree_v1_start_break(rt, tree);  /* a woodcutter finished */
```

## How it's built

Four systems, each one channel in and one out:

```
species, age, season, seed, detail
        |
   tree_v1_spec ──> [tree_rig] ──> tree_v1_rig ──> [tree_mesh] ──> tree_v1_body ──> [tree_colorize] ──> tree_v1_model   (drawn)
                          │
                          └─ bones, pieces ──> tree_v1_motion ──> [tree_animate] ──> tree_v1_pose   (every frame; poses the model)
                                                 ^
                     wind_speed, wind_heading, gust_damping, time, break_time, break_heading, seed
```

### `tree_rig`: the skeleton

The rig grows the branch structure from the genome. It works in five steps:
1. **Branches.** It grows a stem polyline and up to three orders of branches, using Weber and Penn (1995) style parameters: counts, angles, divergence or whorls, a crown-shape envelope, curvature, and gnarliness.
   - **Excurrent species** (pine, spruce, birch, coconut) keep the stem running to the top.
   - **Decurrent species** (oak, apple) turn their upper limbs into leaders.
   - `cluster` pushes branches toward the tip. That is how umbrella crowns (acacia, dragonwood) and a palm's single crown of fronds come about.
2. **Radii.** Branch radii follow the pipe model (Leonardo's rule, with exponent 2). They are scaled so the trunk matches the species' slenderness, then thickened 1.4 times for the cel look (`style.md` §3).
3. **Wind bones.** There is a rigid stump, three stem bones, two bones per limb, and one per twig, capped at 128. Each bone's natural frequency comes from a cantilever beam made of the species' wood. Bare trees catch less wind.
4. **Break pieces.** The pieces are the stump, the log above the cut, and each limb.
5. **Foliage anchors.** Anchors sit on the outer branches, sized from the genome, and are capped at 260 per tree.

### `tree_mesh`: the body

The mesh system wraps the skeleton and dresses it for the season:
- **Bark.** Bark is built as rings along every branch, with relief and shade from the genome's bark style. The styles are furrowed, plated, shaggy, mottled, fluted, ringed, and papery.
  - Ringed (a palm's leaf scars) and papery (a birch's lenticels) need rings closer together than the rig's points, so the mesh splits the stem's segments for them.
- **The stump.** The stump flares at its base and has a cut face where the log will break off.
- **Foliage.** Foliage is chosen per clump by season:
  - **Clumps:** displaced icospheres, for broadleaves.
  - **Needles:** stretched tufts.
  - **Rosettes:** flattened pads (acacia, dragonwood).
  - **Fronds:** pinnate leaflets along each first-order limb, after a bare stalk, for the coconut, and on long drooping limbs, the willow's curtains. Fronds take the place of clumps; deciduous ones drop in winter.
- **Seasons.**
  - Deciduous trees are bare in winter, keeping `winter_keep` of their dead leaves (an oak's marcescence).
  - Spring brings blossom, and `blossom_leaf` says how much leaf is out beside it: cherries flower on bare wood.
  - Fall drops `fall_drop` of the leaves.
  - Fruit hangs in summer and fall: unripe, then ripe. A palm hangs its fruit in a cluster under the crown.
- **Level of detail.** `detail` sets the ring sides (12, 9, or 7 on the trunk), the icosphere subdivision, the share of foliage clumps kept (all, two thirds, or half, grown to cover the gaps), and whether sprigs appear.

### `tree_colorize`: the paint

Colorize maps each vertex's paint region (bark, wood, leaf, blossom, fruit, dead leaf, dead wood) and shade to the genome's colours for the season. Fall mixes two colours per clump for variety. Mature and ancient trees grow moss on surfaces that face up. In winter, snow settles on anything facing up more than the genome's `snow` allows, which is never for tropical trees.

### `tree_animate`: the simulator

This is the only stage that runs every frame. It returns a 4×4 matrix per bone, plus whether the tree is broken and whether it has settled.
- **Wind.** Each bone's response is the closed-form steady state of a damped oscillator driven by three gust sinusoids, plus a resonant sway, so there's no state to integrate and any time can be evaluated directly. Bends follow the wind and run across it.
- **The break.** The log falls about the cut as a rod toppling under gravity, integrated at a fixed 1/120 s step. When the crown hits the ground, the limbs fly off ballistically and slide to rest.
- **Sleep.** `settled` lets the instance sleep: a tree in no wind settles at once, and a broken tree once everything rests.

## Species

Each species is a genome (`tree_v1_genome`): about 55 fields of shape, seasons, wood, and colour, with ranges from the channel schema. The genomes are entries of the config table `config/tree_v1_genomes/`, a hidden detail of the pipeline: the game sees only their names (`FOUNDRY_TREE_V1_SPECIES_OAK`), and the native runtime compiles their values in. Selecting one in the workspace opens the foundry's generic config editor: a widget for each field from the channel schema, grouped and coloured as the table's `table.json` says.

| Species | Latin | Mature height (m) | Leaves | Crown | Foliage | Bark | Extras |
|---|---|---|---|---|---|---|---|
| Acacia | *Vachellia tortilis* | 8 | evergreen | cylindrical (umbrella) | rosettes | furrowed | cream blossom, seed pods |
| Apple | *Malus domestica* | 5.5 | deciduous | spherical | clumps | plated | blossom, apples |
| Baobab | *Adansonia digitata* | 13 | deciduous (dry season) | cylindrical, short | clumps | smooth | bottle trunk, white flowers, hanging pods |
| Birch | *Betula pendula* | 11 | deciduous | tend flame | clumps | papery | catkins, gold in fall |
| Bristlecone pine | *Pinus longaeva* | 6 | evergreen | tapered, twisted | needles | fluted | half its limbs dead and bare when ancient, snow |
| Cedar of Lebanon | *Cedrus libani* | 15 | evergreen | cylindrical, flat tiers | rosettes (level needle plates) | furrowed | flat-topped when old, heavy snow |
| Cherry | *Prunus serrulata* | 7 | deciduous | spherical | clumps | mottled | pink blossom on bare wood |
| Coconut | *Cocos nucifera* | 12 | evergreen | a single crown | fronds | ringed | coconuts |
| Coast redwood | *Sequoia sempervirens* | 16 | evergreen | conical, high above a clear trunk | needles | fluted, buttressed | red wood, the tallest in the set |
| Dragonwood | *Dracaena draconis* (fantasy) | 7 | evergreen | cylindrical (umbrella) | rosettes | mottled | red wood, orange blossom |
| American elm | *Ulmus americana* | 15 | deciduous | inverse conical (vase) | clumps | furrowed | upswept limbs arching out, yellow in fall |
| Ebony | *Diospyros ebenum* | 12 | evergreen | tend flame | clumps | plated | |
| Hickory | *Carya ovata* | 11 | deciduous | tend flame | clumps | shaggy | gold in fall |
| Italian cypress | *Cupressus sempervirens* | 14 | evergreen | flame (a narrow column) | clumps (small, dense) | shaggy | little snow |
| Jacaranda | *Jacaranda mimosifolia* | 12 | deciduous (briefly) | hemispherical (umbrella) | clumps | furrowed | violet blossom on nearly bare wood |
| Larch | *Larix decidua* | 14 | deciduous | conical | needles | plated | a conifer that turns gold and drops its needles |
| Lignum vitae | *Guaiacum officinale* | 6 | evergreen | spherical | clumps | mottled | blue blossom |
| Mahogany | *Swietenia macrophylla* | 15 | evergreen | hemispherical | clumps | plated, buttressed | red spring flush, seed capsules |
| Mango | *Mangifera indica* | 9 | evergreen | spherical | clumps | furrowed | bronze new leaves, blossom, mangoes |
| Mesquite | *Prosopis glandulosa* | 6 | deciduous | hemispherical (sprawling) | clumps | shaggy | yellow catkins, bean pods |
| Oak | *Quercus robur* | 10 | deciduous | hemispherical | clumps | furrowed | keeps dead leaves in winter |
| Olive | *Olea europaea* | 6 | evergreen | spherical | clumps | fluted | blossom, gnarled when ancient |
| Osage | *Maclura pomifera* | 8 | deciduous | hemispherical | clumps | furrowed | gold in fall |
| Pine | *Pinus sylvestris* | 10 | evergreen | hemispherical, high above a bare orange trunk | needles (separate pads) | plated | snow |
| Sugar maple | *Acer saccharum* | 14 | deciduous | spherical (broad oval) | clumps | furrowed | red, orange, and gold in fall |
| Spruce | *Picea abies* | 13 | evergreen | conical (tiered spire) | needles | plated | hanging cones, heavy snow |
| Teak | *Tectona grandis* | 14 | deciduous (dry season) | spherical | clumps | furrowed | large leaves, white panicles |
| Weeping willow | *Salix babylonica* | 11 | deciduous | hemispherical, weeping | fronds (drooping strands) | furrowed | bare hanging twigs in winter, catkins |

Wood densities and stiffness come from the USDA *Wood Handbook* where the species is listed, so heavy woods sway slower and fall harder.

### Adding or tuning a species

Genomes are data, so a new species needs no new code unless it needs a new kind of foliage or bark. The loop runs entirely through MCP:
1. **Create it.** `config_set` (table `tree_v1_genomes`) with `from` copies the closest species, and `values` reshapes it. Every value is checked against the channel's types and ranges, and enums are given by name.
2. **Look at it.** Photograph it at several settings, and compare it with its reference photos:
   - `pipeline_sheet` with rows `species` (narrowed by `row_values`) and cols `season`, then cols `age`;
   - `pipeline_view` for close-ups, including from below and from the side;
   - the entry's `reference` photos (below), side by side with the views, or in the viewport's corner (`workspace_select` with `reference`).
3. **Tune and repeat.** Change values with `config_set`, then rephotograph. `config_get` with `schema` lists every field's range.
4. **Check the native build.** `perf_stress` with plan `quick` and phase `diversity` checks that native output still matches WebAssembly. `config_set` regenerates the native runtime's compiled-in genomes itself.

The eight species added on 2026-10-11 were made this way. Coconut needed two additions to the genome channel (its draft was patched in place): `fronds` foliage and `ringed` bark. Birch needed `papery` bark.

Four more were added the same day for biome coverage: weeping willow (wetland and riverbank), baobab (dry tropical woodland), bristlecone pine (alpine treeline), and coast redwood (temperate rainforest). The willow's curtains are `fronds` on long limbs that droop to the ground (clumps sat like beads on the twig tips). That needed one change to `tree_mesh_v1`: deciduous fronds now drop in winter, keeping `winter_keep` of them as dead leaves, where before they stayed on (coconut is evergreen, so it is unchanged). Native output still matches WebAssembly on every stage for all four in winter and summer, and for coconut.

Six more were added on 2026-10-12, with every species compared against its reference photos: sugar maple (temperate hardwood forest, the fall blaze), Italian cypress (Mediterranean column), cedar of Lebanon (mountain, flat tiers), American elm (the vase-shaped street tree), larch (a deciduous conifer of mountains and the boreal north), and jacaranda (subtropical highland, violet blossom). They needed no new code. The cedar's level needle plates are `rosettes` foliage; with `needles` the crown was one blob. Italian cypress, cedar of Lebanon, and jacaranda aren't in the *Wood Handbook*, so their density and stiffness are estimates; larch uses western larch's.

The comparison also changed six existing species, which had drifted from their photos:
- **Pine** read as a broadleaf blob: big needle clumps filled the crown from a third of the way up. A Scots pine has a long bare orange trunk under a high crown of separate pads on long limbs. It now has `crown_ratio` 0.5, a `hemispherical` crown, 26 limbs in whorls of 3 (`branch_length` 0.38), needle pads at `clump` 0.045 and `leafiness` 0.9, 4 twigs, and orange upper bark. Young pines also get the high crown, because `crown_ratio` doesn't change with age.
- **Acacia** stacked plates down the trunk; it is now an umbrella on a bare trunk (limbs at 40°, curving up: `branch_curve` 0.35, `twig_curve` 0.5).
- **Birch** was a dense blob; it is airier, with the white trunk and limbs showing (`clump` 0.04, `leafiness` 0.5, hanging twigs at `twig_curve` -1.9).
- **Cherry** was upright; it now spreads (`branch_angle` 65, `branch_length` 0.8).
- **Mesquite** sprawled on the ground when mature; it now stands with an open crown (`crown_ratio` 0.65, `branch_angle` 50).
- **Sugar maple** (new) was narrow at first; it is a broad oval with 16 limbs (`branch_length` 0.8).

Native output matches WebAssembly on every stage for all eleven, in all four seasons, young and ancient (88 configurations).

### Reference photos

Each species links to photos of the real tree in `demo/reference/trees/<species>/` through its entry's `reference` list (`config_set` with `reference`), 1 to 3 photos each. Every photo is public domain or CC0, from Wikimedia Commons, so no attribution is required. Its JSON sidecar still records the source page, original file, license, author, and retrieval date. Each photo is an 800-pixel copy. Tick **Reference** in the viewport (or `workspace_select` with `reference: true`) to show the photo in the corner beside the live tree; clicking it steps through the photos. They are also workspace items under **Reference**.

Free whole-tree photos were scarce for a few species. Ebony has only a photo of its leaves and hickory one of its bark. The bristlecone photo is a cultivated Rocky Mountain bristlecone (*Pinus aristata*), shrubby where the game's tree is the gnarled Great Basin one. The second elm photo is a European white elm (*Ulmus laevis*).

Spruce was revised after review, because its top ended in a round clump with a gap below it. Conical crowns now taper their top limbs to 6% of the bottom limbs' length (it was 20%), so pine and spruce end in a point. Spruce also got smaller needle tufts (`clump` 0.03), flatter limb tips (`tip_up` 0.35), shorter twigs (`twig_length` 0.4), more tiers (`branches` 48), and fewer dead limbs when ancient (`dead_limbs` 0.08).

## Flow and caching

The viewport's **Flow** tab draws this graph: `species` feeds a purple lookup box (`config/tree_v1_genomes`), and the entry it finds binds the rig's `genome` field. `pipeline_flow` returns the same as data.

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `rig` | species, age, season, seed, detail | 28 × 4 × 4 × 1000 × 3 = 1,344,000 |
| `mesh` | the same (its input is the whole rig) | 1,344,000 |
| `colorize` | the same | 1,344,000 |
| `animate` | every parameter | every frame, per instance |

Wind and time never rebuild a tree: they rerun only `animate`, which reads two fields of the rig (`bones`, `pieces`), not the mesh. In the native runtime, every tree with the same five build parameters shares one mesh, so a forest of 10 kinds of tree costs 10 meshes, however many instances it has.

The probe (`pipeline_flow` with `probe`, or the Flow tab's button) varies each parameter and compares every stage's output. It found one place a cache could be shared:
- **`detail` reaches `animate`** through the rig's bones, but never changes its output. The skeleton is the same at every level of detail, so poses could be shared across them.
- **Conditional dependencies are invisible to it.** `break_heading` shows no change when probed on a standing tree, because it only matters once the tree breaks.

## Performance

Native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan), from `demo/perf/report-tree_v1-*.json`:

| | Native | WebAssembly (foundry) |
|---|---|---|
| Growing a tree (rig + mesh + colorize) | about 0.6 ms; 2.4 ms from spawn to drawable (p50) | about 70 ms |
| Posing a tree (`animate`) | about 7 µs | about 15 ms, almost all instance setup |
| Triangles | 3,600 (p50) to 23,500 (max) at high detail | |

- **Forest.** 3200 trees run at 4.1 ms p95, sharing 10 meshes with a 99.7% cache hit rate. About 16,000 trees fit a 60 fps frame by CPU frame time, but the GPU frame measured on its own exceeds 16.7 ms above about 12,800.
- **Bulk breaks.** Felling 100 trees in one frame raises the frame time from 0.4 to 0.6 ms. They settle in 3 s, and memory comes back to within 0.1 MB.
- **Soak.** The runtime's memory returns to the same level after every drain.
- **Matches what was approved.** Every stress run checks 100 sampled trees and their poses byte for byte against WebAssembly.

Run `perf_stress` with plan `quick` after a change and `standard` before asking for approval (`design/quality.perf.md`). To fly through a forest yourself, run:

```sh
build/linux-release/stress/stress_tree_v1 --plan explore --window
```

## QA

- **Contact sheets.** `pipeline_sheet`: species × season, species × age, age × seed.
- **The QA video.** The viewport's **Video** tab, or `pipeline_video`, exports every combination of species, age, season, and detail at about a second each, behind a 5-second title slide. Each clip orbits the tree with its parameters called out, and an index of start times sits beside the video in `qa/videos/`. The last full run, with 18 species, was 864 variations: 14.5 minutes of video, exported by the native runtime in about 2. With 22 species it was 1,056 variations, exported in 145 s; with 28 it is 1,344.
- **The live instance.** In the foundry, select `pipeline:tree_v1` or a species under Config. The **View** tab plays the wind and the break.

## Known limits

- **Level of detail and distinct trees.** The runtime doesn't yet pick `detail` from distance, or cap how many distinct seeds it builds at each quality setting (`design/systems.md`, "Stable randomness"). The game sets both directly.
- **Coconuts.** They hang inside the crown, where the drooping fronds hide them from the side, as on a real palm. They show from above and below.
- **Break debris.** Limbs that fly off can end partly in the ground, and falling leaves aren't particles yet.
- **Multi-stemmed trees.** Mesquite approximates its multiple stems with limbs that leave the trunk near the ground. The rig grows one stem.
