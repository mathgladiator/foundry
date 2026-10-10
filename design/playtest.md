# Play Testing

Play testing is a core part of defining every system and every pipeline, and of checking how the systems in a pipeline work together. It must be possible at every level. It is the primary interaction medium: the phase gates in `spine.md` are passed by play testing, and a version becomes final when a human approves it in a play test.

## Controls

A play test uses two inputs:

- **A gamepad for movement.** It drives the asset the way a player would: walking a humanoid, flying a dragon, or orbiting the camera around a static thing.
- **A control pad for state.** It sets the pipeline instance's parameters and state. The control pad is not designed by hand: it is derived from the composition of systems in the pipeline and from the interaction path that is currently active.

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

## Levels of testing

Play testing happens at four levels, from narrowest to widest:

1. **System.** One black box in isolation. Its input channel is editable on the control pad and its output channel is visualized: a skeleton as bones, a mesh with wireframe, a texture flat.
2. **Pipeline.** A full pipeline instance, driven by the gamepad and control pad. This is where most approvals happen.
3. **Composition.** Pipelines that work together: a sword mounted in a hand, armor on a body, a chest on a pedestal, rooms joined through doorways.
4. **Crowd.** Many instances at once, with real quality sliders, to check variety, caching, and performance. For example: 200 trees in a forest with wind, or a parameter sweep laid out as a grid.

## What a play test shows

The views follow `style.md` §3 and §10:

- the explore camera (13 m) and battle camera (19 m), in cel with ink;
- a black silhouette thumbnail at battle scale;
- an orthographic lineup next to the 1.8 m hero and a hex;
- a parameter sweep: the same pipeline across many inputs side by side, so the visual, pattern-matching side of the brain can spot outliers.

## Automated checks

While a play test runs, the host checks for problems continuously and reports them to both the human and the agent:

- **Conformance:** the asset stays inside its conformance shape, and mounted items stay inside their mount shapes.
- **Collision and self-intersection:** body parts overlapping during animations, armor clipping through skin.
- **Contact:** feet on the ground, hands on the grip of a held weapon.
- **Soundness:** no crashes or memory leaks, including at 100x speed.
- **Budgets:** triangle counts, generation time, and GPU memory, against `style.md` §7.

## The agent plays too

Everything the human can do in a play test, the agent can do through MCP: drive the gamepad and control pad, change the camera, take screenshots, and read the automated checks. The agent should play test its own work before asking for a human's approval.

## Recording and replay

Because systems receive time from the state machine and never read a clock, a play test is deterministic: the same inputs always give the same output. A play test session can therefore be recorded as its sequence of gamepad and control pad inputs, and replayed later. That turns play tests into regression tests:

- **Migration:** replay the same recording against an old and a forked version of a system, and compare the results (see "Migration as an experiment" in `spine.md`).
- **Native vs. WebAssembly:** replay a recording against both builds to catch differences.
- **Feedback with context:** when a human rejects something or gives feedback, it is attached to the recording and the exact moment, so the agent can reproduce what the human saw.

## How play testing feeds the design

Play testing is the back pressure that keeps the waterfall honest. A system can be correct on its own and still fail inside a pipeline: an animation that looks fine on a bare skeleton may clip once the body mesh exists, or a jump wind-up may feel wrong when the gamepad drives it. When a play test exposes a problem in a final system, the fix follows the normal process: the agent forks the system, and the human approves the fork in another play test.

## Open questions

1. **The physical control pad.** Is the control pad an on-screen panel in the SDL3 app, a second physical device (such as a MIDI controller with sliders), or both?

It's an SDL game pad, and we only need one. We are biasing towards xbox controller pattern.


2. **Recording format.** Is a recording stored as a channel with a disk format (`flow.md`), so it is versioned like everything else?

Recording of inputs is format to mirror the input channel, and the AI should be able to construct it.

3. **What counts as approval.** Does approving a version require a recorded play test to be attached, so that every final system has at least one replayable test?

Oh, no. Approval is something a human can do after playing it. The point of a replayable test is to produce things that can be watched in bulk. So, I can see a screen with like 16 recordings all at once and get a feel for where to dive in as  I play with it.