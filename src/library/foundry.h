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

typedef struct foundry_runtime_desc {
  SDL_GPUDevice *device;              /* NULL runs everything but submission */
  SDL_GPUTextureFormat color_format;  /* the render pass's color target */
  SDL_GPUTextureFormat depth_format;  /* and its depth target */
  foundry_quality quality;
  uint32_t update_threads;  /* 0 picks from the machine's cores */
  uint32_t build_threads;   /* 0 picks from the machine's cores */
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
void foundry_set_quality(foundry_runtime *rt, foundry_quality quality);

foundry_status foundry_set_transform(foundry_runtime *rt, foundry_instance instance, const float transform[16]);
foundry_status foundry_is_sleeping(foundry_runtime *rt, foundry_instance instance, bool *sleeping);
/* Releases everything the instance holds; its handle goes stale. */
foundry_status foundry_destroy(foundry_runtime *rt, foundry_instance instance);

#ifdef __cplusplus
}
#endif
