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
