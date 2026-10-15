# The API

The game never talks to systems directly. The host generates a **runtime**: a single native library that merges the engine (`submit.md`) with every approved system and pipeline (`spine.md`, "The native build"). This document describes that runtime's front door, the API a game uses to drive it.

The API must be clean enough for a human programmer to use comfortably, not just an agent. That means generated code with readable names, a small number of concepts, and no need to know how pipelines are built inside.

## Principles

- **A C API.** The runtime is C++ inside, but its front door is plain C. C is the easiest interface to call from any language, and it matches SDL3, which the game already uses.
- **SDL3-style data, no new math types.** The API takes the same plain data the game already hands to SDL3's GPU API: matrices as `float[16]` (column-major), vectors as `float[3]`, and SDL types such as `SDL_GPUDevice` where they apply. SDL3 itself has no matrix type, so plain float arrays are the common ground. The runtime never wraps math in structures of its own, so nothing is copied or converted on the way to the GPU.
- **Generated, not handwritten.** The API is generated from the pipeline JSON files (`flow.md`). Each pipeline gets its own typed functions and parameter structs, describing the pipeline's parameters rather than the channels inside it, so the API always matches what was approved.
- **One header.** A game includes only `runtime.h`, which is mechanically assembled from fragment headers (`ORG.md`, "`src/library`").
- **No system escapes the runtime.** Systems are private details of the runtime. A game cares about macro-level configuration (quality, camera, lights, the shader) and about setting each pipeline's parameters, nothing lower. No system name, system version, or channel type ever appears in the API (`ORG.md`, "Systems are private").
- **The game says what; the runtime says how much.** The game decides every parameter of everything in its world. The runtime decides how much of it to compute and draw: each instance's level of detail, whether and how often it is posed, and whether it is too small to draw, from what the camera sees and the quality setting (`submit.md`, "Cost"). A pipeline's `detail` parameter is the finest level the game allows, not the level drawn.
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

**Built so far:** Jolt v5.6.0 is linked beside the runtime and into the foundry's host. The game's world arrives as `foundry_physics` (`tag = FOUNDRY_PHYSICS_JOLT`, `jolt_version = JPH_VERSION_ID`, the `JPH::PhysicsSystem*`, and an object layer per `foundry_purpose`), and `foundry_runtime_create` refuses a mismatched version with a reason. The runtime adds each instance's bodies when its build is ready, swaps them on a rebuild, moves them with `foundry_set_transform`, and removes them on destroy (`tests/results_test.cpp`, `tests/region_test.cpp`). Queries, active entities, and sensors are designed, not built.

### Active entities and sensors

