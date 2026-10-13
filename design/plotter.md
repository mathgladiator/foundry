# The plotter: filling a region of hexes

A **plotter** turns a description of a place ("a cypress swamp", "an oak wood with a bramble edge", "a scree slope") into placements that any runtime can compute for an arbitrary set of hex cells, predictably. The game registers a **region** (a set of cells with their attributes) with a plotter, then asks it for growth: "give me up to 50 things that belong here". The region answers with placements (what goes where), and the game spawns them, owns them, and tells the region when they go away. The plotter only plots. It is not a simulator: it keeps no clock, ages nothing, and decides no consequences. The game paces growth and drives every instance.

The mental model is a survival game. A forest is a resource that the player harvests one tree at a time (an axe) or many at once (a bomb), and that the game can grow back by asking the plotter to fill what was cleared. When the plotter has nothing left to add, the region is stable, and it stays exactly as it is until something is taken away.

This document is a design. Nine rounds of the human's review are folded into the text, and no questions are open. Its first steps (Jolt, the tree's stump, and the first plotter, a "happy glen") are in the current milestone in `PLAN.md`; the rest waits.

**Built so far** (see "Status" at the end for the detail): the `plot_v1_*` channels and the `plot_scatter_v1` plot system (native matches WebAssembly); plotters as a workspace primitive, with the happy glen; the foundry's plotter view and its MCP tools; regions in the runtime's C API; and a region run in the stress tool. Footprints are single, flower7, flower19, and flower37 so far: lines (`line2`, `line3`), `tri3`, and boxes are designed, not built. Active entities, sensors, and contacts in results are designed, not built.

## Vocabulary

| Term | Meaning |
|---|---|
| **Cell** | One hex: pointy-top, circumradius 1 m (`style.md` §2), addressed by axial coordinates `(q, r)`. |
| **Footprint** | The cells a thing reserves on the ground: one cell, or an arrangement of cells (a line, a hex-like cluster, a box; `spine.md`, "Layer 1"). It is the footprint of the thing's conformance, its largest extent over its life, so a sapling reserves the cells its ancient self will need. Occupancy is by footprint; collision is by the smaller shape the pipeline outputs inside it. |
| **Region** | A set of cells with their attributes, bound to one plotter and a world seed. The region is a bookkeeping index of what occupies its cells. It never owns instances, and it plots on its own, without looking at its neighbours. |
| **Plotter** | A workspace item (`plotters/<name>.json`) that binds a **plot system** to a **biome config**. It is approved like a pipeline. |
| **Plot system** | A new kind of system: a pure black box with a stable input channel (cells, occupancy, exclusions, how many) and a stable output channel (placements). It does nothing but plot. New placement ideas (rows for an orchard, a ring of mushrooms) are new plot systems. |
| **Biome config** | The data a plot system is bound to: which kinds of thing go in this biome, how dense, how far apart, where. It is checked against the plot system's config channel, as a genome is checked against its channel. |
| **Kind** | One thing a plotter can place: a pipeline with some of its parameters fixed (`tree_v1` with `species: baldcypress`), or a fixed asset (`fixed/boulder_moss`). |
| **Layer** | A group of kinds placed together under one spacing rule: canopy, understory, ground cover, rocks. Layers resolve in order, and later layers see earlier ones. |
| **Candidate** | A place the plotter could put something: made by a cell from a hash, so it never depends on the rest of the region. |
| **Placement** | A candidate the plotter has handed to the game: "this kind, with these parameters, at (x, y, z), turned this way", plus a stable slot ID and its footprint. Its `y` comes from the heights the game passed in. |
| **Occupant** | A placement the region knows is there, because it handed it out or because the game restored it from a save. |

## The key scenario

1. A designer writes a biome description: *"A blackwater cypress swamp: bald cypress and water tupelo standing apart, Spanish moss, buttonbush and saw palmetto under them, blue flag iris and cattail at the open edges, river stones here and there."*
2. The agent turns it into a biome config for a plot system. It picks kinds from the pipelines' species tables (each book lists a biome per species), and sets densities, spacings, and clustering from what such a forest is like: stems per hectare, canopy cover, and its reference photos. It then plays the plotter in the foundry against generated regions and tunes it until the scene reads right from the game's camera.
3. The human play tests the plotter in the foundry (see "In the foundry"), and approves it or rejects it.
4. The approved plotter is compiled into the runtime. The game registers regions with it and asks for growth. The same seed, cells, and occupants give the same placements on every machine.

## Regions of hexes

### Coordinates

Cells use axial coordinates `(q, r)`, with `s = -q - r` implied. The distance between two cells is `(|dq| + |dr| + |ds|) / 2`, and a cell's centre in metres is `x = √3 (q + r/2)`, `z = 1.5 r` (circumradius 1). The region's transform places the grid in the world.

### Size and borders

