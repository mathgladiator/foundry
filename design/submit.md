# Submit: the engine for dynamic content

This document describes the engine that runs pipeline instances: how they update, how they sleep, and how they get drawn. The pipeline instance is the core unit of the engine. The foundry uses this engine for play testing, and the goal is for the main game to adopt it, along with the mega shader, once it has proven itself here.

Every system takes part in one or both of two phases each frame:

1. **Update.** Pure CPU work. Systems such as an animator, a simulator, or the instance's timeline advance state. Nothing touches the GPU.
2. **Render.** The instance's drawables are gathered into a queue, which is reorganized around the GPU and submitted.

Because the output of the update phase ends up on the GPU, channels have to be designed with the GPU in mind: their layouts should be ready to upload with little or no conversion, the same principle `flow.md` applies to disk formats.

## The instance

Every pipeline instance carries a small default state, alongside its pipeline-specific parameters:

| Field | Meaning | Who sets it |
|---|---|---|
| `timeline` | The instance's personal clock. Systems read time from here, never from a global clock (`spine.md`, "State and time"). | The engine advances it while the instance is awake. Time does not move while the instance sleeps. |
| `drawable` | Whether the instance should be drawn at all. This is a macro-level concern: the game decides what is in the world. | The game. |
| `wake_up` | A flag meaning "an external signal arrived; run me again". | The game, or another instance (such as a lever opening a door). |
| `sleeping` | Whether the instance is excluded from the update phase. | The engine, as a side effect of the systems' votes (below), or the game. |
| `transform` | Where the instance is in the world. | The game. |

## The update phase

### The active set

The update phase only runs instances in the **active set**: the instances that are awake. Most of a world is asleep at any moment: closed chests, finished animations, distant trees. Keeping them out of the update phase is the main source of CPU savings.

- **Falling asleep.** At the end of its update, each system in the instance votes on whether it still needs to run. When every system votes no, the instance goes to sleep. A closed chest's animation has reached its end, so it sleeps until someone interacts with it. The game can also put an instance to sleep according to its own rules, for example when every player is too far away for it to matter. "Too far" is one setting from the quality sliders, shared with the culler below.
- **Waking up.** A sleeping instance never wakes itself. It needs an explicit external signal: the game sets `wake_up` (a player opens the chest, the wind changes, a player walks into range), and the instance joins the active set on the next frame.

Sleeping does not mean invisible. A sleeping chest is still drawn; its last render output is simply reused (see "Retained drawables" below). Its `timeline` stops while it sleeps and resumes from the same moment when it wakes.

### Broadcasting updates

The update phase is organized around systems, not instances. Rather than walking each instance through its whole pipeline, the engine takes each system and broadcasts the update to every active instance that uses it, in one batch. This keeps each system's code and data hot in the CPU cache, and because systems are pure functions from channel to channel, batches can be split across threads without locks.

The order of systems follows the pipeline's DAG (`spine.md`), so a system always runs after the systems that feed it.

### What update produces

Besides its new state, an instance's update can produce:

- **Render changes:** a new pose, new mesh, or new parameters for the render phase.
- **Sound events:** with a position, played by the audio layer (`sounds.md`).
- **Sensor signals:** for the game (`systems.md`, "Sensors and head rings").
- **Sleep votes:** as described above.

## The render phase

### Drawables

A drawable is one thing to draw: a mesh, a material (a set of mega shader parameters), a transform, per-instance data (palette row, bone offset, aura strength), and a pass (opaque, transparent, and so on). One instance can produce several drawables: a humanoid's body, its armor pieces, its aura shell, and a particle effect on its head ring.

### Retained drawables

Drawables are retained, not rebuilt every frame. An instance re-emits its drawables only when its update produced render changes. Sleeping instances, and awake instances whose output didn't change, keep last frame's drawables as they are. In a mostly sleeping world, the render phase does very little work per instance.

### The submit queue

Each frame, the submit queue turns the retained drawables into GPU work:

1. **Cull.** Drop drawables that can't be seen (see "Culling" below).
2. **Key.** Give each drawable a sort key: a single 64-bit integer packing, from most to least significant, the pass, the graphics pipeline (shader variant plus blend and raster state), the material and texture array, the mesh, and depth.
3. **Sort.** Sort by key. Opaque drawables sort front to back (so the GPU can skip hidden pixels); transparent ones sort back to front.
4. **Batch.** Merge consecutive drawables with the same pipeline, material, and mesh into one instanced draw.
5. **Submit.** Upload the frame's per-instance data, then record the draws, changing GPU state only when the key changes.

### Culling

The culler drops a drawable if any of these is true:

- **Outside the view.** Its instance's conformance shell (`spine.md`, Layer 1), used as a bounding volume, is outside the camera's view.
- **Too far.** It is beyond the "too far" distance from the quality sliders. This is the same setting the game uses to put distant instances to sleep.
- **Too small.** Its projected size on screen is below a threshold, with three settings: 10 pixels (low quality), 7 pixels (medium), and 4 pixels (high).

