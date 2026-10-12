# The Plan

This file is how the project is managed. It holds one current milestone, with TODO items that I, the human, will validate. The agent works on the current milestone; when it's done, I rename it and set the next one.

- **I set scope.** The current milestone's TODOs define what is in scope, so we stay on the QA track and don't wander. Anything I add to the current milestone is in scope; anything else waits.
- **The agent can suggest.** Ideas beyond the current milestone go under "Future milestones" as suggestions. They are not in scope until I move them.
- **Maximum progress, then questions.** The agent makes as much progress as it can, then writes its questions at the end of the current milestone. I answer them inline, so questions and answers stay together.
- **Summaries on request.** As a separate step, I can ask the agent to summarize a finished milestone in a new file and suggest the next current milestone.

# Current milestone: a complex system

- [x] **rocks that break uniquely**
  - `rock_v1` (`book/rock_v1.md`): 8 materials (granite, basalt, sandstone, limestone, slate, marble, obsidian, river stone), each with its own fracture; 4 sizes, moss, and season.
  - Pieces are cut at a lower level of detail when the rock is built, so a break never rebuilds; a closed-form animator throws, bounces, and settles them.
  - To validate: each material whole, mid-break, and settled (Start `break_time` in the View tab), against its reference photos.
- [x] **bushes that pair with the trees**
  - `bush_v1` (`book/bush_v1.md`): 14 species across the trees' biomes, swamp included; several stems from the ground, the trees' wind, flowers and berries by season.
  - Fruit can be picked: the `fruit` parameter applies only to species whose config marks them `pickable`, and is disabled for the rest.
  - To validate: every species by age and season, fruit on and off, and the soak's drain check (RSS stays over tolerance while the heap is flat).
- [x] **flowers that pair with the trees**
  - `flower_v1` (`book/flower_v1.md`): 15 species in clumps, eight head forms, buds, bloom, seed heads, and winter stalks by season, swaying in the trees' wind.
  - To validate: every species by season, against its reference photos and beside the trees of its biome.
- [x] **trees have pickable fruit**
  
## Not in this milestone

To keep the milestone focused, these wait even though they are designed: trees and other nature, crafted things, buildings, humanoids, creatures, magic, auras, sound, shadows, the sleep and wake engine beyond what static meshes need, and recording and replay.

# Future milestones


# Suggestions are great
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
