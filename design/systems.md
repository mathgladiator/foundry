# The Systems

This document walks through the asset systems we plan to build, from the simplest to the hardest. Each one follows the spine (`spine.md`): a conformance shape, an interaction idiom, and a set of versioned black boxes that talk to each other across byte-array boundaries.

## Static things

We start with the easiest case: a static system that renders a single thing. The system defines an asset class by its conformance shape (for example, the human capsule). It then uses a mesh generator such as Meshy to produce the shape, and the agent resizes the result until it looks right. This is a fixed, narrow pipeline that produces simple assets, and it gives the AI full fidelity to reason about the size of the static thing relative to the conformance shape. For example, it ensures a garden gnome isn't 20 feet tall.

Meshy runs only in the foundry, at build time. Its output is baked into the asset system as data, so the runtime never calls out to it.

## Nature

From there, we build up toward nature. We could use the static-thing system to mint a handful of trees. Or we can go big and build a tree simulator.

### Trees

A tree is driven by a physical control system: wind direction, wind speed, and gust damping.

A tree's conformance defines the stump and the height at which branches start. This separates the constrained space used for collision detection from the unconstrained foliage above it. The conformance is therefore a stump with an unbounded top, with the implicit assumption that everything above the stump is purely visual. In general, a conformance is a union of simple shapes.

A tree is made of these black boxes:

- **Skeleton.** Builds the tree's height and branch structure, including the branches that respond to wind. Its input is the tree's genome, plus season and age (as enums) so that trees can change over time.
- **Body.** Stitches mesh geometry onto the skeleton. It takes the same genome, season, and age inputs. This is an example of shared parameters shrinking the validation space. Distance to the viewer is an implicit input, so the body can pick a level of detail.
- **Coloring.** Colors the tree from its genome, age, and season.
- **Simulator.** Updates the rig's values each frame so that an individual instance can be animated.

This is a complex system, but the key is that the runtime is responsible for every part of generation, the cache levels between black boxes are clear, and ownership is well defined. The game can simply say: "at this position, I want a spruce that is young, in autumn, with wind from the west."

The game states the most it wants and provides the inputs. The runtime and caching layer then minimize context switching between resources and collapse parameters to maximize cache hits:

- **Level of detail** is a runtime-controlled variable, tied directly to the game's graphics settings.
- **Shared meshes.** The runtime is free to share meshes between trees with the same enums, and to cap how many distinct trees it generates.
- **Stable randomness.** Every tree gets a unique ID that seeds its randomization, so players with the same configuration see the same world. That variety can be reduced according to the graphics setting: the default might allow 50 distinct trees per enum set, while a lower setting allows only 10, with each instance ID mapped onto one of those 10.

A tree can also be broken, which needs another black box for breaking apart the generated mesh. Breaking requires knowledge of how the body was built, so the body also generates the break pieces, along with a keyframe animation of the tree breaking. The pipeline therefore branches based on the interaction: when the break interaction fires, the model's output switches over and a faux-physics simulator takes over, with particles falling away.

So each asset instance has a state machine, controlled entirely by the runtime, which decides which interaction path to invoke. In a small way, this is a new communication boundary: it is implied by how the asset system is composed, together with the controller's current state.

### Rocks

Rocks are similar to trees but much simpler. They can only break. They have a material type that controls the mesh generator, so the main black boxes are the mesh body constructor and the faux-physics system for breakage.

### Other natural elements

These are proposals, each reusing a system above:

- **Shrubs and ground cover.** A light version of the tree system: a smaller genome, no break interaction, and heavy instancing. Ground cover has no collision at all (an empty conformance) and exists purely for visuals.
- **Stumps and fallen logs.** The tree body's break pieces, promoted to static things. This gives forests debris that matches the living trees.
- **Ore and crystal nodes.** A rock variant whose material and break interaction yield resources. Crystal is one of the specular materials in `style.md` §6.
- **Mushrooms and flowers.** Small genome-driven static things, grown in groups so they read at battle distance (`style.md` §3 sets a 0.6 m minimum height).
- **Water features.** Ponds, streams, and waterfalls are elemental surfaces, which `style.md` §6 says get no ink. They are probably closer to terrain than to asset systems; see the questions below.

