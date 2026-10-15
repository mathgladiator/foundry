# The tree pipeline (`tree`)

`tree` grows a tree from a species, an age, a season, and a seed, and then animates it: it sways in the local wind or, once broken, topples and throws its limbs. It is the first pipeline the foundry built end to end, and the first to run in the flattened native runtime.

- **The pipeline:** `demo/pipelines/tree.json` (draft)
- **Systems:** `tree_rig`, `tree_mesh`, `tree_colorize`, `tree_animate` (drafts, in `src/systems/`)
- **Channels:** the `tree_*` family (drafts, in `src/channels/`)
- **Genomes:** 32 species in the config table `demo/config/tree_genomes/` (drafts; `design/flow.md`, "Config"), each linked to public-domain or CC0 photos in `demo/reference/trees/`
- **Design:** `design/systems.md`, "Trees"

## What the game asks for

The pipeline's parameters are its whole interface. The generated C API (`src/library/tree.h`, part of `runtime.h`) gives each one a field of `foundry_tree_params` and a setter, and each event a trigger.

| Parameter | Type | Range | What it does | Rebuilds the tree? |
|---|---|---|---|---|
| `species` | config | 32 species | Everything that makes an oak an oak (see "Species") | yes |
| `age` | enum | sapling, young, mature, ancient | Height and girth, branching, dead limbs and moss on ancient trees | yes |
| `season` | enum | winter, spring, summer, fall | Leaves, blossom, fruit, fall colour, bare winters, snow | yes |
| `fruit` | bool | | The tree still carries its fruit (in the seasons it fruits). Clear it when the player picks it; the tree rebuilds bare and shares that build with every other picked tree like it. It applies only to the 14 species whose fruit can be picked (`pickable`, see "Fruit"); for the rest it is disabled and ignored, and their cones or propagules stay on | yes, where it applies |
| `stump` | bool | | Only the stump is left. The game sets it once a felled tree's fall is over (`break_over` in its result); the mesh draws the stump and its roots, and the trunk's collision body becomes the stump's | yes |
| `seed` | u32 | 1 to 1000 | Which of the distinct trees of that species, age, and season | yes |
| `detail` | enum | high, medium, low | The finest level of detail the game allows (ring sides, foliage clumps' facets, sprigs); the runtime draws coarser levels by size on screen (`design/submit.md`, "Cost") | yes |
| `wind_speed` | f32 | 0 to 25 m/s | The local wind; 0 lets a standing tree sleep | no: the pose only |
| `wind_heading` | f32 | 0 to 360° | The direction the wind blows toward, from +Z toward +X | no |
| `gust_damping` | f32 | 0 to 1 | 0 is raw gusts, 1 is a steady wind | no |
| `time` | time | | The instance's clock; the runtime runs it | no |
| `break_time` | event | | Seconds since the tree broke, negative while it stands. The game starts it with `foundry_tree_start_break` | no |
| `break_heading` | f32 | 0 to 360° | The direction the tree falls | no |

```c
foundry_tree_params p;
foundry_tree_default_params(&p);
p.species = FOUNDRY_TREE_SPECIES_BIRCH;
p.season = FOUNDRY_TREE_SEASON_FALL;
p.seed = 42;
p.fruit = true;
foundry_instance tree = foundry_tree_spawn(rt, &p, transform);
foundry_tree_set_wind_speed(rt, tree, 9.0f);
if (foundry_tree_fruit_applies(p.species))  /* offer picking only where there is fruit */
  foundry_tree_set_fruit(rt, tree, false);  /* the player picked it */
foundry_tree_start_break(rt, tree);  /* a woodcutter finished */
/* ... after later updates, when the tree is in foundry_changed's list: */
foundry_tree_result r;
foundry_tree_get_result(rt, tree, &r);
if (r.break_over && !last.break_over) foundry_tree_set_stump(rt, tree, true);  /* its rising edge (or the game's own timer) */
```

Each update leaves the tree a result of current values (`design/api.md`, "Instance results"): `break_over` (it broke and its fall has come to rest) and `ready` (the build it last asked for is the one drawn: after `set_stump`, the stump). `foundry_changed` lists the instances whose results changed, so the game reads only those.

When the runtime has the game's Jolt world (`foundry_runtime_desc.physics`), each tree has one solid collision body: a cylinder of `stump_radius` up to the crown, or up to the cut once it's a stump. The runtime adds it when the build is ready, swaps it on a rebuild (the stump for the trunk), and removes it with the tree (`tests/results_test.cpp`: a felled oak's body goes from 3.40 m tall to 0.60 m).

## How it's built

Four systems, each one channel in and one out:

```
species, age, season, fruit, stump, seed, detail
        |
   tree_spec ──> [tree_rig] ──> tree_rig ──> [tree_mesh] ──> tree_body ──> [tree_colorize] ──> tree_model   (drawn)
                          │
                          └─ bones, pieces ──> tree_motion ──> [tree_animate] ──> tree_pose   (every frame; poses the model)
                                                 ^
                     wind_speed, wind_heading, gust_damping, time, break_time, break_heading, seed
```

### `tree_rig`: the skeleton

The rig grows the branch structure from the genome. It works in five steps:
1. **Branches.** It grows a stem polyline and up to three orders of branches, using Weber and Penn (1995) style parameters: counts, angles, divergence or whorls, a crown-shape envelope, curvature, and gnarliness.
   - **Excurrent species** (pine, spruce, birch, coconut) keep the stem running to the top.
   - **Decurrent species** (oak, apple) turn their upper limbs into leaders.
   - `cluster` pushes branches toward the tip. That is how umbrella crowns (acacia, dragonwood) and a palm's single crown of fronds come about.
2. **Radii.** Branch radii follow the pipe model (Leonardo's rule, with exponent 2). They are scaled so the trunk at the ground matches the species' `trunk_diameter` from nature, by age (`age_girth`). A swamp tree's `swell` widens the bottom quarter of the stem into a bottle-like base, tapering up.
5. **Stump and footprint.** `stump_radius` is the trunk's radius at the cut (swell included): what collides. `footprint_radius` is the ground contact of the species at its largest (ancient) age, its flared, swollen base with the flare at its mean around the trunk, and `footprint_cells` the smallest hex arrangement whose inner circle, inset 0.1 m, holds it: 1 cell up to 0.766 m, 7 up to 1.9 m, 19 up to 3.51 m, else 37 (`design/plotter.md`, "Footprints and conformance"). An oak's is 7 cells (1.71 m), an apple's 1 (0.49 m), a bald cypress's 19, a baobab's 37.
3. **Wind bones.** There is a rigid stump, three stem bones, two bones per limb, and one per twig, capped at 128. Each bone's natural frequency comes from a cantilever beam made of the species' wood. Bare trees catch less wind.
4. **Break pieces.** The pieces are the stump, the log above the cut, and each limb.
5. **Foliage anchors.** Anchors sit on the outer branches, sized from the genome, and are capped at 260 per tree.

### `tree_mesh`: the body

The mesh system wraps the skeleton and dresses it for the season:
- **Bark.** Bark is built as rings along every branch, with relief and shade from the genome's bark style. The styles are furrowed, plated, shaggy, mottled, fluted, ringed, and papery.
  - Ringed (a palm's leaf scars) and papery (a birch's lenticels) need rings closer together than the rig's points, so the mesh splits the stem's segments for them.
- **The stump.** The stump flares at its base and has a cut face where the log will break off. With `stump` set, the mesh draws only the stump, capped with end grain, and its knees and prop roots: no log, limbs, foliage, fruit, or moss.
- **Collision.** A solid cylinder from the ground (`physics_body`): the trunk up to the crown, or the stump up to the cut. It is static for the build: the trunk stands still however the wind moves the crown, and a falling log has no collision.
- **Foliage.** Foliage is chosen per clump by season:
  - **Clumps:** displaced icospheres, for broadleaves.
  - **Needles:** stretched tufts.
  - **Rosettes:** flattened pads (acacia, dragonwood).
  - **Fronds:** pinnate leaflets along each first-order limb, after a bare stalk, for the coconut, and on long drooping limbs, the willow's curtains. Fronds take the place of clumps; deciduous ones drop in winter.
- **Seasons.**
  - Deciduous trees are bare in winter, keeping `winter_keep` of their dead leaves (an oak's marcescence).
  - Spring brings blossom, and `blossom_leaf` says how much leaf is out beside it: cherries flower on bare wood.
  - Fall drops `fall_drop` of the leaves.
  - Fruit hangs in the seasons the genome's `fruit_seasons` gives, as ripe as it says (colorize mixes `fruit_unripe` to `fruit_ripe`, each fruit a little ahead or behind), on the outer lower side of the clumps, out from the trunk and past the clump's bumps so it shows. Where a deciduous clump has dropped, the fruit hangs on the bare twig (a baobab's pods in the dry season). Pickable fruit shows only while the request's `fruit` is set; cones and propagules always show. A palm hangs its fruit in a bunch against the trunk, below the leaflets of the frond bases.
- **Roots and moss**, for swamp trees:
  - **Knees** (`knees`): blunt woody cones rising from the roots around a bald cypress, more and taller with age.
  - **Prop roots** (`prop_roots`): a mangrove's roots arching from the lower trunk out and down into the ground.
  - **Spanish moss** (`hanging_moss`): grey strands hanging from the near-level limbs and twigs in every season, as two crossed ribbons riding the branch's bone, coloured by `moss_color`.
  Knees and roots are rigid on the stump's bone, so they stay put when the tree breaks.
- **Level of detail.** `detail` sets the ring sides (12, 9, or 7 on the trunk), the foliage clumps' facets (an icosahedron subdivided once, 80 faces; the icosahedron, 20; an octahedron 15% larger, 8), and whether sprigs appear. Every level keeps every clump, so the crown's silhouette stays where it was. Levels used to keep two thirds and then half of the clumps, grown to cover the gaps; keeping them all with fewer facets halves the difference from the finest at medium and costs fewer triangles (`demo/perf/report-tree-detail-20261013T002122Z.json`).

### `tree_colorize`: the paint

Colorize maps each vertex's paint region (bark, wood, leaf, blossom, fruit, dead leaf, dead wood, hanging moss) and shade to the genome's colours for the season. Fall mixes two colours per clump for variety. Mature and ancient trees grow moss on surfaces that face up. In winter, snow settles on anything facing up more than the genome's `snow` allows, which is never for tropical trees.

### `tree_animate`: the simulator

This is the only stage that runs every frame. It returns a 4×4 matrix per bone, plus whether the tree is broken and whether it has settled.
- **Wind.** Each bone's response is the closed-form steady state of a damped oscillator driven by three gust sinusoids, plus a resonant sway, so there's no state to integrate and any time can be evaluated directly. Bends follow the wind and run across it.
- **The break.** The log falls about the cut as a rod toppling under gravity, integrated at a fixed 1/120 s step. When the crown hits the ground, the limbs fly off ballistically and slide to rest.
- **Sleep.** `settled` lets the instance sleep: a tree in no wind settles at once, and a broken tree once everything rests.

## Species

Each species is a genome (`tree_genome`): about 55 fields of shape, seasons, wood, and colour, with ranges from the channel schema. The genomes are entries of the config table `config/tree_genomes/`, a hidden detail of the pipeline: the game sees only their names (`FOUNDRY_TREE_SPECIES_OAK`), and the native runtime compiles their values in. Selecting one in the workspace opens the foundry's generic config editor: a widget for each field from the channel schema, grouped and coloured as the table's `table.json` says.

| Species | Latin | Mature height (m) | Leaves | Crown | Foliage | Bark | Extras |
|---|---|---|---|---|---|---|---|
| Acacia | *Vachellia tortilis* | 8 | evergreen | cylindrical (umbrella) | rosettes | furrowed | cream blossom, seed pods |
| Apple | *Malus domestica* | 5.5 | deciduous | spherical | clumps | plated | blossom, apples |
| Bald cypress | *Taxodium distichum* | 15 | deciduous (a conifer) | conical to tend flame | needles | shaggy | swollen buttressed base, knees, Spanish moss, rust-orange in fall |
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
| Southern live oak | *Quercus virginiana* | 12 | evergreen | hemispherical, far wider than tall | clumps | furrowed | low sweeping limbs, heavy Spanish moss |
| Mahogany | *Swietenia macrophylla* | 15 | evergreen | hemispherical | clumps | plated, buttressed | red spring flush, seed capsules |
| Mango | *Mangifera indica* | 9 | evergreen | spherical | clumps | furrowed | bronze new leaves, blossom, mangoes |
| Red mangrove | *Rhizophora mangle* | 6 | evergreen | hemispherical | clumps | mottled | arching prop roots, propagules |
| Mesquite | *Prosopis glandulosa* | 6 | deciduous | hemispherical (sprawling) | clumps | shaggy | yellow catkins, bean pods |
| Oak | *Quercus robur* | 10 | deciduous | hemispherical | clumps | furrowed | keeps dead leaves in winter |
| Olive | *Olea europaea* | 6 | evergreen | spherical | clumps | fluted | blossom, gnarled when ancient |
| Osage | *Maclura pomifera* | 8 | deciduous | hemispherical | clumps | furrowed | gold in fall |
| Pine | *Pinus sylvestris* | 10 | evergreen | hemispherical, high above a bare orange trunk | needles (separate pads) | plated | snow |
| Sugar maple | *Acer saccharum* | 14 | deciduous | spherical (broad oval) | clumps | furrowed | red, orange, and gold in fall |
| Spruce | *Picea abies* | 13 | evergreen | conical (tiered spire) | needles | plated | hanging cones, heavy snow |
| Teak | *Tectona grandis* | 14 | deciduous (dry season) | spherical | clumps | furrowed | large leaves, white panicles |
| Water tupelo | *Nyssa aquatica* | 14 | deciduous | tend flame, high | clumps | furrowed | swollen bottle base, red-purple fall, purple fruit |
| Weeping willow | *Salix babylonica* | 11 | deciduous | hemispherical, weeping | fronds (drooping strands) | furrowed | bare hanging twigs in winter, catkins |

Wood densities and stiffness come from the USDA *Wood Handbook* where the species is listed, so heavy woods sway slower and fall harder. Red mangrove isn't listed, so its values are estimates for a dense tropical hardwood.

### Trunks and footprints

Each genome's `trunk_diameter` is its mature trunk's diameter at the ground, from nature (typical for the species, held near the earlier proportions so no tree's look jumps). The footprint is what the species reserves on the hex grid at its largest age: the ancient tree's flared, swollen base (computed by `tree_rig`; plotters read it).

| Species | Trunk (m) | Footprint radius (m) | Cells | Species | Trunk (m) | Footprint radius (m) | Cells |
|---|---|---|---|---|---|---|---|
| Acacia | 0.5 | 0.50 | 1 | Lignum vitae | 0.45 | 0.55 | 1 |
| Apple | 0.4 | 0.49 | 1 | Southern live oak | 1.8 | 3.17 | 19 |
| Bald cypress | 1.2 | 3.10 | 19 | Mahogany | 1.4 | 2.72 | 19 |
| Baobab | 3.0 | 4.42 | 37 | Mango | 0.9 | 1.10 | 7 |
| Birch | 0.35 | 0.32 | 1 | Red mangrove | 0.35 | 0.35 | 1 |
| Bristlecone pine | 0.8 | 1.31 | 7 | Sugar maple | 0.8 | 0.90 | 7 |
| Cedar of Lebanon | 1.4 | 1.34 | 7 | Mesquite | 0.5 | 0.61 | 1 |
| Cherry | 0.5 | 0.57 | 1 | Oak | 1.1 | 1.71 | 7 |
| Coconut | 0.4 | 0.32 | 1 | Olive | 0.7 | 1.56 | 7 |
| Italian cypress | 0.6 | 0.57 | 1 | Osage | 0.6 | 0.76 | 1 |
| Dragonwood | 1.0 | 1.14 | 7 | Pine | 0.6 | 0.58 | 1 |
| Ebony | 0.7 | 0.85 | 7 | Coast redwood | 2.0 | 3.19 | 19 |
| American elm | 1.1 | 1.71 | 7 | Spruce | 0.7 | 0.67 | 1 |
| Hickory | 0.6 | 0.62 | 1 | Teak | 0.9 | 1.10 | 7 |
| Jacaranda | 0.6 | 0.69 | 1 | Water tupelo | 0.75 | 2.48 | 19 |
| Larch | 0.7 | 0.67 | 1 | Weeping willow | 0.9 | 1.17 | 7 |

A swamp tree's swell makes its footprint large: a water tupelo's 0.75 m trunk swells to a 2.5 m base, nineteen cells.

### Fruit

Each genome says whether its fruit can be picked (`pickable`: food, nuts, spice, or fodder a player gathers by hand) and how ripe it is per season (`fruit_seasons`: winter, spring, summer, fall; 0 is none). A pickable species' fruit follows the game's `fruit` switch; a species that isn't pickable has the switch disabled, and whatever it bears (cones, propagules, capsules) stays on. The workspace disables the `fruit` checkbox for those species, and the QA video gives them one clip with `fruit` marked disabled.

| Species | Bears | Pickable | Ripeness (W, Sp, Su, F) | Notes |
|---|---|---|---|---|
| Acacia | coiled seed pods | yes (fodder) | 0.8, 0, 0.4, 1 | pods ripen in the dry season and hang on |
| Apple | apples | yes | 0, 0, 0.35, 1 | |
| Bald cypress | round cones | no | 0, 0, 0.3, 1 | green, browning and shattering by winter; new |
| Baobab | velvety pods | yes | 1, 0.8, 0.2, 0.6 | ripen in the dry season, hanging on the bare tree |
| Birch | tiny catkins and nutlets | no | none | too small to draw |
| Bristlecone pine | cones | no | 0.9, 0.6, 0.5, 1 | held for years |
| Cedar of Lebanon | upright barrel cones | no | none | they stand on the limbs, which the mesh can't draw yet |
| Cherry | few, bitter | no | none | *Prunus serrulata* is an ornamental that rarely fruits |
| Coconut | coconuts | yes | 0.6 all year | fruits continuously |
| Italian cypress | round cones | no | 0.8, 0.6, 0.6, 1 | held into a second year |
| Dragonwood | berries | yes | 0.3, 0, 0.4, 1 | after the dragon's blood tree: green, then black, then ripe; new |
| Ebony | small round fruit | yes | 0, 0.2, 0.6, 1 | edible (Sturtevant); I found no source for the ripe colour, so it uses the genus's yellow-brown; new |
| American elm | samaras | no | none | papery, spring |
| Hickory | nuts in husks | yes | 0, 0, 0.3, 1 | new |
| Jacaranda | flat woody capsules | no | none | |
| Larch | small cones | no | 1, 0.4, 0.6, 1 | they stay on the bare winter twigs |
| Lignum vitae | orange capsules, red arils | no | 0, 0, 0.5, 1 | showy, not food; new |
| Southern live oak | acorns | yes | 0, 0, 0.3, 1 | |
| Mahogany | upright woody capsules | no | 1, 0.5, 0.3, 0.7 | split in the dry season |
| Mango | mangoes | yes | 0, 0.3, 1, 0 | ripen in summer |
| Red mangrove | propagules | no | 0.6 all year | seedlings, not food |
| Sugar maple | samaras | no | none | |
| Mesquite | bean pods | yes | 0, 0, 0.5, 1 | |
| Oak | acorns | yes | 0, 0, 0.3, 1 | new |
| Olive | olives | yes | 1, 0, 0.25, 0.6 | green in summer, black by winter; new |
| Osage orange | hedge apples | yes | 0, 0, 0.4, 1 | big green balls in fall; new |
| Pine | cones | no | 0.8, 0.6, 0.6, 1 | new |
| Coast redwood | small cones | no | 0.8, 0.6, 0.6, 1 | |
| Spruce | hanging cones | no | 0.8, 0.6, 0.7, 1 | |
| Teak | small dry capsules | no | none | |
| Water tupelo | dark purple drupes | yes | 0, 0, 0.4, 1 | ripen in autumn (USDA Forest Service) |
| Weeping willow | catkins | no | none | |

Sources for the less familiar ones: the dragon's blood tree's berries ripen green to black to orange over about five months after its early-spring flowers ([Wikipedia](https://en.wikipedia.org/wiki/Dracaena_cinnabari), [Llifle](https://llifle.com/Encyclopedia/TREES/Family/Dracaenaceae/32991/Draco_cinnabari)); lignum vitae's capsule ripens orange around seeds in a red aril ([Levy Preserve](https://levypreserve.org/plant-listings/guaiacum-officinale/)); water tupelo's fruit turns dark purple as it ripens in autumn ([USDA Forest Service](https://www.fs.usda.gov/nsl/Wpsm/Nyssa.pdf)); East Indian ebony bears an edible fruit ([Pl@ntUse, after Sturtevant](https://uses.plantnet-project.org/en/Diospyros_(Sturtevant,_1919))).

Fruit is drawn larger than life (`style.md` §3 wants it chunky): at true size an acorn on a 10 m oak is one pixel at the sheet's scale. After the fix that moved fruit out of the clumps, every pickable species was checked up close at its ripest season with `fruit` on and off. Oak acorns, tupelo drupes, and live oak acorns were darkened to stand out from their fall leaves, and fruit grew 1.2 to 2 times (`fruit_size`; apple 0.065 to 0.08 m, oak 0.03 to 0.06 m, acacia 0.05 to 0.09 m). Before the fix, fruit sat at 0.92 of the clump radius, inside the clump's bumps: an apple tree's 14,560 triangles of apples changed 0.07% of the picture.

### Adding or tuning a species

Genomes are data, so a new species needs no new code unless it needs a new kind of foliage or bark. The loop runs entirely through MCP:
1. **Create it.** `config_set` (table `tree_genomes`) with `from` copies the closest species, and `values` reshapes it. Every value is checked against the channel's types and ranges, and enums are given by name.
2. **Look at it.** Photograph it at several settings, and compare it with its reference photos:
   - `pipeline_sheet` with rows `species` (narrowed by `row_values`) and cols `season`, then cols `age`;
   - `pipeline_view` for close-ups, including from below and from the side;
   - the entry's `reference` photos (below), side by side with the views, or in the viewport's corner (`workspace_select` with `reference`).
3. **Tune and repeat.** Change values with `config_set`, then rephotograph. `config_get` with `schema` lists every field's range.
4. **Check the native build.** `perf_stress` with plan `quick` and phase `diversity` checks that native output still matches WebAssembly. `config_set` regenerates the native runtime's compiled-in genomes itself.

The eight species added on 2026-10-11 were made this way. Coconut needed two additions to the genome channel (its draft was patched in place): `fronds` foliage and `ringed` bark. Birch needed `papery` bark.

Four more were added the same day for biome coverage: weeping willow (wetland and riverbank), baobab (dry tropical woodland), bristlecone pine (alpine treeline), and coast redwood (temperate rainforest). The willow's curtains are `fronds` on long limbs that droop to the ground (clumps sat like beads on the twig tips). That needed one change to `tree_mesh`: deciduous fronds now drop in winter, keeping `winter_keep` of them as dead leaves, where before they stayed on (coconut is evergreen, so it is unchanged). Native output still matches WebAssembly on every stage for all four in winter and summer, and for coconut.

Six more were added on 2026-10-12, with every species compared against its reference photos: sugar maple (temperate hardwood forest, the fall blaze), Italian cypress (Mediterranean column), cedar of Lebanon (mountain, flat tiers), American elm (the vase-shaped street tree), larch (a deciduous conifer of mountains and the boreal north), and jacaranda (subtropical highland, violet blossom). They needed no new code. The cedar's level needle plates are `rosettes` foliage; with `needles` the crown was one blob. Italian cypress, cedar of Lebanon, and jacaranda aren't in the *Wood Handbook*, so their density and stiffness are estimates; larch uses western larch's.

The comparison also changed six existing species, which had drifted from their photos:
- **Pine** read as a broadleaf blob: big needle clumps filled the crown from a third of the way up. A Scots pine has a long bare orange trunk under a high crown of separate pads on long limbs. It now has `crown_ratio` 0.5, a `hemispherical` crown, 26 limbs in whorls of 3 (`branch_length` 0.38), needle pads at `clump` 0.045 and `leafiness` 0.9, 4 twigs, and orange upper bark. Young pines also get the high crown, because `crown_ratio` doesn't change with age.
- **Acacia** stacked plates down the trunk; it is now an umbrella on a bare trunk (limbs at 40°, curving up: `branch_curve` 0.35, `twig_curve` 0.5).
- **Birch** was a dense blob; it is airier, with the white trunk and limbs showing (`clump` 0.04, `leafiness` 0.5, hanging twigs at `twig_curve` -1.9).
- **Cherry** was upright; it now spreads (`branch_angle` 65, `branch_length` 0.8).
- **Mesquite** sprawled on the ground when mature; it now stands with an open crown (`crown_ratio` 0.65, `branch_angle` 50).
- **Sugar maple** (new) was narrow at first; it is a broad oval with 16 limbs (`branch_length` 0.8).

Native output matches WebAssembly on every stage for all eleven, in all four seasons, young and ancient (88 configurations).

Four swamp trees were added on 2026-10-12, to pair with the swamp shrubs (saw palmetto, buttonbush) and flowers (blue flag iris, cattail): bald cypress, water tupelo, red mangrove, and southern live oak. They needed four additions to the genome channel (its draft patched in place): `swell`, `knees`, `prop_roots`, and `hanging_moss` with `moss_color`, built by `tree_rig` (the swell) and `tree_mesh` (the rest), and painted by `tree_colorize`. Every other species has them at 0. Compared with their photos, the cypress got finer needle pads and a lower flare, the tupelo a higher crown on a taller trunk, the mangrove more roots, and the live oak more moss.

The same day `fruit` became a parameter (a `bool` on `tree_spec`), so the game can pick a tree's fruit: the tree rebuilds without it. Then every species' fruit was researched and two fields were added to the genome (patched in place): `pickable` and `fruit_seasons` (see "Fruit").

### Reference photos

Each species links to photos of the real tree in `demo/reference/trees/<species>/` through its entry's `reference` list (`config_set` with `reference`), 1 to 3 photos each. Every photo is public domain or CC0, from Wikimedia Commons, so no attribution is required. Its JSON sidecar still records the source page, original file, license, author, and retrieval date. Each photo is an 800-pixel copy. Tick **Reference** in the viewport (or `workspace_select` with `reference: true`) to show the photo in the corner beside the live tree; clicking it steps through the photos. They are also workspace items under **Reference**.

Free whole-tree photos were scarce for a few species. Ebony has only a photo of its leaves and hickory one of its bark. The bristlecone photo is a cultivated Rocky Mountain bristlecone (*Pinus aristata*), shrubby where the game's tree is the gnarled Great Basin one. The second elm photo is a European white elm (*Ulmus laevis*).

Spruce was revised after review, because its top ended in a round clump with a gap below it. Conical crowns now taper their top limbs to 6% of the bottom limbs' length (it was 20%), so pine and spruce end in a point. Spruce also got smaller needle tufts (`clump` 0.03), flatter limb tips (`tip_up` 0.35), shorter twigs (`twig_length` 0.4), more tiers (`branches` 48), and fewer dead limbs when ancient (`dead_limbs` 0.08).

## Flow and caching

The viewport's **Flow** tab draws this graph: `species` feeds a purple lookup box (`config/tree_genomes`), and the entry it finds binds the rig's `genome` field. Red lines show what reaches the GPU: `colorize`'s vertex fields and indices to the vertex and index buffers (once per build, shared), and `animate`'s matrices to the bone buffer (every frame). `pipeline_flow` returns the same as data.

| Stage | Cache key | Distinct outputs |
|---|---|---|
| `rig` | species, age, season, fruit, stump, seed, detail | (14 × 2 + 18) × 4 × 4 × 2 × 1000 × 3 = 4,416,000: `fruit` counts only where it applies |
| `mesh` | the same (its input is the whole rig) | 4,416,000 |
| `colorize` | the same | 4,416,000 |
| `animate` | every parameter | every frame, per instance |

Wind and time never rebuild a tree: they rerun only `animate`, which reads two fields of the rig (`bones`, `pieces`), not the mesh. In the native runtime, every tree with the same six build parameters shares one mesh, so a forest of 10 kinds of tree costs 10 meshes, however many instances it has.

The probe (`pipeline_flow` with `probe`, or the Flow tab's button) varies each parameter and compares every stage's output. It found one place a cache could be shared:
- **`detail` reaches `animate`** through the rig's bones, but never changes its output. The skeleton is the same at every level of detail, so the runtime poses a tree once and draws whichever level its size asks for with that pose (it checks that the poses match before it does).
- **`fruit` reaches `rig`**, whose output carries the request through to the mesh, but it never changes the skeleton, and `animate` never changes with it. Probed on an oak in fall, it changes the rig's output (the request it carries), the mesh, and the colour; probed on a willow, it changes nothing, because the foundry reads it as off where it doesn't apply. The rig could be cached without it, and picking fruit would then rebuild only the mesh and colour.
- **`stump` reaches `rig`** the same way: it changes the request the rig carries, not the skeleton.
- **Conditional dependencies are invisible to it.** `break_heading` shows no change when probed on a standing tree, because it only matters once the tree breaks.

## Levels of detail

From the detail sizes (`demo/perf/report-tree-detail-20261013T002122Z.json`, 32 sampled trees, half of them stumps; `design/quality.perf.md`, "Detail sizes"):

| Level | Triangles (mean) | Drawn below, at high quality | medium | low |
|---|---|---|---|---|
| high | 6,927 | | | |
| medium | 3,076 | 32 px | 256 px or more | 256 px or more |
| low | 1,696 | 16 px | 256 px or more | 256 px or more |
| far (the runtime's) | 661 | never | 12 px | 256 px or more |

A size is the tree's bounding sphere on screen, in a 1080-pixel-tall view; at high quality a level switches in where the median tree differs from the finest on at most 10% of its pixels (20% at medium, 30% at low).

## Performance

Since the levels of detail changed (2026-10-13), the quick plan has been rerun (`demo/perf/report-tree-20261013T003154Z.json`): native matches WebAssembly on 100 of 100 sampled trees and their poses; 800 trees hold the budget at 0.7 ms p95 with 10 generations for 10 sets; felling 100 at once raises the frame from 0.2 to 0.5 ms, and they settle in 3.0 s. The numbers below are from the last standard run, before; it hasn't been rerun.

Native runtime, on an AMD Ryzen AI Max+ 395 with a Radeon 8060S (Vulkan), from the standard stress plan (`demo/perf/report-tree-20261012T193254Z.json`, 32 species, with `stump` and collision bodies in a toy Jolt world):

| | Native | WebAssembly (foundry) |
|---|---|---|
| Growing a tree (rig + mesh + colorize) | about 0.35 ms mean (0.11 + 0.21 + 0.03); 0.9 ms from spawn to drawable (p50), 4.2 ms p99 (a mature southern live oak in summer, in fruit) | about 70 ms (measured with 18 species) |
| Posing a tree (`animate`) | about 8 µs | about 15 ms, almost all instance setup |
| Triangles | 583 (p50), 17,017 (p95), 42,868 (max) at high detail | |

Half the sampled trees are now stumps (a few hundred triangles each), which is why the median triangles and growing time dropped from the last run (4,905 triangles and 1.9 ms p50 before `stump`).

- **Forest.** All 25,600 trees fit a 60 fps frame (14.9 ms p95), sharing 10 meshes with a 99.96% cache hit rate, each with its trunk's body in the toy Jolt world (25,600 bodies).
- **Bulk breaks.** Felling 100 trees in one frame raises the frame time from 0.4 to 0.5 ms. They settle in 3.0 s, and memory comes back within 0.1 MB. 1,600 felled at once still fit the frame budget.
- **Soak.** Five drains all came back within 7.5 MB, and the bodies balanced at every drain (none left in the runtime or the world).
- **Matches what was approved.** Every stress run through the foundry checks 100 sampled trees and their poses byte for byte against WebAssembly (100/100). `pipeline_set` with `verify_native` also matched every stage for all 32 species in winter and fall, fruit on and off (96 configurations), and for 24 stumps (8 species, including the swamp trees, at sapling, mature, and ancient, in wind and with a break started).
- **A stump is still.** A stump's rig keeps only the stump's piece and makes every bone rigid, so a stump sleeps in any wind and starting a break on it does nothing (`"settled": true`, `"broken": false`).

Run `perf_stress` with plan `quick` after a change and `standard` before asking for approval (`design/quality.perf.md`). To fly through a forest yourself, run:

```sh
build/linux-release/stress/stress_tree --plan explore --window
```

## QA

- **Contact sheets.** `pipeline_sheet`: species × season, species × age, age × seed.
- **The QA video.** The viewport's **Video** tab, or `pipeline_video`, exports every combination of species, age, season, and detail at about a second each, behind a 5-second title slide. Each clip orbits the tree with its parameters called out, and an index of start times sits beside the video in `qa/videos/`. The last full run, with 18 species, was 864 variations: 14.5 minutes of video, exported by the native runtime in about 2. With 22 species it was 1,056 variations, exported in 145 s. With 32 species and `fruit` it is 2,208, because species whose fruit isn't pickable get one clip with `fruit` marked disabled instead of two; with `stump` it doubles to 4,416, so pin `stump` (and `fruit` or `detail`) to keep a run short. Every discrete parameter's value is shown under its name at the top of each clip.
- **The live instance.** In the foundry, select `pipeline:tree` or a species under Config. The **View** tab plays the wind and the break.

## Known limits

- **Fruit is chunky and placed by rule.** It hangs on the clumps' outer lower side, larger than life. A coconut bunch shows from below and the side but hides under the frond skirt from above. Cedar's cones stand upright on real trees, which the mesh can't draw, so cedar has none.
- **Distinct trees.** The runtime picks the level of detail by size on screen now, but doesn't yet cap how many distinct seeds it builds at each quality setting (`design/systems.md`, "Stable randomness").
- **Break debris.** Limbs that fly off can end partly in the ground, and falling leaves aren't particles yet.
- **Multi-stemmed trees.** Mesquite approximates its multiple stems with limbs that leave the trunk near the ground. The rig grows one stem.
