/* runtime.h: the game-facing API (design/api.md). Assembled by tools/gen_pipelines.py from
 * the fragments in src/library/: foundry.h, region.h, each pipeline by name, then the plotters. Do not edit. */
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

/* A plotter's kind, as "pipeline param..." (e.g. "tree_v1 oak"), and how many kinds it has. */
const char *foundry_plotter_kind_name(foundry_plotter plotter, uint32_t kind);
uint32_t foundry_plotter_kind_count(foundry_plotter plotter);

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

/* Generated by tools/gen_pipelines.py from plotters/*.json. Do not edit. */
/* The approved plotters (design/plotter.md): pass one in foundry_region_desc. */
#define FOUNDRY_PLOTTER_HAPPY_GLEN_V1 ((foundry_plotter)0)
#define FOUNDRY_PLOTTER_COUNT 1u
