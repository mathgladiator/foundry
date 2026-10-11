# The Spine

At its core, the foundry is an AI harness exposed via MCP so that a human can stay in the loop and direct the construction of asset pipelines. An asset pipeline is a factory that produces dynamic game assets on the fly. For example, a hero unit is a pipeline: a base model that can wear different visual armor or carry different mounted weapons. This is a key scenario, and it demands carefully designed layers that work well across an entire ecosystem of assets.

This document defines a pipeline and describes its invariants, so that AI-driven work makes steady, linear progress instead of accumulating complexity quadratically and drowning in validation. Asset quality is the macro problem when using AI. We tackle it with phase gates, because when the AI has complete control, fixing one aspect of quality can quietly regress another. We also establish solid communication boundaries between the phases of asset production, which enables a mix-and-match system capable of near-infinite diversity.

At its core, the foundry is a graphics engine with a solid understanding of geometry, which it provides to the game. Game logic, world building, and music belong to the game.

These documents describe the foundry at maximum expression: everything a pipeline could support. Each project then tailors it. `style.md`, for example, is the current project's style, and a project opts in to it rather than every pipeline being bound by it.

## Vocabulary

- **System.** A black box the agent builds, with one fixed role: it takes one channel in and produces another channel out. It is a pure function: a clean sandbox of compute between channels. Systems are versioned and immutable once final, and they are private to the runtime: a game never sees one (`api.md`). Each frame, a system takes part in the update phase (CPU state, such as an animator or a timeline), the render phase (producing drawables), or both (`submit.md`).
- **Channel.** The structure that crosses the boundary between two systems. Channels are shared: any number of systems can read or write the same channel. A channel is written as a JSON schema, not C++: a schema is easier to diff, approve, and generate from, and the host generates everything else from it (the C++ structure, the packer, the disk format, and JSON conversion; `flow.md`).
- **Pipeline.** A directed acyclic graph (DAG) of systems, plus the parameters that drive it from the top and a state machine. The DAG ultimately produces an object with a specific shape and texture. A pipeline is what the game asks for ("a young spruce in autumn"). It can branch and reuse systems at channel boundaries, so different interaction paths (a tree swaying versus a tree breaking) can share most of their systems.
- **Pipeline instance.** One live asset in the game, with its own parameter values and state. The instance is the core unit of the engine: it has its own timeline, and it sleeps when nothing is happening to it (`submit.md`).

The concrete package layout (directories, generated packers, disk formats) is described in `flow.md`.

## Layer 1: Conformance sizing

Starting the foundry requires a set of fundamental units. For example, a game with a hex grid defines the hex cell as its foundational unit. The hex cell then defines the conformance size of a player, which is a capsule. Other games may use different grids to define a unit of size, but the goal is the same: a defined size, so that every model can always be put into perspective against a conformance size and a base unit. The base unit may also be an expanded footprint, such as a group of hex cells; for example, a unit can occupy three adjacent hex cells, with the capsule's base being the circle inscribed in that footprint. Each pipeline produces assets within a set of conformance sizes. (See `style.md` §2 for the concrete scale, yardsticks, and size classes.)

The capsule is only the most common conformance shape. In general, a conformance is a union of simple shapes: capsules, boxes, and spheres. Some regions of it may be unbounded and non-colliding; a tree's conformance, for example, is a colliding stump with an unbounded, purely visual canopy above it. Conformance shapes also define mount points: a pedestal can expose a sphere, and any object whose conformance fits inside that sphere can be placed there.

Beyond collision and mounting, conformance shapes do two more jobs:

- **Sensors.** Invisible shapes that collide with nothing but report overlaps to the game, such as a fireball's blast sphere. A sensor can also be just an origin and an outward direction, such as a monster's eye, from which the game builds its own geometry (a vision cone). Sensors are how the game learns that something happened.
- **Mount surfaces.** Planes that the game can treat as walkable (an ice bridge spell) and that effects can mount on (fire spreading across a floor).
- **Sound volumes.** Rough room volumes and walls that the audio layer uses to muffle sounds and add reverb (`sounds.md`). Precision isn't needed.

Conformance shapes do not have to share one grid, even within a project. The terrain may be hex while a house on it is built from boxes. What matters is that every piece of generated geometry has a conformance to root on, and that it is always clear whether things of one shape fit with things of another.

This provides a geometric baseline that works well in a game: it can be used to define a hit box or a movement capsule, keeping the world tidy for traditional game logic. Beyond making assets fit within the world and within other pipelines, the conformance shape is also the outermost bounding shell used for performance optimizations such as visibility culling, and for other interaction concerns.

## Layer 2: Interactivity capabilities

The next layer of a pipeline is the interactivity capabilities layer. It defines the controls a player can exercise, the senses and behaviors a monster has, and generally how an asset can act and be acted upon in the world. It is expressed as a set of capability bits, which form a finite communication channel between the pipeline and the systems below it.

