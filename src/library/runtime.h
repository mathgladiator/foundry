/* runtime.h: the game-facing API (design/api.md). Assembled by tools/gen_pipelines.py from
 * the fragments in src/library/: foundry.h, region.h, sky.h, each pipeline by name, then the plotters. Do not edit. */
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

typedef enum foundry_bush_v1_age {
  FOUNDRY_BUSH_V1_AGE_YOUNG = 0,
  FOUNDRY_BUSH_V1_AGE_MATURE = 1,
  FOUNDRY_BUSH_V1_AGE_OLD = 2,
  FOUNDRY_BUSH_V1_AGE_COUNT = 3
} foundry_bush_v1_age;

typedef enum foundry_bush_v1_season {
  FOUNDRY_BUSH_V1_SEASON_WINTER = 0,
  FOUNDRY_BUSH_V1_SEASON_SPRING = 1,
  FOUNDRY_BUSH_V1_SEASON_SUMMER = 2,
  FOUNDRY_BUSH_V1_SEASON_FALL = 3,
  FOUNDRY_BUSH_V1_SEASON_COUNT = 4
} foundry_bush_v1_season;

typedef enum foundry_bush_v1_detail {
  FOUNDRY_BUSH_V1_DETAIL_HIGH = 0,
  FOUNDRY_BUSH_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_BUSH_V1_DETAIL_LOW = 2,
  FOUNDRY_BUSH_V1_DETAIL_COUNT = 3
} foundry_bush_v1_detail;

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

typedef enum foundry_flower_v1_season {
  FOUNDRY_FLOWER_V1_SEASON_WINTER = 0,
  FOUNDRY_FLOWER_V1_SEASON_SPRING = 1,
  FOUNDRY_FLOWER_V1_SEASON_SUMMER = 2,
  FOUNDRY_FLOWER_V1_SEASON_FALL = 3,
  FOUNDRY_FLOWER_V1_SEASON_COUNT = 4
} foundry_flower_v1_season;

typedef enum foundry_flower_v1_detail {
  FOUNDRY_FLOWER_V1_DETAIL_HIGH = 0,
  FOUNDRY_FLOWER_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_FLOWER_V1_DETAIL_LOW = 2,
  FOUNDRY_FLOWER_V1_DETAIL_COUNT = 3
} foundry_flower_v1_detail;

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

typedef enum foundry_grass_v1_season {
  FOUNDRY_GRASS_V1_SEASON_WINTER = 0,
  FOUNDRY_GRASS_V1_SEASON_SPRING = 1,
  FOUNDRY_GRASS_V1_SEASON_SUMMER = 2,
  FOUNDRY_GRASS_V1_SEASON_FALL = 3,
  FOUNDRY_GRASS_V1_SEASON_COUNT = 4
} foundry_grass_v1_season;

typedef enum foundry_grass_v1_detail {
  FOUNDRY_GRASS_V1_DETAIL_HIGH = 0,
  FOUNDRY_GRASS_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_GRASS_V1_DETAIL_LOW = 2,
  FOUNDRY_GRASS_V1_DETAIL_MAT = 3,
  FOUNDRY_GRASS_V1_DETAIL_COUNT = 4
} foundry_grass_v1_detail;

typedef enum foundry_grass_v1_ground {
  FOUNDRY_GRASS_V1_GROUND_SOIL = 0,
  FOUNDRY_GRASS_V1_GROUND_WATER = 1,
  FOUNDRY_GRASS_V1_GROUND_ROCK = 2,
  FOUNDRY_GRASS_V1_GROUND_SAND = 3,
  FOUNDRY_GRASS_V1_GROUND_PATH = 4,
  FOUNDRY_GRASS_V1_GROUND_COUNT = 5
} foundry_grass_v1_ground;

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

typedef enum foundry_rock_v1_size {
  FOUNDRY_ROCK_V1_SIZE_SMALL = 0,
  FOUNDRY_ROCK_V1_SIZE_MEDIUM = 1,
  FOUNDRY_ROCK_V1_SIZE_LARGE = 2,
  FOUNDRY_ROCK_V1_SIZE_HUGE = 3,
  FOUNDRY_ROCK_V1_SIZE_COUNT = 4
} foundry_rock_v1_size;

typedef enum foundry_rock_v1_moss {
  FOUNDRY_ROCK_V1_MOSS_NONE = 0,
  FOUNDRY_ROCK_V1_MOSS_LIGHT = 1,
  FOUNDRY_ROCK_V1_MOSS_HEAVY = 2,
  FOUNDRY_ROCK_V1_MOSS_COUNT = 3
} foundry_rock_v1_moss;

typedef enum foundry_rock_v1_season {
  FOUNDRY_ROCK_V1_SEASON_WINTER = 0,
  FOUNDRY_ROCK_V1_SEASON_SPRING = 1,
  FOUNDRY_ROCK_V1_SEASON_SUMMER = 2,
  FOUNDRY_ROCK_V1_SEASON_FALL = 3,
  FOUNDRY_ROCK_V1_SEASON_COUNT = 4
} foundry_rock_v1_season;

typedef enum foundry_rock_v1_detail {
  FOUNDRY_ROCK_V1_DETAIL_HIGH = 0,
  FOUNDRY_ROCK_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_ROCK_V1_DETAIL_LOW = 2,
  FOUNDRY_ROCK_V1_DETAIL_COUNT = 3
} foundry_rock_v1_detail;

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

typedef enum foundry_tree_v1_age {
  FOUNDRY_TREE_V1_AGE_SAPLING = 0,
  FOUNDRY_TREE_V1_AGE_YOUNG = 1,
  FOUNDRY_TREE_V1_AGE_MATURE = 2,
  FOUNDRY_TREE_V1_AGE_ANCIENT = 3,
  FOUNDRY_TREE_V1_AGE_COUNT = 4
} foundry_tree_v1_age;

typedef enum foundry_tree_v1_season {
  FOUNDRY_TREE_V1_SEASON_WINTER = 0,
  FOUNDRY_TREE_V1_SEASON_SPRING = 1,
  FOUNDRY_TREE_V1_SEASON_SUMMER = 2,
  FOUNDRY_TREE_V1_SEASON_FALL = 3,
  FOUNDRY_TREE_V1_SEASON_COUNT = 4
} foundry_tree_v1_season;

typedef enum foundry_tree_v1_detail {
  FOUNDRY_TREE_V1_DETAIL_HIGH = 0,
  FOUNDRY_TREE_V1_DETAIL_MEDIUM = 1,
  FOUNDRY_TREE_V1_DETAIL_LOW = 2,
  FOUNDRY_TREE_V1_DETAIL_COUNT = 3
} foundry_tree_v1_detail;

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

/* Generated by tools/gen_pipelines.py from the workspace's plotters/. Do not edit. */
/* The approved plotters (design/plotter.md): pass one in foundry_region_desc. */
#define FOUNDRY_PLOTTER_HAPPY_GLEN_V1 ((foundry_plotter)0)
#define FOUNDRY_PLOTTER_COUNT 1u