To compute projected size, the culler needs the projection: the viewport height in pixels and the camera's vertical field of view. For a bounding sphere of radius `r` at distance `d`, the projected height is about `viewport_height × r / (d × tan(fov / 2))` pixels. The engine pushes these values to the culler whenever the window or camera changes.

## Platform target

The engine uses **SDL3's GPU API**, which runs on Vulkan, Metal, and Direct3D 12. That covers Windows, macOS, and Linux, without depending on OpenGL (which macOS has deprecated). The web is not a target. We stick to what the API guarantees on every backend.

| We get | Notes |
|---|---|
| Instanced draws with a base vertex and first instance | Meshes can share buffers without rebasing indices |
| Indirect draws, including several draws from one buffer | Lets a compute shader decide what to draw later |
| Compute shaders | For particles, skinning, and culling on the GPU |
| Storage buffers readable from vertex and fragment shaders | Per-instance data and bone matrices without textures |
| Texture arrays, cube maps, and 3D textures | Palettes, skins, and shadow maps |
| Depth textures with comparison samplers | Hardware shadow map filtering |
| Push-style uniforms per draw | Small per-draw values without managing uniform buffers |
| Transfer buffers with "cycling" | Streaming uploads without stalling the GPU |

What we don't get: geometry and tessellation shaders, and anything specific to one backend (such as ray tracing). Graphics pipelines are immutable objects that bake in the shaders, vertex layout, blend state, and render target formats, so a "shader variant" in this document means a pipeline object.

Each backend needs its own shader format (SPIR-V for Vulkan, MSL for Metal, DXIL for Direct3D 12). We write shaders once, in HLSL, and compile them for every backend with SDL's `SDL_shadercross` tool, at build time.

## The mega shader

The renderer uses one mega shader (an "uber shader") that implements the look in `style.md` §6: cel bands with a soft terminator, warm light and cool saturated shadow, rim light, specular only for metal and wet materials, ink, and cast shadows (below).

- **Few pipelines, many parameters.** Build a small number of pipeline objects, only for things that change the vertex layout or blend state: static vs. skinned, and opaque vs. cutout vs. transparent vs. additive. Everything else in `style.md` §6 is a uniform or per-instance value, so changing it doesn't create a new pipeline or break a batch.
- **Ink as a second pass.** The inverted-hull ink in `style.md` §6 is the same batch drawn again, with front faces culled and vertices pushed out along their normals. It reuses the same buffers.
- **A shader explorer.** Because the shader's parameters are just values, the foundry can expose all of them live on the control pad (`playtest.md`). That makes the foundry the place to explore and tune the shader before the main game adopts it.
- **Shared with the game.** The runtime exports the shader, lets the game configure it through defines, and caches compiled versions on disk (`api.md`, "The shader").
- **The look is opt-in.** The mega shader implements `style.md` because that is the current project's style. Another project can swap in its own style values, or its own shader.

## Rendering many meshes efficiently

On modern GPUs, the number of triangles is rarely the bottleneck. The costs are draw calls, state changes, and memory traffic. Everything below is aimed at drawing many different dynamic meshes with as few state changes as possible.

- **Shared buffers.** Store meshes in a few large vertex and index buffers, one per vertex format, instead of one buffer per mesh. Each draw names its mesh by index offset and base vertex.
- **Palettes instead of textures.** When a project uses flat palettes (`style.md` §5), a material can be a palette row index instead of a texture, so most assets share one material and batch together. Real textures (skins, bark when a project wants it) go into texture arrays, so switching between them is an index, not a bind.
- **Instancing through a storage buffer.** Per-instance data (a 3×4 transform, palette row, parameters, bone offset) goes into one storage buffer, streamed every frame. The shader reads its entry using the instance index. Many trees, chests, or soldiers then become a single draw each.
- **GPU skinning for crowds.** The update phase samples animations on the CPU and writes bone matrices into one storage buffer. Each instance has an offset into it, so skinned characters can be instanced too. Sleeping instances keep their last pose, so their bones aren't re-uploaded.
- **Streaming without stalls.** Upload per-frame data through transfer buffers mapped with cycling turned on, so the CPU writes into a fresh buffer while the GPU still reads last frame's.
- **Mesh optimization.** Use indexed triangle lists, reordered for the GPU's vertex cache, overdraw, and vertex fetch (the `meshoptimizer` library does all three). Triangle strips rarely beat an optimized indexed list on modern GPUs and make batching harder, which is why the shared library stage in `spine.md` is a mesh optimizer rather than a stripifier.
- **Level of detail.** LOD meshes are just more meshes in the shared buffers; the sort key's mesh field changes, nothing else does.
- **Later: GPU-driven culling.** Once the CPU culler becomes a bottleneck, a compute shader can cull instances and write indirect draw commands, so the CPU no longer touches each instance per frame. This is an optimization, not a starting point.

