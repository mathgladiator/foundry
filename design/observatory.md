# Observatories: the sky, its light, and its shadows

An **observatory** is a planet's sky. It holds the sun, the moons, the air, the stars and their constellations, the clouds, and the aurora. The runtime turns it, with a state the game sets, into the sky drawn behind everything and the light everything is drawn in. The workspace keeps one per planet in `observatories/<name>.json`: `earth` and `mars` so far.

An observatory isn't a pipeline. Nothing is built for it and it has no versions, systems, or channels. It is data: a few numbers and a star chart, which the game hands to the runtime (`api.md`, "The sky"). It is also how the foundry first tests shadow maps (`submit.md`, "Shadows"): every observatory shows a sundial.

## What it holds

| Part | What it is | Earth | Mars |
|---|---|---|---|
| The year | the axial tilt, the year's length in its own days, where the northern spring equinox falls | 23.44°, 365.25 days, 0.2164 (March 20) | 25.19°, 668.6 sols, 0 |
| The pole | where its north pole points among the stars (J2000) | the celestial pole | RA 317.68°, Dec 52.89°, near Deneb |
| The sun | colour, size, intensity | 0.53° | 0.35° |
| The air | single scattering: Rayleigh, Mie (scattering, absorption, and a forward lobe per channel), ozone, scale heights, the planet's radius | Earth's | thin, dusty: little Rayleigh, Mie that absorbs blue and scatters blue forward (a butterscotch day, a blue sunset) |
| Moons | sidereal period, inclination, node, phase at the year's start, size at its usual distance, that distance, brightness, colour; in the orbital plane or the equator | the Moon at 384,400 km, drawn at 1° (twice its size) | Phobos at 6,000 km (rises in the west, crosses in hours), Deimos at 20,070 km (a bright point) |
| Stars | background stars at density 1 (count), the seed the whole chart and Milky Way come from, stars crowding the galaxy's plane, and the Milky Way's glow with its dark rift | 60,000 to magnitude 7.6 (240,000 to 8.8 baked), seed 7 | the same sky |
| Constellations | named stars (RA, Dec, magnitude, B−V colour) and the lines between them | 17: Orion, the dippers, Cassiopeia, Cygnus, Lyra, Aquila, Scorpius, Leo, Taurus, Gemini, Auriga, Canis Major, Crux, Sagittarius, Boötes, Pegasus | `constellations_from: earth` |
| Clouds | their height, a cloud's size, a seed, a colour | 1.6 km, 600 m | 4 km, 1.2 km, dusty |
| The aurora | the latitude of its quiet oval, and its colours low and high | 67°, green below and red-violet above | 70° |
| Other colours | a lit constellation's glow, the night sky's own glow, the ground under views that draw one | | rust ground |

Its `start` is the state it opens in, saved from its inspector.

**Seeds.** The sky is generated from two seeds in its file: `stars.seed` (every background star and the Milky Way) and `clouds.seed` (the cloud map). Star *k* takes its place, brightness, and colour from the seed and *k* alone, so a seed always bakes the same sky (`sky_test` bakes twice and compares), and a larger count only adds stars. The inspector's **Generation** row and `observatory_set` (`star_seed`, `cloud_seed`) set them and write them into the file.

## Its state

The game, the inspector, and `observatory_set` all set the same state:

| | Range | What it does |
|---|---|---|
| `time_of_day` | 0 to 1 from midnight | Local solar time: 0.5 puts the sun on the meridian. With the latitude and the year, it places the sun, the moons, and the stars |
| `year` | 0 to 1 | The sun's place along its path (so the seasons and the length of the day), which stars are up at night, and the moons' phases |
| `latitude_deg` | −90 to 90 | Where the observer stands: the sun's height, the pole star's, the aurora's distance |
| `clouds` | 0 clear to 1 overcast | Cover of the cloud map, in the sky and as shadows on the ground |
| `cloud_wind` | m/s along +X and +Z | How the clouds and their shadows drift |
| `aurora` | 0 to 1 | A storm's strength; it also brings the oval toward the equator |
| `star_density` | 0 to 4 | How many background stars show, brightest first: 0 only the constellations' stars, 0.5 (the default) half the observatory's count, 1 all of it (to magnitude 7.6), 4 four times as many (to 8.8). The Milky Way fades below 0.5, as on a hazier or moonlit night |
| `moon_distance_km` | per moon, km (0: its usual distance) | Nearer, a moon is bigger in the sky (its size scales with its usual distance over this one) and lights the ground more (as the square, up to 25 times). Its orbit keeps its period |
| `constellations` | per constellation: `dark`, `lit`, or `connected` | Lit stars glow; connected ones also glow along their lines |
| `shadows` | on or off | The key light's shadow map |