A region is small: about 500 cells (a hex of radius 12 is 469 cells, about 1,200 m²). A large forest is many adjacent regions of the same plotter, and a world is many regions of different plotters. A region can be a single hex.

Borders are hard. A cell belongs to at most one region, and regions never look across a border: each region plots only from its own cells, even when its neighbour has the same plotter. Two adjacent regions of one swamp can therefore show an edge where their spacing tests didn't see each other. That is accepted. A plot system handles it in its own config, with its edge fields (thinning trees near the border, say), rather than by passing context between regions, which would make every region depend on its neighbours.

### Describing cells and their attributes

Every plot system takes the same kind of input: a **list of hex regions, each with a priority and attribute values**. A cell's value comes from the highest-priority region that covers it, so regions nest: a broad blob of soil, a pond inside it, a path across both. That list is one channel, `plot_v1_regions`, fixed for every plot system, so the game describes its ground once, in one struct, and hands it to any plotter. Plotting is game specific, and the plotter machinery is a batch manager over plot systems, each a black box that can live in several versions side by side. While the project is young, the channel evolves in place. Later, changing it is a channel upgrade, which needs migration and regenerated code that keep QA cheap (`spine.md`, "Channels change rarely"). The attributes a region can set:

| Attribute | Meaning |
|---|---|
| `available` | Whether anything may be placed with its footprint on this cell (false for a road, a building's plot, a campsite) |
| `keep_clear_m` | Keep everything that blocks at least this far from the cell's centre: a clearing around a well, a margin along a path |
| `ground` | An enum: soil, water, rock, sand, path |
| `height` | The ground height of the cell's centre, in metres |
| `moisture` | 0 to 1 |

A plot system reads these and nothing else from the game, and its config decides what they mean for its biome ("cattails only on water", "no trees within 2 m of a path cell"). The region's cells are the union of the listed regions. Each later override needs only a higher priority, so a region is written as a few broad blobs, then the overrides that matter most, such as a path cut through the wood:

```json
{"regions": [
  {"priority": 0, "shape": {"hex": {"center": [0, 0], "radius": 12}}, "set": {"ground": "soil", "moisture": 0.6}},
  {"priority": 1, "shape": {"hex": {"center": [4, -3], "radius": 3}}, "set": {"ground": "water"}},
  {"priority": 2, "shape": {"line": {"from": [-12, 6], "to": [12, -6]}}, "set": {"ground": "path", "available": false, "keep_clear_m": 1.5}},
  {"priority": 0, "shape": {"cells": [[13, -2], [13, -1]]}, "set": {"ground": "soil"}}
]}
```

| Shape | Cells |
|---|---|
| `cells` | An explicit list |
| `hex` | Every cell within `radius` of `center` (the "large hex", `3R(R+1) + 1` cells) |
| `ring` | Every cell at exactly `radius` from `center` |
| `line` | The cells along a hex line between two cells (a path, a hedge) |
| `distance` | Every cell where a named distance functor is at most `max`: the general form of `hex` and `ring` |
| `union`, `difference`, `intersection` | Set operations on the above |

Equal priorities that overlap resolve in list order, so the result never depends on anything but the list. The C API takes the list as it is (shapes stay compact: a radius-12 hex is one entry, not 469 cells), and the plot system resolves cells itself. A change of region is a new list; the engine diffs the cells the two lists resolve to.

The engine never computes terrain. The game owns the ground and tells the region what it needs to know.

## Footprints and conformance

Things that collide occupy cells: a thing's **footprint** is the arrangement of hex cells its conformance stands on (`spine.md`, "Layer 1"; `style.md` §2). Shrubs block on their footprint as trees do. Only flowers and ground cover are free:

| Footprint | Cells | Used by |
|---|---|---|
| `single` | 1 | Everything today: the hero, props, saplings, most trees, bushes |
| `line2`, `line3` | 2 or 3 in a row | A fallen log, a fence segment, a long boulder |
| `tri3` | 3 around a vertex | A thick trunk; a Huge creature |
| `flower7` | a cell and its 6 neighbours | An old buttressed cypress, a great baobab |
| `flower19` | radius 2 | A giant; a colossal tree |
| `box w×h` | a parallelogram of cells | Small buildings, ruins, a campsite |

**The conformance is the maximum, and collision is an output.** A kind's conformance is its largest extent over its life, so its footprint is the footprint of its largest age: a placement reserves the cells an ancient cypress will need, even while it is a sapling, and aging in place never collides with a neighbour. What actually collides is a shape the pipeline outputs with each build, inside the conformance: a sapling's thin stem, a mature trunk, a stump. It is static for the build: a tree's collision is its trunk, which stands still however the wind moves the crown. Anything that animates inside a system (a falling log, flying rock pieces) has no collision; breaks stay closed form and self-contained in their systems. Because collision is a pipeline output, it can be any geometry the pipeline can make (several capsules for a mangrove's prop roots), not just one capsule. It is written as a **collision description**: a JSON-described channel of the shapes Jolt Physics knows (spheres, boxes, capsules, tapered capsules, cylinders, convex hulls, and compounds of them). Jolt is linked into the runtime, and into the foundry's host for viewing and checks; the description is the boundary between a pipeline's black box and it.

