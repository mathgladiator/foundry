# The Pipelines

This document walks through the asset pipelines we plan to build, from the simplest to the hardest. Each one follows the spine (`spine.md`): a conformance shape, an interaction idiom, and a set of versioned systems (single-function black boxes) connected by channels.

The foundry's job is to mint things that the game can use. Larger structures, such as dungeons, terrain, and the world itself, belong to the game.

## Static things

We start with the easiest case: a pipeline that renders a single static thing. It defines an asset class by its conformance shape (for example, the human capsule). It then uses a mesh generator such as Meshy to produce the shape, and the agent resizes the result until it looks right. This is a fixed, narrow pipeline that produces simple assets, and it gives the AI full fidelity to reason about the size of the static thing relative to the conformance shape. For example, it ensures a garden gnome isn't 20 feet tall.

Meshy runs only in the foundry, at build time. Its output is baked into a disk format (`flow.md`), so the runtime never calls out to it.

## Nature

From there, we build up toward nature. We could use the static-thing pipeline to mint a handful of trees. Or we can go big and build a tree simulator.

### Trees

A tree is driven by a physical control system: wind direction, wind speed, and gust damping. Wind is a global property of the world. The game samples it at each tree's location and passes it in, so trees in the same area sway together. Trees can also be built from instanced geometry, which makes it cheap to mint new tree forms.

A tree's conformance defines the stump and the height at which branches start. This separates the constrained space used for collision detection from the unconstrained foliage above it. The conformance is therefore a stump with an unbounded top, with the implicit assumption that everything above the stump is purely visual.

A tree pipeline is made of these systems:

- **Skeleton.** Builds the tree's height and branch structure, including the branches that respond to wind. Its input is the tree's genome, plus season and age (as enums) so that trees can change over time.
- **Body.** Stitches mesh geometry onto the skeleton. It takes the same genome, season, and age inputs. This is an example of shared parameters shrinking the validation space. Distance to the viewer is an implicit input, so the body can pick a level of detail.
- **Coloring.** Colors the tree from its genome, age, and season.
- **Simulator.** Updates the rig's values each frame so that an individual instance can be animated.

This is a complex pipeline, but the key is that the runtime is responsible for every part of generation, the cache levels between systems are clear, and ownership is well defined. The game can simply say: "at this position, I want a spruce that is young, in autumn, with the local wind."

The game states the most it wants and provides the inputs. The runtime and caching layer then minimize context switching between resources and collapse parameters to maximize cache hits:

- **Level of detail** is a runtime-controlled variable, tied directly to the game's quality sliders.
- **Shared meshes.** The runtime is free to share meshes between trees with the same enums, and to cap how many distinct trees it generates.
- **Stable randomness.** Every tree gets a unique ID that seeds its randomization, so players with the same configuration see the same world. That variety can be reduced according to the quality setting: the default might allow 50 distinct trees per enum set, while a lower setting allows only 10, with each instance ID mapped onto one of those 10.

A tree can also be broken. Breaking requires knowledge of how the body was built, so the body also generates the break pieces, along with a keyframe animation of the tree breaking. The break pieces don't need to be exact, just reasonable and informed by the mesh, so they are always generated at a middle level of detail regardless of the current one. When the break interaction fires, the state machine switches the tree to its break path: the model's output switches over and a faux-physics simulator takes over, with particles falling away.

### Rocks

Rocks are similar to trees but much simpler. They can only break. They have a material type that controls the mesh generator, so the main systems are the mesh body constructor and the faux-physics system for breakage.

### Other natural elements

These are proposals, each reusing a pipeline above:

- **Shrubs and ground cover.** A light version of the tree pipeline: a smaller genome, no break interaction, and heavy instancing. Ground cover has no collision at all (an empty conformance) and exists purely for visuals.
- **Stumps and fallen logs.** The tree body's break pieces, promoted to static things. This gives forests debris that matches the living trees.
- **Ore and crystal nodes.** A rock variant whose material and break interaction yield resources. Crystal is one of the specular materials in `style.md` §6.
- **Mushrooms and flowers.** Small genome-driven static things, grown in groups so they read at battle distance (`style.md` §3 sets a 0.6 m minimum height).

Water and terrain are out of scope here. They are large and different enough that they get their own, isolated designs.

