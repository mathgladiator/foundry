# Flow

This document describes how packages are laid out and how data flows between them. It is a first draft, based on the vocabulary in `spine.md`: systems are black boxes, channels are the boundaries between them, and pipelines compose systems.

## Package layout

| Path | What it is |
|---|---|
| `systems/$name_$version/*` | One system: an independent black box in its own namespace. Each version is a separate directory, so forking a system means creating a new directory. |
| `channels/$channel.h` | A channel: the structure that crosses a boundary. Systems are free to share channels. |
| `packer/$channel_boundary.{h,cpp}` | Generated code that packs the structure in `channels/$channel.h` into a `byte[]` and unpacks it again. Used only in the WebAssembly development build. |
| `disk/$disk.{h,cpp}` | A disk format: how a channel is written to and read from a file. |

## Systems

A system has one function: one channel in, one channel out. It lives in its own namespace (derived from `$name_$version`), which lets the native game build hold several versions of the same system side by side. A system never reads a clock or any other hidden input; everything it needs arrives through its input channel, including time (see "State and time" in `spine.md`).

## Channels

Channels are the contract between systems, so they change rarely. The agent can create a channel, or upgrade one, but:

- it needs the user's consent and a good written reason, and
- the host must restart to pick it up, which can invalidate prior builds.

The host, not the agent, generates the packer for every channel. That generated code is how boundaries are enforced.

## Packers: development only

In the WebAssembly development build, each channel crosses the boundary as a byte array, using its generated packer. In the native game build, the packers are not needed: systems are merged and pass channels as direct structures.

## Disk formats

Some channels need to live on disk. A static mesh, for example, is generated once in the foundry (from Meshy output, resized by the agent) and loaded by the game.

Disk formats are designed for fast loading, in the style of Cap'n Proto: the file is the structure's bytes laid out as they are in memory, so loading is close to a straight copy with no parsing step. That requires handling byte order (endianness) on platforms where it differs. Formats should favor layouts that can be uploaded directly to the GPU.

## Open questions

1. **Endianness.** Do we pick one byte order for files (little-endian, which covers almost every current target) and swap on load where needed, or store the byte order in the file header?

For files, pick one that is most common for modern machines AMD64 that players have.

2. **Alignment and versioning of disk formats.** Is a disk format tied to one channel version, so a channel upgrade means a new disk format? Does each file carry a header with its format version so stale files are detected rather than misread?

The disk format should have a header and it is strongly fixed to the channel type, and channel types become versioned things as immutable artifacts.

3. **Pipelines in the layout.** Systems, channels, packers, and disk formats each have a directory. Where does a pipeline definition live (for example, `pipelines/$name_$version`), and what is it: data, generated code, or a system of its own?

I think each pipeline has is a JSON file that illustrates the connections of the systems, and they are what the tool shows in the workspace tool that the user sees for approval. Each system has a JSON manifest as well with metadata about approval and notes. This way, I can have the system get notes in bulk rather than piece mealing it. I see the tool mirroring the directory structure of pipelines, systems, fixed assets, and other useful inputs that humans want to poke at or approve. This can also include concept art as well. So, we have meshy/concepts/*.png along with a JSON describing the prompt and the cost.
