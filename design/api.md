# The API

The game never talks to systems directly. The host generates a **runtime**: a single native library that merges the engine (`submit.md`) with every approved system and pipeline (`spine.md`, "The native build"). This document describes that runtime's front door, the API a game uses to drive it.

The API must be clean enough for a human programmer to use comfortably, not just an agent. That means generated code with readable names, a small number of concepts, and no need to know how pipelines are built inside.

## Principles

- **A C API.** The runtime is C++ inside, but its front door is plain C. C is the easiest interface to call from any language, and it matches SDL3, which the game already uses.
- **SDL3-style data, no new math types.** The API takes the same plain data the game already hands to SDL3's GPU API: matrices as `float[16]` (column-major), vectors as `float[3]`, and SDL types such as `SDL_GPUDevice` where they apply. SDL3 itself has no matrix type, so plain float arrays are the common ground. The runtime never wraps math in structures of its own, so nothing is copied or converted on the way to the GPU.
- **Generated, not handwritten.** The API is generated from the pipeline JSON files (`flow.md`). Each pipeline gets its own typed functions and parameter structs, describing the pipeline's parameters rather than the channels inside it, so the API always matches what was approved.
- **One header.** A game includes only `runtime.h`, which is mechanically assembled from fragment headers (`ORG.md`, "`src/library`").
- **No system escapes the runtime.** Systems are private details of the runtime. A game cares about macro-level configuration (quality, camera, lights, the shader) and about setting each pipeline's parameters, nothing lower. No system name, system version, or channel type ever appears in the API (`ORG.md`, "Systems are private").
- **Few concepts.** A game deals with the runtime, pipelines, instances, events, the camera and lights, and the shader.
- **Everything goes through handles.** The game holds a handle for each instance and does everything through it, including destruction. Destruction must go through the runtime so that every resource is released properly: GPU buffers, cache references, open sounds. Handles stay safe after deletion: using a stale handle is a no-op that returns an error, never a crash.
- **Calls between frames.** The game calls the API between frames, from one thread. The runtime parallelizes internally (`submit.md`, "Broadcasting updates").
- **Events are drained, not called back.** Output events (sounds, sensors, deletions) go into queues that the game drains once per frame. This keeps the order deterministic and avoids surprises from callbacks firing mid-frame.

## The runtime

```c
foundry_runtime_desc desc = {
    .device  = gpu_device,               /* an SDL_GPUDevice* */
    .quality = FOUNDRY_QUALITY_MEDIUM,
};
foundry_runtime *rt = foundry_runtime_create(&desc);

while (running) {
    /* ... the game calls the API here ... */
    foundry_frame(rt, dt);               /* update, then render */
    foundry_events events;
    foundry_drain(rt, &events);          /* this frame's output events */
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

## Events

`foundry_drain` fills one structure with everything that happened this frame. Each queue is a plain array with a count:

| Queue | Contents |
|---|---|
| `sounds` | Sound events: instance, event, edge (`start` / `end`), position, handle (`sounds.md`) |
| `sensors` | Sensor overlaps: which sensor, which instance it touched |
| `signals` | Animation signals a game may care about: a sword swing's hit frame, a chest fully open |
| `lifecycle` | Instances falling asleep, waking, and being deleted |

The ordering rule matters most for lifetimes: when an instance is deleted, all of its open `end` events appear in the queue **before** its deletion event. A listener that tracks open sounds or effects can therefore always clean up correctly.

### The sound manager

The sound manager is the first listener. It is a separate service with its own small API: the game passes it the `sounds` and `lifecycle` queues each frame, along with the listener's transform, and it plays, loops, and stops sounds through SDL3's audio (`sounds.md`). A game that wants a different audio engine can skip it and read the queues directly.

```c
foundry_sound_manager *snd = foundry_sound_manager_create(rt);
/* each frame: */
foundry_sound_process(snd, &events, listener_transform);
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