## Crafted things

### Chests

Chests come in many geometries, but they share a simple controller: a single hinge. That needs only a two-part mesh and an animation between open and closed.

The black boxes are:

- **Mesh generator.** Produces the two parts, with a handful of parameters and types.
- **Skinning and coloring.** Chests of one type can be skinned differently. All chests share the same palette, so every part of the mesh pulls from a common texture space and a chest's entire coloring is parameterized.
- **Animation controller.** Tracks the current state and animation progress, and advances continuously. Speed is an input: a thief picking a lock slowly makes the lid open slowly, instead of the game showing a progress bar. If the thief is interrupted, the progress reverses from wherever it was.

### Other crafted items

These are proposals, grouped by which existing controller they reuse:

- **Single hinge (the chest controller):** levers, switches, trap doors, cabinets, wardrobes. Doors also belong to the building system's room animation, which could reuse this controller.
- **Break (the rock controller):** barrels, crates, pots, and urns. These add a contents drop to the break interaction.
- **Wind and cloth (the tree simulator and the cape):** banners, flags, awnings, and tents.
- **Rotation:** windmills, water wheels, cart wheels, and winches. This is a new controller type, but a simple one.
- **Emissive effects:** torches, braziers, and candles. A static thing plus a flame effect in the style of `style.md` §6.
- **Static furniture:** tables, chairs, beds, and shelves. These go through the static-thing pipeline, and many can be mounted on building doodad points.
- **Traps:** pressure plates and spike traps. Hinge or slide controllers with a trigger interaction.

## A building system for houses, forts, and dungeons

A box conformance system defines the edges, walls, and floor segments. Each piece is a box-shaped slot, and the building system fills slots with pieces.

The box conformance then drives a very specific mesh system. For example, a wall can have three variants (chosen consistently by instance ID) at several levels of health, along with the animations between those levels. A wood wall and a stone wall fall apart differently.

The main inputs are the material type and the number of distinct damage stages. The central black box stamps out the pieces: straight walls, floor variants, stairs, corner walls, columns, and so on. It produces functions that build a dungeon or building out of pieces and layers. It also supports mounting doodads using another conformance shape: for example, a pedestal can have a spherical conformance, and any static object that fits inside the sphere can be placed on it.

The black boxes are:

- **Mesh generator.** For a given material, produces a palette of building pieces, choosing the variant from a stable ID and the current health.
- **Skinning.** Variants for color and health, which pick a texture along with a color variation.
- **Doodad mounting.** Exports positions where other static objects can be placed.
- **Room animation.** Doors, trap doors, and gates opening and closing.

With wood and stone systems in place, we can define dungeons. This black box system is responsible for generating the pieces that go into the boxes, in the right state. The higher-level dungeon belongs to game design, which works with the full range of available building blocks.

As part of testing, the foundry can add a final step that assembles rooms to evaluate the game level. Humans can also supply specific approved rooms, along with standard ways of connecting them. For example, a room with only doors offers a simple way to join other rooms through shared walls, checked with collision detection. This forms the basis of a dungeon creator.

### Pieces and slots

To make the building system concrete, this is a proposed starting set of pieces. Each fills one box slot:

| Piece | Slot | Notes |
|---|---|---|
| Wall, straight | one edge | variants by ID; damage stages by health |
| Wall, corner | corner | joins two walls; must match both materials |
| Wall, doorway / window | one edge | carries a mount point for the room animation (door, gate, shutter) |
| Floor | one cell | variants by ID; damage can open a hole to the level below |
| Stairs | one or two cells | spans two levels; needs walkable collision, not just a box |
| Column | corner | structural; may hold up a ceiling |
| Ceiling / roof | one cell | a roof is the exterior side of a ceiling |
| Doodad mount | sphere inside a cell | pedestal, wall sconce, shelf |

### Testing rooms

The room-combination step is where the building system pays for itself. Proposed checks:

- Every approved room joins every other through a standard connector (a doorway on a shared wall) without collision overlaps.
- A path exists from every doorway to every other doorway, using the capsule conformance from `spine.md`.
- Every damage stage of every piece still blocks or allows movement as the game expects (for example, a broken wall becomes passable at the stage the game declares).
- Doodads mounted in a room stay inside their sphere through every damage stage of the piece they are mounted on.

