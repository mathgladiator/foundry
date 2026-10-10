# The Spine

At its core, the foundry is an AI harness exposed via MCP so that a human can stay in the loop and direct the construction of asset systems. An asset system is a factory that produces dynamic game assets on the fly. For example, a hero unit is an asset system: a base model that can wear different visual armor or carry different mounted weapons. This is a key scenario, and it demands carefully designed layers that work well across an entire ecosystem of assets.

This document defines an asset system and describes its invariants, so that AI-driven work makes steady, linear progress instead of accumulating complexity quadratically and drowning in validation. Asset quality is the macro problem when using AI. We tackle it with phase gates, because when the AI has complete control, fixing one aspect of quality can quietly regress another. We also establish solid communication boundaries between the phases of asset production, which enables a mix-and-match system capable of near-infinite diversity.

## Layer 1: Conformance sizing

Starting the foundry requires a set of fundamental units. For example, a game with a hex grid defines the hex cell as its foundational unit. The hex cell then defines the conformance size of a player, which is a capsule. Other games may use different grids to define a unit of size, but the goal is the same: a defined size, so that every model can always be put into perspective against a conformance size and a base unit. The base unit may also be an expanded footprint, such as a group of hex cells; for example, a unit can occupy three adjacent hex cells, with the capsule's base being the circle inscribed in that footprint. Each asset system produces assets within a set of conformance sizes. (See `style.md` §2 for the concrete scale, yardsticks, and size classes.)

This provides a geometric baseline that works well in a game: it can be used to define a hit box or a movement capsule, keeping the world tidy for traditional game logic. Beyond making assets fit within the world and within other asset systems, the conformance shape is also the outermost bounding shell used for performance optimizations such as visibility culling, and for other interaction concerns.

## Layer 2: Interactivity capabilities

The next layer of an asset system is the interactivity capabilities layer. It defines the controls a player can exercise, the senses and behaviors a monster has, and generally how an asset can act and be acted upon in the world. It is expressed as a set of capability bits, which form a finite communication channel between the asset system and the layers below it.

For instance, a humanoid character has walk, run, idle, and so on for movement, with jumping and swimming as another set of capability bits. Treasure chests, levers, and the like operate on a simple interact channel. Rocks and trees can have a channel that signals a break, alongside an interact channel that signals they are about to break. This layer can also carry signals such as health, which inform everything downstream; a wall factory, for example, can use health to decide when to start breaking and how broken to appear.

## Process

This gives us a clear pipeline that we keep rigid. A conformance shape must be reasoned about and approved. The interaction idiom must be reasoned about and approved. Isolating how we build with agents this way saves tokens, but more importantly it minimizes the churn of letting an agent run wild.

### Versioned waterfall

We embrace a waterfall design methodology, made workable by versioning. Every layer of an asset system is versioned, and once a version is approved it is marked **final**: it is immutable from then on. The agent never edits a final version. To change anything, it must fork, creating a new version that branches from the old one. This is a philosophical shift: a decision is final, but it is never a dead end, because the response to a bad or outdated decision is a new branch rather than an edit in place.

Forking buys us three things:

- **Protection.** Existing asset systems, and anything in the game built on them, stay pinned to the versions they were approved against. New work cannot silently break them.
- **Migration as an experiment.** Because the old and new versions both exist, we can test a specific migration directly: run an asset system against both, compare the outputs, and move dependents over deliberately, one at a time.
- **Trivial caching.** A final version never changes, so anything derived from it can be cached forever.

This also means we deliberately set aside the classic DRY ("don't repeat yourself") principle and embrace WET ("write everything twice"). Forking duplicates rather than shares, and that is fine: an agent is not bound by the human limits on reasoning that make duplication expensive, and this mirrors how software is already versioned and evolves organically. Duplication is the price of moving fast without breaking what is already approved.

### The UI

