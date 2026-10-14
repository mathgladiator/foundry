/* runtime.h: the game-facing API (design/api.md). Assembled by tools/gen_pipelines.py from
 * the fragments in src/library/: foundry.h, region.h, sky.h, fire.h, ground.h, each pipeline by name, then the plotters. Do not edit. */
#pragma once

/* foundry.h: the runtime, instances, and the frame (design/api.md).
 *
 * The first fragment of runtime.h. Each pipeline adds its own fragment, with
 * a parameter struct, a spawn function, one setter per parameter, and one
 * trigger per event. Calls come from one thread, between frames. */
#include <SDL3/SDL_gpu.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct foundry_runtime foundry_runtime;

/* A handle to one pipeline instance: an index and a generation. A stale
 * handle is safe: every call with it returns FOUNDRY_STATUS_STALE_HANDLE. */
typedef uint64_t foundry_instance;
#define FOUNDRY_NO_INSTANCE ((foundry_instance)0)

typedef enum foundry_status {
  FOUNDRY_OK = 0,
  FOUNDRY_STATUS_STALE_HANDLE = 1,
  FOUNDRY_STATUS_BAD_PARAMETER = 2,
  FOUNDRY_STATUS_WRONG_PIPELINE = 3,
  FOUNDRY_STATUS_FAILED = 4
} foundry_status;

typedef enum foundry_quality {
  FOUNDRY_QUALITY_LOW = 0,
  FOUNDRY_QUALITY_MEDIUM = 1,
  FOUNDRY_QUALITY_HIGH = 2
} foundry_quality;

/* What a collision body is for (design/api.md, "Physics"). The game maps each
 * to a Jolt object layer, and so decides what collides with what. */
typedef enum foundry_purpose {
  FOUNDRY_PURPOSE_SOLID = 0,  /* a trunk, a rock */
  FOUNDRY_PURPOSE_SOFT = 1,   /* a bush a player can push into */
  FOUNDRY_PURPOSE_QUERY = 2,  /* found by rays and clicks only: a flower */
  FOUNDRY_PURPOSE_SENSOR = 3, /* reports what touches it */
  FOUNDRY_PURPOSE_COUNT = 4
} foundry_purpose;

/* The game's Jolt Physics world, as a blind pointer. The tag and the version
 * are the runtime's quick check that the pointer really is Jolt, built like
 * its own: pass FOUNDRY_PHYSICS_JOLT and JPH_VERSION_ID from Jolt's headers. */
#define FOUNDRY_PHYSICS_JOLT 0x544c4f4au /* 'JOLT' */
typedef struct foundry_physics {
  uint32_t tag;                            /* FOUNDRY_PHYSICS_JOLT */
  uint64_t jolt_version;                   /* JPH_VERSION_ID */
  void *system;                            /* a JPH::PhysicsSystem* the game created */
  uint16_t layers[FOUNDRY_PURPOSE_COUNT];  /* the object layer for each purpose */
} foundry_physics;

typedef struct foundry_runtime_desc {
  SDL_GPUDevice *device;              /* NULL runs everything but submission */
  SDL_GPUTextureFormat color_format;  /* the render pass's color target */
  SDL_GPUTextureFormat depth_format;  /* and its depth target */
  foundry_quality quality;
  uint32_t update_threads;  /* 0 picks from the machine's cores */
  uint32_t build_threads;   /* 0 picks from the machine's cores */
  const foundry_physics *physics; /* NULL: no collision bodies */
} foundry_runtime_desc;

foundry_runtime *foundry_runtime_create(const foundry_runtime_desc *desc);
void foundry_runtime_destroy(foundry_runtime *rt);
/* Why the last foundry_runtime_create failed. */
const char *foundry_last_error(void);

/* One frame: update every awake instance by dt seconds, then record uploads
 * into a command buffer before the render pass, then draw inside it. */
void foundry_update(foundry_runtime *rt, float dt);
void foundry_prepare(foundry_runtime *rt, SDL_GPUCommandBuffer *cmd);
void foundry_draw(foundry_runtime *rt, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass);

void foundry_set_camera(foundry_runtime *rt, const float view[16], const float projection[16]);
/* The render target's size in pixels: the runtime judges each instance's size on screen from it
 * and the camera's projection, to pick its level of detail and how often it animates. */
void foundry_set_viewport(foundry_runtime *rt, uint32_t width, uint32_t height);
/* How much the runtime spends (design/submit.md, "Cost"): higher quality keeps finer detail
 * farther out, draws smaller things, and animates small things more often. */
void foundry_set_quality(foundry_runtime *rt, foundry_quality quality);

/* The look (design/submit.md, "The look"): a per-pixel grade the runtime applies last to everything
 * it draws, for accessibility and effects, whatever the models are. A game sets it from its
 * settings (colour vision, contrast) and may change it any frame (a buff's tint, a cutscene's
 * greyscale). The default changes nothing. */
typedef enum foundry_vision {
  FOUNDRY_VISION_TYPICAL = 0,
  FOUNDRY_VISION_PROTANOPIA = 1,   /* red-blind: reds and greens are moved toward blue and brightness */
  FOUNDRY_VISION_DEUTERANOPIA = 2, /* green-blind */
  FOUNDRY_VISION_TRITANOPIA = 3    /* blue-blind: blues and yellows are moved into red and green */
} foundry_vision;

typedef enum foundry_look_effect {
  FOUNDRY_LOOK_EFFECT_NONE = 0,
  FOUNDRY_LOOK_EFFECT_GREYSCALE = 1, /* black and white */
  FOUNDRY_LOOK_EFFECT_TINT = 2       /* greyscale toward one hue (all red), keeping brightness */
} foundry_look_effect;

typedef struct foundry_look {
  foundry_vision vision;
  float contrast;           /* 0 as made, 1 the strongest: an S-curve on brightness, and a little more colour */
  float saturation;         /* 1 as made, 0 grey, 2 doubled */
  float edges;              /* 0..1: lines between colours, where the game draws a pass that sees neighbours */
  foundry_look_effect effect;
  float tint[3];            /* the tint's hue (rgb, 0..1) */
  float strength;           /* the effect's, 0..1: fade it in and out */
} foundry_look;

void foundry_look_default(foundry_look *look);
void foundry_set_look(foundry_runtime *rt, const foundry_look *look);
void foundry_get_look(foundry_runtime *rt, foundry_look *look);

foundry_status foundry_set_transform(foundry_runtime *rt, foundry_instance instance, const float transform[16]);
foundry_status foundry_is_sleeping(foundry_runtime *rt, foundry_instance instance, bool *sleeping);
/* The instances whose update results changed in the last update (an animation
 * ended, a requested rebuild is now drawn). Each result holds current values:
 * read it through its pipeline (foundry_<pipeline>_get_result) and diff it with
 * the last one for rising and falling edges. The array is valid until the
 * next update. */
uint32_t foundry_changed(foundry_runtime *rt, const foundry_instance **instances);

/* Releases everything the instance holds; its handle goes stale. */
foundry_status foundry_destroy(foundry_runtime *rt, foundry_instance instance);

#ifdef __cplusplus
}
#endif

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

/* sky.h: the sky, its light, and its shadows (design/observatory.md).
 *
 * An observatory is a planet's sky: its sun, moons, air, stars, and
 * constellations. It isn't a pipeline (nothing is built or versioned for it): the
 * game hands it in as data, sets its state whenever it changes (the time of day
 * and year, the latitude, clouds, aurora, which constellations are lit), and the
 * runtime renders the key light's shadow map in foundry_prepare, lights every
 * instance by it in foundry_draw (the sun, or a moon at night; shadows, the
 * clouds' shadows, fog, and exposure), and draws the sky last. Without one, the
 * light is the fixed one. The world's axes: +Y up, +X east, -Z north.
 *
 * A fragment of runtime.h, after region.h. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct foundry_sky_star {
  float ra_hours, dec_deg; /* J2000 */
  float magnitude;
  float color_index; /* B-V */
} foundry_sky_star;

typedef struct foundry_sky_constellation {
  const char *name;
  const foundry_sky_star *stars;
  uint32_t star_count;
  const uint16_t *lines; /* line_count pairs of indices into stars */
  uint32_t line_count;
} foundry_sky_constellation;

typedef struct foundry_sky_moon {
  float period_days;     /* sidereal, in the planet's days */
  float inclination_deg; /* to its orbit's plane */
  float node_deg;
  float phase;           /* its place in its orbit when the year starts, 0 to 1 */
  float size_deg;        /* as seen from the ground, at its distance */
  float distance_km;     /* its usual distance; 0: unknown */
  float brightness;      /* at full, relative to Earth's moon */
  float color[3];
  bool equatorial;       /* orbits in the planet's equator, not its orbital plane */
} foundry_sky_moon;

typedef struct foundry_observatory_desc {
  float axial_tilt_deg, year_days;
  float equinox;                    /* the year's fraction at the northern spring equinox */
  float pole_ra_deg, pole_dec_deg;  /* where its north pole points among the stars */
  float sun_color[3], sun_size_deg, sun_intensity;
  /* The air, for single scattering: per kilometre, heights in kilometres. */
  float planet_km, air_km;
  float rayleigh[3], rayleigh_km;
  float mie[3], mie_absorb[3], mie_km, mie_g[3];
  float ozone[3];
  const foundry_sky_moon *moons; /* the first two are drawn */
  uint32_t moon_count;
  const foundry_sky_constellation *constellations;
  uint32_t constellation_count;
  uint32_t stars, star_seed; /* faint background stars */
  float milky_way;
  float cloud_height_m, cloud_size_m;
  uint32_t cloud_seed;
  float cloud_color[3];
  float aurora_latitude_deg, aurora_low[3], aurora_high[3];
  float lit_color[3]; /* a lit constellation's glow */
  float night[3];     /* the night sky's own glow */
  float ground[3];
} foundry_observatory_desc;

/* Earth's sky, with no moons and no constellations. */
void foundry_observatory_default(foundry_observatory_desc *desc);
/* Bakes its star chart (a fraction of a second) and draws it from the next frame; NULL removes the sky. */
foundry_status foundry_sky_set_observatory(foundry_runtime *rt, const foundry_observatory_desc *desc);

typedef enum foundry_constellation_state {
  FOUNDRY_CONSTELLATION_DARK = 0,
  FOUNDRY_CONSTELLATION_LIT = 1,       /* its stars glow */
  FOUNDRY_CONSTELLATION_CONNECTED = 2  /* and its lines */
} foundry_constellation_state;

typedef struct foundry_sky_state {
  float time_of_day;   /* 0 to 1 from midnight; 0.5 is local noon */
  float year;          /* 0 to 1 from the start of the year */
  float latitude_deg;
  float clouds;        /* 0 clear to 1 overcast */
  float cloud_wind[2]; /* the clouds' drift, m/s along +X and +Z */
  float aurora;        /* 0 none to 1 a great storm */
  float star_density;  /* 0 only the constellations' stars, 1 the observatory's star count, up to 4 */
  float moon_distance_km[2]; /* the first two moons' distances now; 0: their usual distance */
  const uint8_t *constellations; /* per constellation: a foundry_constellation_state */
  uint32_t constellation_count;
  bool shadows;
  float shadow_centre[3]; /* where the shadow map looks */
  float shadow_radius;    /* metres; 0: 60 m, ahead of the camera */
} foundry_sky_state;

void foundry_sky_state_default(foundry_sky_state *state);
void foundry_sky_set_state(foundry_runtime *rt, const foundry_sky_state *state);

/* The light the sky gives now, for the game's own shading. False without a sky. */
typedef struct foundry_sky_light {
  float sun_direction[3], sun_altitude_deg;
  float key_direction[3], key_color[3]; /* the sun's, or a moon's at night */
  float ambient[3];
  float exposure;
  float moon_direction[3], moon_lit; /* the first moon, and the fraction of it lit */
} foundry_sky_light;
bool foundry_sky_get_light(foundry_runtime *rt, foundry_sky_light *out);

#ifdef __cplusplus
}
#endif

/* fire.h: fire and smoke (design/submit.md, "Fire").
 *
 * Buildings carry fire points: hearths, forges, torches, and braziers that are always lit,
 * chimneys and smoke holes that carry a hearth's smoke, and the points a building burns from.
 * The runtime keeps one fire system: each frame it turns the fire points of the instances near
 * the camera into flames, sparks, glows, and smoke (a particle system), draws them after
 * everything solid, and lights what stands near a fire with it (the mega shader takes the
 * nearest fires' light). Each point names a fire config (compiled in from config/fire_v1_configs/):
 * its colours, size, flicker, smoke, and light. Smoke drifts with the world's wind; a building
 * burns when the game says how much. Fire is for looks: it never changes what a pipeline builds.
 *
 * A fragment of runtime.h, after sky.h. */