For instance, a humanoid character has walk, run, idle, and so on for movement, with jumping and swimming as another set of capability bits. Treasure chests, levers, and the like operate on a simple interact channel. Rocks and trees can have a channel that signals a break, alongside an interact channel that signals they are about to break. This layer can also carry signals such as health, which inform everything downstream; a wall pipeline, for example, can use health to decide when to start breaking and how broken to appear.

### State and time

Each pipeline instance has a state machine that holds every parameter the pipeline needs, and the game manages it. The parameters live in a dynamic property bag: during development the bag crosses the WebAssembly boundary as a channel, and at runtime it is a native structure. Each system caches the properties it reads during evaluation, so the bag's overhead is paid once rather than on every access.

Time is one of those parameters. Systems never read a clock; they receive time from the state machine like any other input. This keeps every system deterministic for a given set of inputs, which is what makes caching, replaying play tests, and running at 100x speed meaningful.

The state machine also decides which interaction path is active, but continuous progress belongs to the game. A system provides a continuous function (lid angle as a function of progress, for example), and the game decides how fast progress moves and whether it reverses.

### What a pipeline gives the game

A pipeline defines a thing and provides the hooks to use it: interactions, animation signals, mount points and connections, sensors, drawing, animation, and sound events (`sounds.md`). It does not decide consequences. When a fireball's sensor overlaps a wooden wall, the game lowers the wall's health; the pipelines only make the overlap visible and the damage drawable.

## Process

This gives us a clear pipeline that we keep rigid. A conformance shape must be reasoned about and approved. The interaction idiom must be reasoned about and approved. Isolating how we build with agents this way saves tokens, but more importantly it minimizes the churn of letting an agent run wild.

### Versioned waterfall

We embrace a waterfall design methodology, made workable by versioning. Every system is versioned, and once a version is approved it is marked **final**: it is immutable from then on. The agent never edits a final version. To change anything, it must fork, creating a new version that branches from the old one. This is a philosophical shift: a decision is final, but it is never a dead end, because the response to a bad or outdated decision is a new branch rather than an edit in place.

Forking buys us three things:

- **Protection.** Existing pipelines, and anything in the game built on them, stay pinned to the system versions they were approved against. New work cannot silently break them.
- **Migration as an experiment.** Because the old and new versions both exist, we can test a specific migration directly: run a pipeline against both, compare the outputs, and move dependents over deliberately, one at a time.
- **Trivial caching.** A final version never changes, so anything derived from it, for a given set of inputs, can be cached forever.

This also means we deliberately set aside the classic DRY ("don't repeat yourself") principle and embrace WET ("write everything twice"). Forking duplicates rather than shares, and that is fine: an agent is not bound by the human limits on reasoning that make duplication expensive, and this mirrors how software is already versioned and evolves organically. Duplication is the price of moving fast without breaking what is already approved.

The word **fork** is reserved for versioning. When a pipeline takes a different route at runtime (a tree switching to its break path), that is an **interaction path**, chosen by the state machine.

### Channels change rarely

Systems fork freely, but channels are the contract between them. Channels are versioned, immutable artifacts like systems. The agent can create a channel or propose an upgrade, but an upgrade goes through its own approval workflow: the user sees the change, along with a written reason, and approves it. Approving it invalidates every system and pipeline that depends on the old channel, and they must be forked onto the new one. That is the deliberate cost of changing a contract. A channel change also requires a host restart.

### The UI

The UI stays very simple: the foundry exposes an MCP server and hands the work of construction over to the agent, while the human approves each phase gate and decides when a version becomes final. Approval happens through play testing (`playtest.md`), which is the primary way both the human and the agent interact with a pipeline. Because final systems are immutable, each system's output can be cached, and the foundry's branches can track the game's branches. Pipelines we build can then go directly into the game across a boundary that is crisp, clean, and cacheable.

## Moving fast

At its core, we want a very lean communication boundary, so during development systems exchange simple byte arrays. This requires distinguishing between **runtime layers** and **validation layers**. Conformance is a validation layer: it is ultimately about the AI validating assets, and it passes requirements forward into the final blobs for the runtime to act on. The interactivity capabilities layer is a hybrid. At build time, it forwards its capabilities to the agent and to validation, which together shape how the final blob is manifested. At runtime, it becomes the per-instance state machine described above. Runtime systems such as animation, mesh generation, skinning, and mounting, on the other hand, need to pass information between each other at runtime. Since we want these boundaries to be versioned and rigid, each channel has generated code to pack it into and unpack it from a byte array. This keeps communication very simple, and it also opens the door to updating the system in real time through the agent.

