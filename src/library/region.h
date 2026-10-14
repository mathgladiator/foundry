/* region.h: regions of hex cells filled by plotters (design/plotter.md, design/api.md "Regions").
 *
 * A fragment of runtime.h, after foundry.h. The game registers a region (its
 * ground, as prioritized hex regions) with an approved plotter, then asks it for
 * growth. The region answers with placements; the game spawns them as ordinary
 * instances, owns them, and tells the region when they go away. The region holds
 * no instance handles: it is bookkeeping, and the plotter keeps no clock.
 *
 * Positions are in the region's space: axial cell (q, r) has its centre at
 * x = sqrt(3) (q + r/2), z = 1.5 r (pointy-top cells, circumradius 1 m), and the
 * region's transform places that space in the world. */

#ifdef __cplusplus
extern "C" {
#endif

/* A region: an index and a generation, safe when stale like an instance handle. */
typedef uint64_t foundry_region;
#define FOUNDRY_NO_REGION ((foundry_region)0)

/* An approved plotter: one of the FOUNDRY_PLOTTER_* values (generated). */
typedef uint32_t foundry_plotter;

typedef enum foundry_ground {
  FOUNDRY_GROUND_SOIL = 0,
  FOUNDRY_GROUND_WATER = 1,
  FOUNDRY_GROUND_ROCK = 2,
  FOUNDRY_GROUND_SAND = 3,
  FOUNDRY_GROUND_PATH = 4
} foundry_ground;

typedef enum foundry_hex_shape {
  FOUNDRY_HEX_CELLS = 0, /* an explicit list of cells */
  FOUNDRY_HEX_HEX = 1,   /* every cell within radius of (q, r) */
  FOUNDRY_HEX_RING = 2,  /* every cell at exactly radius from (q, r) */
  FOUNDRY_HEX_LINE = 3   /* the cells along a line from (q, r) to (q2, r2) */
} foundry_hex_shape;

/* Which attributes a hex region sets (the rest come from lower regions). */
#define FOUNDRY_SET_GROUND 1u
#define FOUNDRY_SET_AVAILABLE 2u
#define FOUNDRY_SET_KEEP_CLEAR 4u
#define FOUNDRY_SET_HEIGHT 8u
#define FOUNDRY_SET_MOISTURE 16u

/* One hex region of the ground. A cell takes its values from the highest
 * priority region covering it (ties: the later one); a cut removes cells. */
typedef struct foundry_hex_region {
  foundry_hex_shape shape;
  const int32_t *cells; /* FOUNDRY_HEX_CELLS: q, r pairs */
  uint32_t cell_count;  /* pairs */
  int32_t q, r, q2, r2;
  uint32_t radius;
  int32_t priority;
  bool cut;
  uint32_t set; /* FOUNDRY_SET_* bits */
  foundry_ground ground;
  bool available;     /* things with footprints may stand here */
  float keep_clear_m; /* keep blocking things this far from these cells */
  float height;       /* the ground's height at the cells' centres, metres */
  float moisture;     /* 0 dry to 1 wet */
} foundry_hex_region;

typedef struct foundry_region_desc {
  foundry_plotter plotter;
  uint32_t world_seed;
  float transform[16]; /* the region's space in the world (column-major); all zeros: identity */
  const foundry_hex_region *regions;
  uint32_t region_count;
} foundry_region_desc;

/* A placement's stable identity: the cell that made it, its layer, and its index. */
typedef struct foundry_slot {
  int32_t q, r;
  uint32_t layer, index;
} foundry_slot;

/* Something the plotter says belongs. Save it beside the instance spawned from it. */
typedef struct foundry_placement {
  foundry_slot slot;
  uint32_t kind;       /* the plotter's kind (foundry_plotter_kind_name) */
  float position[3];   /* region space; y is the centre cell's height */
  float yaw;           /* radians about +Y */
  uint32_t seed;
  float footprint_m;   /* the ground it reserves, metres (0: none) */
  uint32_t footprint_cells;
  bool regrown;        /* placed by a regrow: it starts young */
} foundry_placement;

typedef struct foundry_exclusion {
  float position[3]; /* region space */
  float radius;
} foundry_exclusion;

typedef enum foundry_grow_mode {
  FOUNDRY_GROW_SEED = 0,   /* a new region */
  FOUNDRY_GROW_REGROW = 1  /* filling what was cleared: things start young */
} foundry_grow_mode;

foundry_region foundry_region_create(foundry_runtime *rt, const foundry_region_desc *desc);
/* Forgets the region's index; destroys no instances. */
foundry_status foundry_region_destroy(foundry_runtime *rt, foundry_region region);

/* On load: puts back the placements a save holds, before growing. If any is
 * invalid (an unknown kind, overlapping footprints, outside the region), none is
 * kept and FOUNDRY_STATUS_FAILED comes back: drop them and grow the region anew. */
foundry_status foundry_region_restore(foundry_runtime *rt, foundry_region region, const foundry_placement *placements,
                                      uint32_t count);

/* Writes up to max placements that work now into out, in the plotter's order,
 * and records them as standing there. *volume (if not null) gets how many more
 * would fit: 0 means the region is full and stable. Returns how many it wrote. */
uint32_t foundry_region_grow(foundry_runtime *rt, foundry_region region, foundry_grow_mode mode,
                             const foundry_exclusion *exclusions, uint32_t exclusion_count, const float *focus,
                             foundry_placement *out, uint32_t max, uint32_t *volume);

/* Spawns a placement's kind with its parameters, at its place in the world. */
foundry_instance foundry_placement_spawn(foundry_runtime *rt, foundry_region region, const foundry_placement *placement);

/* The things in these slots are gone: their cells are free for a later grow. */
foundry_status foundry_region_vacate(foundry_runtime *rt, foundry_region region, const foundry_slot *slots, uint32_t count);

/* Replaces the region's ground. Writes the slots whose footprints left it into
 * lost (up to capacity) and forgets them; returns how many there were. */
uint32_t foundry_region_set_regions(foundry_runtime *rt, foundry_region region, const foundry_hex_region *regions,
                                    uint32_t count, foundry_slot *lost, uint32_t lost_capacity);

/* The slots whose things stand within radius metres of center (region space). */
uint32_t foundry_region_find(foundry_runtime *rt, foundry_region region, const float center[3], float radius,
                             foundry_slot *out, uint32_t capacity);

/* How many things stand in the region. */
uint32_t foundry_region_count(foundry_runtime *rt, foundry_region region);

/* One cell of a field (design/plotter.md, "Fields"): a pipeline instance that covers many cells, each
 * showing the tile the pipeline's per-cell stage chooses. Spawned by a region's plotter
 * (foundry_region_spawn_fields), or directly with a field pipeline's foundry_<pipeline>_spawn_field. */
typedef struct foundry_field_cell {
  int32_t q, r;          /* axial; the centre is at x = sqrt(3) (q + r/2), z = 1.5 r */
  foundry_ground ground;
  float moisture;        /* 0 dry to 1 wet */
  float height;          /* the ground's height at the centre, metres */
} foundry_field_cell;
/* Spawns the plotter's fields over the region's cells, one instance per field (grass, say), in the
 * region's space. Writes up to max handles into out; returns how many fields the plotter has. The
 * game owns them like any instance; spawn them again after foundry_region_set_regions. */
uint32_t foundry_region_spawn_fields(foundry_runtime *rt, foundry_region region, foundry_instance *out, uint32_t max);
/* A boundary (design/boundary.md): what walls a region in so its player can't leave while the world
 * seems to go on. Its hull is the ring of cells around the region, counterclockwise as seen from
 * above, each with its edge: what stops the player there and what lies beyond. */
typedef enum foundry_edge {
  FOUNDRY_EDGE_AUTO = 0,  /* the boundary's sectors give it, by direction */
  FOUNDRY_EDGE_LEDGE,     /* walk to it and look out: the land drops away to a valley far below */
  FOUNDRY_EDGE_RIDGE,     /* a cliff rises */
  FOUNDRY_EDGE_MOUNTAIN,  /* a steep slope climbs into peaks */
  FOUNDRY_EDGE_FOREST,    /* trunks too thick to pass */
  FOUNDRY_EDGE_JUNGLE,
  FOUNDRY_EDGE_SEA,       /* a beach and open water */
  FOUNDRY_EDGE_MARSH      /* reeds and pools */
} foundry_edge;
typedef struct foundry_hull_cell {
  int32_t q, r;       /* axial */
  foundry_edge edge;
  float height;       /* the ground's height at the centre, metres */
  float rise;         /* 0 to 1: how high the land climbs behind it (lowland to peaks); < 0: the boundary's */
  uint32_t opening;   /* 0 none, or what leaves the region through it: 1 a road, 2 a lane, 3 a river in, 4 a river out */
  uint32_t opening_width;  /* its width, cells */
} foundry_hull_cell;
/* The ring of cells just outside a region, counterclockwise as seen from above, each with the
 * height of the region's cell beside it, edge auto, and rise -1: a boundary's hull for it, for the
 * game to give edges and rises. Writes up to capacity cells; returns how many there are. */
uint32_t foundry_region_hull(foundry_runtime *rt, foundry_region region, foundry_hull_cell *out, uint32_t capacity);
/* Spawns the region's plotter's boundary (its plotter file's boundary: a pipeline with its parameters)
 * around the region with this hull, in the region's space. FOUNDRY_NO_INSTANCE if it has none. */
foundry_instance foundry_region_spawn_boundary(foundry_runtime *rt, foundry_region region, const foundry_hull_cell *cells,
                                               uint32_t count);
/* Fills a boundary's figures (the trees and bushes it leaves for the game; ground.h) with the systems the
 * region's plotter's boundary names (its draw.figures), each at its figure's place: what a game does
 * with its own systems. Spawns nothing until the boundary's build is ready (call again). Writes up to
 * capacity handles; returns how many it spawned. */
uint32_t foundry_region_spawn_figures(foundry_runtime *rt, foundry_region region, foundry_instance boundary, foundry_instance *out,
                                      uint32_t capacity);
/* A plotter's boundary, as "pipeline param..." (e.g. "boundary_v1 around_valley temperate"), or "" if none. */
const char *foundry_plotter_boundary_name(foundry_plotter plotter);

/* A plotter's fields, as "pipeline param..." (e.g. "grass_v1 meadow"), and how many it has. */
const char *foundry_plotter_field_name(foundry_plotter plotter, uint32_t field);
uint32_t foundry_plotter_field_count(foundry_plotter plotter);
/* An approved plotter by its name (e.g. "happy_glen_v1"), or FOUNDRY_NO_PLOTTER; and a plotter's name. */
#define FOUNDRY_NO_PLOTTER ((foundry_plotter)0xffffffffu)
foundry_plotter foundry_plotter_find(const char *name);
const char *foundry_plotter_name(foundry_plotter plotter);
/* A plotter's kind, as "pipeline param..." (e.g. "tree_v1 oak"), and how many kinds it has. */
const char *foundry_plotter_kind_name(foundry_plotter plotter, uint32_t kind);
uint32_t foundry_plotter_kind_count(foundry_plotter plotter);

#ifdef __cplusplus
}
#endif