#ifdef __cplusplus
extern "C" {
#endif

/* The wind that carries smoke and bends flames: metres per second, toward a heading in degrees
 * from +Z toward +X (the pipelines' wind_speed and wind_heading). */
void foundry_set_wind(foundry_runtime *rt, float speed, float heading_deg);

/* How much an instance burns: 0 not at all (its hearths and torches still burn), 1 its blaze
 * points alight, up to 2 a raging blaze (its lit fires and chimneys burn harder too). */
foundry_status foundry_set_burning(foundry_runtime *rt, foundry_instance instance, float amount);
foundry_status foundry_get_burning(foundry_runtime *rt, foundry_instance instance, float *amount);

/* The fire configs compiled in: how many, and one's id and name (NULL past the end). */
uint32_t foundry_fire_config_count(void);
const char *foundry_fire_config_name(uint32_t index, uint32_t *id);

#ifdef __cplusplus
}
#endif

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

/* Generated by tools/gen_pipelines.py from pipelines/boundary_v1.json. Do not edit. */
/* boundary_v1: A boundary (design/boundary.md): walls a playable region in so the player can't leave, while it looks like the world goes on. The game gives a hull (a config entry here: a predefined shape, or sectors by direction; a game gives its own cells) whose cells each name an edge (a ledge to look out from, a ridge's cliff, a mountain, a forest or jungle too thick to pass, a sea, a marsh) and a rise; rim resolves it into a smooth rim with the edges blended along it; terrain grows the world beyond it from those seeds out past the view distance, as a quadtree of terrain nodes with their own images that the runtime draws as a cut by distance, morphing between levels, with the rim's dressing and the walls that keep the player in. Trees and bushes within the figure range are figures: the game fills them from its own systems (the foundry with tree_v1 and bush_v1). */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_boundary_v1_hull {
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_COAST = 0,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_FOREST = 1,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_HIGHLAND = 2,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_JUNGLE = 3,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_MARSH = 4,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_MOUNTAINS = 5,
  FOUNDRY_BOUNDARY_V1_HULL_AROUND_VALLEY = 6,
  FOUNDRY_BOUNDARY_V1_HULL_BAY = 7,
  FOUNDRY_BOUNDARY_V1_HULL_CANYON = 8,
  FOUNDRY_BOUNDARY_V1_HULL_COAST = 9,
  FOUNDRY_BOUNDARY_V1_HULL_GLADE = 10,
  FOUNDRY_BOUNDARY_V1_HULL_JUNGLE_CLEARING = 11,
  FOUNDRY_BOUNDARY_V1_HULL_L_SHAPE = 12,
  FOUNDRY_BOUNDARY_V1_HULL_MARSH_EDGE = 13,
  FOUNDRY_BOUNDARY_V1_HULL_MOUNTAIN_PASS = 14,
  FOUNDRY_BOUNDARY_V1_HULL_PLATEAU = 15,
  FOUNDRY_BOUNDARY_V1_HULL_SHAPE_HEXAGON = 16,
  FOUNDRY_BOUNDARY_V1_HULL_SHAPE_RIVER_VALLEY = 17,
  FOUNDRY_BOUNDARY_V1_HULL_VALLEY = 18,
  FOUNDRY_BOUNDARY_V1_HULL_COUNT = 19
} foundry_boundary_v1_hull;
/* A hull by its name (as a config entry or a figure names it), or -1. */
int foundry_boundary_v1_hull_find(const char *name);

typedef enum foundry_boundary_v1_biome {
  FOUNDRY_BOUNDARY_V1_BIOME_ALPINE = 0,
  FOUNDRY_BOUNDARY_V1_BIOME_BOREAL = 1,
  FOUNDRY_BOUNDARY_V1_BIOME_DESERT = 2,
  FOUNDRY_BOUNDARY_V1_BIOME_MEDITERRANEAN = 3,
  FOUNDRY_BOUNDARY_V1_BIOME_TEMPERATE = 4,
  FOUNDRY_BOUNDARY_V1_BIOME_TROPICAL = 5,
  FOUNDRY_BOUNDARY_V1_BIOME_VOLCANIC = 6,
  FOUNDRY_BOUNDARY_V1_BIOME_COUNT = 7
} foundry_boundary_v1_biome;
/* A biome by its name (as a config entry or a figure names it), or -1. */
int foundry_boundary_v1_biome_find(const char *name);

typedef enum foundry_boundary_v1_season {
  FOUNDRY_BOUNDARY_V1_SEASON_WINTER = 0,
  FOUNDRY_BOUNDARY_V1_SEASON_SPRING = 1,
  FOUNDRY_BOUNDARY_V1_SEASON_SUMMER = 2,
  FOUNDRY_BOUNDARY_V1_SEASON_FALL = 3,
  FOUNDRY_BOUNDARY_V1_SEASON_COUNT = 4
} foundry_boundary_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_boundary_v1_season_find(const char *name);

typedef enum foundry_boundary_v1_detail {
  FOUNDRY_BOUNDARY_V1_DETAIL_HIGH = 0,
  FOUNDRY_BOUNDARY_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_BOUNDARY_V1_DETAIL_LOW = 2,
  FOUNDRY_BOUNDARY_V1_DETAIL_COUNT = 3
} foundry_boundary_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_boundary_v1_detail_find(const char *name);

typedef struct foundry_boundary_v1_params {
  foundry_boundary_v1_hull hull;  /* The hull: one entry from config/boundary_v1_hulls/ (a predefined shape, or sectors by direction around a ring); a game gives its own cells (foundry_boundary_v1_spawn_hull). */
  foundry_boundary_v1_biome biome;  /* The biome: its colours, tree forms, and scale. */
  foundry_boundary_v1_season season;
  foundry_boundary_v1_detail detail;  /* How finely: the finest terrain spacing (2, 4, 8 m), the images (64, 32, 16 texels a node), and the dressing's density. */
  float view_distance;  /* How far beyond the region the world reaches. [500, 12000] m */
  float figure_range;  /* How far beyond the rim trees and bushes are figures for the game to fill from its own systems (design/boundary.md, Figures); past it the boundary draws its own woods. 0: it draws them all. [0, 300] m */
  float blend;  /* How far edges blend into each other along the rim. [1, 20] m */
  uint32_t seed;  /* The world's seed. [1, 1000000] */
} foundry_boundary_v1_params;

