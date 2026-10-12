# The API

The game never talks to systems directly. The host generates a **runtime**: a single native library that merges the engine (`submit.md`) with every approved system and pipeline (`spine.md`, "The native build"). This document describes that runtime's front door, the API a game uses to drive it.

The API must be clean enough for a human programmer to use comfortably, not just an agent. That means generated code with readable names, a small number of concepts, and no need to know how pipelines are built inside.

## Principles

- **A C API.** The runtime is C++ inside, but its front door is plain C. C is the easiest interface to call from any language, and it matches SDL3, which the game already uses.
- **SDL3-style data, no new math types.** The API takes the same plain data the game already hands to SDL3's GPU API: matrices as `float[16]` (column-major), vectors as `float[3]`, and SDL types such as `SDL_GPUDevice` where they apply. SDL3 itself has no matrix type, so plain float arrays are the common ground. The runtime never wraps math in structures of its own, so nothing is copied or converted on the way to the GPU.
- **Generated, not handwritten.** The API is generated from the pipeline JSON files (`flow.md`). Each pipeline gets its own typed functions and parameter structs, describing the pipeline's parameters rather than the channels inside it, so the API always matches what was approved.
- **One header.** A game includes only `runtime.h`, which is mechanically assembled from fragment headers (`ORG.md`, "`src/library`").
- **No system escapes the runtime.** Systems are private details of the runtime. A game cares about macro-level configuration (quality, camera, lights, the shader) and about setting each pipeline's parameters, nothing lower. No system name, system version, or channel type ever appears in the API (`ORG.md`, "Systems are private").
- **Few concepts.** A game deals with the runtime, pipelines, instances and their results, physics (active entities: players and creatures, by handle), the camera and lights, the shader, and, if it fills its world from biomes, regions.
- **Everything goes through handles.** The game holds a handle for each instance and does everything through it, including destruction. Destruction must go through the runtime so that every resource is released properly: GPU buffers, cache references, open sounds. Handles stay safe after deletion: using a stale handle is a no-op that returns an error, never a crash.
- **Calls between frames.** The game calls the API between frames, from one thread. The runtime parallelizes internally (`submit.md`, "Broadcasting updates").
- **Results are read, not called back.** Each update leaves every instance a small result of current values: one flag per event it may fire (`break_over`), its sensor contacts, its sounds. The update lists the instances whose results changed through no action of the game's, and the game reads those and diffs them against what it saw last, for rising and falling edges. This keeps the order deterministic and avoids surprises from callbacks firing mid-frame.

## The runtime

```c
foundry_runtime_desc desc = {
    .device  = gpu_device,               /* an SDL_GPUDevice* */
    .quality = FOUNDRY_QUALITY_MEDIUM,
    .physics = &(foundry_physics){.tag = FOUNDRY_PHYSICS_JOLT, .jolt_version = JPH_VERSION_ID,
                                   .system = jolt_physics_system, .layers = purpose_layers},  /* checked, then used */
};
foundry_runtime *rt = foundry_runtime_create(&desc);

while (running) {
    /* ... the game calls the API here ... */
    foundry_frame(rt, dt);               /* update, then render */
    const foundry_instance *changed;
    uint32_t n = foundry_changed(rt, &changed);  /* the instances whose results changed this update */
    for (uint32_t i = 0; i < n; ++i) { /* read each one's result through its pipeline */ }
}

foundry_runtime_destroy(rt);
```

`foundry_frame` runs one full frame from `submit.md`: wake, update, sleep, emit events, submit. Games that want more control can call `foundry_update` and `foundry_render` separately.

## Pipelines and versions

Each approved pipeline gets a generated fragment of `runtime.h`, with functions and structs named by pipeline and version, plus unversioned names for the version the game currently uses:

```c
/* generated fragment: chest_v3 */
typedef struct { foundry_material material; uint32_t seed; } foundry_chest_v3_params;
foundry_instance foundry_chest_v3_spawn(foundry_runtime *rt,
                                        const foundry_chest_v3_params *params,
                                        const float transform[16]);
void foundry_chest_v3_set_progress(foundry_runtime *rt, foundry_instance chest, float progress);

/* generated fragment: chest (the version this game uses) */
typedef foundry_chest_v3_params foundry_chest_params;
#define foundry_chest_spawn        foundry_chest_v3_spawn
#define foundry_chest_set_progress foundry_chest_v3_set_progress
```

Upgrading the game to a newer approved pipeline means regenerating the unversioned fragment, and old and new versions can run side by side during migration.

## Instances

`foundry_instance` is a small handle (an index and a generation counter) that the game stores in its own entities. The game's world is the game's concern: it uses the runtime to populate that world with life, and keeps its own data on its own side of the handle.

```c
foundry_chest_params params = { .material = FOUNDRY_MATERIAL_OAK, .seed = 1234 };
foundry_instance chest = foundry_chest_spawn(rt, &params, transform);

foundry_set_transform(rt, chest, new_transform);
foundry_set_drawable(rt, chest, false);  /* hide it without deleting it */
foundry_destroy(rt, chest);              /* fires open end events, releases everything, then a deletion event */
```

### Parameters and interactions

Parameters and interaction paths come from the pipeline's channels, as generated functions. Setting a parameter or starting an interaction wakes the instance automatically, so the game never has to remember to set `wake_up` itself:

```c
foundry_chest_set_progress(rt, chest, 0.4f);   /* the game owns progress (spine.md, "State and time") */
foundry_tree_set_wind(rt, tree, wind_at(pos)); /* wind as float[3] */
foundry_wall_set_health(rt, wall, 0.25f);
foundry_rock_break(rt, rock);                  /* switches to the break interaction path */
```

### Queries

The game reads geometry back from an instance, since geometry is what the foundry provides. Results are written into arrays the caller provides, in world space:

| Function | Returns |
|---|---|
| `foundry_get_conformance` | The instance's conformance shapes |
| `foundry_get_mount` | A mount point's transform, as `float[16]` (a hand, a pedestal, a head ring) |
| `foundry_get_surfaces` | Mount surfaces, with their walkable flags |
| `foundry_get_sensors` | Each sensor's origin and outward direction, as `float[3]` pairs |
| `foundry_get_sound_volumes` | Rough room volumes and walls for occlusion and reverb |
| `foundry_is_sleeping` | Whether the instance is currently asleep |

### Mounting

```c
foundry_attach(rt, sword, hero, "hand_right");
foundry_attach(rt, stun_stars, ogre, "head_ring");
foundry_detach(rt, sword);
```

An attached instance follows its parent's mount point. Destroying a parent detaches its children first; the game decides whether to destroy them too.

## Physics

The runtime takes part in the game's physics, which is a baseline (small-p physics for a game that occasionally does something wild), not a sandbox. Jolt Physics is linked beside it as a static library, and there is one Jolt world, which the game owns. The game hands the runtime a blind pointer to it at creation (`foundry_runtime_desc.physics`), wrapped with a tag and Jolt's version, which the runtime checks before use. With it, the game maps each purpose an instance's shape can have (solid body, soft body, query-only shape, each sensor purpose such as reach to pick) to a Jolt object layer and mask; pipelines name purposes, never masks. The runtime keeps every instance's collision body and sensors in that world, and the game moves its entities (players, creatures) through it, updating Jolt itself. The foundry's host and tools build toy worlds of their own the same way. No Jolt type appears in `runtime.h`.