## Crafted things

### Chests

Chests come in many geometries, but they share a simple controller: a single hinge. That needs only a two-part mesh and an animation between open and closed.

The systems are:

- **Mesh generator.** Produces the two parts, with a handful of parameters and types.
- **Skinning and coloring.** Chests of one type can be skinned differently. All chests share the chest pipeline's palette, so every part of the mesh pulls from a common texture space and a chest's entire coloring is parameterized. (Palettes are per pipeline: chests and buildings each have their own.)
- **Animation.** A continuous function from progress to the lid's pose. The game logic owns progress: a thief picking a lock slowly makes the game advance progress slowly, so the lid opens slowly instead of the game showing a progress bar. If the thief is interrupted, the game reverses progress from wherever it was, and the lid follows.

### Other crafted items

These are proposals, grouped by which existing controller they reuse:

- **Single hinge (the chest controller):** levers, switches, trap doors, cabinets, wardrobes. Doors also belong to the building pipeline's room animation, which could reuse this controller.
- **Break (the rock controller):** barrels, crates, pots, and urns. These add a contents drop to the break interaction.
- **Wind and cloth (the tree simulator and the cape):** banners, flags, awnings, and tents.
- **Rotation:** windmills, water wheels, cart wheels, and winches. This is a new controller type, but a simple one.
- **Emissive effects:** torches, braziers, and candles. A static thing plus a flame effect from the magic pipeline below.
- **Static furniture:** tables, chairs, beds, and shelves. These go through the static-thing pipeline, and many can be mounted on building doodad points.
- **Traps:** pressure plates and spike traps. Hinge or slide controllers with a trigger interaction.

## A building pipeline for houses, forts, and dungeons

A box conformance defines the edges, walls, and floor segments. Each piece is a box-shaped slot, and the building pipeline fills slots with pieces. The box does not have to match the terrain's grid: a square house can sit on hex terrain. The box's dimensions are a property of the conformance. For example, a wood wall's conformance could be 2 m high, 1 m wide, and 8 cm thick, while a stone wall's could be 1.5 m high, 1 m wide, and 20 cm thick.

The box conformance then drives a very specific mesh system. For example, a wall can have three variants (chosen consistently by instance ID) at several levels of health, along with the animations between those levels. A wood wall and a stone wall fall apart differently. Every wall has a front and a back, and the system defines what each side looks like (plaster inside, stone outside). Which side faces where is part of the wall's channel.

The main inputs are the material type and the number of distinct damage stages. The central system stamps out the pieces: straight walls, floor variants, stairs, corner walls, columns, and so on. It also supports mounting doodads using another conformance shape: for example, a pedestal can have a spherical conformance, and any object that fits inside the sphere can be placed on it. Doodads can be static or interactive (a chest on a pedestal, a lever on a wall); the building pipeline only says where things can mount and whether they fit.

The systems are:

- **Mesh generator.** For a given material, produces a palette of building pieces, choosing the variant from a stable ID and the current health.
- **Skinning.** Variants for color and health, which pick a texture along with a color variation.
- **Doodad mounting.** Exports positions and mount shapes where other objects can be placed.
- **Room animation.** Doors, trap doors, and gates opening and closing.

### What belongs to the game

The building pipeline's responsibility is narrow: fill each box, with the right orientation and enough variety. Everything above that belongs to the game's world builder:

- **Independent pieces.** Every wall is its own instance with its own health, even when two rooms share it.
- **Structural collapse.** Whether destroying a column brings down the floor above is the game's call; it owns the dependency graph between pieces.
- **Mixed materials.** A building can mix materials (a stone foundation with wooden upper floors), and the game graph decides where. We don't need perfect transitions. A pipeline that must support a mix can accept both conformances and generate every combination of the two.
- **Dungeons.** The game assembles dungeons from pieces and approved rooms. The foundry mints the pieces.

### Pieces and slots

This is a proposed starting set of pieces. Each fills one box slot:

