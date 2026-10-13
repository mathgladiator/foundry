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