## Humanoid heroes and villains

This is the hardest type, because it needs a long pipeline. We start with a capsule conformance whose height ranges from a gnome to an elf, so height comes first. The interaction model covers essentially everything.

### Parameters and skeleton

Next come the core parameters that define a humanoid: height, race, bulk, and face. As a philosophy, we want a good balance of parameters, but not so many that malformed humanoids appear. The parameters should produce humanoids that look like they could really exist.

That points to the next black box: the rigging system, which exports a skeleton. It takes the inputs relevant to the skeleton into direct account, using medical and physiological literature, or best guesses from fantasy lore about how the proportions would work mechanically. (The style layer in `style.md` §4 then exaggerates that realistic structure toward the house look.)

At this level, animations are also adjusted in proportion to the inputs. The key feedback loop here is approving that the skeletons look real, that the animations look good, and that the mount points work with various things mounted. For example, we must handle both one-handed and two-handed mounts. At this stage, QA uses simple vectors as stand-ins to confirm that the hands mount correctly during animations. The stand-ins are a QA-only side channel; the black box itself only exports mount points based on its configuration.

The skeleton also has a play-test mode to validate that everything looks right. This covers contact points: feet must touch the ground, and the animation system can use inverse kinematics to adjust animations slightly as needed. The same applies to climbing. This is where the controller model matters, because it determines both the animations to aim for and any important transition animations, and the resulting runtime is what ships.

In this model, the humanoid doesn't really use files. The system *is* the file that produces the model.

The AI can pull baseline animations from Meshy and from other sources to get a sense of things. It should favor a continuous movement system in which the runtime treats jumping as physics, so that the physics of motion and the animation agree. For example, either a jump is instant on input (and the animation flows from its current state into the next), or a jump has a wind-up that stops the player. This is why play testing is required: this component pushes back on whatever system is directing control.

### Body mesh

The skeleton is the foundation. Next comes filling out the mesh. Here we can pull reference studies from Meshy and build the mesh per race. Since we are thinking generically, we want bald, unclothed, purely fleshy creatures. Generation takes a level-of-detail parameter, since this is the best place to reason about level of detail. That could be an automated layer, or generated internally, depending on how well the AI can reason about the form of these creatures.

This is another checkpoint: the animations and the mesh must work well together on a creature with flesh. The face needs to be interchangeable, so this phase uses only a basic skin. The test runs every animation and looks for self-intersections between body parts. The AI reads the report and judges how bad the overlap is. The system can flag which animations are problematic, so the user can reject them. It can do this across thousands of parameter combinations, which pushes back on the layers above.

### Skin

The next step is skinning, which can use the associated Meshy outputs to give each humanoid skin. The skin itself is a parameterized surface: skin color, vein color, hair color, and even how much body hair is generated are all controlled by parameters. The end result is a set of textures with normal maps. The output of this stage is a naked, fleshed-out humanoid.

### Face

Next comes the face: eyes, nose, mouth, and scars. This is a flexible composition that produces a range of possible faces, and it lets us bulk-approve the basis vectors.

At every step, the tooling shows the human a large number of parameter combinations side by side, so the visual, pattern-matching side of the brain can spot problems.

### Armor and mounts

The next step is armor and weapons. Armor works at two levels.

First, **skin replacement**: the skin texture is a cached input to the armor system, so armor can be painted directly onto the skin in a specific region.

Second, **mounted pieces**: the mesh system must also generate mount points that describe the surface where things can safely sit without conflict, which gives the armor system a way to fit or build armor that works well. Pieces that extend outward, such as pauldrons on the shoulders, can be ornate. The goal is to define armor like plate, which has a base layer on the skin (such as chain mail) with a breastplate on top. Mount points can also describe things like sleeves that have their own animation controller, constrained between a minimum and maximum cone so they can swing freely.

The core armor slots are torso, legs, feet, and hands. These can change per game, so each system needs flexible armor mount points. For example, we may want a trinket whose mount point is a sphere with a smaller sphere hanging from it: conformance places an object in the small sphere, and the small sphere bounces along with realistic chain physics.

