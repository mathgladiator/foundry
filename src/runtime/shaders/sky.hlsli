// sky.hlsli: the sky of an observatory (design/observatory.md), per pixel: the
// air's single scattering (Rayleigh, Mie with a forward lobe per channel, and
// ozone), the sun's disc, the moons with their phases, the star chart (a cube
// map turned by the time of day and year) with the lit constellations' glow,
// the aurora's curtains, and the clouds. The same air as sky.cpp, which lights
// the ground from it. Everything is in display units before the exposure; the
// result is exposed and toned.
//
// The including file may move its bindings: SKY_BINDING (the fragment uniform
// slot) and the registers SKY_REG, SKY_T0..SKY_T2, SKY_S0..SKY_S2.

#include "clouds.hlsli"

#ifndef SKY_BINDING
#define SKY_BINDING 0
#define SKY_REG b0
#define SKY_T0 t0
#define SKY_S0 s0
#define SKY_T1 t1
#define SKY_S1 s1
#define SKY_T2 t2
#define SKY_S2 s2
#endif

#include "look.hlsli"

struct SkyData {
  float4 ray_x, ray_y, ray_z;  // a pixel's direction: ray_z + ndc.x * ray_x + ndc.y * ray_y
  float4 eye;                  // xyz the camera (m), w the time (s)
  float4 sun;                  // xyz toward the sun, w its angular radius
  float4 sun_color;            // rgb its colour, w its intensity
  float4 moon_dir[2];          // xyz, w angular radius (0: none)
  float4 moon_color[2];        // rgb at full (display), w unused
  float4 rayleigh;             // rgb per km, w scale height (km)
  float4 mie;                  // rgb per km, w scale height (km)
  float4 mie_absorb;           // rgb per km, w the planet's radius (km)
  float4 mie_g;                // rgb, w the air's height (km)
  float4 ozone;                // rgb per km, w the sky's exposure
  float4 to_stars[3];          // world to the star chart, rows
  float4 stars;                // x twinkle, y the lit glow's pulse, z the star density, w sky radiance to display
  float4 lit_color;            // rgb a lit constellation's glow
  float4 cloud, cloud_wind;    // as clouds.hlsli
  float4 cloud_lit, cloud_shade;
  float4 aurora;               // x strength, y its oval's distance poleward (km), z +1 north is -Z, -1 it is +Z
  float4 aurora_low, aurora_high;
  float4 night;                // rgb the night sky's glow
  float4 ground;               // rgb under the horizon
  LookData look;               // the game's look (look.hlsli)
};

[[vk::binding(SKY_BINDING, 3)]] cbuffer SkyBuffer : register(SKY_REG, space3) { SkyData sky; };

[[vk::combinedImageSampler]][[vk::binding(0, 2)]] TextureCube<float4> star_tex : register(SKY_T0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState star_smp : register(SKY_S0, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] TextureCube<float4> glow_tex : register(SKY_T1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] SamplerState glow_smp : register(SKY_S1, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]] Texture2D<float4> sky_cloud_tex : register(SKY_T2, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]] SamplerState sky_cloud_smp : register(SKY_S2, space2);

static const float kSkyPi = 3.14159265;

