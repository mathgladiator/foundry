# Transforms

This document describes a gap in the design that building the first pipeline exposed, and how the foundry closes it.

## The gap

`spine.md` has one kind of work: a **system**, a pure function from one channel to another, built for WebAssembly during development and merged into the native runtime that ships in the game. Everything the foundry does was meant to be expressed as systems connected by channels.

Building the static-thing pipeline showed that much of the foundry's work is not like that. Fitting a Meshy model to a conformance shape, reducing it to levels of detail, measuring how much quality each reduction loses, and reordering triangles for the GPU all happen **once, in the foundry, before the asset ever reaches a game**. Forcing them into systems caused real problems:

- **Channels for intermediate data.** A mesh had to become a channel (`mesh_v1`), and the reduction's output another (`progressive_mesh_v1`). Channels are contracts with the game's runtime, approved and versioned. These structures were never meant to reach the runtime; they were scaffolding between foundry steps, yet they carried the full weight of a contract.
- **The wrong end target.** A progressive mesh is a good tool, but a poor runtime format. GPUs want fixed vertex and index buffers. A progressive mesh changes vertex positions from level to level (work on the CPU, or extra data on the GPU), can't batch different levels together, and costs memory for levels no one draws. The standard runtime answer is a few discrete levels of detail, each an optimized index buffer. Shipping the progressive mesh as a system would have optimized for the foundry's convenience, not the game's.
- **Constraints that don't buy anything.** Systems must be deterministic across WebAssembly and native builds, so they avoid C library math and hash-map iteration. A foundry step that runs once and bakes its output doesn't need that guarantee; the baked output is what gets approved and shipped. Likewise, systems can't use outside libraries, but foundry steps benefit from them (meshoptimizer for triangle ordering).

## Two kinds of work

The foundry now distinguishes:

| | Runtime system | Pipeline transform |
|---|---|---|
| Runs | in the game (WebAssembly in development, native when shipped) | in the foundry, at build time |
| Shape | one channel in, one channel out | one asset in, one asset out, with parameters |
| Data | channels: approved, versioned contracts | the host's own formats; nothing reaches the game except the baked result |
| Determinism | byte-identical across builds | reproducible: the operation list is recorded and can be replayed |
| Libraries | none beyond `src/common` and the C++ standard library | any the host links (meshoptimizer, cgltf, stb) |
| Approved | per system version | per asset, by looking at the result |

Transforms produce **baked artifacts**. Only those cross into the game, and only through a channel designed for the runtime system that consumes them. For static things, that future static system will read the stamped levels of detail; its channel will be designed when that system is.

## The scratch pad

Assets in progress live in a flat scratch pad. Each item is one directory, and `_` in its name acts as a path separator, so the disk stays flat while the workspace shows a tree. The same convention applies to every name in the workspace (systems, channels, conformance shapes such as `prop_small` and `prop_tiny`, fixed assets):

```
meshy/garden_gnome_concept/   ->  meshy / garden / gnome / concept
meshy/garden_gnome_mesh/      ->  meshy / garden / gnome / mesh
meshy/oak_chest_concept/      ->  meshy / oak / chest / concept
```

Meshy is the first area of the scratch pad. A `<name>_concept` is concept art (an image and its provenance); a `<name>_mesh` is the 3D model Meshy made from that concept, plus everything the foundry does to it. Each item's `item.json` holds its state, notes, provenance, transforms, and results.

## The mesh item

A `<name>_mesh` goes through a chain of transforms, each visible in the workspace before anything is approved:

1. **Import at full fidelity.** Meshy's GLB, with colors taken from its texture, nothing reduced.
2. **Sizing.** Operations recorded in the item and replayed from the import: scale, rotate, translate, ground (lowest point to y = 0), center (the footprint's smallest enclosing circle to the origin), height, and fit (center, ground, and scale into a conformance shape). A hull estimator reports where the mesh stands: its size, whether it's grounded and centred, its footprint radius, its volume, and how it compares to each conformance shape.
3. **Progressive mesh.** Volume-preserving edge collapses (Lindstrom-Turk placement on quadric error), so every level of detail keeps the full mesh's volume. The viewport can show any level.
4. **Recommended levels of detail.** The full mesh and candidate levels are rendered off screen from 14 directions. A level's error is the share of pixels that change, relative to the full mesh's object pixels: a pixel counts fully if it switches between object and background, and partly if it stays object but changes color. The foundry recommends the coarsest levels under 1% (LOD0), 5% (LOD1), and 20% (LOD2). Those are defaults, good seeds: each mesh item sets its own limits, and those are what the human tunes. The limits always stack, each at least 0.1% above the one before (within 0.1% to 60%). Changing one pushes the others out of its way (later ones up, earlier ones down), and every LOD whose limit or level changed is searched again, rebaked, and re-benchmarked; the rest stay as they are. The levels are searched together, so each LOD is never finer than the one before it.
5. **Switch distances.** Each LOD's error is profiled by distance with the game camera (`style.md` section 3: 0.9 rad vertical field of view, 1080 lines). Each view is rendered at 512 pixels and box-filtered down to the size the object covers at each distance, like antialiasing and mipmapping, so differences smaller than a pixel average out as they do on screen. A pixel **disagrees** when its coverage and color differ from the full mesh's by more than a quarter. A LOD's **switch distance** is the first distance, going outward, at which fewer than the item's **switch threshold** of the object's pixels disagree: no effective quality loss. The threshold defaults to 10%, and each mesh item can set its own (0.1% to 50%) with a slider; changing it recomputes only the switch distances, not the LODs or their benchmarks. The sampled curve brackets it, and bisection with direct supersampled renders refines it. LOD0, the closest level shipped, starts at 0. The human sets each LOD's error limit for up-close viewing, and the switch distances follow from it. In the LOD table, View shows a LOD in the viewport, and Zoom shows it from its switch distance with the game's field of view, to check it by eye.
6. **Performance.** Each recommended level is reordered for the GPU's vertex cache (meshoptimizer) and benchmarked on the GPU: instances until below 60 FPS. The graph's instance axis starts where the first LOD drops to 1,000 FPS; below that the GPU is idle and the curves are flat.

**Switching in the game.** The stamped manifest records each LOD's switch distance (for the reference camera) and the object's height on screen there (which holds for any resolution or field of view). The game multiplies switch distances by a quality factor from its settings: above 1 keeps detail longer, below 1 trades quality for performance. With the gap between LODs (the gnome's LOD2 draws over 60 times as many instances as LOD0), that one factor gives a wide, predictable range between quality and performance.

**Approval stamps the result.** When the human approves a mesh item, its three levels of detail are stamped out into `fixed/<name>/`, each as a vertex-cache-optimized triangle list, with the stats and performance budget in the manifest. Those stamped files are what the future static system will load. The stamped asset is final; changing it means approving a changed mesh item.

## Fracture

A fixed asset can break. Breaking it in the game doesn't simulate anything: the runtime swaps the intact asset for its fracture, the asset's LOD1 cut into pieces, and plays the break baked into it. LOD1 is the baseline because pieces are seen briefly, in motion, and from where the asset already stood, so LOD0's detail is wasted on them and LOD2 cuts visibly coarse. The fracture is its own item, `fracture:<name>`, beside the final asset in `fixed/<name>/` (`fracture.bin` and `fracture.json`); the asset stays final, and the human approves the fracture on its own.

**The algorithms.** Every algorithm is a Voronoi diagram; they differ in where the seeds go and how distance is measured, so each suits a material:

| Algorithm | Seeds | Suits |
|---|---|---|
| `voronoi` | Spread through the volume | Plaster, clay: an even, generic break |
| `impact` | Packed toward the impact point | Stone, statues, anything struck: shards at the blow, big chunks away from it |
| `radial` | A ring around the blow's axis at one depth, and a small inner ring | Ceramic, glass, hollow things: wedges radiating from the blow, running through along it |
| `slab` | Stacked up the height, slightly off-centre | Layered stone, slate, brick: tilted layers |
| `splinter` | Spread through the volume, with distance along the longest axis counting a twelfth | Wood, bone: long pieces along the grain |

The foundry recommends one per asset: from its provenance prompt first ("vase" or "ceramic" is radial, "stone" or "statue" is impact, "wood" is splinter, "slate" is slab), then its shape (thin walls for its size are radial, long and thin is splinter, anything else impact). The piece count it recommends grows with the asset's size, from 8 to 32.

**The cut.** Each cell keeps the part of the mesh's surface inside it (convex clipping, interpolating normals and colours). Where two cells meet, the mesh's cross-section on their shared plane is traced into loops, triangulated with its holes (the inside of a vase cut across is a ring), and clipped to the shared face; both pieces get it, facing opposite ways, so they meet exactly. Cut faces take the surface's mean colour, darkened, or a colour the fracture names. Crossings are computed the same way from either side of an edge, so the surface and the caps share their seams. Where the mesh has a gap, a cross-section doesn't close; it's closed with a straight edge and counted (`open_sections`), so a gap shows as a flat patch rather than a hole through a piece. The pieces' volumes add up to the mesh's: on the gnome and the gargoyle, every algorithm keeps the volume to the last printed digit (0.013233 m3 and 0.19758 m3).

**The break.** A small rigid-body simulation, baked at 30 frames a second for 3 seconds by default. The blow sends each piece out from the impact point, along the blow, and up, falling off with distance from the impact and scaled by the asset's size (small things scatter less, big ones no farther than one about 0.8 m across). Pieces fall under gravity onto the ground (y = 0) through impulses at their hull points, with friction and some rolling resistance; they push each other apart as spheres of their own volume, never closer than they sat at rest, and they settle when slow for 0.4 seconds. The fracture stores each piece's rest centre and, per frame, its position and rotation, so playing it is a lookup and a blend between two frames: a skinned mesh with one bone per piece, the same thing the runtime already draws for trees.

**In the foundry.** A fixed asset's inspector has a Fracture section: pick an algorithm (each with its description), the piece count, and a seed, preview the cut (pieces pushed apart and tinted, with their stats), then generate. The fracture item plays its break in the viewport, loops it, scrubs it, and shows the pieces apart. The agent does the same through `fracture_preview` (every algorithm side by side, with the recommendation), `fracture_generate`, and `fracture_view`.

Measured on this machine: the gnome's LOD1 (1,452 triangles) cuts and bakes into 19 pieces in 17 ms and settles at 1.7 s; the gargoyle's (7,628 triangles) into 31 pieces in 86 ms, settling at 2.2 s.

**Not yet.** Pieces collide with each other only as spheres, so they can overlap briefly while they fall; pieces don't break further when they land; and the game API has no fixed assets yet, so the swap and playback in the runtime wait for the static system.

## Triangle strips: measured and dropped

Strips were the standard way to save memory and vertex work when GPUs had tiny post-transform caches (the NvTriStrip era). Modern GPUs reuse transformed vertices well, so an indexed triangle list reordered for that cache (meshoptimizer's `optimizeVertexCache`) is the standard choice, and the foundry uses lists only.

The foundry measured strips before dropping them. The garden gnome's LOD0 on a Radeon 8060S: Meshy's models split almost every vertex along UV seams (8533 vertices for 4131 positions), and a strip can only continue across a shared index, so the 7350 triangles became 3335 separate strips averaging 2.2 triangles. SDL3's GPU API turns primitive restart off, so joining them took 8239 degenerate triangles, 53% of everything the GPU assembled (2.12 times the list's primitives). The benchmark is limited by primitive throughput, so the strip drew about half as many instances at 60 FPS (11.8k vs. 23k), despite using 29% fewer indices.

## Open questions

1. **Approval granularity.** Approving a mesh item approves its sizing, its recommended LODs, and its budget together. Should LODs be approvable separately (accept the mesh but ask for a different LOD1)?
2. **The color term's weight.** A pixel that keeps the same color counts 0, and one whose color moves by a quarter of the full color range or more counts 1, the same as a silhouette change. Is a quarter the right point?
3. **Textures.** Colors are per vertex (sampled from Meshy's texture), so coarse levels blur color regions. When the static system arrives, should stamped assets carry a texture (or palette regions, per `style.md`) instead, which would change the color term?
4. **The static system's channel.** The stamped levels use the foundry's own mesh format for now. The future static system will define the channel it reads; should the stamp step then convert into that channel, or should the format be designed now?
