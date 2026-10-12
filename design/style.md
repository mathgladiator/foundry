# style.md — the house style for generated meshes

One style guide for everything the forges make: characters and monsters (asset.forge), props, rocks, trees and
mechanisms (thing.forge), weapons, effects, and anything Meshy produces for either. It boils down the rules spread
across `forward.asset.notes.md`, `PROMPT.V2.md` §1/§8/§16, `docs/cel.md`, `docs/gear.md`, `docs/plans/giantkin.md`
(style layer), `docs/monsterfy.md`, `docs/magic.md`, thing.forge `docs/trees.md` / `rocks.md` and the Meshy prompt
constants in both `tools/`. Where those sources disagree, this file says which one wins (§9).

This is the style for the current project. The foundry's pipelines are designed for maximum expression, so they
support more than this guide allows (textured bark, normal-mapped skin); a project opts in to this style, and another
project can bring its own.

---

## 1. The target in one paragraph

**Storybook-soft, chunky, graphic.** Wind Waker / Ni no Kuni: simple masses with readable silhouettes, clear planes
and hard creases, flat colour regions, soft 2–3 band cel shading with a wide gentle terminator, warm light and cool
saturated shadows, and light ink outlines in the darkened local colour (never black). Proportions are exaggerated
toward the toy-like: bigger heads, hands, feet and eyes, thicker limbs, small bumps melted into big shapes.
Everything is designed to read at battle distance first and up close second.

## 2. Scale and frame

Every mesh shares one frame so anything can stand next to anything.

| Rule | Value |
|---|---|
| Units | metres |
| Axes | +Y up, +Z forward, −X right (glTF) |
| Origin | footprint centre on the ground (y = 0); flyers: the ground under them; chains: the head cell's centre |
| Yardsticks | the hex (pointy-top, circumradius 1 m, inradius 0.866 m, flat-to-flat 1.732 m) and the 1.8 m hero |
| Rest pose | humanoids A-pose; quadrupeds standing square; chains straight along −Z, segments 1.732 m apart; props closed / stopped |
| Grounding | feet or lowest point at y = 0 ± 1 cm; props sunk / cut flat at y = 0 |

One hex ≈ one hero tall is the core visual comparison. Size against both, always.

### Size classes

| Class | Mesh height | Footprint | Notes |
|---|---|---|---|
| Tiny | 0.3–0.7 m (min 0.6 m visible, §3) | r ≤ 0.25 m | bigger than the footprint suggests, or come in groups |
| Small | 0.6–1.2 m | r 0.25–0.45 m | |
| Medium | 1.2–2.2 m | r 0.30–0.50 m | heroes: 1.70–1.85 m, capsule 0.35 m |
| Large | 2.2–4.5 m | r ≤ 0.866 m | sim height ≤ 2.0 m until per-agent headroom lands |
| Huge | 4.5–9 m | tri3 / flower7 | multi-hex |
| Gargantuan / Colossal | 9 m + | flower19 + | bosses |

