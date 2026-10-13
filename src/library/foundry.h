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