| Piece | Slot | Notes |
|---|---|---|
| Wall, straight | one edge | variants by ID; damage stages by health; front and back faces |
| Wall, corner | corner | joins two walls; may join two materials |
| Wall, doorway / window | one edge | carries a mount point for the room animation (door, gate, shutter) |
| Floor | one cell | variants by ID; damage can open a hole to the level below |
| Stairs | one or two cells | spans two levels; needs walkable collision, not just a box |
| Column | corner | may visually hold up a ceiling; collapse is up to the game |
| Ceiling / roof | one cell | a roof is the exterior side of a ceiling |
| Doodad mount | sphere inside a cell | pedestal, wall sconce, shelf |

### Testing rooms

Even though dungeons belong to the game, the foundry still tests that pieces compose. As a final step, it can assemble rooms, including rooms that humans have supplied and approved, joined through standard connectors. For example, a room with only doors offers a simple way to join other rooms through shared walls, checked with collision detection. Proposed checks:

- Every approved room joins every other through a standard connector (a doorway on a shared wall) without collision overlaps.
- A path exists from every doorway to every other doorway, using the capsule conformance from `spine.md`.
- Every damage stage of every piece still blocks or allows movement as the game expects (for example, a broken wall becomes passable at the stage the game declares).
- Doodads mounted in a room stay inside their sphere through every damage stage of the piece they are mounted on.

## Humanoid heroes and villains

This is the hardest type, because it needs a long pipeline. We start with one capsule conformance with height and radius parameters, covering everything from a gnome to an elf, so height comes first. The interaction model covers essentially everything.

### Parameters and skeleton

Next come the core parameters that define a humanoid: height, race, bulk, and face. As a philosophy, we want a good balance of parameters, but not so many that malformed humanoids appear. The parameters should produce humanoids that look like they could really exist.

That points to the next system: the rigging system, which exports a skeleton. It takes the inputs relevant to the skeleton into direct account, using medical and physiological literature, or best guesses from fantasy lore about how the proportions would work mechanically. (The style layer in `style.md` §4 then exaggerates that realistic structure toward the house look.)

At this level, animations are also adjusted in proportion to the inputs. The key feedback loop here is approving that the skeletons look real, that the animations look good, and that the mount points work with various things mounted. For example, we must handle both one-handed and two-handed mounts. At this stage, QA uses simple vectors as stand-ins to confirm that the hands mount correctly during animations. The stand-ins are a QA-only side channel; the system itself only exports mount points based on its configuration.

The skeleton is play tested (`playtest.md`) to validate that everything looks right. This covers contact points: feet must touch the ground, and the animation system can use inverse kinematics to adjust animations slightly as needed. The same applies to climbing. This is where the controller model matters, because it determines both the animations to aim for and any important transition animations, and the resulting runtime is what ships.

In this model, the humanoid doesn't really use files. The pipeline *is* the file that produces the model.

The AI can pull baseline animations from Meshy and from other sources to get a sense of things. It should favor a continuous movement system in which the runtime treats jumping as physics, so that the physics of motion and the animation agree. For example, either a jump is instant on input (and the animation flows from its current state into the next), or a jump has a wind-up that stops the player. This is why play testing is required: this component pushes back on whatever is directing control.

### Body mesh

The skeleton is the foundation. Next comes filling out the mesh. Here we can pull reference studies from Meshy and build the mesh per race. Since we are thinking generically, we want bald, unclothed, purely fleshy creatures. Generation takes a level-of-detail parameter, since this is the best place to reason about level of detail; this pipeline handles LOD itself rather than using the automatic LOD stage (`spine.md`, "Shared libraries").

This is another checkpoint: the animations and the mesh must work well together on a creature with flesh. The face needs to be interchangeable, so this phase uses only a basic skin. The test runs every animation and looks for self-intersections between body parts. The AI reads the report and judges how bad the overlap is. The system can flag which animations are problematic, so the user can reject them. It can do this across thousands of parameter combinations, which pushes back on the systems above.

### Skin

The next step is skinning, which can use the associated Meshy outputs to give each humanoid skin. The skin itself is a parameterized surface: skin color, vein color, hair color, and even how much body hair is generated are all controlled by parameters. The end result is a set of textures with normal maps. (`style.md` is a starting default, not a fixed rule, and is being loosened to allow this; see its §9.) The output of this stage is a naked, fleshed-out humanoid.

### Face

Next comes the face: eyes, nose, mouth, and scars. One face composition fits every race's head mesh, with coloring and similar parameters doing most of the work of telling races apart. This is a flexible composition that produces a range of possible faces, and it lets us bulk-approve the basis vectors. Other games may need a different split, and we will experiment with it over time.

