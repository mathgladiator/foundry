# Sounds

This document describes how sounds hook into pipelines. The foundry is a graphics engine at its core, so audio stays simple: an event structure, tables, and a swappable player. Sounds are tables, not generators: every sound is a recorded or pre-produced file, picked from a table at runtime. There is no on-the-fly sound generation. Producing the sounds themselves is a separate effort (see `third.party.md`); this document only defines the event structure that connects them to the game.

## Events

Sounds are fired by the update phase (`submit.md`). When a system starts or ends something that has a sound, it emits a sound event:

| Field | Meaning |
|---|---|
| `instance` | The pipeline instance that fired the event. |
| `event` | Which event: `open`, `break`, `footstep`, `cast`, and so on. |
| `edge` | `start` or `end`. |
| `position` | Where the sound happens, as (x, y, z) in world space. |
| `handle` | Identifies one playing sound, so an `end` can stop the matching `start`. |

The audio layer reads the event, looks up the instance's pipeline sound table, and plays the matching entry. Events are fired only by awake instances, since only they run the update phase.

### The sound manager

Playing sounds belong to a **sound manager**, not to the instance's update. Once a sound starts, sleep doesn't change its behavior: a snoring monster can fall asleep in the engine's sense (nothing about it needs updating) while its snore keeps looping, and the waterfall keeps roaring. A looping sound stops only when its `end` event arrives.

### Every start has an end

Because loops outlive the update phase, the engine must guarantee that every `start` eventually gets its `end`, according to the event's lifetime:

- When a system ends an interaction, it fires the `end` itself.
- When an instance is deleted, the engine fires `end` for every event the instance still has open, then announces the deletion. Listeners (the sound manager, and the game) subscribe to deletions through the runtime's front door (`api.md`), so nothing is left playing for an instance that no longer exists.

## Sound tables

Each pipeline (each factory) has its own sound table. A row maps an event to what plays at its start and at its end:

| Event | On start | On end | Notes |
|---|---|---|---|
| `open` | `chest_creak` | `chest_thud` | |
| `lockpick` | `pick_scratch` (loop) | — | stops when the event ends |
| `break` | `wood_crack` | `debris_settle` | |
| `cast` | `spell_charge` (loop) | `spell_release` | the loop covers the telegraph |

### Playback parameters

Each sound entry carries the traditional playback parameters:

| Parameter | Meaning |
|---|---|
| `files` | One or more variants. One is picked each time, avoiding immediate repeats. |
| `volume`, `volume_variance` | Base loudness and random spread per play. |
| `pitch`, `pitch_variance` | Base pitch and random spread, so repeated sounds don't feel mechanical. |
| `loop` | Loop until stopped (by the event's `end`, or when the instance is destroyed). |
| `fade_in`, `fade_out` | Seconds to fade at start and stop. |
| `delay` | Seconds to wait after the event before playing. |
| `spatial` | `3d` (positioned and attenuated) or `2d` (UI and narration). |
| `min_distance`, `max_distance`, `rolloff` | How the sound fades with distance. |
| `follow` | Whether the sound follows the instance (or one of its mount points) as it moves, or stays where it was fired. |
| `bus` | Mixer group: `sfx`, `voice`, `ambient`, `music`, `ui`. |
| `priority`, `max_instances` | When too many sounds play at once, low-priority ones are dropped first, and no more than `max_instances` of this sound play together. |
| `cooldown` | Minimum time between plays of this sound from one instance. |

### Voice and language

A sound table can be parameterized along extra dimensions, so one pipeline serves many characters and markets:

- **Voice** is a pipeline parameter: a humanoid's voice type (for example `gruff_male`, `young_female`, `goblin`).
- **Language** comes from the game's settings.
- **Material** can be another dimension, for sounds such as footsteps on stone vs. wood.

A lookup uses the most specific matching entry and falls back to less specific ones. For a `grunt` event with voice `goblin` and language `fr`:

1. `grunt` / `goblin` / `fr`
2. `grunt` / `goblin` / any language
3. `grunt` / any voice / `fr`
4. `grunt` / any voice / any language

Non-verbal sounds (grunts, footsteps) usually only need the first dimension, while dialogue needs all of them.

## Determinism

Variant and pitch choices use a random seed derived from the instance's ID and an event counter, not a global random generator. A replayed play test (`playtest.md`) therefore sounds exactly the same as the original.

## In play testing

The play test shows a log of sound events next to the view (event, edge, position, and which table entry matched), and the agent can read the same log through MCP. That makes it easy to check that every event has a sound and that loops stop when they should.

## The audio engine

Sounds are played with what SDL3 provides by default (its audio streams, with `SDL3_mixer` if mixing needs it). The audio layer sits behind a clean boundary, so the player can be replaced later (with FMOD, Wwise, or anything else) without touching pipelines or sound tables.

## Occlusion and reverb

Walls muffle sounds behind them, and rooms add reverb. Both come from geometry the pipelines already export: conformance shapes can include rough room volumes and walls (`spine.md`, Layer 1), and the building pipeline exports them for its rooms. Precision isn't needed: a sound in another room is muffled, a sound in a stone hall echoes.

## Out of scope

- **Lip sync.** Dialogue is audio only. Faces are textures for now, so there are no mouth shapes to drive.
- **Music.** The game selects the theme (a boss stage change can switch tracks), outside this event system.

## Layout

Each pipeline's table and files live together, in the style of `flow.md`: `sounds/$pipeline/table.json` for the table and `sounds/$pipeline/*.ogg` for the audio, with voice and language variants in subdirectories. Like other inputs, each file can carry a JSON sidecar with its source, prompt, cost, and license.