void foundry_boundary_v1_default_params(foundry_boundary_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_boundary_v1_spawn(foundry_runtime *rt, const foundry_boundary_v1_params *params,
                                  const float transform[16]);
/* A boundary around the game's own region (design/boundary.md): these cells, counterclockwise
 * around it, in place of params' hull entry's (whose sectors still give auto edges their
 * edge by direction). See foundry_region_hull. */
foundry_instance foundry_boundary_v1_spawn_hull(foundry_runtime *rt, const foundry_boundary_v1_params *params,
                                       const float transform[16], const foundry_hull_cell *cells,
                                       uint32_t count);
foundry_status foundry_boundary_v1_set_hull(foundry_runtime *rt, foundry_instance instance, foundry_boundary_v1_hull value);
foundry_status foundry_boundary_v1_set_biome(foundry_runtime *rt, foundry_instance instance, foundry_boundary_v1_biome value);
foundry_status foundry_boundary_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_boundary_v1_season value);
foundry_status foundry_boundary_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_boundary_v1_detail value);
foundry_status foundry_boundary_v1_set_view_distance(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_boundary_v1_set_figure_range(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_boundary_v1_set_blend(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_boundary_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_boundary_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_boundary_v1_result;
foundry_status foundry_boundary_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_boundary_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/bush_v1.json. Do not edit. */
/* bush_v1: A shrub (design/systems.md, Shrubs and ground cover): the game asks for a species, age, and season, and whether its berries are still on, at a position with the local wind; grow makes the shrub, and the animator sways it in the same wind as the trees and flowers. No break interaction. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_bush_v1_species {
  FOUNDRY_BUSH_V1_SPECIES_BLACKBERRY = 0,
  FOUNDRY_BUSH_V1_SPECIES_BLUEBERRY = 1,
  FOUNDRY_BUSH_V1_SPECIES_BOXWOOD = 2,
  FOUNDRY_BUSH_V1_SPECIES_BUTTONBUSH = 3,
  FOUNDRY_BUSH_V1_SPECIES_CREOSOTE = 4,
  FOUNDRY_BUSH_V1_SPECIES_ELDERBERRY = 5,
  FOUNDRY_BUSH_V1_SPECIES_HAZEL = 6,
  FOUNDRY_BUSH_V1_SPECIES_HIBISCUS = 7,
  FOUNDRY_BUSH_V1_SPECIES_HOLLY = 8,
  FOUNDRY_BUSH_V1_SPECIES_JUNIPER = 9,
  FOUNDRY_BUSH_V1_SPECIES_OLEANDER = 10,
  FOUNDRY_BUSH_V1_SPECIES_PALMETTO = 11,
  FOUNDRY_BUSH_V1_SPECIES_RHODODENDRON = 12,
  FOUNDRY_BUSH_V1_SPECIES_SAGEBRUSH = 13,
  FOUNDRY_BUSH_V1_SPECIES_COUNT = 14
} foundry_bush_v1_species;
/* A species by its name (as a config entry or a figure names it), or -1. */
int foundry_bush_v1_species_find(const char *name);

typedef enum foundry_bush_v1_age {
  FOUNDRY_BUSH_V1_AGE_YOUNG = 0,
  FOUNDRY_BUSH_V1_AGE_MATURE = 1,
  FOUNDRY_BUSH_V1_AGE_OLD = 2,
  FOUNDRY_BUSH_V1_AGE_COUNT = 3
} foundry_bush_v1_age;
/* A age by its name (as a config entry or a figure names it), or -1. */
int foundry_bush_v1_age_find(const char *name);

typedef enum foundry_bush_v1_season {
  FOUNDRY_BUSH_V1_SEASON_WINTER = 0,
  FOUNDRY_BUSH_V1_SEASON_SPRING = 1,
  FOUNDRY_BUSH_V1_SEASON_SUMMER = 2,
  FOUNDRY_BUSH_V1_SEASON_FALL = 3,
  FOUNDRY_BUSH_V1_SEASON_COUNT = 4
} foundry_bush_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_bush_v1_season_find(const char *name);

typedef enum foundry_bush_v1_detail {
  FOUNDRY_BUSH_V1_DETAIL_HIGH = 0,
  FOUNDRY_BUSH_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_BUSH_V1_DETAIL_LOW = 2,
  FOUNDRY_BUSH_V1_DETAIL_COUNT = 3
} foundry_bush_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_bush_v1_detail_find(const char *name);

typedef struct foundry_bush_v1_params {
  foundry_bush_v1_species species;  /* A species: one genome from config/bush_v1_genomes/. */
  foundry_bush_v1_age age;
  foundry_bush_v1_season season;
  bool fruit;  /* The shrub still carries its berries; clear it when they are picked. Applies only to species whose fruit can be picked (pickable); elsewhere it is disabled. */
  uint32_t seed;  /* Which of the distinct shrubs per species, age, and season (the runtime caps how many by quality). [1, 1000] */
  foundry_bush_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_bush_v1_params;

void foundry_bush_v1_default_params(foundry_bush_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_bush_v1_spawn(foundry_runtime *rt, const foundry_bush_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_bush_v1_set_species(foundry_runtime *rt, foundry_instance instance, foundry_bush_v1_species value);
foundry_status foundry_bush_v1_set_age(foundry_runtime *rt, foundry_instance instance, foundry_bush_v1_age value);
foundry_status foundry_bush_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_bush_v1_season value);
foundry_status foundry_bush_v1_set_fruit(foundry_runtime *rt, foundry_instance instance, bool value);
foundry_status foundry_bush_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_bush_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_bush_v1_detail value);
foundry_status foundry_bush_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_bush_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_bush_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Whether fruit applies to this species (its pickable is set). Where it
 * doesn't, the instance ignores fruit and shares its build either way. */
bool foundry_bush_v1_fruit_applies(foundry_bush_v1_species species);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_bush_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_bush_v1_result;
foundry_status foundry_bush_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_bush_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/castle_v1.json. Do not edit. */
/* castle_v1: A part of a castle (design/planner.md, Castles): the game asks for a part (a config entry: the keep, a great hall, a chapel, barracks, stables, a kitchen, storehouses, a tower), its lot's size, and the season; build makes it on its lot, and the house animator sways its banners, swings its bell, raises its smoke, and opens its door. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_castle_v1_part {
  FOUNDRY_CASTLE_V1_PART_BARRACKS = 0,
  FOUNDRY_CASTLE_V1_PART_CHAPEL = 1,
  FOUNDRY_CASTLE_V1_PART_GREAT_HALL = 2,
  FOUNDRY_CASTLE_V1_PART_KEEP = 3,
  FOUNDRY_CASTLE_V1_PART_KITCHEN = 4,
  FOUNDRY_CASTLE_V1_PART_STABLES = 5,
  FOUNDRY_CASTLE_V1_PART_STOREHOUSE = 6,
  FOUNDRY_CASTLE_V1_PART_TOWER = 7,
  FOUNDRY_CASTLE_V1_PART_COUNT = 8
} foundry_castle_v1_part;
/* A part by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_part_find(const char *name);

typedef enum foundry_castle_v1_size {
  FOUNDRY_CASTLE_V1_SIZE_SMALL = 0,
  FOUNDRY_CASTLE_V1_SIZE_MEDIUM = 1,
  FOUNDRY_CASTLE_V1_SIZE_LARGE = 2,
  FOUNDRY_CASTLE_V1_SIZE_KEEP = 3,
  FOUNDRY_CASTLE_V1_SIZE_COUNT = 4
} foundry_castle_v1_size;
/* A size by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_size_find(const char *name);

typedef enum foundry_castle_v1_theme {
  FOUNDRY_CASTLE_V1_THEME_DESERT = 0,
  FOUNDRY_CASTLE_V1_THEME_DRUIDIC = 1,
  FOUNDRY_CASTLE_V1_THEME_DWARVEN = 2,
  FOUNDRY_CASTLE_V1_THEME_ELVEN = 3,
  FOUNDRY_CASTLE_V1_THEME_HEARTLAND = 4,
  FOUNDRY_CASTLE_V1_THEME_MARSH = 5,
  FOUNDRY_CASTLE_V1_THEME_NORDIC = 6,
  FOUNDRY_CASTLE_V1_THEME_STONE_DOME = 7,
  FOUNDRY_CASTLE_V1_THEME_COUNT = 8
} foundry_castle_v1_theme;
/* A theme by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_theme_find(const char *name);

typedef enum foundry_castle_v1_stage {
  FOUNDRY_CASTLE_V1_STAGE_PLANNING = 0,
  FOUNDRY_CASTLE_V1_STAGE_HALF = 1,
  FOUNDRY_CASTLE_V1_STAGE_FINISHED = 2,
  FOUNDRY_CASTLE_V1_STAGE_RUIN = 3,
  FOUNDRY_CASTLE_V1_STAGE_COUNT = 4
} foundry_castle_v1_stage;
/* A stage by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_stage_find(const char *name);

typedef enum foundry_castle_v1_season {
  FOUNDRY_CASTLE_V1_SEASON_WINTER = 0,
  FOUNDRY_CASTLE_V1_SEASON_SPRING = 1,
  FOUNDRY_CASTLE_V1_SEASON_SUMMER = 2,
  FOUNDRY_CASTLE_V1_SEASON_FALL = 3,
  FOUNDRY_CASTLE_V1_SEASON_COUNT = 4
} foundry_castle_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_season_find(const char *name);

typedef enum foundry_castle_v1_detail {
  FOUNDRY_CASTLE_V1_DETAIL_HIGH = 0,
  FOUNDRY_CASTLE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_CASTLE_V1_DETAIL_LOW = 2,
  FOUNDRY_CASTLE_V1_DETAIL_COUNT = 3
} foundry_castle_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_castle_v1_detail_find(const char *name);

typedef struct foundry_castle_v1_params {
  foundry_castle_v1_part part;  /* A part: one entry from config/castle_v1_parts/. */
  foundry_castle_v1_size size;  /* Its lot: small (rect6x5), medium (rect8x6), large (rect10x7), or keep (rect8x8). A planner gives the part's own size. */
  foundry_castle_v1_theme theme;  /* The village's theme from config/village_v1_themes/: its roofs, turrets' tops, colours, ornaments, and fires (heartland keeps the part's own). */
  foundry_castle_v1_stage stage;  /* How far it has come: planning (staked out, its trench dug, stone piled), half (walls half up in scaffolding), finished, or a ruin (broken, roofless, overgrown). */
  foundry_castle_v1_season season;
  uint32_t seed;  /* Which of the distinct builds. [1, 1000] */
  foundry_castle_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_castle_v1_params;

void foundry_castle_v1_default_params(foundry_castle_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_castle_v1_spawn(foundry_runtime *rt, const foundry_castle_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_castle_v1_set_part(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_part value);
foundry_status foundry_castle_v1_set_size(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_size value);
foundry_status foundry_castle_v1_set_theme(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_theme value);
foundry_status foundry_castle_v1_set_stage(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_stage value);
foundry_status foundry_castle_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_season value);
foundry_status foundry_castle_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_castle_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_detail value);
foundry_status foundry_castle_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_castle_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_castle_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since its door opened; negative while shut. The game starts it when someone comes through; it opens, stays a moment, and closes. */
foundry_status foundry_castle_v1_start_door(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_castle_v1_result {
  bool door_over;  /* door started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_castle_v1_result;
foundry_status foundry_castle_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_castle_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/civic_v1.json. Do not edit. */
/* civic_v1: A civic building (design/planner.md, Civic buildings): the game asks for a type (a config entry: a church, a temple, a court, a town hall, a guild hall, a library, a bank, a mage's tower, a market hall, or a centre a square's road rings: a well, a market cross, a fountain, a maypole, a statue, a shrine), its lot's size, how grand it is, and the season; build makes it on its lot, and the house animator swings its bells, sways its banners and ribbons, flickers its lamps, and opens its door. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_civic_v1_type {
  FOUNDRY_CIVIC_V1_TYPE_BANK = 0,
  FOUNDRY_CIVIC_V1_TYPE_CHAPEL = 1,
  FOUNDRY_CIVIC_V1_TYPE_CHURCH = 2,
  FOUNDRY_CIVIC_V1_TYPE_COURT = 3,
  FOUNDRY_CIVIC_V1_TYPE_FOUNTAIN = 4,
  FOUNDRY_CIVIC_V1_TYPE_GUILD_HALL = 5,
  FOUNDRY_CIVIC_V1_TYPE_LIBRARY = 6,
  FOUNDRY_CIVIC_V1_TYPE_MAGE_TOWER = 7,
  FOUNDRY_CIVIC_V1_TYPE_MARKET_CROSS = 8,
  FOUNDRY_CIVIC_V1_TYPE_MARKET_HALL = 9,
  FOUNDRY_CIVIC_V1_TYPE_MAYPOLE = 10,
  FOUNDRY_CIVIC_V1_TYPE_SHRINE = 11,
  FOUNDRY_CIVIC_V1_TYPE_STATUE = 12,
  FOUNDRY_CIVIC_V1_TYPE_TEMPLE = 13,
  FOUNDRY_CIVIC_V1_TYPE_TOWN_HALL = 14,
  FOUNDRY_CIVIC_V1_TYPE_WELL = 15,
  FOUNDRY_CIVIC_V1_TYPE_COUNT = 16
} foundry_civic_v1_type;
/* A type by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_type_find(const char *name);

typedef enum foundry_civic_v1_size {
  FOUNDRY_CIVIC_V1_SIZE_CENTRE = 0,
  FOUNDRY_CIVIC_V1_SIZE_SMALL = 1,
  FOUNDRY_CIVIC_V1_SIZE_LARGE = 2,
  FOUNDRY_CIVIC_V1_SIZE_GREAT = 3,
  FOUNDRY_CIVIC_V1_SIZE_COUNT = 4
} foundry_civic_v1_size;
/* A size by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_size_find(const char *name);

typedef enum foundry_civic_v1_theme {
  FOUNDRY_CIVIC_V1_THEME_DESERT = 0,
  FOUNDRY_CIVIC_V1_THEME_DRUIDIC = 1,
  FOUNDRY_CIVIC_V1_THEME_DWARVEN = 2,
  FOUNDRY_CIVIC_V1_THEME_ELVEN = 3,
  FOUNDRY_CIVIC_V1_THEME_HEARTLAND = 4,
  FOUNDRY_CIVIC_V1_THEME_MARSH = 5,
  FOUNDRY_CIVIC_V1_THEME_NORDIC = 6,
  FOUNDRY_CIVIC_V1_THEME_STONE_DOME = 7,
  FOUNDRY_CIVIC_V1_THEME_COUNT = 8
} foundry_civic_v1_theme;
/* A theme by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_theme_find(const char *name);

typedef enum foundry_civic_v1_stage {
  FOUNDRY_CIVIC_V1_STAGE_PLANNING = 0,
  FOUNDRY_CIVIC_V1_STAGE_HALF = 1,
  FOUNDRY_CIVIC_V1_STAGE_FINISHED = 2,
  FOUNDRY_CIVIC_V1_STAGE_RUIN = 3,
  FOUNDRY_CIVIC_V1_STAGE_COUNT = 4
} foundry_civic_v1_stage;
/* A stage by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_stage_find(const char *name);

typedef enum foundry_civic_v1_grandeur {
  FOUNDRY_CIVIC_V1_GRANDEUR_PLAIN = 0,
  FOUNDRY_CIVIC_V1_GRANDEUR_FINE = 1,
  FOUNDRY_CIVIC_V1_GRANDEUR_GRAND = 2,
  FOUNDRY_CIVIC_V1_GRANDEUR_WONDROUS = 3,
  FOUNDRY_CIVIC_V1_GRANDEUR_COUNT = 4
} foundry_civic_v1_grandeur;
/* A grandeur by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_grandeur_find(const char *name);

typedef enum foundry_civic_v1_season {
  FOUNDRY_CIVIC_V1_SEASON_WINTER = 0,
  FOUNDRY_CIVIC_V1_SEASON_SPRING = 1,
  FOUNDRY_CIVIC_V1_SEASON_SUMMER = 2,
  FOUNDRY_CIVIC_V1_SEASON_FALL = 3,
  FOUNDRY_CIVIC_V1_SEASON_COUNT = 4
} foundry_civic_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_season_find(const char *name);

typedef enum foundry_civic_v1_detail {
  FOUNDRY_CIVIC_V1_DETAIL_HIGH = 0,
  FOUNDRY_CIVIC_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_CIVIC_V1_DETAIL_LOW = 2,
  FOUNDRY_CIVIC_V1_DETAIL_COUNT = 3
} foundry_civic_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_civic_v1_detail_find(const char *name);

typedef struct foundry_civic_v1_params {
  foundry_civic_v1_type type;  /* A type: one entry from config/civic_v1_types/. */
  foundry_civic_v1_size size;  /* Its lot: centre (flower7, a square's middle), small (rect6x5), large (rect9x7), or great (rect12x9). A planner gives the type's own size. */
  foundry_civic_v1_theme theme;  /* The village's theme from config/village_v1_themes/: its roofs, plan, walls, windows, ornaments, colours, and fires (heartland keeps the type's own). */
  foundry_civic_v1_stage stage;  /* How far it has come: planning (its lot staked out, its trench dug, stone and timber piled), half (its walls half up in scaffolding), finished, or a ruin (broken, roofless, overgrown). */
  foundry_civic_v1_grandeur grandeur;  /* How grand it is: plain, fine, grand (gilding, statues, braziers), or wondrous (the exotic: twin domed towers crowned with crystals, flying buttresses and a rose window, burning braziers and a gilded figure, a ring of crystals circling a mage's tower, a gilded dome over a well). */
  foundry_civic_v1_season season;
  uint32_t seed;  /* Which of the distinct builds. [1, 1000] */
  foundry_civic_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_civic_v1_params;

void foundry_civic_v1_default_params(foundry_civic_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_civic_v1_spawn(foundry_runtime *rt, const foundry_civic_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_civic_v1_set_type(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_type value);
foundry_status foundry_civic_v1_set_size(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_size value);
foundry_status foundry_civic_v1_set_theme(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_theme value);
foundry_status foundry_civic_v1_set_stage(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_stage value);
foundry_status foundry_civic_v1_set_grandeur(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_grandeur value);
foundry_status foundry_civic_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_season value);
foundry_status foundry_civic_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_civic_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_detail value);
foundry_status foundry_civic_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_civic_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_civic_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since its door opened; negative while shut. The game starts it when someone comes through; it opens, stays a moment, and closes. */
foundry_status foundry_civic_v1_start_door(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_civic_v1_result {
  bool door_over;  /* door started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_civic_v1_result;
foundry_status foundry_civic_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_civic_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/crop_v1.json. Do not edit. */
/* crop_v1: A crop field (design/planner.md, Crops): the game asks for a crop (a config entry: grain, maize, sunflowers, cabbages, lettuces, carrots, potatoes, onions, pumpkins, beans, tomatoes, grapes, lavender, flax), its lot's size, how far it has grown, the season, and how many wind groups its plants share; build sows it in rows, and the house animator moves each wind group in the gusts, so the field poses with a handful of wind computations. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_crop_v1_type {
  FOUNDRY_CROP_V1_TYPE_BARLEY = 0,
  FOUNDRY_CROP_V1_TYPE_BEANS = 1,
  FOUNDRY_CROP_V1_TYPE_CABBAGE = 2,
  FOUNDRY_CROP_V1_TYPE_CARROT = 3,
  FOUNDRY_CROP_V1_TYPE_FLAX = 4,
  FOUNDRY_CROP_V1_TYPE_GRAPES = 5,
  FOUNDRY_CROP_V1_TYPE_LAVENDER = 6,
  FOUNDRY_CROP_V1_TYPE_LETTUCE = 7,
  FOUNDRY_CROP_V1_TYPE_MAIZE = 8,
  FOUNDRY_CROP_V1_TYPE_OATS = 9,
  FOUNDRY_CROP_V1_TYPE_ONION = 10,
  FOUNDRY_CROP_V1_TYPE_POTATO = 11,
  FOUNDRY_CROP_V1_TYPE_PUMPKIN = 12,
  FOUNDRY_CROP_V1_TYPE_SUNFLOWER = 13,
  FOUNDRY_CROP_V1_TYPE_TOMATO = 14,
  FOUNDRY_CROP_V1_TYPE_WHEAT = 15,
  FOUNDRY_CROP_V1_TYPE_COUNT = 16
} foundry_crop_v1_type;
/* A type by its name (as a config entry or a figure names it), or -1. */
int foundry_crop_v1_type_find(const char *name);

typedef enum foundry_crop_v1_size {
  FOUNDRY_CROP_V1_SIZE_SMALL = 0,
  FOUNDRY_CROP_V1_SIZE_MEDIUM = 1,
  FOUNDRY_CROP_V1_SIZE_LARGE = 2,
  FOUNDRY_CROP_V1_SIZE_COUNT = 3
} foundry_crop_v1_size;
/* A size by its name (as a config entry or a figure names it), or -1. */
int foundry_crop_v1_size_find(const char *name);

typedef enum foundry_crop_v1_growth {
  FOUNDRY_CROP_V1_GROWTH_SOWN = 0,
  FOUNDRY_CROP_V1_GROWTH_SPROUTING = 1,
  FOUNDRY_CROP_V1_GROWTH_GROWING = 2,
  FOUNDRY_CROP_V1_GROWTH_RIPE = 3,
  FOUNDRY_CROP_V1_GROWTH_HARVESTED = 4,
  FOUNDRY_CROP_V1_GROWTH_COUNT = 5
} foundry_crop_v1_growth;
/* A growth by its name (as a config entry or a figure names it), or -1. */
int foundry_crop_v1_growth_find(const char *name);

typedef enum foundry_crop_v1_season {
  FOUNDRY_CROP_V1_SEASON_WINTER = 0,
  FOUNDRY_CROP_V1_SEASON_SPRING = 1,
  FOUNDRY_CROP_V1_SEASON_SUMMER = 2,
  FOUNDRY_CROP_V1_SEASON_FALL = 3,
  FOUNDRY_CROP_V1_SEASON_COUNT = 4
} foundry_crop_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_crop_v1_season_find(const char *name);

typedef enum foundry_crop_v1_detail {
  FOUNDRY_CROP_V1_DETAIL_HIGH = 0,
  FOUNDRY_CROP_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_CROP_V1_DETAIL_LOW = 2,
  FOUNDRY_CROP_V1_DETAIL_COUNT = 3
} foundry_crop_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_crop_v1_detail_find(const char *name);

typedef struct foundry_crop_v1_params {
  foundry_crop_v1_type type;  /* The crop: one entry from config/crop_v1_types/. */
  foundry_crop_v1_size size;  /* Its lot: small (rect6x4), medium (rect8x6), or large (rect12x8). */
  foundry_crop_v1_growth growth;  /* How far it has grown: sown (furrows), sprouting, growing, ripe, or harvested (stubble, and stooks of sheaves for grain). */
  foundry_crop_v1_season season;
  uint32_t wind_groups;  /* How many wind groups its plants share: the wind is computed this many times a frame, however many plants. [1, 16] */
  uint32_t seed;  /* Which of the distinct fields. [1, 1000] */
  foundry_crop_v1_detail detail;  /* Level of detail: thins the plants along each row (every second, every fourth) and drops their finer parts. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_crop_v1_params;

void foundry_crop_v1_default_params(foundry_crop_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_crop_v1_spawn(foundry_runtime *rt, const foundry_crop_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_crop_v1_set_type(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_type value);
foundry_status foundry_crop_v1_set_size(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_size value);
foundry_status foundry_crop_v1_set_growth(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_growth value);
foundry_status foundry_crop_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_season value);
foundry_status foundry_crop_v1_set_wind_groups(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_crop_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_crop_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_detail value);
foundry_status foundry_crop_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_crop_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_crop_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_crop_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_crop_v1_result;
foundry_status foundry_crop_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_crop_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/fence_v1.json. Do not edit. */
/* fence_v1: A fence network (design/planner.md, Walls, fences, and gates): the game gives a kind (a config entry: pickets, rails, split rails, wattle, a hedge, a low stone wall, iron railings) and a network on the hex grid's diagonal lattice, and build makes the whole fence (or one chunk of it) with a post at every node and a swinging gate in each opening; the house animator swings its gates and stirs its hedges. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_fence_v1_kind {
  FOUNDRY_FENCE_V1_KIND_HEDGE = 0,
  FOUNDRY_FENCE_V1_KIND_IRON = 1,
  FOUNDRY_FENCE_V1_KIND_LIVING_WILLOW = 2,
  FOUNDRY_FENCE_V1_KIND_LOW_WALL = 3,
  FOUNDRY_FENCE_V1_KIND_PICKET = 4,
  FOUNDRY_FENCE_V1_KIND_RAIL = 5,
  FOUNDRY_FENCE_V1_KIND_REED = 6,
  FOUNDRY_FENCE_V1_KIND_SPLIT_RAIL = 7,
  FOUNDRY_FENCE_V1_KIND_STONE_SLAB = 8,
  FOUNDRY_FENCE_V1_KIND_WATTLE = 9,
  FOUNDRY_FENCE_V1_KIND_COUNT = 10
} foundry_fence_v1_kind;
/* A kind by its name (as a config entry or a figure names it), or -1. */
int foundry_fence_v1_kind_find(const char *name);

typedef enum foundry_fence_v1_condition {
  FOUNDRY_FENCE_V1_CONDITION_INTACT = 0,
  FOUNDRY_FENCE_V1_CONDITION_RUIN = 1,
  FOUNDRY_FENCE_V1_CONDITION_COUNT = 2
} foundry_fence_v1_condition;
/* A condition by its name (as a config entry or a figure names it), or -1. */
int foundry_fence_v1_condition_find(const char *name);

typedef enum foundry_fence_v1_network {
  FOUNDRY_FENCE_V1_NETWORK_BEND_GENTLE = 0,
  FOUNDRY_FENCE_V1_NETWORK_BEND_SHARP = 1,
  FOUNDRY_FENCE_V1_NETWORK_BRANCH = 2,
  FOUNDRY_FENCE_V1_NETWORK_COMPOUND = 3,
  FOUNDRY_FENCE_V1_NETWORK_CROSS = 4,
  FOUNDRY_FENCE_V1_NETWORK_PADDOCKS = 5,
  FOUNDRY_FENCE_V1_NETWORK_RING = 6,
  FOUNDRY_FENCE_V1_NETWORK_SLOPE = 7,
  FOUNDRY_FENCE_V1_NETWORK_STAR = 8,
  FOUNDRY_FENCE_V1_NETWORK_STRAIGHT = 9,
  FOUNDRY_FENCE_V1_NETWORK_WYE = 10,
  FOUNDRY_FENCE_V1_NETWORK_COUNT = 11
} foundry_fence_v1_network;
/* A network by its name (as a config entry or a figure names it), or -1. */
int foundry_fence_v1_network_find(const char *name);

typedef enum foundry_fence_v1_season {
  FOUNDRY_FENCE_V1_SEASON_WINTER = 0,
  FOUNDRY_FENCE_V1_SEASON_SPRING = 1,
  FOUNDRY_FENCE_V1_SEASON_SUMMER = 2,
  FOUNDRY_FENCE_V1_SEASON_FALL = 3,
  FOUNDRY_FENCE_V1_SEASON_COUNT = 4
} foundry_fence_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_fence_v1_season_find(const char *name);

typedef enum foundry_fence_v1_detail {
  FOUNDRY_FENCE_V1_DETAIL_HIGH = 0,
  FOUNDRY_FENCE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_FENCE_V1_DETAIL_LOW = 2,
  FOUNDRY_FENCE_V1_DETAIL_COUNT = 3
} foundry_fence_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_fence_v1_detail_find(const char *name);

typedef struct foundry_fence_v1_params {
  foundry_fence_v1_kind kind;  /* A kind: one entry from config/fence_v1_kinds/. */
  foundry_fence_v1_condition condition;  /* Intact, or a ruin: broken and leaning, its gates gone, overgrown. */
  foundry_fence_v1_network network;  /* The network to build: an example from config/line_v1_networks/, or the game's own (a planner's line). */
  uint32_t chunks;  /* How many chunks to split it into, by angle round its middle (0 or 1: all of it). [0, 64] */
  uint32_t chunk;  /* Which chunk to build. [0, 63] */
  foundry_fence_v1_season season;
  uint32_t seed;  /* Which of the distinct builds. [1, 1000] */
  foundry_fence_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_fence_v1_params;

void foundry_fence_v1_default_params(foundry_fence_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_fence_v1_spawn(foundry_runtime *rt, const foundry_fence_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_fence_v1_set_kind(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_kind value);
foundry_status foundry_fence_v1_set_condition(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_condition value);
foundry_status foundry_fence_v1_set_network(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_network value);
foundry_status foundry_fence_v1_set_chunks(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_fence_v1_set_chunk(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_fence_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_season value);
foundry_status foundry_fence_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_fence_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_detail value);
foundry_status foundry_fence_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_fence_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_fence_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since the gate opened; negative while shut. The game starts it when someone comes through; it opens, stays a moment, and closes. */
foundry_status foundry_fence_v1_start_door(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_fence_v1_result {
  bool door_over;  /* door started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_fence_v1_result;
foundry_status foundry_fence_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_fence_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/flower_v1.json. Do not edit. */
/* flower_v1: A clump of flowers (design/systems.md, Mushrooms and flowers): the game asks for a species and a season at a position with the local wind; grow makes the clump, in flower, in bud, or gone to seed, and the animator sways it in the same wind as the trees. Ground cover: no conformance, purely visual. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_flower_v1_species {
  FOUNDRY_FLOWER_V1_SPECIES_BIRDOFPARADISE = 0,
  FOUNDRY_FLOWER_V1_SPECIES_BLUEBELL = 1,
  FOUNDRY_FLOWER_V1_SPECIES_CATTAIL = 2,
  FOUNDRY_FLOWER_V1_SPECIES_CONEFLOWER = 3,
  FOUNDRY_FLOWER_V1_SPECIES_DAFFODIL = 4,
  FOUNDRY_FLOWER_V1_SPECIES_DAISY = 5,
  FOUNDRY_FLOWER_V1_SPECIES_EDELWEISS = 6,
  FOUNDRY_FLOWER_V1_SPECIES_FOXGLOVE = 7,
  FOUNDRY_FLOWER_V1_SPECIES_IRIS = 8,
  FOUNDRY_FLOWER_V1_SPECIES_LAVENDER = 9,
  FOUNDRY_FLOWER_V1_SPECIES_LUPINE = 10,
  FOUNDRY_FLOWER_V1_SPECIES_MARIGOLD = 11,
  FOUNDRY_FLOWER_V1_SPECIES_POPPY = 12,
  FOUNDRY_FLOWER_V1_SPECIES_SUNFLOWER = 13,
  FOUNDRY_FLOWER_V1_SPECIES_TULIP = 14,
  FOUNDRY_FLOWER_V1_SPECIES_COUNT = 15
} foundry_flower_v1_species;
/* A species by its name (as a config entry or a figure names it), or -1. */
int foundry_flower_v1_species_find(const char *name);

typedef enum foundry_flower_v1_season {
  FOUNDRY_FLOWER_V1_SEASON_WINTER = 0,
  FOUNDRY_FLOWER_V1_SEASON_SPRING = 1,
  FOUNDRY_FLOWER_V1_SEASON_SUMMER = 2,
  FOUNDRY_FLOWER_V1_SEASON_FALL = 3,
  FOUNDRY_FLOWER_V1_SEASON_COUNT = 4
} foundry_flower_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_flower_v1_season_find(const char *name);

typedef enum foundry_flower_v1_detail {
  FOUNDRY_FLOWER_V1_DETAIL_HIGH = 0,
  FOUNDRY_FLOWER_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_FLOWER_V1_DETAIL_LOW = 2,
  FOUNDRY_FLOWER_V1_DETAIL_COUNT = 3
} foundry_flower_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_flower_v1_detail_find(const char *name);

typedef struct foundry_flower_v1_params {
  foundry_flower_v1_species species;  /* A species: one genome from config/flower_v1_genomes/. */
  foundry_flower_v1_season season;
  uint32_t seed;  /* Which of the distinct clumps per species and season (the runtime caps how many by quality). [1, 1000] */
  foundry_flower_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_flower_v1_params;

void foundry_flower_v1_default_params(foundry_flower_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_flower_v1_spawn(foundry_runtime *rt, const foundry_flower_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_flower_v1_set_species(foundry_runtime *rt, foundry_instance instance, foundry_flower_v1_species value);
foundry_status foundry_flower_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_flower_v1_season value);
foundry_status foundry_flower_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_flower_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_flower_v1_detail value);
foundry_status foundry_flower_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_flower_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_flower_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_flower_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_flower_v1_result;
foundry_status foundry_flower_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_flower_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/gate_v1.json. Do not edit. */
/* gate_v1: A gate in a wall (design/planner.md, Gates): the game asks for a type (a config entry: a palisade's gate, a town gate's arch, a gatehouse, a tower gate, a postern), the wall's kind it stands in (so it matches), and the opening's width (one a wall's model reports); build makes it across the opening, and the house animator opens its doors, sways its banners, and flickers its torches. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_gate_v1_type {
  FOUNDRY_GATE_V1_TYPE_GATEHOUSE = 0,
  FOUNDRY_GATE_V1_TYPE_POSTERN = 1,
  FOUNDRY_GATE_V1_TYPE_TOWER_GATE = 2,
  FOUNDRY_GATE_V1_TYPE_TOWN_GATE = 3,
  FOUNDRY_GATE_V1_TYPE_WOODEN_GATE = 4,
  FOUNDRY_GATE_V1_TYPE_COUNT = 5
} foundry_gate_v1_type;
/* A type by its name (as a config entry or a figure names it), or -1. */
int foundry_gate_v1_type_find(const char *name);

typedef enum foundry_gate_v1_wall {
  FOUNDRY_GATE_V1_WALL_ADOBE_WALL = 0,
  FOUNDRY_GATE_V1_WALL_BRICK_WALL = 1,
  FOUNDRY_GATE_V1_WALL_CURTAIN = 2,
  FOUNDRY_GATE_V1_WALL_DRYSTONE_WALL = 3,
  FOUNDRY_GATE_V1_WALL_DWARF_RAMPART = 4,
  FOUNDRY_GATE_V1_WALL_EARTH_BANK = 5,
  FOUNDRY_GATE_V1_WALL_KEEP_WALL = 6,
  FOUNDRY_GATE_V1_WALL_LIVING_WALL = 7,
  FOUNDRY_GATE_V1_WALL_PALISADE = 8,
  FOUNDRY_GATE_V1_WALL_STONE_CIRCLE = 9,
  FOUNDRY_GATE_V1_WALL_TOWN_WALL = 10,
  FOUNDRY_GATE_V1_WALL_COUNT = 11
} foundry_gate_v1_wall;
/* A wall by its name (as a config entry or a figure names it), or -1. */
int foundry_gate_v1_wall_find(const char *name);

typedef enum foundry_gate_v1_condition {
  FOUNDRY_GATE_V1_CONDITION_INTACT = 0,
  FOUNDRY_GATE_V1_CONDITION_RUIN = 1,
  FOUNDRY_GATE_V1_CONDITION_COUNT = 2
} foundry_gate_v1_condition;
/* A condition by its name (as a config entry or a figure names it), or -1. */
int foundry_gate_v1_condition_find(const char *name);

typedef enum foundry_gate_v1_season {
  FOUNDRY_GATE_V1_SEASON_WINTER = 0,
  FOUNDRY_GATE_V1_SEASON_SPRING = 1,
  FOUNDRY_GATE_V1_SEASON_SUMMER = 2,
  FOUNDRY_GATE_V1_SEASON_FALL = 3,
  FOUNDRY_GATE_V1_SEASON_COUNT = 4
} foundry_gate_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_gate_v1_season_find(const char *name);

typedef enum foundry_gate_v1_detail {
  FOUNDRY_GATE_V1_DETAIL_HIGH = 0,
  FOUNDRY_GATE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_GATE_V1_DETAIL_LOW = 2,
  FOUNDRY_GATE_V1_DETAIL_COUNT = 3
} foundry_gate_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_gate_v1_detail_find(const char *name);

typedef struct foundry_gate_v1_params {
  foundry_gate_v1_type type;  /* A type: one entry from config/gate_v1_types/. */
  foundry_gate_v1_wall wall;  /* The wall it stands in: one entry from config/wall_v1_kinds/. */
  foundry_gate_v1_condition condition;  /* Intact, or a ruin: its doors and portcullis gone, broken down, overgrown. */
  float width;  /* The opening along the wall's line (a wall model's gate_widths: 3 m, or more where openings merge). [0.8, 12] m */
  foundry_gate_v1_season season;
  uint32_t seed;  /* Which of the distinct builds. [1, 1000] */
  foundry_gate_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_gate_v1_params;

void foundry_gate_v1_default_params(foundry_gate_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_gate_v1_spawn(foundry_runtime *rt, const foundry_gate_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_gate_v1_set_type(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_type value);
foundry_status foundry_gate_v1_set_wall(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_wall value);
foundry_status foundry_gate_v1_set_condition(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_condition value);
foundry_status foundry_gate_v1_set_width(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_gate_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_season value);
foundry_status foundry_gate_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_gate_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_detail value);
foundry_status foundry_gate_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_gate_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_gate_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since its doors opened; negative while shut. The game starts it when someone comes through; it opens, stays a moment, and closes. */
foundry_status foundry_gate_v1_start_door(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_gate_v1_result {
  bool door_over;  /* door started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_gate_v1_result;
foundry_status foundry_gate_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_gate_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/grass_v1.json. Do not edit. */
/* grass_v1: Grass as a field (design/plotter.md, "Fields"): a plotter covers a region with it, one tile per hex cell. cell is the per-cell functor (grass_cell_v1): from the cell's coordinates, ground, moisture, and the world's seed, which tile it shows, turned, scaled, and tinted; tile (grass_tile_v1) grows that tile, shared by every cell that picks it. The runtime runs the functor over a field's cells and draws the few distinct tiles instanced, swaying in the wind on the GPU; one cell as an instance (here, and in the stress tool) is the same pipeline. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_grass_v1_species {
  FOUNDRY_GRASS_V1_SPECIES_DUNE = 0,
  FOUNDRY_GRASS_V1_SPECIES_LAWN = 1,
  FOUNDRY_GRASS_V1_SPECIES_MEADOW = 2,
  FOUNDRY_GRASS_V1_SPECIES_PRAIRIE = 3,
  FOUNDRY_GRASS_V1_SPECIES_SEDGE = 4,
  FOUNDRY_GRASS_V1_SPECIES_COUNT = 5
} foundry_grass_v1_species;
/* A species by its name (as a config entry or a figure names it), or -1. */
int foundry_grass_v1_species_find(const char *name);

typedef enum foundry_grass_v1_season {
  FOUNDRY_GRASS_V1_SEASON_WINTER = 0,
  FOUNDRY_GRASS_V1_SEASON_SPRING = 1,
  FOUNDRY_GRASS_V1_SEASON_SUMMER = 2,
  FOUNDRY_GRASS_V1_SEASON_FALL = 3,
  FOUNDRY_GRASS_V1_SEASON_COUNT = 4
} foundry_grass_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_grass_v1_season_find(const char *name);

typedef enum foundry_grass_v1_detail {
  FOUNDRY_GRASS_V1_DETAIL_HIGH = 0,
  FOUNDRY_GRASS_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_GRASS_V1_DETAIL_LOW = 2,
  FOUNDRY_GRASS_V1_DETAIL_MAT = 3,
  FOUNDRY_GRASS_V1_DETAIL_COUNT = 4
} foundry_grass_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_grass_v1_detail_find(const char *name);

typedef enum foundry_grass_v1_ground {
  FOUNDRY_GRASS_V1_GROUND_SOIL = 0,
  FOUNDRY_GRASS_V1_GROUND_WATER = 1,
  FOUNDRY_GRASS_V1_GROUND_ROCK = 2,
  FOUNDRY_GRASS_V1_GROUND_SAND = 3,
  FOUNDRY_GRASS_V1_GROUND_PATH = 4,
  FOUNDRY_GRASS_V1_GROUND_COUNT = 5
} foundry_grass_v1_ground;
/* A ground by its name (as a config entry or a figure names it), or -1. */
int foundry_grass_v1_ground_find(const char *name);

typedef struct foundry_grass_v1_params {
  foundry_grass_v1_species species;  /* A grass: one genome from config/grass_v1_genomes/. */
  foundry_grass_v1_season season;
  uint32_t variant;  /* Which tile: the grass variants, then the plates (water, rock, sand, path, bare soil). In a field, the cell's choice sets it. [0, 12] */
  foundry_grass_v1_detail detail;  /* The finest level of detail the game allows: blades, fewer wider blades, tufts, a mat. The runtime picks coarser levels by size on screen. */
  uint32_t cell_q;  /* The cell's axial coordinate q (a field passes a negative q as its two's-complement bits). [0, 1000] */
  uint32_t cell_r;  /* The cell's axial coordinate r (as cell_q). [0, 1000] */
  foundry_grass_v1_ground ground;  /* The cell's ground. */
  float moisture;  /* The cell's moisture: wetter grass grows taller and greener. [0, 1] */
  uint32_t world_seed;  /* The world's seed: the same cell in another world chooses differently. [1, 1000000] */
  float wind_speed;  /* The local wind; a field's blades sway on the GPU. [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
} foundry_grass_v1_params;

void foundry_grass_v1_default_params(foundry_grass_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_grass_v1_spawn(foundry_runtime *rt, const foundry_grass_v1_params *params,
                                  const float transform[16]);
/* A field (design/plotter.md, "Fields"): one instance covering these cells, each showing the tile
 * the pipeline's cell stage chooses for it. params' cell_q, cell_r, ground, moisture,
 * and variant are each cell's own and ignored here; world_seed is the world's. */
foundry_instance foundry_grass_v1_spawn_field(foundry_runtime *rt, const foundry_grass_v1_params *params,
                                        const float transform[16], const foundry_field_cell *cells,
                                        uint32_t count);
foundry_status foundry_grass_v1_set_species(foundry_runtime *rt, foundry_instance instance, foundry_grass_v1_species value);
foundry_status foundry_grass_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_grass_v1_season value);
foundry_status foundry_grass_v1_set_variant(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_grass_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_grass_v1_detail value);
foundry_status foundry_grass_v1_set_cell_q(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_grass_v1_set_cell_r(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_grass_v1_set_ground(foundry_runtime *rt, foundry_instance instance, foundry_grass_v1_ground value);
foundry_status foundry_grass_v1_set_moisture(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_grass_v1_set_world_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_grass_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_grass_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_grass_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_grass_v1_result;
foundry_status foundry_grass_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_grass_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/house_v1.json. Do not edit. */
/* house_v1: A village house, store, or workshop, outside only (design/systems.md, Village houses): the game asks for a type, a stage of its owner's upgrades (construction, basic, deluxe, premium), a season, a household (solo, family, or extended, which sizes the lot), and a lot shape, with the local wind; build makes the building on its lot with its sign, yard, and moving parts, and the animator sways, spins, smokes, flickers, and opens the door when the game says. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_house_v1_type {
  FOUNDRY_HOUSE_V1_TYPE_ALCHEMIST = 0,
  FOUNDRY_HOUSE_V1_TYPE_BAKERY = 1,
  FOUNDRY_HOUSE_V1_TYPE_CARPENTER = 2,
  FOUNDRY_HOUSE_V1_TYPE_COTTAGE = 3,
  FOUNDRY_HOUSE_V1_TYPE_FARMHOUSE = 4,
  FOUNDRY_HOUSE_V1_TYPE_GENERAL_STORE = 5,
  FOUNDRY_HOUSE_V1_TYPE_INN = 6,
  FOUNDRY_HOUSE_V1_TYPE_MILL = 7,
  FOUNDRY_HOUSE_V1_TYPE_SMITHY = 8,
  FOUNDRY_HOUSE_V1_TYPE_TAILOR = 9,
  FOUNDRY_HOUSE_V1_TYPE_COUNT = 10
} foundry_house_v1_type;
/* A type by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_type_find(const char *name);

typedef enum foundry_house_v1_palette {
  FOUNDRY_HOUSE_V1_PALETTE_AUTUMN = 0,
  FOUNDRY_HOUSE_V1_PALETTE_BLACK = 1,
  FOUNDRY_HOUSE_V1_PALETTE_BLUE = 2,
  FOUNDRY_HOUSE_V1_PALETTE_GREEN = 3,
  FOUNDRY_HOUSE_V1_PALETTE_NATURAL = 4,
  FOUNDRY_HOUSE_V1_PALETTE_NORTHERN = 5,
  FOUNDRY_HOUSE_V1_PALETTE_ORANGE = 6,
  FOUNDRY_HOUSE_V1_PALETTE_PURPLE = 7,
  FOUNDRY_HOUSE_V1_PALETTE_RED = 8,
  FOUNDRY_HOUSE_V1_PALETTE_WHITE = 9,
  FOUNDRY_HOUSE_V1_PALETTE_WHITEWASH = 10,
  FOUNDRY_HOUSE_V1_PALETTE_YELLOW = 11,
  FOUNDRY_HOUSE_V1_PALETTE_COUNT = 12
} foundry_house_v1_palette;
/* A palette by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_palette_find(const char *name);

typedef enum foundry_house_v1_theme {
  FOUNDRY_HOUSE_V1_THEME_DESERT = 0,
  FOUNDRY_HOUSE_V1_THEME_DRUIDIC = 1,
  FOUNDRY_HOUSE_V1_THEME_DWARVEN = 2,
  FOUNDRY_HOUSE_V1_THEME_ELVEN = 3,
  FOUNDRY_HOUSE_V1_THEME_HEARTLAND = 4,
  FOUNDRY_HOUSE_V1_THEME_MARSH = 5,
  FOUNDRY_HOUSE_V1_THEME_NORDIC = 6,
  FOUNDRY_HOUSE_V1_THEME_STONE_DOME = 7,
  FOUNDRY_HOUSE_V1_THEME_COUNT = 8
} foundry_house_v1_theme;
/* A theme by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_theme_find(const char *name);

typedef enum foundry_house_v1_stage {
  FOUNDRY_HOUSE_V1_STAGE_CONSTRUCTION = 0,
  FOUNDRY_HOUSE_V1_STAGE_BASIC = 1,
  FOUNDRY_HOUSE_V1_STAGE_DELUXE = 2,
  FOUNDRY_HOUSE_V1_STAGE_PREMIUM = 3,
  FOUNDRY_HOUSE_V1_STAGE_RUIN = 4,
  FOUNDRY_HOUSE_V1_STAGE_COUNT = 5
} foundry_house_v1_stage;
/* A stage by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_stage_find(const char *name);

typedef enum foundry_house_v1_season {
  FOUNDRY_HOUSE_V1_SEASON_WINTER = 0,
  FOUNDRY_HOUSE_V1_SEASON_SPRING = 1,
  FOUNDRY_HOUSE_V1_SEASON_SUMMER = 2,
  FOUNDRY_HOUSE_V1_SEASON_FALL = 3,
  FOUNDRY_HOUSE_V1_SEASON_COUNT = 4
} foundry_house_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_season_find(const char *name);

typedef enum foundry_house_v1_household {
  FOUNDRY_HOUSE_V1_HOUSEHOLD_SOLO = 0,
  FOUNDRY_HOUSE_V1_HOUSEHOLD_FAMILY = 1,
  FOUNDRY_HOUSE_V1_HOUSEHOLD_EXTENDED = 2,
  FOUNDRY_HOUSE_V1_HOUSEHOLD_COUNT = 3
} foundry_house_v1_household;
/* A household by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_household_find(const char *name);

typedef enum foundry_house_v1_footprint {
  FOUNDRY_HOUSE_V1_FOOTPRINT_USUAL = 0,
  FOUNDRY_HOUSE_V1_FOOTPRINT_ALTERNATE = 1,
  FOUNDRY_HOUSE_V1_FOOTPRINT_COUNT = 2
} foundry_house_v1_footprint;
/* A footprint by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_footprint_find(const char *name);

typedef enum foundry_house_v1_detail {
  FOUNDRY_HOUSE_V1_DETAIL_HIGH = 0,
  FOUNDRY_HOUSE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_HOUSE_V1_DETAIL_LOW = 2,
  FOUNDRY_HOUSE_V1_DETAIL_COUNT = 3
} foundry_house_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_house_v1_detail_find(const char *name);

typedef struct foundry_house_v1_params {
  foundry_house_v1_type type;  /* A type: one entry from config/house_v1_types/. */
  foundry_house_v1_palette palette;  /* A colour scheme from config/house_v1_palettes/, mixed over the type's colours part by part: a player's colour or a village's theme (natural keeps the type's own). */
  foundry_house_v1_theme theme;  /* The village's theme from config/village_v1_themes/: its roofs, plan, walls, windows, foundation, ornaments, colours, and fires (heartland keeps the type's own). */
  foundry_house_v1_stage stage;  /* How far its owner has come, or a ruin (a premium house fallen to wreck: roofless, broken, overgrown). Every stage stands on the same footprint. */
  foundry_house_v1_season season;
  foundry_house_v1_household household;  /* Who lives or works there, which sets the lot's size: solo, family, or extended (several generations). */
  foundry_house_v1_footprint footprint;  /* The type's usual lot shape (wide, deep, round, long, or broad) or its other one. */
  uint32_t seed;  /* Which of the distinct buildings per type, stage, and season (the runtime caps how many by quality). [1, 1000] */
  foundry_house_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_house_v1_params;

void foundry_house_v1_default_params(foundry_house_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_house_v1_spawn(foundry_runtime *rt, const foundry_house_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_house_v1_set_type(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_type value);
foundry_status foundry_house_v1_set_palette(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_palette value);
foundry_status foundry_house_v1_set_theme(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_theme value);
foundry_status foundry_house_v1_set_stage(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_stage value);
foundry_status foundry_house_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_season value);
foundry_status foundry_house_v1_set_household(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_household value);
foundry_status foundry_house_v1_set_footprint(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_footprint value);
foundry_status foundry_house_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_house_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_detail value);
foundry_status foundry_house_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_house_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_house_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since the door opened; negative while it is shut. The game starts it when someone comes to the door (foundry_house_v1_start_door); it opens, stays for a visit, and closes. */
foundry_status foundry_house_v1_start_door(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_house_v1_result {
  bool door_over;  /* door started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_house_v1_result;
foundry_status foundry_house_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_house_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/road_v1.json. Do not edit. */
/* road_v1: A road cell (design/planner.md, Roads): the game asks for a kind (a config entry: lane, street, high street, market, square, alley, bridge, quay, gate), what its six neighbours are (a mask), which half of the hex an edge piece takes (a stall, a bench, crates, a planter, a cart; it leaves every way through open), which edges a fence runs along (left clear for fence_v1), its role, and the season; build paves it and puts what stands at its edges by the kind's rules (verges, planters and crates against buildings, lamp and sign posts, quays), and the house animator sways awnings and flickers lanterns. A road never builds a fence or a centre. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_road_v1_kind {
  FOUNDRY_ROAD_V1_KIND_ALLEY = 0,
  FOUNDRY_ROAD_V1_KIND_BOARDWALK = 1,
  FOUNDRY_ROAD_V1_KIND_BRIDGE = 2,
  FOUNDRY_ROAD_V1_KIND_DRYSTONE_WAY = 3,
  FOUNDRY_ROAD_V1_KIND_ELVEN_WAY = 4,
  FOUNDRY_ROAD_V1_KIND_GATE = 5,
  FOUNDRY_ROAD_V1_KIND_HIGH_STREET = 6,
  FOUNDRY_ROAD_V1_KIND_LANE = 7,
  FOUNDRY_ROAD_V1_KIND_MARKET = 8,
  FOUNDRY_ROAD_V1_KIND_MOSS_PATH = 9,
  FOUNDRY_ROAD_V1_KIND_QUAY = 10,
  FOUNDRY_ROAD_V1_KIND_SAND_LANE = 11,
  FOUNDRY_ROAD_V1_KIND_SQUARE = 12,
  FOUNDRY_ROAD_V1_KIND_STREET = 13,
  FOUNDRY_ROAD_V1_KIND_COUNT = 14
} foundry_road_v1_kind;
/* A kind by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_kind_find(const char *name);

typedef enum foundry_road_v1_condition {
  FOUNDRY_ROAD_V1_CONDITION_INTACT = 0,
  FOUNDRY_ROAD_V1_CONDITION_RUIN = 1,
  FOUNDRY_ROAD_V1_CONDITION_COUNT = 2
} foundry_road_v1_condition;
/* A condition by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_condition_find(const char *name);

typedef enum foundry_road_v1_half {
  FOUNDRY_ROAD_V1_HALF_E0_1 = 0,
  FOUNDRY_ROAD_V1_HALF_E1_2 = 1,
  FOUNDRY_ROAD_V1_HALF_E2_3 = 2,
  FOUNDRY_ROAD_V1_HALF_E3_4 = 3,
  FOUNDRY_ROAD_V1_HALF_E4_5 = 4,
  FOUNDRY_ROAD_V1_HALF_E5_0 = 5,
  FOUNDRY_ROAD_V1_HALF_NONE = 6,
  FOUNDRY_ROAD_V1_HALF_COUNT = 7
} foundry_road_v1_half;
/* A half by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_half_find(const char *name);

typedef enum foundry_road_v1_role {
  FOUNDRY_ROAD_V1_ROLE_PATH = 0,
  FOUNDRY_ROAD_V1_ROLE_BRIDGE = 1,
  FOUNDRY_ROAD_V1_ROLE_COUNT = 2
} foundry_road_v1_role;
/* A role by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_role_find(const char *name);

typedef enum foundry_road_v1_season {
  FOUNDRY_ROAD_V1_SEASON_WINTER = 0,
  FOUNDRY_ROAD_V1_SEASON_SPRING = 1,
  FOUNDRY_ROAD_V1_SEASON_SUMMER = 2,
  FOUNDRY_ROAD_V1_SEASON_FALL = 3,
  FOUNDRY_ROAD_V1_SEASON_COUNT = 4
} foundry_road_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_season_find(const char *name);

typedef enum foundry_road_v1_detail {
  FOUNDRY_ROAD_V1_DETAIL_HIGH = 0,
  FOUNDRY_ROAD_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_ROAD_V1_DETAIL_LOW = 2,
  FOUNDRY_ROAD_V1_DETAIL_COUNT = 3
} foundry_road_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_road_v1_detail_find(const char *name);

typedef struct foundry_road_v1_params {
  foundry_road_v1_kind kind;  /* A road kind: one entry from config/road_v1_kinds/. */
  foundry_road_v1_condition condition;  /* Intact, or a ruin: weeds through its paving, its stalls and lamps fallen, overgrown. */
  uint32_t mask;  /* What each neighbour is, 3 bits per edge in the hex directions' order: 0 road, 1 a building's front, 2 open land, 3 water, 4 wall, 5 gate, 6 a waterfront, 7 a building's side or back. A planner gives it; turn the cell by sixths so the mask is the smallest of its six turns, to share builds. [0, 262143] */
  foundry_road_v1_half half;  /* Which half of the hex the kind's edge piece takes: the half holding edges h and h + 1 whole (cut through the middles of edges h + 2 and h + 5), or none. A planner gives it, never on an edge the cell's way through uses. */
  uint32_t fences;  /* Which edges a fence runs along (bit d for edge d, where the neighbour is open land): the road keeps a strip there clear for fence_v1. [0, 63] */
  uint32_t doors;  /* Which edges face a building's door (bit d for edge d): nothing stands against them, and the cell takes no half (a planner's strict way). [0, 63] */
  foundry_road_v1_role role;  /* A path, or a bridge over water. */
  foundry_road_v1_season season;
  uint32_t seed;  /* Which of the distinct cells. [1, 1000] */
  foundry_road_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_road_v1_params;

void foundry_road_v1_default_params(foundry_road_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_road_v1_spawn(foundry_runtime *rt, const foundry_road_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_road_v1_set_kind(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_kind value);
foundry_status foundry_road_v1_set_condition(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_condition value);
foundry_status foundry_road_v1_set_mask(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_road_v1_set_half(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_half value);
foundry_status foundry_road_v1_set_fences(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_road_v1_set_doors(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_road_v1_set_role(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_role value);
foundry_status foundry_road_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_season value);
foundry_status foundry_road_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_road_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_detail value);
foundry_status foundry_road_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_road_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_road_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_road_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_road_v1_result;
foundry_status foundry_road_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_road_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/rock_v1.json. Do not edit. */
/* rock_v1: A rock (design/systems.md, Rocks): the game asks for a material, a size, moss, and a season; the body makes the rock and the pieces its material breaks into, and the animator throws the pieces when the game breaks it. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_rock_v1_material {
  FOUNDRY_ROCK_V1_MATERIAL_BASALT = 0,
  FOUNDRY_ROCK_V1_MATERIAL_GRANITE = 1,
  FOUNDRY_ROCK_V1_MATERIAL_LIMESTONE = 2,
  FOUNDRY_ROCK_V1_MATERIAL_MARBLE = 3,
  FOUNDRY_ROCK_V1_MATERIAL_OBSIDIAN = 4,
  FOUNDRY_ROCK_V1_MATERIAL_RIVERSTONE = 5,
  FOUNDRY_ROCK_V1_MATERIAL_SANDSTONE = 6,
  FOUNDRY_ROCK_V1_MATERIAL_SLATE = 7,
  FOUNDRY_ROCK_V1_MATERIAL_COUNT = 8
} foundry_rock_v1_material;
/* A material by its name (as a config entry or a figure names it), or -1. */
int foundry_rock_v1_material_find(const char *name);

typedef enum foundry_rock_v1_size {
  FOUNDRY_ROCK_V1_SIZE_SMALL = 0,
  FOUNDRY_ROCK_V1_SIZE_MEDIUM = 1,
  FOUNDRY_ROCK_V1_SIZE_LARGE = 2,
  FOUNDRY_ROCK_V1_SIZE_HUGE = 3,
  FOUNDRY_ROCK_V1_SIZE_COUNT = 4
} foundry_rock_v1_size;
/* A size by its name (as a config entry or a figure names it), or -1. */
int foundry_rock_v1_size_find(const char *name);

typedef enum foundry_rock_v1_moss {
  FOUNDRY_ROCK_V1_MOSS_NONE = 0,
  FOUNDRY_ROCK_V1_MOSS_LIGHT = 1,
  FOUNDRY_ROCK_V1_MOSS_HEAVY = 2,
  FOUNDRY_ROCK_V1_MOSS_COUNT = 3
} foundry_rock_v1_moss;
/* A moss by its name (as a config entry or a figure names it), or -1. */
int foundry_rock_v1_moss_find(const char *name);

typedef enum foundry_rock_v1_season {
  FOUNDRY_ROCK_V1_SEASON_WINTER = 0,
  FOUNDRY_ROCK_V1_SEASON_SPRING = 1,
  FOUNDRY_ROCK_V1_SEASON_SUMMER = 2,
  FOUNDRY_ROCK_V1_SEASON_FALL = 3,
  FOUNDRY_ROCK_V1_SEASON_COUNT = 4
} foundry_rock_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_rock_v1_season_find(const char *name);

typedef enum foundry_rock_v1_detail {
  FOUNDRY_ROCK_V1_DETAIL_HIGH = 0,
  FOUNDRY_ROCK_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_ROCK_V1_DETAIL_LOW = 2,
  FOUNDRY_ROCK_V1_DETAIL_COUNT = 3
} foundry_rock_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_rock_v1_detail_find(const char *name);

typedef struct foundry_rock_v1_params {
  foundry_rock_v1_material material;  /* A material: one entry from config/rock_v1_materials/. */
  foundry_rock_v1_size size;
  foundry_rock_v1_moss moss;
  foundry_rock_v1_season season;
  uint32_t seed;  /* Which of the distinct rocks per material and size (the runtime caps how many by quality). [1, 1000] */
  foundry_rock_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float break_heading;  /* Where the blow sends the pieces, from +Z toward +X. [0, 360] deg */
} foundry_rock_v1_params;

void foundry_rock_v1_default_params(foundry_rock_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_rock_v1_spawn(foundry_runtime *rt, const foundry_rock_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_rock_v1_set_material(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_material value);
foundry_status foundry_rock_v1_set_size(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_size value);
foundry_status foundry_rock_v1_set_moss(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_moss value);
foundry_status foundry_rock_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_season value);
foundry_status foundry_rock_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_rock_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_detail value);
foundry_status foundry_rock_v1_set_break_heading(foundry_runtime *rt, foundry_instance instance, float value);
/* Seconds since the rock broke; negative while it is whole. The game starts it (foundry_rock_v1_start_break). */
foundry_status foundry_rock_v1_start_break(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_rock_v1_result {
  bool break_over;  /* break started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_rock_v1_result;
foundry_status foundry_rock_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_rock_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/terrain_v1.json. Do not edit. */
/* terrain_v1: A terrain (design/terrain.md): the ground of a playable region, built inside a shape whose sides are given (each an aspect: its edge, height, and rise) with entrances, exits, and water in and out on them, and flats kept level for a settlement planner. relief builds and plans it as hexbound columns of spans (design/import.hexbound.world.md): hills by the parameters, rivers carved from inlet to outlet, terraced rock and caves, roads joining every opening and flat graded on a navigation mesh, bridges with their mounts; mesh draws it by the hexbound rules. Its hull goes to a boundary so the world beyond starts where it ends, and its open land to a plotter. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_terrain_v1_shape {
  FOUNDRY_TERRAIN_V1_SHAPE_CANYON_PASS = 0,
  FOUNDRY_TERRAIN_V1_SHAPE_COAST_TOWN = 1,
  FOUNDRY_TERRAIN_V1_SHAPE_CROSSROADS = 2,
  FOUNDRY_TERRAIN_V1_SHAPE_GLADE = 3,
  FOUNDRY_TERRAIN_V1_SHAPE_HIGHLAND = 4,
  FOUNDRY_TERRAIN_V1_SHAPE_MARSH_BASIN = 5,
  FOUNDRY_TERRAIN_V1_SHAPE_RIVER_VALLEY = 6,
  FOUNDRY_TERRAIN_V1_SHAPE_COUNT = 7
} foundry_terrain_v1_shape;
/* A shape by its name (as a config entry or a figure names it), or -1. */
int foundry_terrain_v1_shape_find(const char *name);

typedef enum foundry_terrain_v1_flat {
  FOUNDRY_TERRAIN_V1_FLAT_HAMLET = 0,
  FOUNDRY_TERRAIN_V1_FLAT_NONE = 1,
  FOUNDRY_TERRAIN_V1_FLAT_TOWN = 2,
  FOUNDRY_TERRAIN_V1_FLAT_VILLAGE = 3,
  FOUNDRY_TERRAIN_V1_FLAT_COUNT = 4
} foundry_terrain_v1_flat;
/* A flat by its name (as a config entry or a figure names it), or -1. */
int foundry_terrain_v1_flat_find(const char *name);

typedef enum foundry_terrain_v1_second_flat {
  FOUNDRY_TERRAIN_V1_SECOND_FLAT_HAMLET = 0,
  FOUNDRY_TERRAIN_V1_SECOND_FLAT_NONE = 1,
  FOUNDRY_TERRAIN_V1_SECOND_FLAT_TOWN = 2,
  FOUNDRY_TERRAIN_V1_SECOND_FLAT_VILLAGE = 3,
  FOUNDRY_TERRAIN_V1_SECOND_FLAT_COUNT = 4
} foundry_terrain_v1_second_flat;
/* A second_flat by its name (as a config entry or a figure names it), or -1. */
int foundry_terrain_v1_second_flat_find(const char *name);

typedef enum foundry_terrain_v1_biome {
  FOUNDRY_TERRAIN_V1_BIOME_ALPINE = 0,
  FOUNDRY_TERRAIN_V1_BIOME_BOREAL = 1,
  FOUNDRY_TERRAIN_V1_BIOME_DESERT = 2,
  FOUNDRY_TERRAIN_V1_BIOME_MEDITERRANEAN = 3,
  FOUNDRY_TERRAIN_V1_BIOME_TEMPERATE = 4,
  FOUNDRY_TERRAIN_V1_BIOME_TROPICAL = 5,
  FOUNDRY_TERRAIN_V1_BIOME_VOLCANIC = 6,
  FOUNDRY_TERRAIN_V1_BIOME_COUNT = 7
} foundry_terrain_v1_biome;
/* A biome by its name (as a config entry or a figure names it), or -1. */
int foundry_terrain_v1_biome_find(const char *name);

typedef enum foundry_terrain_v1_season {
  FOUNDRY_TERRAIN_V1_SEASON_WINTER = 0,
  FOUNDRY_TERRAIN_V1_SEASON_SPRING = 1,
  FOUNDRY_TERRAIN_V1_SEASON_SUMMER = 2,
  FOUNDRY_TERRAIN_V1_SEASON_FALL = 3,
  FOUNDRY_TERRAIN_V1_SEASON_COUNT = 4
} foundry_terrain_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_terrain_v1_season_find(const char *name);

typedef struct foundry_terrain_v1_params {
  foundry_terrain_v1_shape shape;  /* The shape: its corners, each side's aspect, and its openings (config/terrain_v1_shapes/). A game gives its own. */
  foundry_terrain_v1_flat flat;  /* A flat kept level for a settlement planner, if any (a town is optional). */
  foundry_terrain_v1_second_flat second_flat;  /* Another flat, if any. */
  foundry_terrain_v1_biome biome;  /* The biome: the colours (a boundary's, so the two match). */
  foundry_terrain_v1_season season;
  float relief;  /* How high hills stand above the sides' blend. [0, 60] m */
  float hill_size;  /* How wide a hill is. [8, 200] m */
  float roughness;  /* Small bumps and hollows. [0, 1] */
  float ridges;  /* How much of the relief is ridged rather than rolling. [0, 1] */
  float cliffs;  /* How much steep land is terraced rock. [0, 1] */
  float caves;  /* How often cliffs open into cave mouths. [0, 1] */
  float river_depth;  /* How deep rivers run below their banks. [0.5, 6] m */
  uint32_t seed;  /* The terrain's seed. [1, 1000000] */
} foundry_terrain_v1_params;

void foundry_terrain_v1_default_params(foundry_terrain_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_terrain_v1_spawn(foundry_runtime *rt, const foundry_terrain_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_terrain_v1_set_shape(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_shape value);
foundry_status foundry_terrain_v1_set_flat(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_flat value);
foundry_status foundry_terrain_v1_set_second_flat(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_second_flat value);
foundry_status foundry_terrain_v1_set_biome(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_biome value);
foundry_status foundry_terrain_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_season value);
foundry_status foundry_terrain_v1_set_relief(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_hill_size(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_roughness(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_ridges(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_cliffs(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_caves(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_river_depth(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_terrain_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_terrain_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_terrain_v1_result;
foundry_status foundry_terrain_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_terrain_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/tree_v1.json. Do not edit. */
/* tree_v1: A tree (design/systems.md, Trees): the game asks for a species, age, and season at a position with the local wind; the rig grows the skeleton, the mesh wraps it, colorize paints it, and the animator sways it in the wind or breaks it. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_tree_v1_species {
  FOUNDRY_TREE_V1_SPECIES_ACACIA = 0,
  FOUNDRY_TREE_V1_SPECIES_APPLE = 1,
  FOUNDRY_TREE_V1_SPECIES_BALDCYPRESS = 2,
  FOUNDRY_TREE_V1_SPECIES_BAOBAB = 3,
  FOUNDRY_TREE_V1_SPECIES_BIRCH = 4,
  FOUNDRY_TREE_V1_SPECIES_BRISTLECONE = 5,
  FOUNDRY_TREE_V1_SPECIES_CEDAR = 6,
  FOUNDRY_TREE_V1_SPECIES_CHERRY = 7,
  FOUNDRY_TREE_V1_SPECIES_COCONUT = 8,
  FOUNDRY_TREE_V1_SPECIES_CYPRESS = 9,
  FOUNDRY_TREE_V1_SPECIES_DRAGONWOOD = 10,
  FOUNDRY_TREE_V1_SPECIES_EBONY = 11,
  FOUNDRY_TREE_V1_SPECIES_ELM = 12,
  FOUNDRY_TREE_V1_SPECIES_HICKORY = 13,
  FOUNDRY_TREE_V1_SPECIES_JACARANDA = 14,
  FOUNDRY_TREE_V1_SPECIES_LARCH = 15,
  FOUNDRY_TREE_V1_SPECIES_LIGNUMVITAE = 16,
  FOUNDRY_TREE_V1_SPECIES_LIVEOAK = 17,
  FOUNDRY_TREE_V1_SPECIES_MAHOGANY = 18,
  FOUNDRY_TREE_V1_SPECIES_MANGO = 19,
  FOUNDRY_TREE_V1_SPECIES_MANGROVE = 20,
  FOUNDRY_TREE_V1_SPECIES_MAPLE = 21,
  FOUNDRY_TREE_V1_SPECIES_MESQUITE = 22,
  FOUNDRY_TREE_V1_SPECIES_OAK = 23,
  FOUNDRY_TREE_V1_SPECIES_OLIVE = 24,
  FOUNDRY_TREE_V1_SPECIES_OSAGE = 25,
  FOUNDRY_TREE_V1_SPECIES_PINE = 26,
  FOUNDRY_TREE_V1_SPECIES_REDWOOD = 27,
  FOUNDRY_TREE_V1_SPECIES_SPRUCE = 28,
  FOUNDRY_TREE_V1_SPECIES_TEAK = 29,
  FOUNDRY_TREE_V1_SPECIES_TUPELO = 30,
  FOUNDRY_TREE_V1_SPECIES_WILLOW = 31,
  FOUNDRY_TREE_V1_SPECIES_COUNT = 32
} foundry_tree_v1_species;
/* A species by its name (as a config entry or a figure names it), or -1. */
int foundry_tree_v1_species_find(const char *name);

typedef enum foundry_tree_v1_age {
  FOUNDRY_TREE_V1_AGE_SAPLING = 0,
  FOUNDRY_TREE_V1_AGE_YOUNG = 1,
  FOUNDRY_TREE_V1_AGE_MATURE = 2,
  FOUNDRY_TREE_V1_AGE_ANCIENT = 3,
  FOUNDRY_TREE_V1_AGE_COUNT = 4
} foundry_tree_v1_age;
/* A age by its name (as a config entry or a figure names it), or -1. */
int foundry_tree_v1_age_find(const char *name);

typedef enum foundry_tree_v1_season {
  FOUNDRY_TREE_V1_SEASON_WINTER = 0,
  FOUNDRY_TREE_V1_SEASON_SPRING = 1,
  FOUNDRY_TREE_V1_SEASON_SUMMER = 2,
  FOUNDRY_TREE_V1_SEASON_FALL = 3,
  FOUNDRY_TREE_V1_SEASON_COUNT = 4
} foundry_tree_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_tree_v1_season_find(const char *name);

typedef enum foundry_tree_v1_detail {
  FOUNDRY_TREE_V1_DETAIL_HIGH = 0,
  FOUNDRY_TREE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_TREE_V1_DETAIL_LOW = 2,
  FOUNDRY_TREE_V1_DETAIL_COUNT = 3
} foundry_tree_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_tree_v1_detail_find(const char *name);

typedef struct foundry_tree_v1_params {
  foundry_tree_v1_species species;  /* A species: one genome from config/tree_v1_genomes/. */
  foundry_tree_v1_age age;
  foundry_tree_v1_season season;
  bool fruit;  /* The tree still carries its fruit; clear it when the fruit is picked. Applies only to species whose fruit can be picked (pickable); elsewhere it is disabled. */
  bool stump;  /* Only the stump is left. The game sets it once a felled tree's break is over (break_over in its result); the trunk's collision becomes the stump's. */
  uint32_t seed;  /* Which of the distinct trees per species, age, and season (the runtime caps how many by quality). [1, 1000] */
  foundry_tree_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
  float break_heading;  /*  [0, 360] deg */
} foundry_tree_v1_params;

void foundry_tree_v1_default_params(foundry_tree_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_tree_v1_spawn(foundry_runtime *rt, const foundry_tree_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_tree_v1_set_species(foundry_runtime *rt, foundry_instance instance, foundry_tree_v1_species value);
foundry_status foundry_tree_v1_set_age(foundry_runtime *rt, foundry_instance instance, foundry_tree_v1_age value);
foundry_status foundry_tree_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_tree_v1_season value);
foundry_status foundry_tree_v1_set_fruit(foundry_runtime *rt, foundry_instance instance, bool value);
foundry_status foundry_tree_v1_set_stump(foundry_runtime *rt, foundry_instance instance, bool value);
foundry_status foundry_tree_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_tree_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_tree_v1_detail value);
foundry_status foundry_tree_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_tree_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_tree_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_tree_v1_set_break_heading(foundry_runtime *rt, foundry_instance instance, float value);
/* Whether fruit applies to this species (its pickable is set). Where it
 * doesn't, the instance ignores fruit and shares its build either way. */
bool foundry_tree_v1_fruit_applies(foundry_tree_v1_species species);
/* Seconds since the tree broke; negative while it stands. The game starts it (foundry_tree_v1_start_break). */
foundry_status foundry_tree_v1_start_break(foundry_runtime *rt, foundry_instance instance);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_tree_v1_result {
  bool break_over;  /* break started and its animation has come to rest */
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_tree_v1_result;
foundry_status foundry_tree_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_tree_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/wall_v1.json. Do not edit. */
/* wall_v1: A wall network (design/planner.md, Walls, fences, and gates): the game gives a kind (a config entry: a palisade, a town wall, a castle's curtain, a keep's wall, an earth bank, a brick wall) and a network on the hex grid's diagonal lattice (nodes and links along lines through opposite corners, with gates and towers), and build makes the whole wall (or one chunk of it) with its joins and its openings, reporting where its gates go (gate_v1); the house animator sways its banners. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_wall_v1_kind {
  FOUNDRY_WALL_V1_KIND_ADOBE_WALL = 0,
  FOUNDRY_WALL_V1_KIND_BRICK_WALL = 1,
  FOUNDRY_WALL_V1_KIND_CURTAIN = 2,
  FOUNDRY_WALL_V1_KIND_DRYSTONE_WALL = 3,
  FOUNDRY_WALL_V1_KIND_DWARF_RAMPART = 4,
  FOUNDRY_WALL_V1_KIND_EARTH_BANK = 5,
  FOUNDRY_WALL_V1_KIND_KEEP_WALL = 6,
  FOUNDRY_WALL_V1_KIND_LIVING_WALL = 7,
  FOUNDRY_WALL_V1_KIND_PALISADE = 8,
  FOUNDRY_WALL_V1_KIND_STONE_CIRCLE = 9,
  FOUNDRY_WALL_V1_KIND_TOWN_WALL = 10,
  FOUNDRY_WALL_V1_KIND_COUNT = 11
} foundry_wall_v1_kind;
/* A kind by its name (as a config entry or a figure names it), or -1. */
int foundry_wall_v1_kind_find(const char *name);

typedef enum foundry_wall_v1_condition {
  FOUNDRY_WALL_V1_CONDITION_INTACT = 0,
  FOUNDRY_WALL_V1_CONDITION_RUIN = 1,
  FOUNDRY_WALL_V1_CONDITION_COUNT = 2
} foundry_wall_v1_condition;
/* A condition by its name (as a config entry or a figure names it), or -1. */
int foundry_wall_v1_condition_find(const char *name);

typedef enum foundry_wall_v1_network {
  FOUNDRY_WALL_V1_NETWORK_BEND_GENTLE = 0,
  FOUNDRY_WALL_V1_NETWORK_BEND_SHARP = 1,
  FOUNDRY_WALL_V1_NETWORK_BRANCH = 2,
  FOUNDRY_WALL_V1_NETWORK_COMPOUND = 3,
  FOUNDRY_WALL_V1_NETWORK_CROSS = 4,
  FOUNDRY_WALL_V1_NETWORK_PADDOCKS = 5,
  FOUNDRY_WALL_V1_NETWORK_RING = 6,
  FOUNDRY_WALL_V1_NETWORK_SLOPE = 7,
  FOUNDRY_WALL_V1_NETWORK_STAR = 8,
  FOUNDRY_WALL_V1_NETWORK_STRAIGHT = 9,
  FOUNDRY_WALL_V1_NETWORK_WYE = 10,
  FOUNDRY_WALL_V1_NETWORK_COUNT = 11
} foundry_wall_v1_network;
/* A network by its name (as a config entry or a figure names it), or -1. */
int foundry_wall_v1_network_find(const char *name);

typedef enum foundry_wall_v1_season {
  FOUNDRY_WALL_V1_SEASON_WINTER = 0,
  FOUNDRY_WALL_V1_SEASON_SPRING = 1,
  FOUNDRY_WALL_V1_SEASON_SUMMER = 2,
  FOUNDRY_WALL_V1_SEASON_FALL = 3,
  FOUNDRY_WALL_V1_SEASON_COUNT = 4
} foundry_wall_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_wall_v1_season_find(const char *name);

typedef enum foundry_wall_v1_detail {
  FOUNDRY_WALL_V1_DETAIL_HIGH = 0,
  FOUNDRY_WALL_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_WALL_V1_DETAIL_LOW = 2,
  FOUNDRY_WALL_V1_DETAIL_COUNT = 3
} foundry_wall_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_wall_v1_detail_find(const char *name);

typedef struct foundry_wall_v1_params {
  foundry_wall_v1_kind kind;  /* A kind: one entry from config/wall_v1_kinds/. */
  foundry_wall_v1_condition condition;  /* Intact, or a ruin: broken down to a ragged height, its banners gone, overgrown, rubble beside it. */
  foundry_wall_v1_network network;  /* The network to build: an example from config/line_v1_networks/, or the game's own (a planner's line). */
  uint32_t chunks;  /* How many chunks to split it into, by angle round its middle (0 or 1: all of it). [0, 64] */
  uint32_t chunk;  /* Which chunk to build. [0, 63] */
  foundry_wall_v1_season season;
  uint32_t seed;  /* Which of the distinct builds. [1, 1000] */
  foundry_wall_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_wall_v1_params;

void foundry_wall_v1_default_params(foundry_wall_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_wall_v1_spawn(foundry_runtime *rt, const foundry_wall_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_wall_v1_set_kind(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_kind value);
foundry_status foundry_wall_v1_set_condition(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_condition value);
foundry_status foundry_wall_v1_set_network(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_network value);
foundry_status foundry_wall_v1_set_chunks(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_wall_v1_set_chunk(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_wall_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_season value);
foundry_status foundry_wall_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_wall_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_detail value);
foundry_status foundry_wall_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_wall_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_wall_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_wall_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_wall_v1_result;
foundry_status foundry_wall_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_wall_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from pipelines/waterfront_v1.json. Do not edit. */
/* waterfront_v1: A waterfront building (design/planner.md, Waterfronts): the game asks for a type (a pier, a boathouse, a quay warehouse, a fishery), a size, how far below the ground the water lies, and the season; build makes it on its part-land, part-water lot with its boats, crane, and nets, and the house animator rocks the boats and sways the nets and the crane's jib. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundry_waterfront_v1_type {
  FOUNDRY_WATERFRONT_V1_TYPE_BOATHOUSE = 0,
  FOUNDRY_WATERFRONT_V1_TYPE_FISHERY = 1,
  FOUNDRY_WATERFRONT_V1_TYPE_PIER = 2,
  FOUNDRY_WATERFRONT_V1_TYPE_WAREHOUSE = 3,
  FOUNDRY_WATERFRONT_V1_TYPE_COUNT = 4
} foundry_waterfront_v1_type;
/* A type by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_type_find(const char *name);

typedef enum foundry_waterfront_v1_theme {
  FOUNDRY_WATERFRONT_V1_THEME_DESERT = 0,
  FOUNDRY_WATERFRONT_V1_THEME_DRUIDIC = 1,
  FOUNDRY_WATERFRONT_V1_THEME_DWARVEN = 2,
  FOUNDRY_WATERFRONT_V1_THEME_ELVEN = 3,
  FOUNDRY_WATERFRONT_V1_THEME_HEARTLAND = 4,
  FOUNDRY_WATERFRONT_V1_THEME_MARSH = 5,
  FOUNDRY_WATERFRONT_V1_THEME_NORDIC = 6,
  FOUNDRY_WATERFRONT_V1_THEME_STONE_DOME = 7,
  FOUNDRY_WATERFRONT_V1_THEME_COUNT = 8
} foundry_waterfront_v1_theme;
/* A theme by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_theme_find(const char *name);

typedef enum foundry_waterfront_v1_condition {
  FOUNDRY_WATERFRONT_V1_CONDITION_INTACT = 0,
  FOUNDRY_WATERFRONT_V1_CONDITION_RUIN = 1,
  FOUNDRY_WATERFRONT_V1_CONDITION_COUNT = 2
} foundry_waterfront_v1_condition;
/* A condition by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_condition_find(const char *name);

typedef enum foundry_waterfront_v1_size {
  FOUNDRY_WATERFRONT_V1_SIZE_SMALL = 0,
  FOUNDRY_WATERFRONT_V1_SIZE_MEDIUM = 1,
  FOUNDRY_WATERFRONT_V1_SIZE_LARGE = 2,
  FOUNDRY_WATERFRONT_V1_SIZE_COUNT = 3
} foundry_waterfront_v1_size;
/* A size by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_size_find(const char *name);

typedef enum foundry_waterfront_v1_season {
  FOUNDRY_WATERFRONT_V1_SEASON_WINTER = 0,
  FOUNDRY_WATERFRONT_V1_SEASON_SPRING = 1,
  FOUNDRY_WATERFRONT_V1_SEASON_SUMMER = 2,
  FOUNDRY_WATERFRONT_V1_SEASON_FALL = 3,
  FOUNDRY_WATERFRONT_V1_SEASON_COUNT = 4
} foundry_waterfront_v1_season;
/* A season by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_season_find(const char *name);

typedef enum foundry_waterfront_v1_detail {
  FOUNDRY_WATERFRONT_V1_DETAIL_HIGH = 0,
  FOUNDRY_WATERFRONT_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_WATERFRONT_V1_DETAIL_LOW = 2,
  FOUNDRY_WATERFRONT_V1_DETAIL_COUNT = 3
} foundry_waterfront_v1_detail;
/* A detail by its name (as a config entry or a figure names it), or -1. */
int foundry_waterfront_v1_detail_find(const char *name);

typedef struct foundry_waterfront_v1_params {
  foundry_waterfront_v1_type type;  /* A type: one entry from config/waterfront_v1_types/. */
  foundry_waterfront_v1_theme theme;  /* The village's theme from config/village_v1_themes/: its colours, moss, and fires. */
  foundry_waterfront_v1_condition condition;  /* Intact, or a ruin: broken down, its boats and lamp gone, overgrown. */
  foundry_waterfront_v1_size size;  /* Its lot: small (rect3x4), medium (rect4x5), or large (rect5x6); the back two rows are land, the rest water. */
  float drop;  /* How far below the lot's ground the water lies (a planner gives it). [0, 4] m */
  foundry_waterfront_v1_season season;
  uint32_t seed;  /* Which of the distinct buildings. [1, 1000] */
  foundry_waterfront_v1_detail detail;  /* Level of detail; the runtime picks it from distance and quality. */
  float wind_speed;  /*  [0, 25] m/s */
  float wind_heading;  /* Toward which the wind blows, from +Z toward +X. [0, 360] deg */
  float gust_damping;  /*  [0, 1] */
} foundry_waterfront_v1_params;

void foundry_waterfront_v1_default_params(foundry_waterfront_v1_params *params);
/* Returns FOUNDRY_NO_INSTANCE if a parameter is out of range. */
foundry_instance foundry_waterfront_v1_spawn(foundry_runtime *rt, const foundry_waterfront_v1_params *params,
                                  const float transform[16]);
foundry_status foundry_waterfront_v1_set_type(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_type value);
foundry_status foundry_waterfront_v1_set_theme(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_theme value);
foundry_status foundry_waterfront_v1_set_condition(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_condition value);
foundry_status foundry_waterfront_v1_set_size(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_size value);
foundry_status foundry_waterfront_v1_set_drop(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_waterfront_v1_set_season(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_season value);
foundry_status foundry_waterfront_v1_set_seed(foundry_runtime *rt, foundry_instance instance, uint32_t value);
foundry_status foundry_waterfront_v1_set_detail(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_detail value);
foundry_status foundry_waterfront_v1_set_wind_speed(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_waterfront_v1_set_wind_heading(foundry_runtime *rt, foundry_instance instance, float value);
foundry_status foundry_waterfront_v1_set_gust_damping(foundry_runtime *rt, foundry_instance instance, float value);

/* The instance's update result, as current values (see foundry_changed). */
typedef struct foundry_waterfront_v1_result {
  bool ready;  /* the build last asked for (by spawn or a setter) is the one drawn */
} foundry_waterfront_v1_result;
foundry_status foundry_waterfront_v1_get_result(foundry_runtime *rt, foundry_instance instance, foundry_waterfront_v1_result *out);

#ifdef __cplusplus
}
#endif

/* Generated by tools/gen_pipelines.py from the workspace's plotters/. Do not edit. */
/* The approved plotters (design/plotter.md): pass one in foundry_region_desc. */
#define FOUNDRY_PLOTTER_HAPPY_GLEN_V1 ((foundry_plotter)0)
#define FOUNDRY_PLOTTER_COUNT 1u