At every step, the tooling shows the human a large number of parameter combinations side by side, so the visual, pattern-matching side of the brain can spot problems.

### Armor and mounts

The next step is armor and weapons. Armor works at two levels.

First, **skin replacement**: the skin texture is a cached input to the armor system, so armor can be painted directly onto the skin in a specific region. Skins and armor share the layered GPU cache described in `spine.md`: everyone with the same skin shares one texture, and each armor painted on top of it becomes a new shared resource.

Second, **mounted pieces**: the mesh system must also generate mount points that describe the surface where things can safely sit without conflict, which gives the armor system a way to fit or build armor that works well. Pieces that extend outward, such as pauldrons on the shoulders, can be ornate. The goal is to define armor like plate, which has a base layer on the skin (such as chain mail) with a breastplate on top. Mount points can also describe things like sleeves that have their own animation controller, constrained between a minimum and maximum cone so they can swing freely.

The core armor slots are torso, legs, feet, and hands. These can change per game, so each pipeline needs flexible armor mount points. For example, we may want a trinket whose mount point is a sphere with a smaller sphere hanging from it: conformance places an object in the small sphere, and the small sphere bounces along with realistic chain physics.

We also want a cape, which defines a conformance region behind the player and above the weapon, so the cape can flow in the wind and respond to movement. Details like these need strong boundaries to keep them separate, but each gets its own system.

### Hands

The hardest part will be the hands and their orientation. At its core, the skeleton system must validate that one-handed configurations (dagger, sword, or shield) and two-handed configurations (greatsword or staff) work with the right vectors. Those vectors are crucial for realism. Rather than a joint system, the hand system depends on what is held, using keyframe animations and quick interpolation between poses. Hands have their own independent level of detail, since they are complex. This alone is a separate system that needs its own QA.

The humanoid pipeline is the keystone of complexity.

## Creatures

Once the humanoid pipeline exists, creatures mostly come down to locomotion, and the workflow is similar to the humanoid one with different targets. For example, the AI might create a naga, octopod, or genie movement and animation system. That can then be mixed and matched with reptile, fur, or energy-storm mesh and skin systems.

There is no universal skeleton format. Each mixture of locomotion and body systems is its own pipeline and must be approved, which is where we test that the systems interlink. Two models are both valid, chosen per pipeline for whichever optimizes better:

- **Mesh from skeleton.** A dynamic meshing system, in the style of Spore, builds the mesh directly from whatever skeleton it is given.
- **Fixed skeleton channel.** The skeleton is fixed as a channel, and every body system written against that channel works with every locomotion system that produces it.

We want to encourage reuse between systems. A specific creature pipeline is an approved mixture of them, so that consistently high-quality output is achievable.

## Bosses

Each boss is a large undertaking, since a boss fight may have stages or visible telegraphing. Fortunately, bosses don't need to scale, and the pipelines above should be able to express complex boss models.

Boss stages are usually just parameters that feed the control system, and the hard part is tweening between states. A boss can also change completely: play a transition animation and swap the whole instance for another pipeline (a radical, JRPG-style transformation).

A few examples of how bosses could be built from existing pipelines:

- **Ancient dragon (Gargantuan).** A creature locomotion system with wings, on a multi-hex footprint (`style.md` §2). Stage two: a wing breaks, using the tree's break-pieces approach applied to a limb, and the dragon is grounded, switching its locomotion controller from flying to walking. Breath attacks are telegraphed by an emissive glow building in the throat before the effect fires.
- **Treant.** The tree pipeline scaled up, with a humanoid-style locomotion skeleton driving the trunk. Each limb has its own break interaction and health, and the wind simulator keeps running on the canopy while it fights.
- **Siege golem.** A body assembled from building pieces (stone walls, columns) mounted on a creature skeleton. As its health drops it sheds pieces through the building pipeline's damage stages, exposing a glowing core in the final stage. This reuses the wall damage machinery directly.
- **Hydra.** A chain-creature body with several head mount points. Severing a head uses the break interaction; regrowing two in its place is a state machine transition that mounts new heads at the same points.
- **Lich.** A humanoid with a telegraphed area spell from the magic pipeline: a ground decal fills in over a fixed time before the effect lands. The game drives the fill as progress, the same way it drives the chest lid. In its final stage, the lich transforms: a transition animation swaps it for a new pipeline entirely.