## How a frame of sky is made (`src/runtime/sky/`)

`sky::compute` is pure: an observatory and a state give a frame. The world's axes are +X east, +Y up, and −Z north.

1. **The sun.** Its ecliptic longitude comes from the year and the equinox, and its equatorial direction from the tilt. The local sidereal time follows from the time of day, so the sun crosses the meridian at local noon. One rotation takes the planet's equatorial frame to the observer's horizon at the latitude, and it places everything.
2. **The stars.** A second rotation turns Earth's chart to the planet's pole, so Mars's night turns around Deneb. The sky shader samples the chart through both rotations.
3. **The moons.** Each one moves along its orbit by the days since the year began. Its phase is how much of its face the sun lights, so new, crescent, and full come out of the geometry. The full moon falls on January 13, as in 2025.
4. **The air.** Single scattering along the view ray (16 steps, each with 4 toward the sun) gives the sky's colour, and the same code on the CPU gives the light:
   - **The key light:** the sun's light through the air. It turns red and weak near the horizon and is gone below it.
   - **The ambient:** the sky's own light on the ground, at half the saturation the air gives it.
   - Both are calibrated against the sun 60° up, so noon matches the classic look (key about 1, shade about 0.55) on any planet.
5. **Night.** The brightest moon that is up lights the ground whenever it is brighter than the sun (so the moment the sun's light fades, there is no gap waiting for it), blue to night eyes, scaled by its phase and its nearness. The night sky has its own faint glow.
6. **Exposure.** The eye opens as the light goes, up to 14 times, but never all the way (the light to the power 0.65), so what it sees darkens from noon to night. On January 13 at 45° north (a full moon), the light seen is 1.45 at noon, 0.58 at sunset, 0.16 in the dark before the moon is high, and 0.41 at a moonlit midnight (`sky_test`). The rise after dusk is the full moon climbing. The sky adapts less again (to the power 0.75), so dusk and night stay deep. The sky is toned by luminance, keeping its hue, so a bright sky near the sun stays its colour instead of going white.
7. **The aurora.** It needs a dark sky, is stronger near the equinoxes (by 30%), and its oval moves toward the equator with the storm. Looked at from a pole, it is behind you during a great storm.

## How it is drawn

- **The sky pass** (`shaders/sky.hlsl`, `sky.hlsli`) is a triangle at the far plane, drawn after everything else, so only the pixels nothing covers pay for it. It draws:
  - the air's scattering;
  - the sun's disc, with limb darkening;
  - each moon, lit as a sphere by the sun so its phase and terminator are right, with faint maria;
  - the star chart: a 1024² half-float cube map baked once per observatory, twinkling, and hidden by a bright sky, the moons, and clouds. Its alpha holds each background star's rank by brightness, so the density cuts it live, with no rebake. Stars dim by 10^−0.3 per magnitude rather than the real 10^−0.4, so the faint field reads as a field (stylized, like the Moon's size);
  - the lit constellations' glow and the Milky Way (a two-channel 512² cube; the glow is rebaked when the constellations' states change), pulsing slowly;
  - the aurora: curtains marched through 22 layers from 95 km, in three arcs over a diffuse glow, rayed, swaying, and rippling;
  - the clouds: a layer at their height, lit on the sun's side with a silver lining, darker in their thick middles, and hazy far away.
- **The cloud map** (`clouds.hlsli`) is two tiling noises turned against each other, cut by the cover and drifting with the wind. The sky and the ground read the same function, so a cloud's shadow sits under the cloud.
- **The light** (`shaders/light.hlsli`) is shared by every mesh shader, skinned and fields in the runtime and the foundry's viewport:
  - the `style.md` §6 cel bands, between the ambient and the key;
  - under a sky, the key's shadow map (4×4 taps with bilinear weights, a soft edge about two texels wide), the clouds' shadow, fog toward the horizon, and the exposure.
  - Without a sky it is exactly the classic fixed light, and the stress plans and every view without a sky are unchanged.