- Footprint ≤ 0.866 m (inradius) for anything that fights in one hex.
- **Footprint arrangements.** A footprint is the set of hex cells a thing stands on (`spine.md`, "Layer 1"). The arrangements are `single` (1 cell), `line2` and `line3` (cells in a row), `tri3` (3 cells around a vertex), `flower7` (a cell and its 6 neighbours), `flower19` (radius 2), and `box w×h` (a parallelogram of cells, for small buildings). A thing's footprint is the smallest arrangement whose inner circle (or box), shrunk by 0.1 m, holds its ground contact. Footprints turn in 60° steps. Models are built flat. A footprint may span slightly uneven cells (how uneven is the plotter's config, `plotter.md`); the model is set at the height the placement gives and its base is buried where the ground is higher.
- **Trees take their footprint from their trunk.** Trees are drawn at real heights, so their stumps are real widths too (a mature bald cypress's buttressed base is 2 to 3 m across). The footprint is that of the species' largest age, so an old cypress or baobab can be `flower7` and its sapling reserves the same cells. What collides now is the pipeline's collision output, inside it. The canopy is visual and overhangs freely.
- Swept reach in idle ≤ 1.0 m (T/S/M), ≤ 1.35 m for Large and for tails, weapons and wings on any class.
- Multi-hex: ground contact stays inside the footprint cells shrunk by 0.1 m; parts entirely above 2.0 m may
  overhang one extra hex; chain bodies ≤ 0.866 m radius at segment centres, ≤ 0.5 m at joints.
- **Size drives proportions** (allometry): a big thing is not a scaled small thing. Bigger creatures get relatively
  smaller heads and slower cadence; an ancient dragon is not a scaled wyrmling.

## 3. Readability (the hard constraints)

The game camera is FOV 0.9 rad, pitch 0.62, 13 m exploring / 19 m in battle. A hero is ~155 px tall exploring and
~106 px in battle on 1080p; 1 px ≈ 1.7 cm in battle.

1. **Silhouette first.** Every asset must be identifiable as a solid black shape at 19 m. QA sheets show a
   battle-scale silhouette thumbnail.
2. **Minimum mesh height 0.6 m** (≈ 35 px in battle). Smaller is a speck under the ink.
3. **Minimum feature thickness 6 cm.** Ink is 1–2 px (2–3.5 cm in battle); anything thinner becomes just a line.
   Applies to horns, staffs, tails, ears, branches, edge-on blades. (Trees thicken their pipe-model radii ×1.4 for
   this.)
4. **Separation.** Body segments read as distinct masses: waists, joints, withers, pedicels. A pillar with no
   breaks is a failure.
5. **Exaggerate the identifying feature**, not everything: a troll's long arms and big hands, a spider's two
   tagmata, a scorpion's high tail.

## 4. Form: how to build a mesh for cel

The shader turns every normal change into a band edge, so geometry is designed for it.

- **Few clean planes.** Smoothing groups: faces meeting under ~60° (gear) / 30° (faceted rock) share normals,
  everything sharper keeps a hard crease. A cel ramp should see a handful of planes, not a gradient.
- **Big masses, simplified.** Small additive bumps (muscle bellies, bony knobs) shrink and blend into the large
  forms (`style_simplify` 0.6–0.7). Limbs read as a few planes.
- **Edited normals for soft areas.** Faces and bellies shade from a smoothed proxy (ellipsoid / smoothed shell) so
  shadow shapes are clean; hard creases keep split normals.
- **No high-frequency geometry noise.** Perlin knobbliness, fbm and bumps must be low frequency; procedural `bump`
  is 0 in cel because bumps break bands into speckle.
- **Symmetry.** Bilateral creatures are built (or mirrored after decimation) exactly symmetric about x = 0.
- **Primitives that suit the look:** lofts (rings along a path) for hafts, grips, limbs; bevelled slabs (outline
  extruded with per-point thickness and bevel) for blades, boards, guards; SDF primitives → dual contouring for
  organic bodies; noise-displaced icosphere clumps for broadleaf foliage (they read as cel blobs with ink); flat
  pads / mats / alpha-cut cards for needles, fronds and cloth.
- **Watertight with real interiors.** Cuts are capped (wood end grain, fresh lighter stone) or hollowed with a rim;
  every fracture fragment gets an interior material.
- **Wear in the silhouette, not the texture.** Nicks are cut into edges; cracks are geometry or masked seams.

### The style layer (proportion exaggeration)

References (Meshy, measured anatomy) give **structure**; a per-plan style layer then exaggerates. Factors are
multipliers on derived lengths/girths, all pinnable, and `style.amount = 0` returns the fitted structure.

| Knob | Shared gentle default | Giantkin | Canine |
|---|---|---|---|
| head | 1.12 | 1.16 | 1.4 |
| eyes / ears | 1.15 / 1.08 | — | 1.35 / 1.12 |
| hands / feet | — | 1.22 / 1.15 | paws 1.3 |
| limb girth | 0.92 (legs) | 1.1 | 0.84 |
| brow / jaw / hump | — | 1.25 / 1.15 / 1.2 | — |
| simplify | 0.6 | 0.7 | 0.6 |

Humanoids: chunky heroic, **5.5–6.5 heads tall** (head 0.28–0.33 m), big hands and feet. Valid range across plans
4.5–9 heads.

## 5. Colour

- **Flat albedo regions.** Colour lives in named paint regions (one flat colour each, plus metal / rough /
  emission), not in textures. No high-frequency albedo noise: it turns to mush at distance.
- **Budget: one dominant colour + one accent.** The shader adds two tones of light; textures must not add a third.
  Accents ≤ 10 % of the area.
- **Value structure:** belly / underside lighter, back / top darker. Up-facing surfaces may take a lighter
  weathered or mossy material (`skin_top`).
- **House palette** (curated, muted because the shader lifts chroma): moss, forest, bark, sand, slate, bone, rust,
  ember, teal, sea, frost, charcoal; dusk purple sparingly. Colour words in a vibe map into this set.
- **Saturated but harmonious, never neon.**
- **Palettes are data.** Parts name palette roles (`body`, `accent`, `eye`, `edge`, `trim`, `field`…), so recolouring
  never touches geometry.
- **Standard paint regions for hard-surface gear:** body, edge (a bevel strip along cutting edges: the edge
  highlight band), grip (leather), haft (wood), trim (fittings, gold above high ornament), gem, string, field
  (heraldic colour).
- **No baked light.** Textures and Meshy outputs must be flat, matte, even albedo with no painted shadows or
  highlights (they fight the ramp).

## 6. Shading and ink (what the renderer does, so design for it)

| | Default | Meaning for the mesh |
|---|---|---|
| bands / terminator / soft | 3 / 0.02 / 0.2 | broad planes get 2–3 clean tones |
| light | warm (1.04, 0.98, 0.88), from upper side (0.62, 0.68, 0.40) | real shadow shapes on the side away from +X+Y |
| shadow | cool (0.50, 0.52, 0.72), +25 % saturation | shadows stay coloured, never grey |
| rim | 0.28 band | silhouettes pop against backgrounds |
| specular | metal / wet only (water, crystal, ice) | everything else is matte |
| ink | inverted hull, 3 px at 1080 lines, colour = local albedo × 0.28 saturated +35 % | outlines follow the silhouette, so the silhouette is the drawing |
| ink by LOD | 1 / 0.8 / 0, fades out below ~75 px tall | LOD2 must read with no ink at all |
| ink by material | none on elemental surfaces (flame, water, storm, ice) and alpha-cut cards; ½ on leaf cards | |
| cast shadows | the sun in the world; up to 3 marked lights near the player in total (`submit.md`) | a surface in cast shadow drops into its shadow band, so cast shadows use the same cool tint and never go grey |
| shadow edge | crisp, matching the cel terminator | shadows read as flat shapes, like the bands, not soft gradients |

Things cast shadows on each other and on the world: a character's shadow falls on the ground and on the wall behind
them, and a tree's canopy shades whoever stands under it. Ground shadows anchor assets to the world, so they also help
readability at battle distance (§3).

Intersection lines (arm over chest) are not inked yet, so separate overlapping masses by value, not by relying on
lines. The v1 `painted` look remains only for comparison.

Effects follow the same language: 2-band shells, a flat hot core, flat two-tone spark shapes with an ink border,
element-tinted ink (darkened, never black), ~20 % of the caster's own colour mixed into shadow and ink.

## 7. Budgets and LODs

| Asset | LOD0 | LOD1 | LOD2 | Materials |
|---|---|---|---|---|
| Humanoid | 12k | 4k | 1.2k | |
| Monster (S/M/L) | 6k (≤ 10k S, ≤ 15k L/M skinned import) | 2k | 600 | one per group: body, membrane, armour, gear |
| Huge / Gargantuan | ≤ 25k / ≤ 40k | | | 1; textures 2048² (G), 1024² otherwise |
| Chain creature | ≤ 4k per segment | | | 1 |
| Weapon | 300–1280 | ornaments dropped, rings halved | silhouette only (no bevels, studs) | paint regions |
| Thing (prop / rock / tree) | e.g. boulder ~4k (faceted ~1.1k) | 10 levels, each ~0.7× the previous | | per material |
| Parts (tier 1 primitive bodies) | ≤ 24 parts, ≤ 4 deep | | | palette roles |

**LODs drop detail, never silhouette.** Ornaments, bevels and studs go first; the outline survives to the last level.
Tileable textures, when used at all, are small (256²–512²) and per metre.

## 8. Meshy and image prompts (common style block)

All Meshy jobs (concept, image-to-3D, text-to-3D, retexture) append one shared style block, plus a staging block for
the job kind. Proposed canonical wording, merged from the five tool constants:

- **Style:** `stylized storybook fantasy, cel-shaded, hand-painted look, chunky readable shapes, bold silhouette,
  simplified masses, clear joint and part separation, saturated but not neon colours`
- **Texture prompt:** `flat even matte albedo, clean colour regions, no baked lighting or shadows, no dirt, no fine
  noise`
- **Staging (concepts):** `single subject centred, whole subject in frame with a generous margin, plain white (or
  light grey for props) background, no floor, no cast shadow, even flat lighting, orthographic camera, no text, no
  labels`
- **Isolation (parts, props):** `nothing else in the scene: no body, no mannequin, no stand, no base, no people`
- **Pose:** characters in a symmetrical A-pose (~45° arms, palms down/in, bare feet flat), creatures in exact
  side / front / top views with the tail laid straight back (no curl, no coil), props closed.

Meshy output is a reference for structure; the style layer and paint regions make it house style.

## 9. Where the sources disagree (resolved here)

| Conflict | Ruling |
|---|---|
| asset.forge humanoid / part / variant prompts say "World of Warcraft style", "stylized-realistic"; refs, thing.forge and PROMPT.V2 §16 say storybook / cel | **Storybook-soft wins** (the §16 decision). Swap the WoW wording in `meshy_seed.py`, `meshy_part.py`, `meshy_variant.py` for §8's block on their next edit. Existing humanoid bases stay as they are. |
| Bark, leaf and rock textures (tileable, patterned) vs "no high-frequency albedo noise" | Allowed at low contrast, faded toward the mean colour with distance (afrt already does this in cel). New generators prefer paint regions. |
| Armour procedural surfaces (mail lattice, scales, rivets, stitching) | Same as above: legacy, fade with distance; v2 gear uses geometry + flat regions only. |
| Prop staging background light grey (thing.forge) vs pure white (asset.forge) | Either; it only has to be plain and removable. |
| Humanoid skin (`systems.md`): textures with normal maps, veins, body hair vs "no high-frequency albedo noise" and procedural `bump` 0 | Allowed and tunable per pipeline. The pipelines support both; this guide is opt-in per project (see the top of this file). Tune skin detail in play testing until the cel bands stay clean. |

## 10. Review checklist

Before an asset is shown for approval, render the sheets and check:

- [ ] Side + top orthographic next to the 1.8 m hero and the hex, gauge lines behind (lineup, notes §9).
- [ ] Explore (13 m) and battle (19 m) views in cel with ink, plus a black silhouette thumbnail.
- [ ] Identifiable as a silhouette at battle scale; no feature under 6 cm; mesh ≥ 0.6 m.
- [ ] Footprint and swept reach inside the class limits; grounded at y = 0.
- [ ] One dominant colour + one accent, accents ≤ 10 %, belly lighter than back, no baked light, no fine noise.
- [ ] Band edges are clean (no speckle, no faceting blotches on faces/bellies).
- [ ] Every LOD keeps the silhouette; LOD2 reads without ink.
- [ ] Triangle and material budget met.
- [ ] Deterministic from its spec / recipe seed.