Each kind collides only as much as a player will feel:
- **Trees** collide at their trunk.
- **Bushes** collide, but softly: a player can push a little way into a bush before it stops them. How soft is the black box's own choice, through its collision description, which can say anything Jolt can (a smaller shape, a soft contact that allows some penetration).
- **Rocks** collide exactly, with a convex hull of the rock.
- **Flowers** don't collide at all. They still output a shape for queries, so a ray or a click can find them.

**Sensors.** Each system can also output sensor shapes (a cylinder, a capsule, a sphere) that collide with nothing and say when something is near: the reach of a bush to pick, a tree to chop. The game registers its **active entities** with the runtime, by handle: every player and creature, since the game is multiplayer. A registration declares an external ID, which the runtime only tracks and reports back, and a collision mask, which filters what the entity takes part in. The game manages masks at a higher level, so behaviour (a deer that notices hunters but not other deer) is the game's, expressed as masks. The game moves its entities however it likes, updating Jolt itself; the registration is so the runtime can emit events correctly and attribute them (which player reached the bush, which creature noticed which player). Collision between active entities and sensors, including other entities' sensors, is detected at full fidelity. After an update, each instance's or entity's result lists the entities touching its sensors and how far each one is. The game shows the prompt, and when several things are in reach of one button, it picks the nearest. When a tree becomes a stump, its collision description simply changes to the stump, so the cells around it become walkable again, while the region keeps the footprint reserved for the regrowth to come.

**Models are flat; placements look for smooth ground.** By convention, every model a system produces stands on a flat surface at y = 0 (`style.md` §2, "Grounding"). The plotter prefers smoothish cells: its config sets, per layer, how much the heights under a footprint may differ, and it doesn't place where they differ more. A placement's `y` is the height of its footprint's centre cell, from the heights the game passed in. Where the model and the ground still disagree a little, the model is simply buried, as if a mudslide had covered its base. No negotiation between model and ground is needed.

Flowers and ground cover have no conformance and no footprint. They are placed freely, many to a cell, and never block anything.

A tree's footprint comes from its trunk. Trees are drawn at their real heights, so their trunks should be real too: a mature bald cypress's buttressed base is two to three metres across, wider than one cell. `tree_v1` gains a stump size from nature (the trunk's diameter at the ground, by species and age, including any swell or buttress), and the rig picks the footprint from it: the smallest arrangement whose inner circle, shrunk by 0.1 m (`style.md` §2), holds the stump at its largest age. A canopy is visual and overhangs freely. See `systems.md`, "Trees".

## Plotters

A plotter is a new workspace primitive: `plotters/<name>.json`, beside `pipelines/`. It binds a plot system to a biome config. This is the first one, `plotters/happy_glen_v1.json` (abridged; `book/plotter_happy_glen_v1.md` describes it whole):

```json
{
  "kind": "plotter",
  "name": "happy_glen_v1",
  "state": "draft",
  "system": "plot_scatter_v1",
  "distinct_seeds": 4,
  "view": {"preset": "glen", "radius": 12, "seed": 7, "shared": {"season": "summer", "wind_speed": 3}},
  "config": {
    "grove_scale_cells": 7, "grove_octaves": 2,
    "layers": [
      {"name": "canopy", "spacing_m": 7.0, "density": 1.5, "grove": [0.25, 1.7], "edge": {"low": 1.4, "cells": 4},
       "ground": ["soil"], "max_height_delta_m": 0.6, "regrows": true,
       "kinds": [
         {"weight": 0.55, "pipeline": "tree_v1", "params": {"species": "oak"},
          "vary": {"age": {"young": 0.2, "mature": 0.55, "ancient": 0.25}}, "on_regrow": {"age": "sapling"},
          "footprint_m": 1.71, "footprint_cells": 7},
         {"weight": 0.12, "pipeline": "tree_v1", "params": {"species": "willow"}, "max_share": 0.2, "...": "..."}],
       "regrow_kinds": [
         {"weight": 0.6, "pipeline": "tree_v1", "params": {"species": "oak"}, "on_regrow": {"age": "sapling"}},
         {"weight": 0.4, "pipeline": "tree_v1", "params": {"species": "birch"}, "on_regrow": {"age": "sapling"}}]},
      {"name": "hedges", "spacing_m": 2.2, "density": 7.0, "keep_clear": {"canopy": 2.6},
       "prefer_near": {"canopy": {"within_m": 6.0, "boost": 2.5}}, "edge": {"low": 3.0, "cells": 2}, "...": "..."},
      {"name": "stones", "spacing_m": 4.0, "density": 0.9, "regrows": false,
       "kinds": [{"weight": 1.0, "pipeline": "rock_v1", "params": {"material": "riverstone", "moss": "light"},
                  "vary": {"size": {"small": 0.75, "medium": 0.25}}}]}
    ]
  }
}
```

`footprint_m` and `footprint_cells` are measured, not written: when the foundry loads a plotter, it builds each kind whose footprint is missing at its pipeline's largest age, reads the footprint (a tree's from its rig, others from what their solid and soft collision covers at the ground), and writes it back, so the native runtime can compile it in. `distinct_seeds` caps how many seeds of a kind and its parameters exist (the quality cap, `systems.md`, "Stable randomness"), so a region shares builds. `view` is only the foundry's starting view.

