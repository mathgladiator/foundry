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
- **Too small.** Its projected size on screen is below a threshold, with three settings: 10 pixels (low quality), 7 pixels (medium), and 4 pixels (high). Built (see "Cost" below).

To compute projected size, the culler needs the projection: the viewport height in pixels and the camera's vertical field of view. For a bounding sphere of radius `r` at distance `d`, the projected height is about `viewport_height × r / (d × tan(fov / 2))` pixels. The engine pushes these values to the culler whenever the window or camera changes.

## Cost: the runtime decides what to compute and draw

The game decides *what* is in the world and every parameter of it. The runtime decides *how much of it to compute and draw*, from what the camera sees and the quality setting (`api.md`, "Quality"). A game that fills its world from plotters can't sensibly make these choices itself: it doesn't know what is on screen, how big it looks, or what anything costs. Each decision is measured, on/off, by the plotter's scale plan (`quality.perf.md`, "Scale"), with what it costs in looks.

Every frame, before posing anything, the runtime classifies each instance: whether it is in view (its bounding sphere against the frustum, and a looser test with a margin, so what turns into view is already posed), and its **projected size**: the bounding sphere's diameter on screen, in pixels, from the viewport the game sets (`foundry_set_viewport`) and the camera's projection. Distance, not depth, so turning the camera doesn't change it. Then:

- **Detail by size.** A pipeline whose parameters include an enum `detail` (finest first) has levels of detail. The game's value is the finest it allows; the runtime draws a coarser level when the instance is small enough that the coarser level looks like the finest. Where "looks like" is measured, per pipeline, by the stress tool's detail sizes (`quality.perf.md`, "Detail sizes"), and compiled into the runtime from the pipeline's `draw.detail_pixels`: for each quality, the size below which each level gives way to the next. A level comes back only 15% above where it left (hysteresis), so nothing flickers at a threshold. A pipeline that hasn't measured its sizes always draws the game's level.
- **Coarser levels share the pose.** The instance's own build is posed; a coarser level is drawn with that pose. So the runtime uses a coarser level only once it has checked, per pair of builds, that it has the same bones moving the same way (the two poses hash equal at two moments, swaying and with every event fired). A rock's pieces fly differently at each level (their mass comes from their shape), so its levels never pass, and it keeps the game's.
- **The far level.** Below its coarsest level, every pipeline gets one more, made by the runtime: the coarsest build's mesh, its vertices clustered on a grid of 12 cells across (vertices of clearly different colours stay apart), about a third of the coarsest's triangles. A cluster keeps one of its vertices' bones, so the pose still drives it. It switches in where the pipeline measured it, like any other level.
- **Too small.** Below 4 pixels (high quality), 7 (medium), or 10 (low), an instance isn't drawn.
- **Posed when seen.** An instance out of view, or too small to draw, isn't posed. Its time still advances, and a pose is a pure function of time (`spine.md`, "State and time"), so the first pose when it comes into view is exact. An instance with an event playing (a tree falling) is posed wherever it is, so its result arrives.
- **Posed as often as it shows.** A small instance is posed less often: every frame at 160 pixels and above (high quality), every 2nd frame from 48, every 4th from 16, every 8th from 8, and every 16th below. Instances take turns, so the work is even across frames, and one that missed too many frames is posed at once.
- **Retained poses.** Each instance keeps a slot in a persistent bone buffer on the GPU. Only poses that changed, and are drawn, go up each frame: packed into one upload, then scattered into their slots by a compute pass. A sleeping or rarely posed instance costs nothing to prepare beyond its 64-byte instance record.

- **Fields.** A field (`plotter.md`, "Fields") is one instance over many cells. It is never posed; the wind moves its tiles in the vertex shader. It is culled and given a level per chunk of 8 x 8 cells, by a tile's size at the chunk's nearest point. Its levels never cull to nothing, since the field is the ground. The tiles of the chunks in view are streamed as 32-byte instances, one draw per tile build.

The update and prepare phases touch only what can change: classification and the choice of who is posed run across the update threads, builds waiting to switch in and results that may have changed are kept on lists, and draws are bucketed by build with a counting sort.

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
- **GPU skinning for crowds.** The update phase samples animations on the CPU and writes bone matrices into one storage buffer. Each instance has an offset into it, so skinned characters can be instanced too. Sleeping instances keep their last pose, so their bones aren't re-uploaded. Built: each instance has a persistent slot, and a compute pass scatters the changed poses into theirs (see "Cost").
- **Streaming without stalls.** Upload per-frame data through transfer buffers mapped with cycling turned on, so the CPU writes into a fresh buffer while the GPU still reads last frame's.
- **Mesh optimization.** Use indexed triangle lists, reordered for the GPU's vertex cache, overdraw, and vertex fetch (the `meshoptimizer` library does all three). Triangle strips rarely beat an optimized indexed list on modern GPUs and make batching harder, which is why the shared library stage in `spine.md` is a mesh optimizer rather than a stripifier.
- **Level of detail.** LOD meshes are just more meshes in the shared buffers; the sort key's mesh field changes, nothing else does. Each fixed asset's manifest gives every LOD a switch distance (where fewer than its switch threshold, 10% by default, of its pixels visibly disagree with the full mesh); the engine picks the LOD by camera distance times the quality factor from the game's settings (`transforms.md`). For pipelines, built: the runtime picks a level by projected size against each pipeline's measured switch sizes, which use the same 10% rule at high quality (see "Cost").
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

### Built so far

The sky's key light (the sun, or a moon at night) has one shadow map (`observatory.md`):
- **The map:** 2048² of depth, orthographic around what the game names (or 60 m ahead of the camera), snapped to its texels, and biased by slope.
- **Casters:** the skinned instances drawn this frame; fields receive shadows but don't cast them.
- **Sampling:** the mega shader's light (`src/runtime/shaders/light.hlsli`) samples it with soft edges and fades it out at its reach.

The clouds' shadows come from the same cloud map the sky draws. Its test scene is the observatory's sundial. Lights other than the sun and cascades aren't built.

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
