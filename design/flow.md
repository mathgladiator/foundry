# Flow

This document describes how packages are laid out and how data flows between them (the source tree itself is organized in `ORG.md`). Paths under `src/` are in the source tree; the others are in the workspace root (`demo/` while developing the foundry), using the vocabulary in `spine.md`: systems are black boxes, channels are the boundaries between them, and pipelines compose systems.

## Package layout

| Path | What it is |
|---|---|
| `pipelines/$name_$version.json` | A pipeline: the DAG of systems it connects. |
| `config/$table/` | A config table, such as `tree_v1_genomes`: named records of one channel that a pipeline picks from by name (see "Config"). `table.json` names the channel and how the editor shows it; each `$entry.json` (`oak.json`) holds one record's `value` with its state and notes. Entries are approved like any other item. |
| `src/systems/$name_$version/*` | One system: an independent black box in its own namespace, plus its JSON manifest. Each version is a separate directory, so forking a system means creating a new directory. |
| `src/channels/$family_v$version_$feature.json` | A channel's schema (`tree_v1_rig`; see "Naming" under "Channels"): the structure that crosses a boundary, written as JSON rather than C++ so it's easy to diff, approve, and generate from. `tools/gen_channels.py` generates everything under `src/generated/` from it. Channels are versioned, immutable artifacts. Systems are free to share channels. Its approval state lives beside it in `$family_v$version_$feature.manifest.json`. |
| `src/generated/channels/$channel_$version.h` | The C++ structure generated from the schema. |
| `src/generated/packer/$channel_boundary.{h,cpp}` | Generated code that packs a channel's structure into a `byte[]` and unpacks it again. Used only in the WebAssembly development build. |
| `src/generated/disk/$disk.{h,cpp}` | Generated code for a disk format: how a channel is written to and read from a file. |
| `src/generated/json/$channel_$version.{h,cpp}` | Generated conversion between a channel and JSON, so the host's MCP tools can run systems with JSON input and show their output. |
| `meshy/$name_concept/*` | Meshy concept art: `image.png` and `item.json` (state, notes, provenance). `_` in names acts as a path separator in the workspace tree, so the disk stays flat (`design/transforms.md`). |
| `meshy/$name_mesh/*` | A Meshy mesh item: `model.glb` (Meshy's model), `thumbnail.png`, and `item.json` (state, notes, provenance, sizing ops, fit, stats, recommended LODs, performance). `model.glb` is committed directly, whatever its size (Meshy deletes results after 3 days, and they cost credits to remake). Working files (`import.bin`, `full.bin`, `progressive.bin`, `rec_*.bin`) are regenerated and not committed. |
| `meshy/ledger.json` | Every Meshy charge, checked against the credit budget in the workspace's `foundry.json`. |
| `foundry.json` | The workspace's configuration: MCP address, Meshy keys file, and credit budget. |
| `fixed/$name/*` | A stamped fixed asset, written when its mesh item is approved: `lod0.bin`, `lod1.bin`, `lod2.bin` (vertex-cache-optimized triangle lists), `thumbnail.png`, and a final `manifest.json` (provenance, sizing ops, fit, stats, LODs with their errors, and the performance budget). This is what the future static system loads. |
| `reference/$topic/$subject/` | Reference material: photos of real things that assets are compared against, such as `reference/trees/pine/01.jpg`. Each photo has a JSON sidecar with its `file`, title, and provenance (source page, original, license, author, retrieval date). Only public-domain or CC0 photos go here, so none needs attribution. Each is a workspace item (`reference:trees_pine_01`) for viewing; reference material isn't approved (see "Reference"). |
| `fixed/$name/fracture.bin`, `fracture.json` | A fixed asset's fracture: its LOD1 cut into pieces with the break baked (`transforms.md`, "Fracture"), and its own manifest (state, settings, stats, notes). It is its own item (`fracture:$name`), approved separately from the final asset. |
| `conformance/$name.json` | A conformance shape that assets are sized against, such as `prop_small` or `hero`, with its footprint on the hex grid (`single` for all of today's; `spine.md`, "Layer 1"). |
| `plotters/$name.json` | A plotter: a plot system bound to a biome config (its kinds, spacings, densities, and fields), with its shared properties and reference photos. A workspace primitive beside pipelines, approved like one (`plotter.md`). |
| `sounds/$pipeline/*` | A pipeline's sound table and audio files (`sounds.md`). |
| `qa/videos/$pipeline-$timestamp.mp4` | A QA video of every variation of a pipeline, with its index beside it (`.mp4.json`); regenerated, not committed (`quality.perf.md`, "The QA video"). |
| `perf/report-$pipeline-$timestamp.json` | A stress run's report, committed, so `perf/` is the performance timeline of every pipeline, draft and final. Preset plans live in `perf/plans/`; journals and raw samples in `perf/runs/`, not committed (`quality.perf.md`). |

## The workspace

The workspace tool, which the user sees, mirrors this directory structure: pipelines, systems, fixed assets, concept art, and any other inputs a human wants to inspect or approve. Approval happens there.

## Pipelines

A pipeline is a JSON file describing the connections between its systems: which system feeds which, through which channel. The workspace tool draws it as a graph, and that graph is what the user approves.

It has three parts (`pipelines/tree_v1.json` is the first):

- **`params`**: what the game provides: enums (age, season), config entries (picked by name from a table in `config/`; `"type": "config", "table": "tree_v1_genomes"`), booleans (`"type": "bool"`, such as a tree's `fruit`, which the game clears when the fruit is picked; tools enumerate them like an enum of false and true), gated parameters (`"when": {"param": "species", "field": "pickable"}`: the parameter applies only where the chosen config entry sets that bool field; elsewhere every stage reads it as off (false, the first value, or the minimum), so its builds are shared, the workspace disables its control, `pipeline_flow` counts it once there, the QA video gives it one clip labelled disabled, and the generated API adds `foundry_tree_v1_fruit_applies(species)` so the game can ask before offering the action), numbers with ranges, the instance's `time`, and events. `time` runs from the instance's timeline, never a clock. An `event` (`break_time`) is a clock that stays negative until the game starts it; the generated API gives it a trigger named without `_time` (`foundry_tree_v1_start_break`), and the stress tool's events phase fires it in bulk (`quality.perf.md`).
- **`stages`**: each runs one system. Its input is either an upstream stage's output passed straight through (`"@rig"`), or an object binding the input channel's fields to parameters (`"$age"`), upstream fields (`"@rig.bones"`), or literals. A stage with `"phase": "update"` runs every frame (a simulator); the rest are built and cached.
- **`draw`**: which stage's output the viewport draws, which stage poses it, optionally which model fields size the conformance capsule (`"conformance": {"radius": "stump_radius", "height": "crown_base"}`), and which give the footprint at the largest age (`"footprint": {"radius": "footprint_radius", "cells": "footprint_cells"}`; plotters measure it). A model may also carry `collision`, a list of `physics_v1_body` (Jolt shapes with a purpose: solid, soft, query, or sensor): the foundry checks it in a toy Jolt world and draws it over the model (the View tab's Collision toggle), and the native runtime adds it to the game's Jolt world. Drawable channels follow a convention rather than a type, so the host knows no pipeline's channels: a model has `positions` and `indices` (and may have `normals`, packed RGBA `colors`, and skinning in `bone_a`, `bone_b`, `bone_w`); a pose has `matrices` (column-major 4x4 per bone) and may have `settled`. The host decodes them through the generated JSON registry, and reports the rest of their scalar fields as facts.

The foundry caches every stage's output by system build and input, so changing the wind reruns only the simulator, and changing the season reruns only what the season reaches.

The Flow tab (and `pipeline_flow`) shows this for any pipeline. A config parameter feeds a purple lookup box of its own (`species` into `lookup config/tree_v1_genomes`), since the game passes a name and the stage receives the entry it finds; each parameter's line is labelled with the input field it binds where the names differ (`species` binds `genome`). Parameters run down the left in the pipeline's order and stages sit in columns by depth; the layout is fixed rather than optimised against crossings (a layout that moved to avoid them flickered between two arrangements). Red lines leave the stages whose output the renderer uploads, labelled with the fields they carry, for boxes in a last column that name the destination: the draw model's vertex fields to the GPU vertex buffer and its `indices` to the index buffer (uploaded once per build, shared by every instance), and the pose's `matrices` to the bone storage buffer (streamed every frame). A line from a stage before the last column runs in a lane under the stages, so it never passes behind one; `pipeline_flow` lists the same as `gpu`. The graph sits on a canvas: scroll to zoom about the mouse, drag to pan, and **Fit** shows all of it. It fits once when first shown and otherwise moves only when asked, so resizing the window leaves it still. It shows each stage's cache key (every parameter that reaches it, directly or through what it reads upstream), how many distinct outputs it can have, and what each parameter reruns. Its probe varies each parameter and compares every stage's output, so a dependency that is declared but never changes an output stands out as a place where a cache could be shared. A probe only sees what changes from the values it starts at: a parameter that matters only on another path (a tree's break heading, while it stands) shows no change.

## Config

Some data belongs to a pipeline without being code or a game parameter: a tree's species, a creature's breeds. It lives in a config table, `config/$table/`, named like the channels it feeds (`$family_v$version_$what`, so `tree_v1_genomes`). Each entry is one record of the table's channel (`tree_v1_genome`), so its fields, types, and ranges come from the channel's schema and every change is checked against it.

The table is a hidden detail of its pipeline. The game sees only the entries' names, as the values of the parameter that picks one (`FOUNDRY_TREE_V1_SPECIES_OAK`); `tools/gen_pipelines.py` compiles every entry's value into the native runtime, so nothing is read at run time.

The host knows no table. Its editor is generic: the schema gives each field's widget and range, and `table.json` configures the rest for its pipeline:

```json
{
  "channel": "tree_v1_genome",
  "doc": "Tree species for tree_v1 ...",
  "editor": {
    "groups": [{"label": "Trunk and crown", "fields": ["height", "slender", "crown_shape"]}],
    "colors": ["leaf_summer", "bark_light"]
  }
}
```

`groups` orders the fields under collapsible headings (fields it doesn't name go under "Other"), and `colors` names the `vec3` fields to edit as colours. The agent uses the same path: `config_get` reads a table, an entry, and (with `schema`) every field's range; `config_set` changes an entry or creates one `from` another, checks it, rebuilds the live instance, and regenerates the native runtime.

### Reference

An entry can link to photos of the real thing it models: its `reference` list, beside `value`, holds workspace-relative paths into `reference/` (`["reference/trees/pine/01.jpg"]`), set with `config_set` and `reference`. The list is metadata, not part of the channel, so it never reaches a system or the runtime. When the live instance uses an entry that has photos, the viewport offers a **Reference** checkbox that shows them in its corner, beside the model; clicking the photo steps to the next. `workspace_select` with `reference` does the same for the agent. The host finds the photos from the pipeline's config parameters, so like the editor it knows no table.

Photos are pulled from free sources, today Wikimedia Commons, limited to public domain and CC0. Each keeps a provenance sidecar like any other outside file, and is stored as an 800-pixel copy. The agent compares a species with its photos before calling the species done.

## Systems

A system has one function: one channel in, one channel out. It lives in its own namespace (derived from `$name_$version`), which lets the native game build hold several versions of the same system side by side. A system never reads a clock or any other hidden input; everything it needs arrives through its input channel, including time (see "State and time" in `spine.md`).

### Manifests

Each system has a JSON manifest with metadata: its approval state, who approved it and when, and the notes left on it. Because notes live in manifests, the user can review and annotate many systems in one sitting, and the agent can pick up all the notes in bulk rather than one at a time.

## Channels

Channels are the contract between systems, so they change rarely. Each channel version is an immutable artifact. A schema's field types are `u32`, `i32`, `f32`, `bool`, `string`, `vec3`, another schema by its id (a nested structure, such as `tree_v1_bone`), and arrays of any of them; a `u32` can name its values (`"enum"`), and `min` and `max` give tools a range (the config editor's sliders). The agent can create a channel, or propose an upgrade, but:

### Naming

A channel is named `$family_v$version_$feature`: the family it belongs to, its version, then what it carries (`tree_v1_genome`, `tree_v1_rig`, `tree_v1_pose`). Its schema says the three parts (`"family": "tree", "version": 1, "feature": "rig"`), and the generator derives the id, the file name, and the C++ structure (`channels::tree_v1_rig`) from them. The family comes first so a pipeline's channels sort together, and the version second so `tree_v1_*` and `tree_v2_*` read as two generations of one family. Upgrading one channel gives it the next version (`tree_v2_rig`) without renaming the rest of the family. Systems keep their own names (`tree_rig_v1`), so a system and the channel it outputs never share a name.

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