What a biome config can say (for `plot_scatter_v1`, the first plot system):

- **Kinds.** A pipeline with some parameters fixed and others varied by weights (age, size), or a fixed asset. The kinds are listed explicitly, so the QA process judges the plot as it was approved. A species added later never joins a biome without review. The foundry checks every pipeline, parameter, and config entry by name when it loads the plotter, as it checks config values against a channel.
- **Diversity caps.** A maximum count or share per kind within a region (`"max_share": 0.4`), so no species takes over a region however the weights and fields fall, and regrowth can't drift into a monoculture. Caps are per region, since regions plot on their own.
- **Layers, in order.** Each has its own spacing and density. A later layer can keep clear of an earlier one (no bush in a trunk), prefer to sit near it (bushes at a tree's drip line), or avoid it.
- **Density.** `density` is in things per 100 cells (about 260 m²), scaled by two fields: the **grove** field (value noise of `grove_scale_cells`, from `grove[0]` in clearings to `grove[1]` in groves) and the **edge** field (`edge.low` on the border cells, easing to 1 at `edge.cells` in).
- **Where.** `ground` lists the grounds a layer (or a kind) takes. Because a path is a ground, keeping a path clear is a filter (`"ground": ["soil"]` never places on `path`); its cells' `available` and `keep_clear_m` keep footprints off and away. `max_height_delta_m` is the most the heights under a footprint may differ.
- **Regrowth.** The region has no memory of what was cleared: once a thing is vacated, its cells are simply free, subject to a new placement like any other. So what comes back depends on how the game asks. A `grow` in **regrow** mode (as opposed to **seed** mode, for a new region) uses each kind's `on_regrow` parameters (a sapling, not an ancient tree), and the config can give regrow mode its own kinds and weights (an oak wood that comes back as birch first). Whether a layer fills again at all is `regrows` (stones don't). The game rules stay in the config, behind one simple boundary. *When* regrowth happens is the game's decision, since the plotter has no clock.
- **Order.** Which placements come first when `grow` is asked for fewer than fit is the plot system's behaviour. `plot_scatter_v1` gives layer by layer, highest priority first, or nearest the focus when the game passes one; other orders (the edges of clearings first) are a later plot system's.

Each plotter is specific to a biome. Its kinds' pipelines share some parameters (season, wind), which the game sets on each instance it holds. There is no region-wide setter, on purpose: a spell must be able to override a parameter where it acts (a blizzard turns a forest to winter only while it lasts, only where it falls). The foundry's plotter view unions those parameters into one set of shared controls for viewing (see "In the foundry").

## Placing deterministically

The plotter's hardest requirement is **locality**. Editing a region (losing cells, gaining cells, a bomb crater) must change things only near the edit. A forest must never reshuffle because a cell was added on the far side, and edits never move anything that exists. Three choices give that.

### 1. Candidates come from cells, not from the region

Each cell makes its own candidates for each layer, from a hash of `(world seed, plotter, layer, q, r, i)`. The hash gives each candidate a position, a kind (by weight), its varied parameters, a seed for the pipeline, and a **priority**. A cell makes the same candidates whatever region it is in. The number of candidates per cell follows the layer's density at that cell, through its fields, so noise makes groves without moving anything. A candidate with a footprint is centred on its footprint (a cell's centre, or a vertex for `tri3`), and its visual position may jitter only as far as keeps its ground contact inside the footprint.

### 2. Spacing is resolved by a local rule

Within a layer, a candidate is kept when no candidate of higher priority lies within the layer's spacing, and when its footprint overlaps no other kept footprint. Every candidate is tested against the original candidates, not only against those already kept: this is a Matérn type II hard-core process. It is order-independent and strictly local: whether a candidate survives depends only on candidates within reach. A greedy "Poisson disk" pass packs more tightly, but whether a candidate survives can then depend on a chain of others that runs across the whole region.

