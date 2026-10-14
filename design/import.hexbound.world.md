# body.md: the hex ground

This is the whole terrain model, reduced to four ideas: **terrain**, **cave**,
**cliff** and **smoothness**. Everything else, including nav, water, gates,
fog and worldgen set pieces, reads from these four and is out of scope here.
Where this document differs from the code today, the difference is listed in
**Cuts** at the end.

---

## 0. The grid

- Hexes are pointy-top, 1 m from center to corner, so neighbors are 1.732 m
  apart. One hex is about one hero tall and holds one fighter.
- Coordinates are axial `(q, r)`. The center is at `x = √3·(q + r/2)`,
  `z = 1.5·r`, and +Y is up. Storage is an odd-r offset rectangle `(col, row)`.
- Corner `i` sits at `60°·i + 30°`. Edge `e` joins corners `e` and `e+1`.
  Each corner is shared by exactly three cells.
- Heights are integers in **1/8 m** (`int16`). All comparisons happen in
  these units, so every cell computes the same answer for the same input.

## 1. Terrain: a column of spans

Each hex is a **column**: a sorted list of solid **spans**, and each span is
three values.

```c
typedef struct Span { int16_t y0, y1; uint8_t material; } Span;  /* y0 < y1 */
```

- Air is whatever lies between spans. Most columns hold one span, from the
  world floor up to the ground.
- **Material** says what the solid is: `grass, dirt, sand, snow, cobble,
  rock`. Each material carries a texture, a walk cost and a smoothness
  (see §4).
- A **surface** is an exposed face of a span:
  - a **top** has air above it. It is ground if there is enough headroom.
  - a **bottom** has air below it. It is a ceiling, or the underside of an
    overhang.
- That is all the terrain stores. Slopes, cliffs and caves are not stored;
  they follow from the spans by the rules below.

## 2. Cave: the gap between spans

A cave is two spans in one column with air between them.

- **Floor** is the top of the lower span, and **ceiling** is the bottom of
  the upper span.
- **Headroom** is ceiling minus floor. A top is **walkable** when its
  headroom is at least 2 m (the top span of a column has unlimited
  headroom).
- Ceilings follow the same smoothness and cliff rules as floors, turned
  upside down. A cave roof rounds off where it should and breaks into
  walls where it should, with no extra code.
- An **overhang** is a cave with a lip. If a ledge's span has no headroom
  above the edge where you would climb it, the climb is refused. Shelves
  therefore block climbing from beneath.
- Arches, the mine, tunnels and cave mouths are all just columns with two
  or more spans.

## 3. Cliff: where surfaces don't fuse

Across each edge, a surface either **fuses** with one surface in the
neighbor cell or it doesn't. A cliff is an edge where it doesn't.

- **Pairing.** A surface pairs with its *mutual nearest* surface of the
  same kind (top with top, bottom with bottom) in the neighbor cell.
  Mutual nearest means each one is the other's closest by height. A
  surface has at most one partner per neighbor.
- **Fusing.** A pair fuses when `|Δy| ≤ smooth(a, b)` (§4).
- **Wall.** Along an edge from A to B, the wall is A's solid minus B's
  solid. It is measured on each half-edge (corner→mid, mid→corner), so it
  follows the slope. Where A and B fused, their profiles meet and no wall
  appears. Walls face B and use the cliff look of A's material: rock for
  rock and cobble, earth for the soft materials.
- **Moving across an edge** depends only on the edge's height difference
  `dy`:

  | edge | rule |
  |---|---|
  | fused, or `\|dy\| ≤ 0.5 m` | walk |
  | up to `jump_up` | jump |
  | up to `climb_max`, and no overhang | climb |
  | down to `drop_max` | drop |

  Each body brings its own `jump_up`, `climb_max` and `drop_max`. The
  ground only reports `dy` and whether the edge fused.

## 4. Smoothness: how the surface is shaped

**Smoothness** is one number per material, in meters: the largest step
that still rounds into a slope instead of breaking into a cliff. For a
pair of materials, the threshold is the lower of the two:
`smooth(a, b) = min(smooth(a), smooth(b))`.