## Shadows

Cast shadows are rendered with shadow maps and applied by the mega shader. `style.md` describes how they look; this section covers how they are computed.

A shadow map works the way it sounds: each shadow-casting light does a small off-screen render from its own point of view, keeping only depth. The mega shader then compares each pixel's distance from the light against that depth buffer to decide whether the pixel is in shadow. This is still the standard real-time technique, and it is fast because the light's render is depth only, at low resolution, and reuses the shared buffers and instancing.

### Lights

Each light carries a marker saying whether it casts shadows, and in what form:

| Form | Example | Shadow map | Look |
|---|---|---|---|
| Infinite (directional) | the sun | one map, optionally split into distance bands (cascades) so nearby shadows stay sharp | crisp |
| Hemisphere | a torch on a wall | covers only the half-space in front of the wall, so it needs fewer renders than a full sphere | crisp |
| Sphere | a lantern hanging in a room | a cube map (six renders) | softer, by filtering the map more widely |

### Budget

- **Lighting:** each object is lit by up to its **4 closest lights**.
- **Shadows:** shadow maps are only rendered for marked lights close to the player, up to **3** at a time. The sun counts as one when it is present. Lights farther away still light things, but without cast shadows.

### Shadow casters

What a light renders into its shadow map depends on the light's distance from the caster and on occlusion:

- **Far or small:** the caster's conformance shape stands in for it. A capsule's shadow is enough for a distant character.
- **Close:** a rough version of the mesh (a low LOD) is used instead, so the shadow has the right silhouette.

Either way, the shadow pass only needs the animated shape and the mesh: it is a pure geometry bus, with no materials or textures involved.

### Validation

Shadow maps fail in well-known ways: acne (speckled self-shadowing), peter-panning (shadows detached from their object), and blurry or blocky edges. Play testing (`playtest.md`) needs a shadow test scene with known geometry and light positions, plus a debug view of each shadow map, so the agent and the human can check them.

## Transparency

Magic effects, auras, and other layers need transparency. Each frame draws in passes:

| Order | Pass | Sorting | Depth write | Notes |
|---|---|---|---|---|
| 0 | Shadow | none needed | on | depth only, once per shadow-casting light (up to 3) |
| 1 | Opaque | front to back | on | most of the world |
| 2 | Cutout | front to back | on | alpha-tested leaf cards and fronds |
| 3 | Ink | follows opaque | on | inverted hull; skipped for elemental and card materials (`style.md` §6) |
| 4 | Transparent | back to front, per instance | off | auras, shields, smoke, water surfaces |
| 5 | Additive | none needed | off | glows, sparks, energy; additive blending gives the same result in any order |

- **Prefer additive** for glows and energy effects: it needs no sorting, so it batches freely.
- **Use premultiplied alpha** for everything else. With premultiplied alpha, one blend mode (`ONE, ONE_MINUS_SRC_ALPHA`) covers both normal transparency and additive effects (a color with zero alpha adds), so passes 4 and 5 can share batches if needed.
- **Sort per instance, not per triangle.** Back-to-front sorting is by instance depth. Overlapping transparent instances can still show minor ordering errors; that is accepted.
- **Aura shells** (`systems.md`) are drawn with depth test on and depth write off, back faces first and then front faces, so the shell looks solid from every side. A fresnel term (stronger at the silhouette) makes the aura read as a glow around the body instead of a tinted coat.
- **If ordering errors become a problem,** weighted blended order-independent transparency (McGuire and Bavoil, 2013) avoids sorting entirely, using two extra render targets.

## Particles

Magic motion is a function of time and parameters (`systems.md`, Magic), and that maps directly onto the GPU:

- **Analytic particles.** For most effects, a particle's position is computed in the vertex shader from its seed, the effect's parameters, and the instance's `timeline`. The CPU uploads nothing per particle, so a rain cloud with thousands of drops costs one instanced draw and no update work beyond advancing time. Because a sleeping instance's timeline stops, its particles freeze too.
- **Stateful particles.** Things that really simulate, such as break pieces falling and bouncing, run on the CPU in the update phase, or in a compute shader when there are many of them.

## One frame

1. The game applies its signals: sets `wake_up` flags, changes parameters, moves instances.
2. Woken instances join the active set, and their timelines resume.
3. Update: each system broadcasts across the active instances that use it, in DAG order.
4. Instances whose systems all voted to sleep leave the active set, and their timelines stop.
5. Sound events and sensor signals go out to the audio layer and the game.
6. Instances with render changes re-emit their drawables.
7. Submit: cull, key, sort, batch, upload, then draw pass by pass, starting with shadows.