The cost is a ceiling: Matérn II keeps at most about `1 / (π s²)` per unit area, for spacing `s`. For canopy spacing 5.5 m, that is about one tree per 95 m², or 37 cells (2.7 per 100 cells). The swamp's canopy asks for 1.8 per 100 cells, and at most 2.5 in a grove. The foundry warns when a config asks for more than its spacing allows.

Layers run in order: a later layer tests its candidates against the earlier layers' kept candidates and occupants. The reach of an edit is bounded by the radii along the longest chain of dependencies. For the swamp, that chain is a shrub's spacing (1.6 m), how near it prefers to sit to a tree (4 m), and that tree's own spacing (5.5 m): about 12 m, or 7 cells.

### 3. What exists is never moved

The plot is an ideal, and the occupants are what the player sees. The plotter places only where the ideal and the occupants agree:
- A candidate is placed only if it is clear of every occupant (by spacing and by footprint), of every exclusion the game passes in, and of every smoothness and attribute rule.
- Occupants are never moved, re-rolled, or removed by the plotter. Even if the current plot would place a neighbour differently, it stays.
- When the region loses cells, the occupants whose footprints touch a lost cell are reported to the game, which destroys or keeps them. The plotter never acts on them itself.

Because placement checks against occupants rather than against its own memory of the plot, the plotter also works with occupants it never placed: things a save restored after the plotter changed version, or things the game put there by hand.

## Growing: the API is in control

The plotter has no clock. The game decides when a region grows and how much, by asking:

```
grow(region, mode = seed | regrow, max = 50, exclusions) -> up to 50 placements
```

- **Eager, bounded growth.** The plotter returns up to `max` placements that work now, in the order its config sets, and records each one as an occupant as it hands it out. That is the whole contract: no proposals to accept, no veto. Fewer than `max` back means the plot is full. When a call returns nothing, the region is stable, and it stays so until something is removed or cells are added.
- **Grow reports the volume left.** Each `grow` also returns how much more the region could take, as a count: the placements still possible after this call. Regrowth always converges to the plot's original density, so the only question is speed, and that is a game setting. The volume tells the game how far from full a region is, so it can schedule regrowth (a heavily logged region regrows first, a full one not at all).
- **Seeding is just a big grow.** A new region is filled by growing it in seed mode until the plot is full, in as many calls as the game likes (a few hundred placements a frame, nearest to the camera first).
- **Regrowth is a later grow.** When the game decides a cleared area should grow back (each game day, after a timer, when the player is far away), it calls `grow` in regrow mode. What comes back, and how it starts, is the plotter's config (`on_regrow`, regrow kinds). The game ages it from there by setting its parameters, within the footprint the placement already reserved. The plotter never ages anything.
- **Exclusions are per call.** The game passes cells or discs to avoid this time: where players and creatures stand, a campfire, a building site. Nothing grows through someone, and exclusions leave no trace in the region.

### Removing things

The game owns every instance and every consequence. An axe, a bomb, and a fire all come down to the same thing: the game tells the region which occupants are gone, and their cells become free for a later `grow`.

What "gone" means depends on the thing. That is the system's concern, not the plotter's:
- A felled tree becomes a **stump**. `tree_v1` gains a `stump` boolean that shows the stump and hides the rest (`systems.md`, "Trees"), and a flag in its update result that is true once its fall is over (`break_over`), so the game knows when to switch it over. Flags are current values, not one-shot events: the game diffs them against the last result to find rising and falling edges, and does what it likes with them (chop, the tree falls, `break_over` rises, the game waits a second on its own timer, then declares it a stump). Every pipeline with an animation gives such a flag for each finite event (a break), in the instance's update result (`api.md`, "Instance results"). Until the game sets `stump`, the tree's collision is still its standing trunk, since collision is per build. The stump's collision shrinks to the stump, but the occupant stays, holding its footprint, until the game removes the stump and frees the cells.
- A tree hit by a bomb breaks through its break path. The game decides when the debris is gone, and then frees the slot.
- A picked bush keeps its slot: picking only clears its `fruit`.

### Saves and versions

The game owns the instances, so the game saves them: each instance's parameters and its placement (slot ID, kind, position, turn, footprint). On load, it spawns them again and **restores** them into the region as occupants, then carries on growing. The plotter's own state is just the region's cells and occupants, so nothing about the plot needs saving.

If restoring fails for any reason (an exception, or saved state that is no longer valid: overlapping footprints, an unknown kind, cells outside the region), the game drops the region's saved occupants and grows the region again from nothing. A region is small enough that regenerating it is an acceptable recovery.

A saved world therefore pins what exists, not the plotter version. If a game update changes a plotter, existing things stay exactly as saved, and only the gaps fill by the new rules. Two game versions can manifest a region differently, by design.

The game runs client and server: the server is the authority, holds the one save, makes every `grow` and removal, runs the physics, and replicates the resulting instances to clients as it replicates anything else. Clients never plot. They draw and mostly predict: they can run the runtime's physics for prediction, but their sensor results go unused, since the server decides every contact.