| material | smooth | feel |
|---|---|---|
| grass, dirt, sand | 1.5 m | rolls |
| snow | 1.25 m | rolls, a little crisper |
| cobble | 1.0 m | graded, like a built road |
| rock | 0.6 m | breaks into crisp cliffs |

Every top (and bottom) is drawn as a **13-point fan**: a center, 6 corners
and 6 edge midpoints, which make 12 triangles. Three rules set its shape:

1. **Center** = the span's own height.
2. **Corner** = the mean height of every surface fused into it across the
   three cells that share the corner. If a chain of fusions would join two
   surfaces of the same cell, drop the link with the largest `dy` first
   (ties go to the larger cell id). Every member of the group gets the
   bit-identical value, so there are no cracks.
3. **Edge midpoint.** If the edge fused,
   `mid = ¼(y_self + y_other) + ¼(c1 + c2)`; otherwise
   `mid = ½·y_self + ¼(c1 + c2)`, where `c1` and `c2` are the edge's two
   corners.

- Material blending uses the same groups: a corner's material weights are
  shares of its fused group, a midpoint averages its two cells if they
  fused, and the center is pure.
- Normals are welded across fused vertices, so fused ground shades smoothly,
  and stay hard along walls.
- **One profile for everyone.** The renderer, collision, height sampling and
  nav all read the same profile function. What you see is what you stand
  on.

## 5. Invariants (what tests must hold)

- **Symmetry.** If A fuses with B across an edge, B fuses with A, and both
  compute the same corners and midpoint.
- **No cracks.** Every corner value is shared exactly by its group.
- **Same answer every time.** Fusion and corner sums use integer height
  units and iterate in cell-id order; no float compare decides topology.
- **A flat field is flat**, a one-step 1/8 m ramp is smooth, and a rock
  step of 0.75 m is a cliff.
- **Edits re-derive.** Changing a column (digging a moat, or harvesting a
  boulder) only rewrites spans. Surfaces, walls and walkability are
  recomputed for the column and its six neighbors.

## 6. Tuning knobs (the whole list)

| knob | value | what it moves |
|---|---|---|
| smoothness per material | §4 table | where cliffs appear |
| step | 0.5 m | what you can walk up without jumping |
| headroom | 2.0 m | which cave floors are walkable |
| height unit | 1/8 m | the finest terrain detail |
| sea level / world floor | 0 m / −16 m | the waterline, and how deep anything can go |

The finicky bit is that a change of 1/8 m near a threshold flips a slope to
a cliff or a cliff to a slope. Grade anything a route depends on, such as
roads, ramps and mine cuts, at least 1/4 m inside its material's smoothness.

---

## Cuts (from today's code to this document)

| today | here | why |
|---|---|---|
| per-span `blend` bits (top/bottom) and a `reserved` field | removed; a span is `y0, y1, material` | worldgen sets both bits on every span (`BLEND_ALL`), so they never decide anything |
| a 6×6 `material_connect_scale` table × `connect_height` (1 m) | one `smooth` value per material, using the min of the pair | same feel and fewer cases. Behavior changes: grass, dirt and sand against themselves rise from 1.25 to 1.5 m, dirt–sand rises from 1.2 to 1.5 m, snow against grass, dirt or sand rises from 1.2 to 1.25 m, and cobble against cobble drops from 1.25 to 1.0 m |
| `material_can_blend` | removed | it only excluded air and water, which are never terrain |
| `MAT_WATER` as a terrain material | removed | water is simulated volume over the air gaps, not solid; worldgen never places it |
| `side_material` per material | a fixed rule: rock and cobble give rock walls, soft materials give earth walls | the same mapping, without the table |
| nav link kinds WALK/UP/DOWN/GAP with a fixed 12 m cap | the edge reports `dy` and whether it fused; the body decides | GAP (leaping over a cell) is a body ability over two edges, not terrain |

What stays exactly as built: spans, the mutual-nearest pairing, the corner
group mean with its same-cell tie-break, the midpoint formula,
solid-minus-solid walls, the 2 m headroom, and one profile shared by the
client and the server.
