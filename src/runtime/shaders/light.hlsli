// light.hlsli: the mega shader's light (design/submit.md; design/observatory.md),
// shared by every path that draws a mesh: the style.md section 6 cel bands, a
// key light and an unlit colour; under a sky, the key's shadow map, the clouds'
// shadows, fog toward the horizon, and the exposure. Without a sky (key_dir.w 0)
// it is the classic fixed light exactly.
//
// The including file may move its bindings: LIGHT_BINDING (the fragment uniform
// slot) and the registers LIGHT_REG, LIGHT_T0, LIGHT_S0, LIGHT_T1, LIGHT_S1.
// SDL GPU binding model for SPIR-V: fragment samplers set 2, uniforms set 3.

#include "clouds.hlsli"

#ifndef LIGHT_BINDING
#define LIGHT_BINDING 0
#define LIGHT_REG b0
#define LIGHT_T0 t0
#define LIGHT_S0 s0
#define LIGHT_T1 t1
#define LIGHT_S1 s1
#endif

struct LightData {
  float4 key_dir;     // xyz toward the key light; w 1 under a sky, 0 the classic light
  float4 key;         // rgb the lit band; w the darker band's factor
  float4 ambient;     // rgb the unlit colour; w the lit threshold
  float4 fog;         // rgb its colour (after exposure); w density per metre
  float4 eye;         // xyz the camera; w the exposure
  float4x4 shadow_vp; // world to the shadow map's clip space (column-major, as view_proj)
  float4 shadow;      // x 1 when on, y its texel in metres, z depth bias, w where it starts fading (of its radius)
  float4 shadow_at;   // xyz its centre, w its radius (m)
  float4 cloud;       // as clouds.hlsli
  float4 cloud_wind;
};

[[vk::binding(LIGHT_BINDING, 3)]] cbuffer LightBuffer : register(LIGHT_REG, space3) { LightData light; };

[[vk::combinedImageSampler]][[vk::binding(0, 2)]] Texture2D<float> shadow_tex : register(LIGHT_T0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState shadow_smp : register(LIGHT_S0, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] Texture2D<float4> cloud_tex : register(LIGHT_T1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] SamplerState cloud_smp : register(LIGHT_S1, space2);

// How much of the key reaches a point: the shadow map, 4 x 4 taps with bilinear weights (a soft
// edge about two texels wide), faded out toward the edge of its reach.
float shadow_term(float3 world, float3 n) {
  if (light.shadow.x < 0.5) return 1.0;
  const float3 p = world + n * light.shadow.y * 1.5;  // off the surface by a texel or so
  const float4 c = mul(light.shadow_vp, float4(p, 1.0));
  const float2 uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
  if (any(uv <= 0.0) || any(uv >= 1.0) || c.z >= 1.0) return 1.0;
  uint w, h;
  shadow_tex.GetDimensions(w, h);
  const float2 tc = uv * float2(w, h) - 0.5;
  const int2 base = int2(floor(tc));
  const float2 f = tc - floor(tc);
  const float z = c.z - light.shadow.z;
  float sum = 0.0;
  [unroll] for (int y = -1; y <= 2; ++y) {
    const float wy = y == -1 ? 1.0 - f.y : y == 2 ? f.y : 1.0;
    [unroll] for (int x = -1; x <= 2; ++x) {
      const float wx = x == -1 ? 1.0 - f.x : x == 2 ? f.x : 1.0;
      const int2 q = clamp(base + int2(x, y), int2(0, 0), int2(w - 1, h - 1));
      sum += (z <= shadow_tex.Load(int3(q, 0)) ? 1.0 : 0.0) * wx * wy;
    }
  }
  const float s = sum / 9.0;
  const float reach = length(world.xz - light.shadow_at.xz) / max(light.shadow_at.w, 1e-3);
  return lerp(s, 1.0, saturate((reach - light.shadow.w) / max(1.0 - light.shadow.w, 1e-3)));
}

// The clouds' shadow: the cloud map where the key's ray through the point meets the clouds.
float cloud_term(float3 world) {
  if (light.cloud.x <= 0.001) return 1.0;
  const float3 k = light.key_dir.xyz;
  const float up = max(k.y, 0.12);
  const float2 p = world.xz + k.xz / up * (light.cloud.z - world.y);
  return 1.0 - light.cloud.w * cloud_density(cloud_tex, cloud_smp, p, light.cloud, light.cloud_wind);
}

// The colour of a surface of this albedo, facing n (toward the viewer for two-sided sheets), here.
float3 shade(float3 albedo, float3 n, float3 world) {
  const float ndl = dot(n, light.key_dir.xyz);
  float lit = ndl > light.ambient.w ? 1.0 : 0.0;
  const bool sky = light.key_dir.w > 0.5;
  if (sky && lit > 0.0) lit *= shadow_term(world, n) * cloud_term(world);
  const float3 key = light.key.rgb * (ndl > 0.45 ? 1.0 : light.key.w);
  float3 c = albedo * lerp(light.ambient.rgb, key, lit);
  if (sky) {
    c *= light.eye.w;
    const float d = distance(world, light.eye.xyz);
    c = lerp(light.fog.rgb, c, exp(-d * light.fog.w));
  }
  return c;
}
