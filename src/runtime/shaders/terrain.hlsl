// terrain.hlsl: a terrain's nodes (design/boundary.md, "Levels"): one node per draw, its record
// (the instance's transform, its tile in the atlas, and its morph) streamed per instance. Each
// vertex rises toward its parent's surface as it nears the distance where the parent takes over,
// so switching levels never pops; its colour is the node's own image, lit by light.hlsli like every
// mesh (steep rock, flagged in the image's alpha, banded in strata by height), and under the classic light (no sky, so no fog) hazed by distance so the far land recedes.
// SDL GPU binding model for SPIR-V: vertex uniforms are set 1, fragment samplers set 2, fragment
// uniforms set 3.

#include "light.hlsli"

[[vk::combinedImageSampler]][[vk::binding(2, 2)]] Texture2D<float4> atlas_tex : register(t2, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]] SamplerState atlas_smp : register(s2, space2);

[[vk::binding(0, 1)]] cbuffer Frame : register(b0, space1) {
  float4x4 view_proj;  // column-major, as the C API passes it
  float4 eye;          // xyz: the camera
  float4 haze;         // rgb its colour; w its density per metre (the classic light only)
};

struct VsIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float2 uv : TEXCOORD2;
  [[vk::location(3)]] float morph : TEXCOORD3;
  // Per node drawn.
  [[vk::location(4)]] float4 row0 : TEXCOORD4;
  [[vk::location(5)]] float4 row1 : TEXCOORD5;
  [[vk::location(6)]] float4 row2 : TEXCOORD6;
  [[vk::location(7)]] float4 tile : TEXCOORD7;   // xy its origin in the atlas, zw its size
  [[vk::location(8)]] float4 range : TEXCOORD8;  // x where the morph starts (m), y 1 / its length
};

struct VsOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float3 normal : TEXCOORD0;
  [[vk::location(1)]] float2 uv : TEXCOORD1;
  [[vk::location(2)]] float3 world : TEXCOORD2;
};

float3 place(VsIn v, float3 p) { return float3(dot(v.row0, float4(p, 1)), dot(v.row1, float4(p, 1)), dot(v.row2, float4(p, 1))); }

VsOut vs_main(VsIn v) {
  float3 p = v.position;
  const float d = distance(place(v, p), eye.xyz);
  p.y += v.morph * saturate((d - v.range.x) * v.range.y);
  const float3 world = place(v, p);
  VsOut o;
  o.position = mul(view_proj, float4(world, 1.0));
  o.normal = float3(dot(v.row0.xyz, v.normal), dot(v.row1.xyz, v.normal), dot(v.row2.xyz, v.normal));
  o.uv = v.tile.xy + v.uv * v.tile.zw;
  o.world = world;
  return o;
}

float4 ps_main(VsOut i) : SV_Target0 {
  const float3 n = normalize(i.normal);
  const float4 texel = atlas_tex.Sample(atlas_smp, i.uv);
  float3 albedo = texel.rgb;
  // Steep rock (alpha toward 0): strata by height, which a top-down image can't hold on a cliff.
  const float band = frac(i.world.y / 6.5 + 0.15 * sin(i.world.x * 0.05) + 0.15 * sin(i.world.z * 0.043));
  albedo *= lerp(1.0, band < 0.3 ? 0.68 : 1.05, 1.0 - texel.a);
  float3 c = shade_lit(albedo, n, i.world);
  if (light.key_dir.w < 0.5) c = lerp(haze.rgb, c, exp(-distance(i.world, eye.xyz) * haze.w));
  return float4(look_apply(light.look, c), 1.0);
}
