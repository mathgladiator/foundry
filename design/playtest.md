# Play Testing

Play testing is a core part of defining every system and every pipeline, and of checking how the systems in a pipeline work together. It must be possible at every level. It is the primary interaction medium: the phase gates in `spine.md` are passed by play testing, and a version becomes final when a human approves it after playing it.

## Controls

A play test uses one SDL gamepad, following the Xbox controller layout. It does two jobs:

- **Movement.** It drives the asset the way a player would: walking a humanoid, flying a dragon, or orbiting the camera around a static thing.
- **State.** An on-screen control pad sets the pipeline instance's parameters and state, navigated with the same gamepad. The control pad is not designed by hand: it is derived from the composition of systems in the pipeline and from the interaction path that is currently active.

A proposed mapping:

| Input | Movement mode | Control pad mode |
|---|---|---|
| Left stick | move | — |
| Right stick | camera | — |
| D-pad | — | move between controls |
| Left / right trigger | — | decrease / increase the selected slider |
| A | jump / interact | press a button, cycle a selector |
| B | — | back / close the pad |
| X, Y | game actions (attack, cast) | — |
| View | toggle the control pad | toggle the control pad |
| Menu | pause, step, time speed | pause, step, time speed |

### Deriving the control pad

Every parameter in a pipeline instance's property bag (`spine.md`, "State and time") becomes a control, chosen by its type:

| Parameter | Control |
|---|---|
| Enum (season, age, material) | selector |
| Continuous value (health, wind speed, lid progress) | slider, which can be animated |
| Event (break, open, cast) | button |
| Instance ID / seed | number field with a "reroll" button |
| Time | play, pause, step, and speed (including 100x) |
| Quality sliders | the same sliders the game's graphics settings expose |

The control pad only shows parameters for the active interaction path. When a tree breaks, its wind controls are replaced by the controls of the break path.

The mega shader's parameters (`style.md` §6) can be added to the control pad too, which makes play testing the place to explore and tune the shader (`submit.md`).

## Moving around

Every 3D view in the foundry (pipelines, config entries, fixed assets, plotters static and live, and observatories) moves the same way (`src/host/app/controls.h`):

- **Orbit:** drag to orbit and use the wheel to zoom. Shift-drag or right-drag pans along the ground, and ctrl-drag (or the **height** field) raises or lowers what the camera orbits. **Centre** puts it back. The live plotter's right button still flies.
- **A gamepad:** the right stick orbits, the left pans, the shoulders raise and lower, and the triggers zoom. Standing at an observatory, either stick looks around and the triggers zoom. While a 3D view shows, the sticks belong to it, and ImGui's gamepad navigation steps aside.
- **Play:** the hero's capsule (1.8 m tall, 0.35 m in radius) walks the scene, moved by Jolt's character controller (`src/host/physics/walker.h`).
  - Controls: WASD or the left stick walks (4 m/s), and Shift or the right trigger runs (8). Space or A jumps (about 1.1 m). Drag or the right stick looks. The wheel or Y moves the camera between behind the capsule and its eyes, and Escape or Back stops.
  - It climbs steps up to 0.45 m and slopes up to 50 degrees. Solid bodies stop it, soft ones part, and queries and sensors don't touch it.
  - What it walks on: in the live plotter, the region's ground, among every trunk the runtime keeps in the Jolt world the foundry hands it, as a game does. In other views, a floor plus the model's collision bodies (a pipeline's) or its mesh (a fixed asset, or an observatory's sundial).
  - Where it starts: on a map (a plotter's region, live or static; a planner's plan; a boundary's world), at the middle, in the first place the capsule fits. It searches rings outward every half metre for open ground: the lowest ground there, with nothing above it (no roof, canopy, or trunk's top), flat enough to stand on, where the capsule touches nothing. A single asset starts back from it, facing it. Either way it faces the way the camera faces.
  - `tests/walker_test.cpp` checks the walk and run speeds, a solid trunk and a soft one, a step and a wall, and the jump.

## Levels of testing

Play testing happens at five levels, from narrowest to widest:

1. **System.** One black box in isolation. Its input channel is editable on the control pad and its output channel is visualized: a skeleton as bones, a mesh with wireframe, a texture flat.
2. **Pipeline.** A full pipeline instance, driven by the gamepad and control pad. This is where most approvals happen.
3. **Composition.** Pipelines that work together: a sword mounted in a hand, armor on a body, a chest on a pedestal, rooms joined through doorways.
4. **Crowd.** Many instances at once, with real quality sliders, to check variety, caching, and performance. For example: 200 trees in a forest with wind, or a parameter sweep laid out as a grid.
5. **Region.** A plotter filling generated regions of hex cells (`plotter.md`, "In the foundry"): shapes and painted attributes, growth step by step or to stable, clearing and regrowth, slopes, adjacent regions and the edge between them, with the plotter's shared properties (season, wind) mapped onto every instance shown.

## What a play test shows

The views follow `style.md` §3 and §10:

- the explore camera (13 m) and battle camera (19 m), in cel with ink;
- a black silhouette thumbnail at battle scale;
- an orthographic lineup next to the 1.8 m hero and a hex;
- a shadow test scene with known geometry and lights, plus a debug view of each shadow map (`submit.md`, "Shadows");
- a parameter sweep: the same pipeline across many inputs side by side, so the visual, pattern-matching side of the brain can spot outliers.

## Automated checks

While a play test runs, the host checks for problems continuously and reports them to both the human and the agent:

- **Conformance:** the asset stays inside its conformance shape, and mounted items stay inside their mount shapes.
- **Collision and self-intersection:** body parts overlapping during animations, armor clipping through skin.
- **Contact:** feet on the ground, hands on the grip of a held weapon.
- **Soundness:** no crashes or memory leaks, including at 100x speed.
- **Budgets:** triangle counts, generation time, and GPU memory, against `style.md` §7.

## The agent plays too

Everything the human can do in a play test, the agent can do through MCP: drive the gamepad and control pad, change the camera, take screenshots, and read the automated checks, the sound event log (`sounds.md`), and which instances are awake or asleep (`submit.md`). The agent should play test its own work before asking for a human's approval.

## Recording and replay

Because systems receive time from the state machine and never read a clock, a play test is deterministic: the same inputs always give the same output. A play test session can therefore be recorded as its sequence of inputs, and replayed later. A recording's format mirrors the pipeline's input channel, so it is just a timeline of input channel values. That means the AI can also write recordings directly, without anyone playing: a scripted walk cycle, a chest opened and interrupted halfway, a spell cast at every intensity.

Recordings are not a requirement for approval; approval is something a human does after playing. Their main purpose is review in bulk:

- **A wall of recordings.** The workspace can show many recordings at once (around 16 on one screen), so a human gets a feel for a pipeline across many inputs and picks where to dive in and play.

Recordings also work as regression tests:

- **Migration:** replay the same recording against an old and a forked version of a system, and compare the results (see "Migration as an experiment" in `spine.md`).
- **Native vs. WebAssembly:** replay a recording against both builds to catch differences.
- **Feedback with context:** when a human rejects something or gives feedback, it is attached to the recording and the exact moment, so the agent can reproduce what the human saw.

## How play testing feeds the design

Play testing is the back pressure that keeps the waterfall honest. A system can be correct on its own and still fail inside a pipeline: an animation that looks fine on a bare skeleton may clip once the body mesh exists, or a jump wind-up may feel wrong when the gamepad drives it. When a play test exposes a problem in a final system, the fix follows the normal process: the agent forks the system, and the human approves the fork in another play test.