- **The shadow map** is 2048² of depth, from the key light: orthographic, snapped to its texels so it doesn't shimmer, and biased by slope.
  - In the runtime it covers what the game names (`shadow_centre` and `shadow_radius`), or 60 m ahead of the camera. Its casters are the skinned instances drawn this frame. Fields receive shadows but don't cast them.
  - In the foundry's viewport it covers the model and the ground its shadow falls on: out to three heights from the model, so long evening shadows fit.

## In the foundry

- **Observatories** in the workspace. Selecting one shows its **sundial**:
  - a paved round with twelve posts that throw long shadows;
  - a pedestal and a bronze dial with hour lines from 6 to 18 for its latitude, so the shadow tells the local time;
  - a gnomon whose edge points at the celestial pole (north, or south below the equator). It has one length at every latitude and only tilts: flat toward the dial near the equator, upright near a pole. The hour lines are a horizontal dial's, so near the equator they crowd toward noon (real horizontal dials do this; an equatorial dial would not).
  
  Orbit it, or **Stand and look**: drag (or a gamepad's sticks) to look around, and use the wheel (or the triggers) to zoom, for the stars, the moon, and the aurora. **Play** walks the hero's capsule around it under the sky (`playtest.md`, "Moving around").
- **Its inspector** shows the clock and date, where the sun and moons are with the moons' phases, the light, and the exposure. Its controls are **Play the day** (hours per second), the time of day, the day of the year, the latitude, the clouds and their wind, the aurora, shadows, and each constellation's state. It saves the state as the observatory's start.
- **A viewer's sky.** Every pipeline, config entry, and plotter has a **Sky** checkbox and an observatory dropdown in its viewer. A model then stands on the observatory's ground under its sky, with its shadow. The **live** plotter hands the sky to the native runtime through the game's API, and its shadow map follows the orbit.
- **MCP.** `observatory_get` lists or describes, `observatory_set` sets the state, plays the day, puts a viewer under a sky (`view`), and saves. `observatory_view` renders the sundial, orbiting or looking. `pipeline_view` and `plotter_view` take `observatory` (or `none`).

## Checked

- **`tests/sky_test.cpp`:**
  - The equinox sun is overhead at noon on the equator, rises due east, and sets due west.
  - At 45° north it stands 68.44° high at the June solstice and 21.56° at the December one, in the south.
  - Below the equator, the noon sun is in the north.
  - Polaris stands at the latitude's height, due north, at any hour.
  - The moon is full on days 12 and 42 of the year (January 13 is the reference).
  - The key light is redder at 3° than at noon, the sky's light is blue, the eye opens at night, and the cloud map tiles.
- **The sundial** on June 21 at 15:00, 52° north (the sun 45.5° up): seen from above, the gnomon's shadow falls northeast across the dial, onto the afternoon hour lines, checked by eye (`observatory_view`).
- **Cost.** The happy glen live at its default view, in one run of each (`plotter:happy_glen_v1`):

  | | Frame rate | Prepare |
  |---|---|---|
  | No sky | 345 fps | 0.05 ms |
  | Earth's sky, no shadows | 310 fps | 0.24 ms |
  | Earth's sky with shadows | 301 fps | 0.27 ms |

  Most of the prepare is the sky's light computed on the CPU each frame.

## Known limits

- **Shadows:** one map, no cascades. Casters come only from what the camera sees, so long shadows from things just out of view are missing. Fields don't cast.
- **The sky is single scattering.** Twilight after the sun is well down is darker than a real one, and the exposure is clamped to keep it from being washed out.
- **The far plane.** The runtime's sky needs nothing drawn at the far plane; the foundry's viewer puts a 2 km ground under its models.
- **The air is computed every frame**, calibration included, on the CPU (about 0.2 ms). Caching the calibration per observatory would halve that.
- **Not compiled into the runtime.** The game passes an observatory as data. Plotters, by contrast, are compiled in by `tools/gen_pipelines.py`.
- **No headless sky.** The CPU renderer draws only the sky's gradient and light, with no sun, stars, clouds, or shadows.