We want to run Claude in this directory and use the foundry while it builds itself. This is the true power of agentic development. The agent has full control: it should know the whole system, because it is the one building it, including the host. That makes the boundaries all the more important, and the agent must work hard to fit each piece of work within a boundary and keep things updatable at runtime. We use WebAssembly as the development runtime and C++ as the language. The boundaries are enforced by construction: every channel's packing code is generated by the host, not written by the agent, and changing a channel needs the user's consent. Pipelines are therefore, in spirit, C++ artifacts with parameters that control instancing. We intentionally blur the line between code and static art assets: each system is a black box behind a strong communication boundary. This gives Claude complete control over each system, while the human operator can probe it and give targeted feedback, so that progress and motion happen together. The key is protecting forward velocity while ensuring the final artifacts can be approved and tested in a variety of ways.

### The native build

The final game build is native. The generated code for each system is merged, the packers go away, and channels are passed as direct structures. Because pipelines may be pinned to different versions of the same system, a single game build can ship several versions side by side, each in its own isolated C++ namespace (`src/systems/$name_$version`, see `ORG.md`). This is the main reason the foundry uses C++ rather than C internally. The game-facing front door is still a plain C API (`api.md`).

WebAssembly is the development box, not the shipping one: it gives us a container that is easy to instrument at runtime, with the generated boundary code sitting right next to each system. Avoiding undefined behavior is best effort: the goal is that each system is a clean sandbox of pure compute between channels, so that what we approved in WebAssembly behaves the same natively.

Shared artifacts have their lifetimes managed by a common reference-counting system, which gives us a clear target for automation: verifying that each pipeline operates soundly in isolation (for example, can it run at 100x speed without crashing or leaking memory?). The WebAssembly container also changes the risk calculus of having AI write C++: because each container has a known, predictable topology, its input and output spaces can be studied systematically and given reasonable coverage with traditional unit tests.

### Shared libraries

Not every stage is specific to one pipeline. The host also exposes shared library systems at various levels of the pipeline: general-purpose stages that many pipelines need and that can run automatically, without per-pipeline design. For example, after meshing, a mesh needs a level-of-detail adjuster and then a mesh optimizer that orders triangles for the GPU (`submit.md` explains why this replaces a triangle stripifier). These stages exist in the runtime as well, not just in the foundry, so they run whenever an asset is instanced. Shared libraries are versioned like everything else, so a pipeline stays pinned to the library versions it was approved against. A new library version never flows into an existing pipeline automatically: adopting it requires forking and re-approval, just like any other change.

**Level of detail** is a shared responsibility, driven by a common pool of quality sliders in the game's configuration. Each pipeline decides whether it needs the automatic LOD stage or whether one of its generators handles LOD directly (as the humanoid body mesh and hands do in `systems.md`).

### Caching and GPU resources

Caching is what makes runtime generation affordable. Because final versions are immutable, the output of each system can be computed once per version and set of inputs, then reused. To get more out of the cache, the runtime collapses inputs where it can: it ties level of detail to the quality sliders, shares meshes between instances with the same parameters, and caps how many distinct variants it generates (see the tree example in `systems.md`).

Generation is on demand, and the cache is shared and layered. For example, every humanoid with the same skin parameters shares one skin texture; armor painted onto that skin becomes a new cached resource layered on top of it, and if more characters wear that armor, they share it too. Cached resources are reference counted and, once unused, released after a timer rather than immediately. Both creation and destruction of GPU resources are amortized to minimize churn and keep generation within a budget. While a new resource is being generated, the old one stays on screen as a fallback.

## Full fidelity

The agentic workflow starts with the coding harness being able to ship these WebAssembly blobs and to define the channels between them, as JSON schemas. The host provides code generation for unpacking the input byte array into C++ structures and for packing the output back into a byte array for the host runtime. As a key requirement, the host must have good ways of visualizing output, so that the agent can pilot it, take screenshots, and feed what it sees back into the system. We can also run collision detection to catch problems that break physics at a precise level, giving the agent deeper insight. The goal is to give the agent enough detail to get things right before a human inspects the result and approves it, rejects it, or provides feedback.

## What's next?

This is the hosting spine. The application runs on SDL3 with a good UI, so that assets can be experimented with in a slower version of the runtime system, where the space of possible outputs is constrained and versioned. The other documents:

- `flow.md`: the package layout, manifests, and disk formats.
- `transforms.md`: asset-pipeline transforms, which run in the foundry and bake their results, as opposed to runtime systems.
- `submit.md`: the engine that updates, sleeps, and draws pipeline instances, and the mega shader.
- `playtest.md`: how humans and the agent play test and approve.
- `systems.md`: the individual pipelines.
- `api.md`: the generated front door a game uses to drive the runtime.
- `sounds.md`: how pipelines fire sound events.
- `third.party.md`: outside services for producing assets, including Meshy.
- `style.md`: the house style.
- `future.md`: ideas deliberately deferred, with what they would cost.