## The API

Plotters are generated into `runtime.h` like pipelines: an enum names each approved plotter, and no system, channel, or kind escapes it. Each placement carries its kind as a pipeline and parameters, so the game spawns it with one generic call.

```c
foundry_region_desc desc = {
    .plotter    = FOUNDRY_PLOTTER_CYPRESS_SWAMP_V1,
    .world_seed = 0x5eed,
    .transform  = grid_to_world,              /* float[16]: the hex grid in the world */
    .regions    = regions, .region_count = n, /* plot_v1_regions: prioritized hex regions with attributes, shared by every plotter */
};
foundry_region swamp = foundry_region_create(rt, &desc);

/* Load: put back what the save holds, before growing. */
foundry_region_restore(rt, swamp, saved_placements, saved_count);

/* Grow: up to 64 placements, avoiding where the players stand. */
foundry_placement out[64];
foundry_exclusion exclusions[2] = {{.position = {3, 0, 4}, .radius = 1.0f}, {.position = {-2, 0, 1}, .radius = 1.0f}};
const uint32_t exclusion_count = 2;  /* where the players stand */
uint32_t volume = 0;  /* how many more placements the region could take after this call */
uint32_t got = foundry_region_grow(rt, swamp, FOUNDRY_GROW_SEED, exclusions, exclusion_count, focus, out, 64, &volume);
for (uint32_t i = 0; i < got; ++i) {
    /* out[i]: kind, parameters, position {x, y, z} (y from the cell heights), turn, footprint, slot */
    foundry_instance h = foundry_placement_spawn(rt, swamp, &out[i]);  /* spawns its kind with its parameters */
    /* the game keeps h with out[i], sets season and wind, and saves both */
}
bool stable = volume == 0;

/* Removal: the axe felled the tree; when its result says the fall is over, it becomes a stump (the occupant stays). */
foundry_tree_v1_start_break(rt, tree);
/* ... after a later update, when the tree is in the update's changed list: */
foundry_tree_v1_result res;
foundry_tree_v1_get_result(rt, tree, &res);
if (res.break_over && !last.break_over) stump_at = now + 1.0;     /* a rising edge: the game's own timer */
/* ... a second later: */
foundry_tree_v1_set_stump(rt, tree, true);  /* the runtime swaps the trunk's body for the stump's */
/* ... and when the player digs the stump out, its cells are free: */
foundry_region_vacate(rt, swamp, &slot, 1);

/* The region changed shape: occupants on lost cells come back for the game to deal with. */
foundry_slot lost[256];
uint32_t lost_count = foundry_region_set_regions(rt, swamp, new_regions, new_count, lost, 256);

foundry_region_destroy(rt, swamp);  /* forgets the index; destroys no instances */
```

- **The region holds no handles.** It indexes occupants by slot. The game holds the handles, and does everything to them as to any instance (wind from an explosion, a break, picking fruit, the season). Queries such as "which slots are within 6 m of the bomb" (`foundry_region_find`) return slots, which the game maps to its handles.
- **Calls return their results.** `grow`, `set_regions`, and `find` write into caller arrays, like the other queries (`api.md`, "Queries").
- **Collision and walking belong to physics.** The region reports reserved footprints (for placement only). What blocks movement is each instance's collision description, which the runtime keeps in its Jolt world for that handle: the game declares what a thing is (a tree, a stump), and the runtime makes its body match, so a stump between cleared cells is walkable around while its footprint stays reserved. Physics here is a baseline, small-p physics for a game that occasionally does something wild, not a sandbox. The game owns the Jolt world and hands the runtime a blind pointer to it, wrapped with a quick signal that it really is Jolt (a tag and Jolt's version), which the runtime checks before using; the runtime keeps instances' bodies and sensors in that world, and the game moves its entities through it. The foundry's host and tools (the viewport, the stress tool) build toy worlds of their own the same way.
- **The game decides every input.** The foundry is a factory: it makes things and says where they fit. Season, wind, a spell's blizzard, and every override are the game's to set and to undo, per instance.

## Fields

Some things are not placed: they cover. Grass is the first. Placing a blade, or a tuft, per instance would make a meadow of a few hectares millions of instances, and almost all of them would be the same few shapes. A **field** instead covers every cell of a region at once, as one instance:

- **A field is a pipeline.** It is an ordinary pipeline with two build stages and a `draw.field` that names them. Every tool works on it as on any pipeline: views, sheets, flow, stress, and native against WebAssembly. One cell as an instance is the same pipeline.
- **The cell stage is a functor.** It reads only a cell's axial coordinates (`q`, `r`), its ground, its moisture, and the world's seed (integers, an enum, and one number), and answers with a **choice**: whether the cell shows anything, which tile, turned by sixths, scaled, and tinted. It holds no state, so a field of any size is the same function iterated over its cells. Patches (bald, lush) come from value noise over the cells' centres, so they run smoothly across cells without any cell knowing its neighbours.
- **The tile stage grows one cell's worth.** It grows a pointy-top hex of circumradius 1 m centred on the origin. Its inputs are the species, the season, the variant the choice names, and the level of detail. It holds no cell coordinates, so a tile is shared by every cell that picks it, and a whole meadow is a few builds: the variants times the levels. After the grass variants come plates for the other grounds (water, rock, sand, path, bare soil), so a field is the region's whole ground.
- **The runtime draws it.** `foundry_<pipeline>_spawn_field` (or `foundry_region_spawn_fields` for a plotter's fields) runs the cell stage natively over the cells and keeps each present cell's choice as 32 bytes of instance data. These are grouped into chunks of 8 x 8 cells, each chunk's tiles in runs of one variant. Each frame, a chunk in view picks its level by the size of a tile at its nearest point, at the sizes the pipeline measured (`quality.perf.md`, "Detail sizes", measured on a patch of 37 cells). Its runs are drawn with that level's build of their variant. A tile's mesh carries each vertex's **sway**, and the vertex shader moves it in the field's wind, in gusts that travel across the field: no bones, no pose, no CPU.
- **Steps between cells.** Each tile's ground has a skirt 4 m down its sides, so neighbouring cells at different heights meet without a gap.
- **The ground is the ground's.** A field grows on the ground; it doesn't colour it. A grass tile's soil is earth, and only its blades take the cell's tint (a vertex's alpha says which).
- **In a plotter.** A plotter's `fields` lists field pipelines with their parameters (`"fields": [{"pipeline": "grass_v1", "params": {"species": "meadow"}}]`). The region hands every resolved cell, with its ground, moisture, and height, to each one. The game owns the field instances like any other, and spawns them again after `foundry_region_set_regions`.

The plotter's picture in the foundry composes its occupants in WebAssembly and leaves fields out. Its live view (below) runs the native runtime and shows them.

## Where the plotter lives

- **The plot system is a system.** It is pure: its input channel carries the bound biome config, the prioritized hex regions (`plot_v1_regions`, the same for every plot system), occupants near the work, exclusions, the seed, the mode, `max`, and the focus, and its output channel carries placements and the volume left. So it is versioned, sandboxed in WebAssembly in the foundry, and checked byte for byte against native, like every system. It is a new kind, though: it doesn't sit in a pipeline's DAG, and it's bound to a config rather than fed by stages (`spine.md`, "Vocabulary").
- **The region is runtime bookkeeping.** The cell set, the occupant index, and the diffs hold state across calls, so they are a small runtime object, beside instances. They call the plot system for each `grow` and `set_regions`.

### In the foundry

Plotters get their own view:
- It generates input regions: shapes (a large hex, a thin strip, a ragged union, a single cell, a pair of adjacent regions to judge their edge), painted attributes (water, a path, unavailable cells, keep-clear margins, slopes), and random ones, plus a brush to paint cells by hand.
- It grows the region step by step (`grow` with a chosen `max`) or to stable, and shows each placement's footprint over the hexes.
- It shows the plotter's **shared properties**: the union of its kinds' pipelines' parameters (season, wind). It maps each one onto every instance shown that has it, so one season control turns the whole swamp to fall.
- It has tools to vacate one thing, clear a radius, add exclusions, and regrow.
- It shows metrics:
  - stems per hectare per layer;
  - canopy cover;
  - nearest-neighbour distances against the spacing;
  - footprint overlaps (there must be none), and how uneven the ground is under each placement;
  - instances and distinct builds;
  - placements per `grow`.

- **Terrain.** The ground's presets take a terrain: a style (flat, hills, ridges, terraces, dunes, mesas, wild), a height, a hill size in cells, a roughness, a jitter per cell, and a smoothing (`plotter_set`'s `terrain`, or the inspector's Ground section). It sets every cell's height (and moisture: valleys are wetter, so their grass is lusher) from noise over the cells. Heights are kept in 5 cm steps as cell regions that set only height and moisture above the ground, so the plot system, the placements, and the fields all stand on it. Smoothing (0 to 1) blurs the heights and then limits the step between neighbouring cells (0.25 m at 1, so 0.3 m with the 5 cm steps; none at 0), for a game that blends between cells and wants slopes, not cliffs. It also lowers the peaks and fills the valleys: a wild 20 m terrain at radius 40 has steps up to 5.1 m and spans 0.9 to 14.6 m as made, 2.25 m and 2.6 to 13.4 m at 0.5, and 0.3 m and 6.5 to 12.4 m at 1 (`plotter_set`, measured from its regions). A pond stays flat, below its shore. The plot system's height rules then keep trees off slopes that are too steep for them.
- **Live.** The viewport shows the region running in the native runtime a game links (`src/host/plot/live.h`): grown to full with its fields, posed, swayed, and drawn every frame with the runtime's cost decisions (`submit.md`, "Cost"). It shows the frame rate, uncapped, with what was drawn and posed. Drag orbits; holding the right button flies. It regrows once the ground stops changing (a dragged slider doesn't regrow every frame), and applies the shared values (season, wind) to every instance and field by name. It shows the native runtime as last built. Unticking **Live** shows the picture composed from WebAssembly.

MCP tools let the agent do the same (`plotter_view`, `plotter_grow`). Every plotter has a book (`book/plotter_<name>.md`) with numbers from runs, and the stress tool gains a region phase: many adjacent regions, carpet-bombed and regrown, measuring `grow` time, frame spikes, and memory.

## How the AI judges "feels right"

A plotter is approved by a human in a play test. The agent prepares it with evidence:
- **Ecology numbers.** The densities, set against published stand densities and spacings for the biome. Trees are drawn at their real heights and, with stump footprints, their real trunk widths, so the numbers carry over more directly than they would at a game scale.
- **Reference photos of the biome**, compared with views from the hero's height and from the game's camera.
- **Many shapes and edits.** Several generated regions, adjacent regions of the same plotter (an edge that reads as natural), a path through the wood, a slope, a clearing, and regrowth. A plotter that looks right only on a big hexagon isn't done.

## Performance and scale

- **Regions are small, worlds are not.** A 500-cell region of swamp holds about 9 trees, 45 shrubs, 20 flower clumps, and 6 stones at base density (about 80 instances). A world of a few hundred regions is tens of thousands of instances, which is where the forest measurements already sit (17,600 trees fit a frame), as long as builds are shared through the quality setting's distinct-seed cap (`systems.md`, "Stable randomness").
- **Plotting is cheap.** It is hashing and neighbour tests. A `grow` touches only the cells it fills plus the plot's reach.
- **Spikes come from the game.** The region doesn't spawn anything, so the game sets the pace: how many placements to spawn per frame, and how fast a season change rolls through its instances.

## What this changes elsewhere

- `spine.md`: conformance footprints of hex cells; the conformance as the maximum extent, with collision a pipeline output (a Jolt collision description) inside it; the plot system as a new kind of system.
- `style.md` §2: the footprint arrangements, and models buried where the ground is uneven.
- `systems.md`: tree trunks from nature, the footprint from the stump at its largest age, a collision output, a stump mode, and a fall-finished flag; soft bushes, convex rocks, query-only flowers, and sensors; nature placed by plotters.
- `CLAUDE.md`: Jolt Physics as the physics engine, linked into the runtime and the host (a new technical decision).
- `future.md`: handing a break's parts to Jolt as dynamic bodies.
- `flow.md`: `plotters/` as a workspace primitive, and footprints in conformance files.
- `api.md`: regions; per-instance update results of current values (a flag per event, sensor contacts with distances, sounds) and a changed list, instead of an event stream; active entities (players and creatures); the runtime as the game's physics.
- `playtest.md`: the plotter view.

## Settled in review

The last two answers:
- **The blind pointer.** The runtime needs only a quick signal that the pointer is Jolt: the game passes it wrapped with a tag and Jolt's version, and the runtime checks those before using it.
- **Layers and masks.** The game maps each purpose to a Jolt object layer and mask when it creates the runtime: a solid body (a trunk, a rock), a soft body (a bush), a query-only shape (a flower), and each sensor purpose a pipeline declares (reach to pick, reach to chop). A pipeline names purposes and never masks, so the same tree works in any game.

## Status

What exists, and where (2026-10-12):

| Piece | Where | Checked by |
|---|---|---|
| The ground and plot channels | `src/channels/plot_v1_*.json` (region, regions, config, layer, kind, clearance, preference, occupant, exclusion, request, placement, cell, result) | `tools/gen_channels.py` |
| The plot system | `src/systems/plot_scatter_v1/` (a system of kind `plot`) | native matches WebAssembly on every grow the foundry verifies (`plotter_grow` with `verify_native`) |
| Plotters | `demo/plotters/happy_glen_v1.json`; loaded, checked, and measured by `src/host/plot/plotter.cpp` | `book/plotter_happy_glen_v1.md` |
| The foundry's plotter view | `src/host/plot/view.cpp`, the Plotters items in the workspace, MCP `plotter_get`, `plotter_set`, `plotter_grow`, `plotter_clear`, `plotter_view` | play testing |
| Regions in the runtime | `src/library/region.h`, `region.cpp`; each plotter compiled into `src/library/plotters.cpp` | `tests/region_test.cpp`: the same 134 placements as the foundry for the glen's ground and seed |
| The region run | `stress_plotter_<name>` (`src/tools/stress/region.cpp`) | `demo/perf/report-plotter_*.json` |