- **Server and client.** In multiplayer, the server runs the physics and decides every contact. Clients mostly predict; they can run the physics for that, but their sensor results go unused.
- **Bodies follow declarations.** Each build outputs a static collision description in Jolt's shapes (`spine.md`, "Layer 1"). The runtime adds, swaps, and removes the instance's body itself: the game declares what a thing is (set a tree's `stump`), and the runtime makes its body match. The game never handles bodies.
- **Collision is as much as a player feels.** A tree collides at its trunk, a bush softly (a player can push a little way in), a rock with its convex hull. A flower doesn't collide, but its shape answers queries.
- **Softness is the black box's choice.** A collision description can say anything Jolt can: a bush might use a smaller shape, or a soft contact that lets a player push in before it pushes back.
- **Queries.** The game asks the world raycasts, overlaps, and shape casts.
- **The cost.** Every game that links the runtime links Jolt, even one that only wants the renderer. That bloat is accepted.

### Active entities and sensors

The game registers its **active entities** with the runtime: every player and creature (the game is multiplayer). A registration declares an external ID, which the runtime only tracks and reports back, and a collision mask, which the game manages at a higher level: it decides what the entity takes part in, so behaviour (a deer that notices hunters but not other deer) stays the game's. The game moves its entities; the registration is so the runtime can emit events correctly and attribute them. An entity is a handle, like an instance, so using a stale one is an error, never a crash. Instances and entities can carry sensor shapes (cylinders, capsules, spheres) that collide with nothing, and the runtime detects contacts between entities and sensors at full fidelity, including another entity's sensors (a deer's aggro radius noticing a hunter). After each update, an instance's or entity's result lists the entities touching its sensors and how far each is from it (one changed list covers both), so the game can show a prompt ("pick", "chop") and, when several things are in reach of one button, pick the nearest. The runtime maintains these relationships; the game syncs them to its UI.

```c
foundry_entity hero = foundry_entity_create(rt, &(foundry_entity_desc){.external_id = player_id, .body = hero_body_id, .mask = PLAYER_MASK});
/* the game moves the hero's body in Jolt itself; the runtime reads it when it updates */
/* after an update, for a bush in the changed list: */
foundry_bush_v1_result r;
foundry_bush_v1_get_result(rt, bush, &r);
for (uint32_t i = 0; i < r.contact_count; ++i)
    offer_pick(r.contacts[i].external_id, bush, r.contacts[i].distance);
foundry_entity_destroy(rt, hero);
```

## Regions

A game that fills its world from biomes registers **regions**: prioritized hex regions with their attributes (ground, height, moisture, and so on), bound to an approved plotter (`plotter.md`). Every plotter takes the same region list, so the game describes its ground once. A region is bookkeeping, not an owner: it indexes what occupies its cells, and it holds no handles.

```c
foundry_region swamp = foundry_region_create(rt, &desc);          /* cells, attributes, plotter, world seed */
foundry_region_restore(rt, swamp, saved, saved_count);              /* on load: what the save holds */
uint32_t volume;
uint32_t got = foundry_region_grow(rt, swamp, FOUNDRY_GROW_SEED, &exclusions, focus, out, 64, &volume);  /* up to 64, and how many more fit */
foundry_instance h = foundry_placement_spawn(rt, &out[0]);          /* the game spawns and owns each one */
foundry_region_vacate(rt, swamp, &slot, 1);                          /* it's gone: its cells are free */
```

- **The game paces growth.** `grow` returns up to the number asked for, and the volume the region could still take; a volume of 0 means it is full and stable. Regrowth is simply a later `grow` in regrow mode, when the game decides, and always converges to the original density. The plotter has no clock.
- **The game owns everything placed.** It spawns placements as ordinary instances, holds their handles, sets their parameters (season, wind, a break, a stump), saves them, and restores them into the region on load.
- **Results are returned.** `grow`, `set_regions` (which reports occupants on lost cells), and `find` write into caller arrays, like the queries above.
- **Regions plot alone.** Each region plots only from its own cells; it never reads its neighbours.
- **The game decides every input.** The foundry is a factory. Overrides such as a spell's blizzard are the game's: it sets the season on the instances the spell covers and restores them afterwards, and there is no region-wide setter.
- **Collision lives in the runtime's Jolt world** (see "Physics"). The region's footprints are only for placement.

## Instance results

Each update leaves every instance a **result**, generated per pipeline like its parameters. A result holds current values, not one-shot events: the game diffs it against the last one it read to find rising and falling edges, and decides what to do with them (wait a second on its own timer, then turn a felled tree into a stump).

| Field | Contents |
|---|---|
| Event flags | One flag per finite animated event, true from when it is over: `break_over` (a felled tree is down, so the game can make it a stump; a rock's pieces are at rest), a chest fully open, a sword swing past its hit frame |
| Contacts | Which active entities touch the instance's sensors (by external ID), and how far each is |
| Sounds | The sounds playing now: event, position, handle (`sounds.md`). The sound manager diffs this against the last result to start and stop them; a sound shorter than an update appears once |
| Sleep | Whether it is asleep |
| Ready | Which build it is drawing, so a rebuild the game asked for (a tree turned stump, its body swapped) shows when it has happened |

`foundry_changed` lists the instances whose results changed in the last update through no action of the game's (an animation ending, a contact starting or ending, a sound, sleep), plus those whose requested rebuild has finished, so the game reads only those. Results last until the next update. Deleting an instance ends its open sounds in its final result before the handle goes stale, so a listener that tracks open sounds or effects can always clean up.

### The sound manager

The sound manager is the first listener. It is a separate service with its own small API: each frame it reads the playing sounds in the changed instances' results, diffs them against what it is playing, with the listener's transform, and it plays, loops, and stops sounds through SDL3's audio (`sounds.md`). A game that wants a different audio engine can skip it and read the results directly.

```c
foundry_sound_manager *snd = foundry_sound_manager_create(rt);
/* each frame, after the update: */
foundry_sound_update(snd, listener_transform);  /* reads the changed results' sounds */
```

## The world

The game also sets the shared context that instances render in:

```c
foundry_set_camera(rt, view, projection);       /* float[16] each; also feeds the culler's projected-size test */
foundry_set_quality(rt, FOUNDRY_QUALITY_HIGH);
foundry_set_sun(rt, direction, color);          /* float[3] each */

foundry_light_desc torch_desc = {
    .position = { 4.0f, 1.8f, -2.0f },
    .color    = { 1.0f, 0.7f, 0.4f },
    .shadow   = FOUNDRY_SHADOW_HEMISPHERE,      /* or NONE, SPHERE */
};
foundry_light torch = foundry_add_light(rt, &torch_desc);
```

## The shader

The mega shader (`submit.md`) is shared between the runtime and the game, so the runtime exports it rather than keeping it private:

- **Export.** The game can get the shader's source and its compiled forms for each backend, along with the layout of its parameters, so the game's own renderer can draw with the same look (terrain, for example, which the foundry doesn't produce).
- **Configuration through defines.** Higher-level features are switched with `#define`s (shadows on or off, the number of lights, ink on or off). The game sets them through the runtime, which builds the matching pipeline objects.
- **Caching with versions.** Compiling shaders and building pipeline objects is slow, so the runtime caches the compiled results on disk. The cache key is the shader's version, its defines, and the GPU backend, so a new shader version or a changed define never picks up a stale compiled shader.

```c
foundry_shader_desc shader = {
    .defines = (const char *[]){ "FOUNDRY_SHADOWS=1", "FOUNDRY_MAX_LIGHTS=4", NULL },
    .cache_dir = "cache/shaders",
};
foundry_configure_shader(rt, &shader);
const foundry_shader_export *exp = foundry_get_shader(rt);
```

## Errors

Every call that can fail returns a status code, and nothing crashes on bad input from the game: a stale handle, an unknown mount name, or an out-of-range parameter is reported and ignored. Bugs inside a system are a different matter: native code can't contain them, so they are meant to be caught earlier, in the instrumented WebAssembly build and in play testing (`spine.md`, "The native build").
