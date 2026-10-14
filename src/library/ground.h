/* ground.h: what a built instance hands the game beyond its looks (design/terrain.md; design/boundary.md,
 * "Figures"): a boundary's figures, the trees and bushes it leaves for the game to fill from its own
 * systems, and a terrain's ground, as hexbound columns of spans (design/import.hexbound.world.md) with
 * its roads, bridges, navigation mesh, and the hull a boundary takes so the two meet.
 *
 * Every call reads the build the instance draws now: until its build is ready they return 0 (or
 * false). Positions are in the world (the instance's transform applied); cells are the instance's
 * own (q, r), so spawn a terrain unturned to share its cells with a region.
 *
 * A fragment of runtime.h, after fire.h. */

#ifdef __cplusplus
extern "C" {
#endif

/* A figure: a tree or bush the instance leaves for the game, with the species it asks for (a tree_v1
 * or bush_v1 genome's name; foundry_tree_v1_species_find turns one into the pipeline's value). */
typedef struct foundry_figure {
  float position[3];  /* where it stands, on the ground, metres */
  float yaw;          /* its turn, degrees */
  uint32_t kind;      /* 0 a tree, 1 a bush */
  char species[32];
  uint32_t age;       /* a tree's sapling, young, mature, ancient (0 to 3); a bush's young, mature, old (0 to 2) */
  float height;       /* about how tall, metres */
  uint32_t seed;
} foundry_figure;
/* Writes up to capacity figures; returns how many there are. */
uint32_t foundry_instance_figures(foundry_runtime *rt, foundry_instance instance, foundry_figure *out, uint32_t capacity);

/* A terrain's ground. A column is a cell's solid spans, bottom up, in 1/8 m units: air lies between
 * them (a cave), and the top of the last is the ground. */
typedef enum foundry_material {
  FOUNDRY_MATERIAL_GRASS = 0,
  FOUNDRY_MATERIAL_DIRT,
  FOUNDRY_MATERIAL_SAND,
  FOUNDRY_MATERIAL_SNOW,
  FOUNDRY_MATERIAL_COBBLE,
  FOUNDRY_MATERIAL_ROCK
} foundry_material;
typedef struct foundry_span {
  int32_t y0, y1;  /* bottom and top, 1/8 m units */
  foundry_material material;
} foundry_span;
/* A cell's flags. */
#define FOUNDRY_CELL_EDGE 1u      /* on the shape's edge */
#define FOUNDRY_CELL_ROAD 2u
#define FOUNDRY_CELL_LANE 4u
#define FOUNDRY_CELL_RIVER 8u
#define FOUNDRY_CELL_LAKE 16u
#define FOUNDRY_CELL_BRIDGE 32u
#define FOUNDRY_CELL_FLAT 64u     /* kept level for a settlement planner */
#define FOUNDRY_CELL_OPENING 128u /* an entrance or exit, or where water flows in or out */
#define FOUNDRY_CELL_CAVE 256u
typedef struct foundry_cell {
  uint32_t spans;    /* how many spans its column has */
  uint32_t flags;
  int32_t water;     /* its water level, 1/8 m units; -32768 dry */
} foundry_cell;
/* A cell of the ground, writing up to capacity of its spans; false if the instance has no ground
 * or the cell isn't in it. */
bool foundry_ground_cell(foundry_runtime *rt, foundry_instance instance, int32_t q, int32_t r, foundry_cell *cell,
                         foundry_span *spans, uint32_t capacity);
/* The ground under a point (world x, z): the top of its cell's column, metres; false if none. */
bool foundry_ground_height(foundry_runtime *rt, foundry_instance instance, float x, float z, float *y);
/* The roads: their cells as q, r pairs, road after road, and each road's cell count. Writes up to
 * the capacities; returns how many roads there are. */
uint32_t foundry_ground_roads(foundry_runtime *rt, foundry_instance instance, int32_t *cells, uint32_t cell_capacity,
                              uint32_t *counts, uint32_t count_capacity);
/* A bridge where a road crosses water: its two mounts on the banks, for a bridge or the game. */
typedef struct foundry_bridge {
  float a[3], b[3];  /* the mounts, world metres */
  float width;       /* metres */
} foundry_bridge;
uint32_t foundry_ground_bridges(foundry_runtime *rt, foundry_instance instance, foundry_bridge *out, uint32_t capacity);
/* The navigation mesh: a hexagon on each walkable top (six vertices each, world metres), each
 * polygon's cell (an index into the ground's cells), and the polygons a walker crosses between, as
 * pairs. Pass null to count; returns the polygons. */
typedef struct foundry_nav_sizes {
  uint32_t vertices, polygons, links;
} foundry_nav_sizes;
uint32_t foundry_ground_nav(foundry_runtime *rt, foundry_instance instance, foundry_nav_sizes *sizes, float *vertices,
                            uint32_t *polygons, uint32_t *cells, uint32_t *links);
/* The hull a boundary takes round this ground (foundry_<boundary>_spawn_hull): the ring just outside
 * it, each cell with its side's edge, height, and rise, and the openings leaving through it. Writes up
 * to capacity cells; returns how many there are. */
uint32_t foundry_ground_hull(foundry_runtime *rt, foundry_instance instance, foundry_hull_cell *out, uint32_t capacity);

#ifdef __cplusplus
}
#endif