We also want a cape, which defines a conformance region behind the player and above the weapon, so the cape can flow in the wind and respond to movement. Details like these need strong boundaries to keep them separate, but each gets its own black box.

### Hands

The hardest part will be the hands and their orientation. At its core, the skeleton system must validate that one-handed configurations (dagger, sword, or shield) and two-handed configurations (greatsword or staff) work with the right vectors. Those vectors are crucial for realism. Rather than a joint system, the hand system depends on what is held, using keyframe animations and quick interpolation between poses. Hands have their own independent level of detail, since they are complex. This alone is a separate system that needs its own QA.

The humanoid system is the keystone of complexity.

## Creatures

Once the humanoid system exists, creatures mostly come down to locomotion, and the workflow is similar to the humanoid one with different targets. For example, the AI might create a naga, octopod, or genie movement and animation system. That can then be mixed and matched with reptile, fur, or energy-storm mesh and skin systems.

We want to encourage reuse between systems. A specific creature asset pipeline is an approved mixture of them, so that consistently high-quality output is achievable.

## Bosses

Each boss is a large undertaking, since a boss fight may have stages or visible telegraphing. Fortunately, bosses don't need to scale, and the systems above should be able to express complex boss models. A few examples of how bosses could be built from existing systems:

- **Ancient dragon (Gargantuan).** A creature locomotion system with wings, on a multi-hex footprint (`style.md` §2). Stage two: a wing breaks, using the tree's break-pieces approach applied to a limb, and the dragon is grounded, switching its locomotion controller from flying to walking. Breath attacks are telegraphed by an emissive glow building in the throat before the effect fires.
- **Treant.** The tree system scaled up, with a humanoid-style locomotion skeleton driving the trunk. Each limb has its own break interaction and health, and the wind simulator keeps running on the canopy while it fights.
- **Siege golem.** A body assembled from building-system pieces (stone walls, columns) mounted on a creature skeleton. As its health drops it sheds pieces through the building system's damage stages, exposing a glowing core in the final stage. This reuses the wall damage machinery directly.
- **Hydra.** A chain-creature body with several head mount points. Severing a head uses the break interaction; regrowing two in its place is a state machine transition that mounts new heads at the same points.
- **Lich.** A humanoid with a telegraphed area spell: a ground decal fills in over a fixed time before the effect lands, using the same continuous-progress controller as the chest lid.

## Magic

Magic is another system where we have keyframe animation like raining and then it can mix with different animation system

Here, we can create the portal base texture, approve it.

Then define the animation that drives the particles.

Then what gives those particles shape and form. is it models. Is it billboards.

We can even use generative AI to build particles that we animate.

## Open questions

### Nature

1. **Coloring vs. `style.md`.** The tree coloring box "updates textures", but `style.md` §5 says color lives in flat paint regions, with no texture noise. Is tree coloring a set of palette-region values chosen by genome, age, and season, or genuinely textures?



2. **Where does wind come from?** Is wind a global world input that every tree samples, or does the game pass it per instance? The tree example implies per instance.

Wind will be a global property derived from an instance's location. It is passed, and it should be possible to use instanced geometry to mint a new tree form.

3. **Break pieces at every level of detail?** If the body generates break pieces, does it generate them for every LOD, or does a break always swap to the full-detail mesh?

Break can do an inbetween. AT core, we just want it to look reasonable. It doesn't always need to be exact, but we want it informed from the mesh. So, it always picks a mid point of quality.

4. **Water and terrain.** Are water and terrain asset systems at all, or are they part of the game's world renderer?

They are isolated since they are big and different.

### Crafted things

5. **Shared palette.** Is the chest palette shared across all crafted things (and buildings), or is it per system?

It would be per system.

6. **Interrupted progress.** When the thief is interrupted and the lid reverses, does that reversal happen in the asset's state machine, or does the game drive the progress value directly?

It would be the game logic, the system provides the continous function.

### Buildings

7. **Hex vs. box.** The world is a hex grid (`spine.md`, `style.md` §2), but buildings use box conformance. Do buildings sit on their own square grid inside the hex world? If so, how do building edges meet hex cells, and how does a capsule standing in a hex cell interact with a box wall?

This is per the game. The terrain may be hex, but it could contain a square house. The point is that the geometry generation has to have a conformance to root on, and it may not be consistent within the same project. However, it should be clear whether things of a shape will work.