The UI stays very simple: the foundry exposes an MCP server and hands the work of construction over to the agent, while the human approves each phase gate and decides when a version becomes final. Each asset system is therefore a pipeline of varying depth, made up of versioned layers. Because final layers are immutable, each layer's output can be cached, and the foundry's branches can track the game's branches. Systems we build can then go directly into the game across a boundary that is crisp, clean, and cacheable.

## Moving fast

At its core, we want a very lean communication boundary, so we use simple byte arrays between each runtime layer. This requires distinguishing between **runtime layers** and **validation layers**. Conformance is a validation layer: it is ultimately about the AI validating assets, and it passes requirements forward into the final blobs for the runtime to act on. Similarly, the interactivity capabilities layer forwards its capabilities to the agent and to validation, which together shape how the final blob is manifested. Runtime layers such as animation, mesh generation, skinning, and mounting, on the other hand, need to pass information between each other at runtime. Since we want these boundaries to be versioned and rigid, we pass a binary blob between layers, with generated code to pack and unpack it. This keeps communication very simple, and it also opens the door to thinking about how the system can be updated in real time by the agent.

We want to run Claude in this directory and use the foundry while it builds itself. This is the true power of agentic development, and it means we must enforce exceptionally strong boundaries. We do this by using WebAssembly as the runtime and C++ as the language. This keeps the human in the loop and makes boundary enforcement easy to validate (for example, by running Claude as a separate user without permission to modify the host). Asset systems are therefore, in spirit, C++ artifacts with parameters that control instancing. We intentionally blur the line between code and static art assets: each layer is a black box behind a strong communication boundary. This gives Claude complete control over each system, while the human operator can probe it and give targeted feedback, so that progress and motion happen together. The key is protecting forward velocity while ensuring the final artifacts can be approved and tested in a variety of ways.

For the final game runtime, the generated code for each WebAssembly module is merged, and the byte-packing overhead disappears in favor of direct structures. Because asset systems may be pinned to different versions of the same layer, a single game build can ship several versions side by side. Each version therefore gets its own isolated C++ namespace, which is the main reason the foundry uses C++ rather than C. Shared artifacts have their lifetimes managed by a common reference-counting system, which gives us a clear target for automation: verifying that each asset system operates soundly in isolation (for example, can it run at 100x speed without crashing or leaking memory?). The WebAssembly container also changes the risk calculus of having AI write C++: because each container has a known, predictable topology, its input and output spaces can be studied systematically and given reasonable coverage with traditional unit tests.

### Shared libraries

Not every stage belongs to an asset system. The host also exposes shared libraries at various levels of the pipeline: general-purpose stages that many asset systems need and that can run automatically, without per-system design or approval. For example, after meshing, a mesh needs a level-of-detail adjuster and then a triangle stripifier. These stages exist in the runtime as well, not just in the foundry, so they run whenever an asset is instanced. That cost is why caching matters: because final versions are immutable, the output of each shared stage can be computed once per version and reused. Shared libraries are versioned like everything else, so an asset system stays pinned to the library versions it was approved against. A new library version never flows into an existing asset system automatically: adopting it requires forking the asset system and re-approving the fork, just like any other change.

## Full fidelity

The agentic workflow starts with the coding harness being able to ship these WebAssembly blobs and to define the communication layers between them. We expect communication-layer changes to be rare, but they require approval, because they determine the shape of each black box the agent controls. The host provides code generation for unpacking the input byte array into C++ structures and for packing the output back into a byte array for the host runtime. As a key requirement, the host must have good ways of visualizing output, so that the agent can pilot it, take screenshots, and feed what it sees back into the system. We can also run collision detection to catch problems that break physics at a precise level, giving the agent deeper insight. The goal is to give the agent enough detail to get things right before a human inspects the result and approves it, rejects it, or provides feedback.

## What's next?

This is the hosting spine. The application runs on SDL3 with a good UI, so that assets can be experimented with in a slower version of the runtime system, where the space of possible outputs is constrained and versioned. Next, we will outline the complexities of the various asset systems.
