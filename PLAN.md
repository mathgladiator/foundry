# The Plan

This file is how the project is managed. It holds one current milestone, with TODO items that I, the human, will validate. The agent works on the current milestone; when it's done, I rename it and set the next one.

- **I set scope.** The current milestone's TODOs define what is in scope, so we stay on the QA track and don't wander. Anything I add to the current milestone is in scope; anything else waits.
- **The agent can suggest.** Ideas beyond the current milestone go under "Future milestones" as suggestions. They are not in scope until I move them.
- **Maximum progress, then questions.** The agent makes as much progress as it can, then writes its questions at the end of the current milestone. I answer them inline, so questions and answers stay together.
- **Summaries on request.** As a separate step, I can ask the agent to summarize a finished milestone in a new file and suggest the next current milestone.

# Current milestone: proof of life

Build the foundry binary so that it shows a workspace UI where I can select things and approve them, with the agent able to drive it end to end.

## To validate

- [ ] **The UI.** Does the workspace look like a good start? It should mirror the project layout (`design/flow.md`, "The workspace"): pipelines, systems, fixed assets, and concept art, each selectable, with approval state and notes.
- [ ] **MCP.** Does the binary launch an MCP server, and can Claude talk to it?
- [ ] **The build.** Is the build system set up so that each system compiles to WebAssembly, and the same systems also build into a native static library?
- [ ] **WebAssembly.** Is everything building and linking, with systems actually running in WebAssembly inside the host?
- [ ] **The agentic flow.** Can the agent create a system, build it, run it, and see the result, all through MCP?
- [ ] **Static meshes, end to end.** The agent and the foundry together produce a static mesh (`design/systems.md`, "Static things"): the workspace updates, I can see the mesh against its conformance shape, and I can approve it.

## Not in this milestone

To keep the milestone focused, these wait even though they are designed: trees and other nature, crafted things, buildings, humanoids, creatures, magic, auras, sound, shadows, the sleep and wake engine beyond what static meshes need, recording and replay, and the generated C game API.

## Decisions

These were settled before starting:

| Decision | Choice |
|---|---|
| UI toolkit | Dear ImGui, on its SDL3 GPU backend |
| Build system | CMake, with two toolchains: the native compiler for the host and static libraries, and `wasi-sdk` (Clang targeting WebAssembly) for systems |
| WebAssembly host runtime | Wasmtime, through its C API |
| MCP transport | HTTP on localhost; Claude Code connects with `claude mcp add --transport http` |
| Meshy budget | 250 credits for this milestone without asking. At current prices that is roughly 6 to 8 static meshes taken from concept image to remeshed 3D, so start with one or two and ask before generating in bulk. |

## Questions from the agent

(None yet.)

# Future milestones

Suggestions from the agent, in a proposed order. Each builds on the one before and ends with something the human can play test and approve. They are not in scope until moved into the current milestone.

1. **Static things, for real.** The full static-thing pipeline (`design/systems.md`): concept images, image to 3D, remesh, resize to a conformance shape, bake to a disk format. The fixed asset library with its two-way conformance index (`design/flow.md`). Provenance sidecars. Play test views: explore and battle cameras, the silhouette thumbnail, and the lineup against the hero and the hex (`design/playtest.md`).
2. **The engine core.** The submit queue, shared buffers, instancing, and the first version of the mega shader with `style.md` §6 (cel bands, rim, ink), with its parameters live on the control pad. Sleep and wake, with the active set visible in the workspace (`design/submit.md`).
3. **Interactive things.** The chest (a hinge driven by game-owned progress) and the rock (a break interaction path), proving the per-instance state machine, the gamepad control pad, and interaction paths. Recording and replay, and the wall of recordings for bulk review.
4. **The native build and the C API.** The merged native runtime, `runtime.h` assembled from fragments, the build checks from `ORG.md` (namespaces, private systems, final-system isolation), and a minimal sample game that spawns and drives instances through the API alone. Replay recordings against both builds and compare.
5. **Nature.** Trees (skeleton, body, coloring, wind simulator, break) and the quality-slider caching that collapses variants.
6. **Buildings.** Box conformance pieces with damage stages, doodad mounts, and the room-combination tests.
7. **Light, shadow, and transparency.** Sun and marked-light shadow maps with the shadow test scene, the transparent and additive passes, and the first magic effects and auras.
8. **Sound.** Sound events, per-pipeline tables, and the sound manager on SDL3 audio.
9. **Humanoids.** Likely several milestones: skeleton and animation, body mesh, skin, face, armor and mounts, hands, sensors and head rings.
10. **Creatures and bosses.** Locomotion mixtures, then bosses assembled from the pipelines above.