float sky_hash(float2 p) {
  p = frac(p * float2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return frac(p.x * p.y);
}
float sky_hash1(float x) { return frac(sin(x * 127.1) * 43758.5453); }
float sky_noise1(float x) {
  const float i = floor(x), f = x - i;
  const float u = f * f * (3.0 - 2.0 * f);
  return lerp(sky_hash1(i), sky_hash1(i + 1.0), u);
}
float sky_noise3(float3 p) {
  const float3 i = floor(p), f = p - i;
  const float3 u = f * f * (3.0 - 2.0 * f);
  const float n = dot(i, float3(1.0, 57.0, 113.0));
  const float a = lerp(sky_hash1(n), sky_hash1(n + 1.0), u.x), b = lerp(sky_hash1(n + 57.0), sky_hash1(n + 58.0), u.x);
  const float c = lerp(sky_hash1(n + 113.0), sky_hash1(n + 114.0), u.x), d = lerp(sky_hash1(n + 170.0), sky_hash1(n + 171.0), u.x);
  return lerp(lerp(a, b, u.y), lerp(c, d, u.y), u.z);
}

float3 sky_density(float h) {
  h = max(h, 0.0);
  return float3(exp(-h / sky.rayleigh.w), exp(-h / sky.mie.w), max(0.0, 1.0 - abs(h - 25.0) / 15.0));
}
float sky_exit(float3 o, float3 d, float r) {
  const float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
  return disc < 0.0 ? -1.0 : -b + sqrt(disc);
}
float sky_ground(float3 o, float3 d, float r) {
  const float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
  if (disc < 0.0) return -1.0;
  const float t = -b - sqrt(disc);
  return t > 0.0 ? t : -1.0;
}
float3 sky_extinction(float3 od) {
  return sky.rayleigh.rgb * od.x + (sky.mie.rgb + sky.mie_absorb.rgb) * od.y + sky.ozone.rgb * od.z;
}
bool sky_sun_depth(float3 p, float3 s, out float3 od) {
  od = 0.0;
  const float rp = sky.mie_absorb.w;
  if (sky_ground(p, s, rp) > 0.0) return false;
  const float t = sky_exit(p, s, rp + sky.mie_g.w);
  if (t <= 0.0) return true;
  const float ds = t / 4.0;
  [unroll] for (int i = 0; i < 4; ++i) {
    const float3 q = p + s * ((i + 0.5) * ds);
    od += sky_density(length(q) - rp) * ds;
  }
  return true;
}
float sky_phase_mie(float mu, float g) {
  const float g2 = g * g;
  return 3.0 / (8.0 * kSkyPi) * ((1.0 - g2) * (1.0 + mu * mu)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// The air's radiance along dir (sun intensity units) and its transmittance.
float3 sky_scatter(float3 dir, out float3 transmit) {
  const float rp = sky.mie_absorb.w;
  const float3 origin = float3(0.0, rp + 0.002, 0.0);
  float t = sky_exit(origin, dir, rp + sky.mie_g.w);
  const float g = sky_ground(origin, dir, rp);
  if (g > 0.0) t = g;
  const float ds = max(t, 0.0) / 16.0;
  float3 od = 0.0, sum_r = 0.0, sum_m = 0.0;
  const float3 s = sky.sun.xyz;
  [loop] for (int i = 0; i < 16; ++i) {
    const float3 p = origin + dir * ((i + 0.5) * ds);
    const float3 d = sky_density(length(p) - rp);
    od += d * (0.5 * ds);
    float3 odl;
    if (sky_sun_depth(p, s, odl)) {
      const float3 tr = exp(-sky_extinction(od + odl));
      sum_r += tr * (d.x * ds);
      sum_m += tr * (d.y * ds);
    }
    od += d * (0.5 * ds);
  }
  const float mu = dot(dir, s);
  const float pr = 3.0 / (16.0 * kSkyPi) * (1.0 + mu * mu);
  const float3 pm = float3(sky_phase_mie(mu, sky.mie_g.r), sky_phase_mie(mu, sky.mie_g.g), sky_phase_mie(mu, sky.mie_g.b));
  transmit = exp(-sky_extinction(od));
  return sky.sun_color.w * (sum_r * sky.rayleigh.rgb * pr + sum_m * sky.mie.rgb * pm);
}

// Toned by luminance, keeping the hue, then a gentle roll toward white only where it overflows: a
// bright sky near the sun stays its colour instead of going white.
float3 sky_tone(float3 x) {
  x = max(x, 0.0);
  const float l = dot(x, float3(0.2126, 0.7152, 0.0722));
  const float3 c = x * ((1.0 - exp(-1.25 * l)) / max(l, 1e-5));
  const float over = max(max(c.r, c.g), c.b);
  return over > 1.0 ? lerp(c / over, float3(1.0, 1.0, 1.0), saturate((over - 1.0) * 0.35)) : c;
}

// The aurora's curtains: folds along the oval, seen through layers from 95 km up, green below and
// red-violet above, rayed, swaying slowly and rippling.
float3 sky_aurora(float3 dir) {
  if (sky.aurora.x <= 0.001 || dir.y <= 0.008) return 0.0;
  const float time = sky.eye.w;
  float3 acc = 0.0;
  [loop] for (int i = 0; i < 22; ++i) {
    const float h = 95.0 + i * 8.5;
    const float t = h / max(dir.y, 0.02);
    if (t > 2800.0) break;
    const float2 p = dir.xz * t;
    const float ahead = -p.y * sky.aurora.z;  // poleward, km
    const float x = p.x;
    const float wave = 95.0 * (sky_noise1(x * 0.0021 + time * 0.012) - 0.5) * 2.0 + 38.0 * sin(x * 0.0105 + time * 0.11) +
                       11.0 * sin(x * 0.047 - time * 0.45);
    const float centre = sky.aurora.y + wave;
    const float width = 12.0 + 12.0 * sky_noise1(x * 0.009 + 3.1 + time * 0.03);
    // Arcs across the oval: the bright one, one poleward, one equatorward, over a diffuse glow.
    const float a = (ahead - centre) / width;
    const float b = (ahead - centre - 75.0 - 40.0 * sky_noise1(x * 0.004 + 7.0 + time * 0.01)) / (width * 1.5);
    const float e = (ahead - centre + 110.0 + 50.0 * sky_noise1(x * 0.003 + 13.0 - time * 0.008)) / (width * 1.2);
    const float d = (ahead - centre + 20.0) / 260.0;
    const float band = exp(-a * a) + 0.5 * exp(-b * b) + 0.35 * exp(-e * e) + 0.12 * exp(-d * d);
    const float rays = 0.35 + 0.65 * sky_noise1(x * 0.2 + time * 0.55) * (0.5 + 0.5 * sky_noise1(x * 0.043 - time * 0.21 + 2.0));
    const float hf = (h - 95.0) / 180.0;
    const float3 col = lerp(sky.aurora_low.rgb, sky.aurora_high.rgb, smoothstep(0.12, 0.75, hf));
    acc += col * band * rays * (exp(-hf * 3.2) + 0.3 * smoothstep(0.35, 1.0, hf)) * saturate(1.0 - t / 2800.0);
  }
  return acc * sky.aurora.x * 0.012;
}

// The sky along a direction, exposed and toned.
float3 sky_color(float3 dir) {
  const float exposure = sky.ozone.w, scale = sky.stars.w;
  float3 d = dir;
  d.y = max(d.y, 0.002);
  d = normalize(d);
  float3 tr;
  float3 c = sky_scatter(d, tr) * scale + sky.night.rgb * (0.65 + 0.35 * (1.0 - d.y));
  float hidden = 0.0;  // how much of the stars something in front hides
  // The sun's disc, darker at its limb.
  const float mu = dot(dir, sky.sun.xyz);
  const float r = sky.sun.w;
  if (mu > cos(r * 1.15)) {
    const float x = saturate(acos(min(mu, 1.0)) / r);
    const float edge = 1.0 - smoothstep(0.92, 1.12, x);
    c += tr * sky.sun_color.rgb * 24.0 * (1.0 - 0.6 * (1.0 - sqrt(saturate(1.0 - x * x)))) * edge;
    hidden = max(hidden, edge);
  }
  // Moons: a lit sphere each, its phase from where the sun is.
  [unroll] for (int k = 0; k < 2; ++k) {
    const float mr = sky.moon_dir[k].w;
    if (mr <= 0.0) continue;
    const float3 m = sky.moon_dir[k].xyz;
    const float cm = dot(dir, m);
    if (cm < cos(mr * 1.2)) continue;
    float3 u = cross(float3(0.0, 1.0, 0.0), m);
    u = dot(u, u) < 1e-6 ? float3(1.0, 0.0, 0.0) : normalize(u);
    const float3 v = cross(m, u);
    const float2 q = float2(dot(dir, u), dot(dir, v)) / sin(mr);
    const float rr = dot(q, q);
    const float z = sqrt(saturate(1.0 - rr));
    const float3 n = normalize(u * q.x + v * q.y - m * z);
    const float lit = saturate(dot(n, sky.sun.xyz) * 1.5 + 0.05);
    const float mare = 0.72 + 0.28 * sky_noise3(float3(q * 3.2, z * 3.2) + k * 9.0);
    const float edge = 1.0 - smoothstep(0.94, 1.0, sqrt(rr));
    c += tr * sky.moon_color[k].rgb * (lit * mare + 0.012) * edge;
    hidden = max(hidden, edge);
  }
  // Stars and the lit constellations, where the sky is dark enough.
  if (dir.y > -0.03) {
    const float3 cd = float3(dot(sky.to_stars[0].xyz, dir), dot(sky.to_stars[1].xyz, dir), dot(sky.to_stars[2].xyz, dir));
    // A background star shows above its rank (alpha); the galaxy's glow (green, of 0.06) fades
    // with the density, as the fainter stars it is made of do.
    const float4 chart = star_tex.SampleLevel(star_smp, cd, 0);
    const float density = sky.stars.z;
    const float2 glows = glow_tex.SampleLevel(glow_smp, cd, 0).rg;
    const float glow = glows.r;
    const float3 st = chart.rgb * saturate((density - chart.a) * 40.0) +
                      float3(0.86, 0.90, 1.0) * glows.g * 0.06 * saturate(density * 2.0);
    const float bright = dot(c, float3(0.2126, 0.7152, 0.0722)) * exposure;
    const float horizon = smoothstep(-0.03, 0.08, dir.y);
    const float vis = exp(-bright * 9.0) * horizon * (1.0 - hidden);
    const float h = sky_hash(floor(cd.xy * 900.0) + floor(cd.z * 900.0));
    const float twinkle = 1.0 + sky.stars.x * sin(sky.eye.w * (3.0 + h * 6.0) + h * 40.0) * (0.25 + 0.75 * (1.0 - saturate(dir.y)));
    c += st * twinkle * vis;
    c += sky.lit_color.rgb * glow * 0.11 * (1.0 + sky.stars.y * sin(sky.eye.w * 1.6)) * exp(-bright * 3.0) * horizon * (1.0 - hidden);
  }
  c += sky_aurora(dir) * (1.0 - hidden * 0.7);
  // Clouds, on a layer: lit on the sun's side, dark in their thick middles, hazy far away.
  if (dir.y > 0.002 && sky.cloud.x > 0.001) {
    const float t = (sky.cloud.z - sky.eye.y) / dir.y;
    const float2 p = sky.eye.xz + dir.xz * t;
    const float dens = cloud_density(sky_cloud_tex, sky_cloud_smp, p, sky.cloud, sky.cloud_wind);
    if (dens > 0.0) {
      const float3 s = sky.sun.xyz;
      const float2 toward = s.xz / max(s.y, 0.12) * 220.0;
      const float behind = cloud_density(sky_cloud_tex, sky_cloud_smp, p + toward, sky.cloud, sky.cloud_wind);
      const float forward = pow(saturate(dot(dir, s)), 6.0);
      float3 col = sky.cloud_lit.rgb * (1.0 - 0.4 * behind) * (0.9 + 0.9 * forward * (1.0 - dens));
      col = lerp(col, sky.cloud_shade.rgb, dens * dens * 0.45);
      const float far = saturate(t / 45000.0);
      col = lerp(col, c, far * 0.85);
      c = lerp(c, col, dens * (1.0 - far * 0.6));
    }
  }
  // Under the horizon: the haze, darkening to the ground's colour (whatever is drawn there covers it).
  if (dir.y < 0.0) c = lerp(c, sky.ground.rgb, saturate(-dir.y * 4.0));
  return sky_tone(c * exposure);
}