8. **Piece dimensions.** What is one box slot in metres? Should a wall be one hex edge long (1 m with a 1 m circumradius)?

This will be a poperty of the conformance. For example, a wood wall conformance shape could be 2 meters high, 1 meter wide, and 8 cm thick. While a stone wall can have 1.5 meters high, 1 meter wide, and 20 cm thick.

9. **Shared walls.** When two rooms share a wall, is that one instance with one health value, or two? Who owns its damage state?

Each wall is independent.

10. **Structural collapse.** Does destroying a wall or column affect the pieces it supports (the floor above, a ceiling), or is every piece independent?

At core, this is a responsibly for the game system as it will be handling a dependency graph. The core responsibility of the system is to fit the box and ensure oreintation and diverity.

11. **Mixed materials.** Can one building mix materials (a stone foundation with wooden upper floors)? If so, who generates the transition piece between them?

Yes, but that will be a property of the game graph. We don't seek perfect transitions, but that would illustrate a system where the generated system can speak two conformance systems and then generate all combinations such that all walls ahre a conformance system but then generate all the permutations based on the input source.

12. **Interior vs. exterior.** Does a wall have distinct interior and exterior faces (plaster inside, stone outside)?

That's a good point. Let's say walls can have a front and back side and that is up to the system to define, but it's a good channel point.

13. **Interactive doodads.** Doodad mounts accept static objects. Can they also accept interactive ones, such as a chest on a pedestal or a lever on a wall?

Yes, as the key is the system is a guidance for the game to mount and whether or not the thing being mounted will fit.

14. **Who builds dungeons?** Does the game assemble dungeons at runtime from pieces and approved rooms, or does the foundry produce finished dungeons?

This will be a game since that requires the larger macro world builder. This project is about minting things that next level or game can leverage.

### Humanoids and creatures

15. **Normal maps vs. `style.md`.** Skinning produces "textures along with normal mapping" with veins and body hair. `style.md` §4–5 bans fine noise and sets procedural bump to 0 because bumps break the cel bands into speckle. Which wins for skin?

I want it to be possible to have some sense, so maybe we tweak style.md to allow for this since this is a generic pipeline, and I want the ability to tune everything as things go one. style.md is what I had, and I may change it.

16. **Height range vs. size classes.** "Gnome to elf" spans the Small and Medium size classes in `style.md` §2. Is that one capsule system with a height parameter, or one system per size class?

That would one capsule system with a height and maybe a radius parameter.

17. **Faces across races.** The face is interchangeable. Does one face composition fit every race's head mesh, or is the face system per race?

Yes, the key will be coloring and what not. This should be flexible in other games, and we will experiment with it as time progress.

18. **Armor combinations.** Painting armor onto the skin texture makes the cached skin depend on the armor. How many skin and armor combinations do we expect to cache per humanoid, and is there a budget?

Ultiamtely, we want on-demand constructure and shared caching such that everyone can share the same skin and then extend it with armor as a new thing such that if more people use that armor, then it too is shared. The thinking here is that we maximize GPU resources, but also make forking easy so we have a reference counted system for re-use and a timer for releasing resources back. We want to amortize both creation and destruciton of GPU resources to minimize churn and provide reasonable generation budgets, so we will have a fallback texture while something is created being the old one.

19. **A common skeleton interface.** Creatures mix and match locomotion systems (naga, octopod) with mesh and skin systems (reptile, fur). That only works if mesh and skin systems accept any skeleton. Is there a shared skeleton format across locomotion types, or is each mixture approved as its own pairing?

No, the mixtures will need to be approved. This is where the systems can be tested to interlinked and having a common skeletal system. We can have a dynamic meshing system like found in Spore to create a mesh from the skeleton, or we have a skeleton fixed as a channel. Both models are considered valid based on optimization.

### Bosses

20. **Stage transitions.** Are boss stages just states in the asset's state machine, or does a stage change swap in a different asset system (for example, a forked version of the dragon)?

These are just parameters that feed the system, so they are likely part of the control system and the hard part is doing the tweening for state changes. It can have forking, and we could simply have a transition animation and change the entire instance (like, a JRPG radical transformation)