The game registers its **active entities** with the runtime: every player and creature (the game is multiplayer). A registration declares an external ID, which the runtime only tracks and reports back, and a collision mask, which the game manages at a higher level: it decides what the entity takes part in, so behaviour (a deer that notices hunters but not other deer) stays the game's. The game moves its entities; the registration is so the runtime can emit events correctly and attribute them. An entity is a handle, like an instance, so using a stale one is an error, never a crash. Instances and entities can carry sensor shapes (cylinders, capsules, spheres) that collide with nothing, and the runtime detects contacts between entities and sensors at full fidelity, including another entity's sensors (a deer's aggro radius noticing a hunter). After each update, an instance's or entity's result lists the entities touching its sensors and how far each is from it (one changed list covers both), so the game can show a prompt ("pick", "chop") and, when several things are in reach of one button, pick the nearest. The runtime maintains these relationships; the game syncs them to its UI.

```c
foundry_entity hero = foundry_entity_create(rt, &(foundry_entity_desc){.external_id = player_id, .body = hero_body_id, .mask = PLAYER_MASK});
/* the game moves the hero's body in Jolt itself; the runtime reads it when it updates */
/* after an update, for a bush in the changed list: */
foundry_bush_result r;
foundry_bush_get_result(rt, bush, &r);
for (uint32_t i = 0; i < r.contact_count; ++i)
    offer_pick(r.contacts[i].external_id, bush, r.contacts[i].distance);
foundry_entity_destroy(rt, hero);
```

## Regions

A game that fills its world from biomes registers **regions**: prioritized hex regions with their attributes (ground, height, moisture, and so on), bound to an approved plotter (`plotter.md`). Every plotter takes the same region list, so the game describes its ground once. A region is bookkeeping, not an owner: it indexes what occupies its cells, and it holds no handles.

```c
foundry_region_desc desc = {.plotter = FOUNDRY_PLOTTER_HAPPY_GLEN, .world_seed = 7,
                            .regions = ground, .region_count = n};      /* foundry_hex_region: shape, priority, attributes */
foundry_region glen = foundry_region_create(rt, &desc);
foundry_region_restore(rt, glen, saved, saved_count);                     /* on load; FAILED: drop them and grow anew */
foundry_placement out[64];
uint32_t volume;
uint32_t got = foundry_region_grow(rt, glen, FOUNDRY_GROW_SEED, exclusions, exclusion_count, focus, out, 64, &volume);
foundry_instance h = foundry_placement_spawn(rt, glen, &out[0]);          /* its kind, its parameters, its place */
uint32_t n = foundry_region_find(rt, glen, centre, 6.0f, slots, 256);     /* what stands within 6 m */
foundry_region_vacate(rt, glen, slots, n);                                /* it's gone: its cells are free */
uint32_t lost = foundry_region_set_regions(rt, glen, ground, count, lost_slots, 256);  /* the ground changed */
foundry_region_destroy(rt, glen);                                         /* forgets the index; destroys no instances */
```

These are built (`src/library/region.h`, `region.cpp`), with each plotter's config and kinds compiled in (`src/library/plotters.cpp`, by `tools/gen_pipelines.py`). `foundry_plotter_kind_name` names a placement's kind (`"tree oak"`).

- **The game paces growth.** `grow` returns up to the number asked for, and the volume the region could still take; a volume of 0 means it is full and stable. Regrowth is simply a later `grow` in regrow mode, when the game decides, and always converges to the original density. The plotter has no clock.
- **The game owns everything placed.** It spawns placements as ordinary instances, holds their handles, sets their parameters (season, wind, a break, a stump), saves them, and restores them into the region on load.
- **Results are returned.** `grow`, `set_regions` (which reports occupants on lost cells), and `find` write into caller arrays, like the queries above.
- **Regions plot alone.** Each region plots only from its own cells; it never reads its neighbours.
- **The game decides every input.** The foundry is a factory. Overrides such as a spell's blizzard are the game's: it sets the season on the instances the spell covers and restores them afterwards, and there is no region-wide setter.
- **Collision lives in the runtime's Jolt world** (see "Physics"). The region's footprints are only for placement.

### Fields

Grass covers rather than being placed (`plotter.md`, "Fields"). A plotter's fields are spawned over a region's cells, one instance per field, and the game owns them like any other instance. A field pipeline can also be spawned directly over cells the game lists:

```c
foundry_instance fields[4];
uint32_t n = foundry_region_spawn_fields(rt, glen, fields, 4);  /* the plotter's grass, over every cell */
foundry_grass_params p;
foundry_grass_default_params(&p);
p.species = FOUNDRY_GRASS_SPECIES_PRAIRIE;
foundry_field_cell cells[] = {{0, 0, FOUNDRY_GROUND_SOIL, 0.6f, 0.0f}, {1, 0, FOUNDRY_GROUND_SOIL, 0.6f, 0.0f}};
foundry_instance lawn = foundry_grass_spawn_field(rt, &p, transform, cells, 2);
foundry_grass_set_wind_speed(rt, lawn, 8.0f);  /* the whole field sways harder */
```

Built (`region.h`; each field pipeline's `foundry_<pipeline>_spawn_field`). A field's parameters other than the cell's own (species, season, wind) apply to the whole field. Setting a build parameter rebuilds its few tiles, not its cells. A field doesn't move with `foundry_set_transform`: its tiles are placed when it is spawned.

## Instance results

Each update leaves every instance a **result**, generated per pipeline like its parameters. A result holds current values, not one-shot events: the game diffs it against the last one it read to find rising and falling edges, and decides what to do with them (wait a second on its own timer, then turn a felled tree into a stump).

| Field | Contents |
|---|---|
| Event flags | One flag per finite animated event, true from when it is over: `break_over` (a felled tree is down, so the game can make it a stump; a rock's pieces are at rest), a chest fully open, a sword swing past its hit frame |
| Contacts | Which active entities touch the instance's sensors (by external ID), and how far each is |
| Sounds | The sounds playing now: event, position, handle (`sounds.md`). The sound manager diffs this against the last result to start and stop them; a sound shorter than an update appears once |
| Sleep | Whether it is asleep |
| Ready | Which build it is drawing, so a rebuild the game asked for (a tree turned stump, its body swapped) shows when it has happened |

**Built so far:** each pipeline's `foundry_<pipeline>_result` carries one `<event>_over` flag per event and `ready`, read with `foundry_<pipeline>_get_result`; `foundry_changed` returns the handles whose results changed in the last update. Contacts, sounds, and sleep are designed, not built.

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
foundry_set_viewport(rt, width, height);        /* the render target, in pixels: sizes on screen are judged in them */
foundry_set_quality(rt, FOUNDRY_QUALITY_HIGH);
foundry_set_sun(rt, direction, color);          /* float[3] each */

foundry_light_desc torch_desc = {
    .position = { 4.0f, 1.8f, -2.0f },
    .color    = { 1.0f, 0.7f, 0.4f },
    .shadow   = FOUNDRY_SHADOW_HEMISPHERE,      /* or NONE, SPHERE */
};
foundry_light torch = foundry_add_light(rt, &torch_desc);
```

### The sky

Built (`observatory.md`): the game hands the runtime an observatory as data, and sets its state whenever it changes. The runtime renders the key light's shadow map in `foundry_prepare`, lights every instance by the sky in `foundry_draw` (the sun, or a moon at night; shadows, the clouds' shadows, fog, exposure), and draws the sky last. This stands in for the sketch's `foundry_set_sun`.

```c
foundry_observatory_desc earth;
foundry_observatory_default(&earth);          /* Earth's air; add moons and constellations */
earth.moons = moons;  earth.moon_count = 1;
foundry_sky_set_observatory(rt, &earth);      /* bakes its star chart; NULL removes the sky */

foundry_sky_state s;
foundry_sky_state_default(&s);
s.time_of_day = 0.75f;  s.year = 0.47f;  s.latitude_deg = 52;
s.clouds = 0.4f;  s.aurora = 0;  s.star_density = 0.5f;  /* up to 4 */
s.moon_distance_km[0] = 0;  /* 0: its usual distance */  s.shadows = true;
s.constellations = states;  s.constellation_count = 17;  /* DARK, LIT, or CONNECTED each */
foundry_sky_set_state(rt, &s);

foundry_sky_light light;                      /* the sun's place, the key light, the ambient, the exposure */
foundry_sky_get_light(rt, &light);
```

### Wind

A pipeline's `wind_heading` is the world's: degrees toward which the wind blows, from +Z toward +X. The runtime turns it into each instance's own frame before posing it, so a meadow of instances turned every which way bends one way. A field's tiles sway in it on the GPU.

### Quality

`foundry_set_quality` (or the descriptor's `quality`) sets how much the runtime spends, through the decisions in `submit.md`, "Cost". Built so far:

| | Low | Medium | High |
|---|---|---|---|
| A coarser level of detail switches in where it differs from the finest on at most this share of the pixels that show the thing (each pipeline's measured sizes) | 30% | 20% | 10% |
| Too small to draw, below | 10 px | 7 px | 4 px |
| Posed every frame at and above, then every 2nd, 4th, 8th frame, and every 16th below the last | 240, 72, 24, 12 px | 200, 60, 20, 10 px | 160, 48, 16, 8 px |

Sizes are the projected diameter of the instance's bounding sphere, in the pixels `foundry_set_viewport` gives.

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

## The look

The look (`submit.md`, "The look") is macro-level configuration: the game sets it from its settings, or changes it any frame for an effect.

```c
foundry_look look;
foundry_look_default(&look);            /* changes nothing */
look.vision = FOUNDRY_VISION_DEUTERANOPIA;
look.contrast = 0.6f;
foundry_set_look(rt, &look);
/* a cutscene: fade to a red duotone */
look.effect = FOUNDRY_LOOK_EFFECT_TINT;
look.tint[0] = 0.85f; look.tint[1] = 0.12f; look.tint[2] = 0.1f;
look.strength = t;                      /* 0 to 1 */
foundry_set_look(rt, &look);
```

## Errors

Every call that can fail returns a status code, and nothing crashes on bad input from the game: a stale handle, an unknown mount name, or an out-of-range parameter is reported and ignored. Bugs inside a system are a different matter: native code can't contain them, so they are meant to be caught earlier, in the instrumented WebAssembly build and in play testing (`spine.md`, "The native build").
