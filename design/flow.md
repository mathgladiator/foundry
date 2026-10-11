# Flow

This document describes how packages are laid out and how data flows between them (the source tree itself is organized in `ORG.md`). Paths under `src/` are in the source tree; the others are in the workspace root (`demo/` while developing the foundry), using the vocabulary in `spine.md`: systems are black boxes, channels are the boundaries between them, and pipelines compose systems.

## Package layout

| Path | What it is |
|---|---|
| `pipelines/$name_$version.json` | A pipeline: the DAG of systems it connects. |
| `src/systems/$name_$version/*` | One system: an independent black box in its own namespace, plus its JSON manifest. Each version is a separate directory, so forking a system means creating a new directory. |
| `src/channels/$channel_$version.json` | A channel's schema: the structure that crosses a boundary, written as JSON rather than C++ so it's easy to diff, approve, and generate from. `tools/gen_channels.py` generates everything under `src/generated/` from it. Channels are versioned, immutable artifacts. Systems are free to share channels. Its approval state lives beside it in `$channel_$version.manifest.json`. |
| `src/generated/channels/$channel_$version.h` | The C++ structure generated from the schema. |
| `src/generated/packer/$channel_boundary.{h,cpp}` | Generated code that packs a channel's structure into a `byte[]` and unpacks it again. Used only in the WebAssembly development build. |
| `src/generated/disk/$disk.{h,cpp}` | Generated code for a disk format: how a channel is written to and read from a file. |
| `src/generated/json/$channel_$version.{h,cpp}` | Generated conversion between a channel and JSON, so the host's MCP tools can run systems with JSON input and show their output. |
| `meshy/$name_concept/*` | Meshy concept art: `image.png` and `item.json` (state, notes, provenance). `_` in names acts as a path separator in the workspace tree, so the disk stays flat (`design/transforms.md`). |
| `meshy/$name_mesh/*` | A Meshy mesh item: `model.glb` (Meshy's model), `thumbnail.png`, and `item.json` (state, notes, provenance, sizing ops, fit, stats, recommended LODs, performance). `model.glb` is committed directly, whatever its size (Meshy deletes results after 3 days, and they cost credits to remake). Working files (`import.bin`, `full.bin`, `progressive.bin`, `rec_*.bin`) are regenerated and not committed. |
| `meshy/ledger.json` | Every Meshy charge, checked against the credit budget in the workspace's `foundry.json`. |
| `foundry.json` | The workspace's configuration: MCP address, Meshy keys file, and credit budget. |
| `fixed/$name/*` | A stamped fixed asset, written when its mesh item is approved: `lod0.bin`, `lod1.bin`, `lod2.bin` (vertex-cache-optimized triangle lists), `thumbnail.png`, and a final `manifest.json` (provenance, sizing ops, fit, stats, LODs with their errors, and the performance budget). This is what the future static system loads. |
| `conformance/$name.json` | A conformance shape that assets are sized against, such as `prop_small` or `hero`. |
| `sounds/$pipeline/*` | A pipeline's sound table and audio files (`sounds.md`). |

## The workspace

The workspace tool, which the user sees, mirrors this directory structure: pipelines, systems, fixed assets, concept art, and any other inputs a human wants to inspect or approve. Approval happens there.

## Pipelines

A pipeline is a JSON file describing the connections between its systems: which system feeds which, through which channel. The workspace tool draws it as a graph, and that graph is what the user approves.

## Systems

A system has one function: one channel in, one channel out. It lives in its own namespace (derived from `$name_$version`), which lets the native game build hold several versions of the same system side by side. A system never reads a clock or any other hidden input; everything it needs arrives through its input channel, including time (see "State and time" in `spine.md`).

### Manifests

Each system has a JSON manifest with metadata: its approval state, who approved it and when, and the notes left on it. Because notes live in manifests, the user can review and annotate many systems in one sitting, and the agent can pick up all the notes in bulk rather than one at a time.

## Channels

Channels are the contract between systems, so they change rarely. Each channel version is an immutable artifact. The agent can create a channel, or propose an upgrade, but:

- an upgrade goes through an approval workflow, where the user sees the change and a good written reason;
- approving it invalidates everything that depends on the old channel, which must be forked onto the new one; and
- the host must restart to pick it up.

The host, not the agent, generates the packer for every channel. That generated code is how boundaries are enforced.

## Packers: development only

In the WebAssembly development build, each channel crosses the boundary as a byte array, using its generated packer. In the native game build, the packers are not needed: systems are merged and pass channels as direct structures.

## Disk formats

Some channels need to live on disk. A static mesh, for example, is generated once in the foundry (from Meshy output, resized by the agent) and loaded by the game.

Disk formats are designed for fast loading, in the style of Cap'n Proto: the file is the structure's bytes laid out as they are in memory, so loading is close to a straight copy with no parsing step. Formats should favor layouts that can be uploaded directly to the GPU (`submit.md`).

- **Byte order.** Files are little-endian, matching the AMD64 machines most players have.
- **Header.** Every file starts with a header naming its channel type and version. A disk format is strongly tied to one channel version, so a stale file is detected and rejected rather than misread.

## Fixed assets

A fixed asset is a baked file, such as a resized Meshy mesh, that a pipeline can summon by name. A small-object static pipeline, for example, draws from a library of fixed assets.

Fixed assets are indexed by conformance size, in both directions:

- **Asset to sizes.** Select a fixed asset, see it, and see every conformance size it fits within.
- **Size to assets.** Select a conformance size and see every fixed asset that fits within it.

The host maintains this index (recomputed whenever an asset or conformance size is added), and the workspace uses it for browsing and approval.