## Magic

Magic is a pipeline built around keyframe animation (rain falling from a cloud, a portal swirling open) that mixes with other animation systems: a caster's hands, a weapon, the ground under a target.

A magic effect is built in stages, each a system that is approved before moving on:

1. **Base surface.** The flat, static foundation of the effect: a portal's base texture, a rune circle on the ground, the glow on a hand. It is approved on its own first, because everything else is layered on it.
2. **Motion.** The animation that drives the particles: where they spawn, how they move, and how long they live. Motion is a function of time and the effect's parameters (radius, intensity, direction), so it is deterministic and replays exactly in a play test.
3. **Form.** What gives the particles shape. The options, chosen per effect:
   - **Billboards:** flat sprites that always face the camera. Cheapest, and good for sparks, embers, and rain.
   - **Meshes:** small models. Good for shards, leaves, and rocks, which need to read from every angle.
   - **Ribbons:** strips that trail behind a moving point. Good for slashes and streaks.
   - **Shells:** layered meshes around a volume. Good for fire, shields, and storms (`style.md` §6 uses 2-band shells with a flat hot core).
4. **Color.** Tints the effect from an element palette, following the effects language in `style.md` §6: flat two-tone shapes with an ink border tinted by the element (never black), and some of the caster's own color mixed into the shadow and ink.

Generative AI can produce particle shapes (sprites, or small meshes from Meshy) that the motion system then animates. Like all Meshy output, these are generated in the foundry and baked, then flattened to the effect's palette so they fit the house style.

### The cast lifecycle

Every effect runs the same state machine:

| State | What happens | Example |
|---|---|---|
| Telegraph | The base surface appears and fills in, warning players | the lich's ground decal |
| Cast | The main burst | the breath attack fires |
| Sustain | The effect holds for its duration | rain keeps falling |
| Dissipate | Particles fade out and the base surface disappears | the portal closes |

As with the chest lid, the game owns progress through the lifecycle (how long the telegraph lasts, whether a cast is interrupted), and the systems provide a continuous function of that progress.

### Mounting and quality

Effects attach to the world through the same mount points as everything else: a caster's hand, a weapon's blade, a building's wall sconce (for torches), or a spot on the ground. Particle counts are tied to the quality sliders, so a low setting thins out the rain rather than removing it.

## Open questions

### Nature

1. **Tree coloring vs. `style.md`.** The tree coloring system "updates textures", but `style.md` §5 says color lives in flat paint regions, with no texture noise. Is tree coloring a set of palette-region values chosen by genome, age, and season, or genuinely textures? (Skin now allows textures and normal maps; does the same loosening apply to trees?)
2. **Instanced tree forms.** "It should be possible to use instanced geometry to mint a new tree form": does this mean building trees out of shared, instanced pieces (branch and leaf clusters), or reusing one tree's instanced geometry as the starting point for a new species?

### Magic

3. **Interacting with the world.** Do effects interact with geometry (rain splashing on a roof, fire scorching a wall), or are they purely visual, with the game handling all consequences?

Right now, they are pure geometry with mount points. We can also have mount surfaces in the future where planes provide walkable geometry as well, so let's include how it can handle players walking on things. These planes can also provide places to mount effects (like fire).


4. **Combining elements.** Can two effects mix (fire meeting a wind spell becomes a firestorm)? If so, is the combination its own approved pipeline, like a creature mixture, or does a system blend two effects at runtime?

My gut is that this is too complex, and I'd rather have more complex runtimes built by AI that are optimal around the interaction instead of merges. For example, if a game combines water and fire, it becomes steam which behaves radically different.

5. **Effects as damage drivers.** When a fireball hits a wooden wall, does the magic pipeline feed anything into the wall (such as a "scorched" parameter), or does the game only lower the wall's health?

This is a game effect thing such that game can export basic collision shapes that are invisible as sensors, and this provides the game with the feedback channel to connect dots. In this effect, a wall on fire will need the game to reduce the health. This entire system provides definition of things/assets and then provides needed hooks to interact, signal animations, mount things and connections, drawing, animation.