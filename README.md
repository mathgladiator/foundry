# foundry

An AI-driven harness for building game assets as **pipelines**: small, versioned C++ systems that generate assets at runtime, instead of static files made by hand.

A human stays in the loop. An AI agent builds and changes the pipelines through an MCP server, and the human play tests the results with a gamepad and approves them. Approved work is final and never edited in place; changes are made by forking a new version.

## Status

Design phase, moving into the first milestone. See [`PLAN.md`](PLAN.md) for the current milestone and what it needs to prove.

## Core ideas

- **Systems, channels, pipelines.** A system is a pure function from one channel (a fixed data structure) to another. A pipeline is a graph of systems that produces an asset, such as a tree, a chest, or a humanoid hero.
- **Conformance.** Every asset is sized against a simple shape (a capsule, box, or sphere), so assets fit the world and each other, and expose mount points, sensors, and surfaces to the game.
- **A versioned waterfall.** Each decision is approved and becomes final. Changing it means forking a new version, so approved assets can never silently break.
- **Two builds.** During development, each system runs in its own WebAssembly sandbox, which is easy to instrument and inspect. The game ships a native build with all systems merged into one runtime, behind a plain C API.
- **An engine for dynamic content.** Instances update on the CPU, sleep when nothing is happening, and are drawn through a sorted, instanced submit queue on SDL3's GPU API, using one shared mega shader.

## Design documents

| Document | What it covers |
|---|---|
| [`ORG.md`](ORG.md) | How the source tree is organized, and its dependency rules |
| [`design/spine.md`](design/spine.md) | The core architecture: vocabulary, conformance, interactivity, versioning, builds, caching |
| [`design/flow.md`](design/flow.md) | Package layout, manifests, disk formats, fixed assets |
| [`design/submit.md`](design/submit.md) | The runtime engine: update, sleep, rendering, shadows, transparency, the mega shader |
| [`design/api.md`](design/api.md) | The generated C API a game uses |
| [`design/playtest.md`](design/playtest.md) | Play testing, recording, and approval |
| [`design/systems.md`](design/systems.md) | The planned pipelines: static things, nature, crafted things, buildings, humanoids, creatures, bosses, magic |
| [`design/sounds.md`](design/sounds.md) | Sound events and tables |
| [`design/style.md`](design/style.md) | The current project's visual style (opt-in) |
| [`design/third.party.md`](design/third.party.md) | Outside services for asset production, including Meshy |
| [`design/future.md`](design/future.md) | Ideas deliberately deferred |

## Technology

C++ (host, runtime, and systems), C (the game-facing API), SDL3 and its GPU API (Vulkan, Metal, Direct3D 12), WebAssembly (development sandbox), HLSL compiled with `SDL_shadercross`, and MCP (the agent's interface).

## License

MIT. See [`LICENSE`](LICENSE).
