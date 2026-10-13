// skinned.hlsl: the mega shader's skinned path (design/submit.md): instances
// of one mesh, each with its own bone matrices, blended from two bones per
// vertex, then lit by light.hlsli (the style.md section 6 cel bands, and under a
// sky its shadows, clouds, fog, and exposure). shadow_ps draws only depth, for
// the sky's shadow map.
// SDL GPU binding model for SPIR-V: vertex storage buffers are set 0,
// vertex uniforms set 1, fragment samplers set 2, fragment uniforms set 3.

#include "light.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<float4> bones : register(t0, space0);  // 3 rows per bone

[[vk::binding(0, 1)]] cbuffer Frame : register(b0, space1) {
  float4x4 view_proj;  // column-major, as the C API passes it
};

struct VsIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float4 color : TEXCOORD2;
  [[vk::location(3)]] uint2 bone_ab : TEXCOORD3;
  [[vk::location(4)]] float bone_w : TEXCOORD4;
  // Per instance: the rows of a 3x4 transform, and the first of its bones.
  [[vk::location(5)]] float4 row0 : TEXCOORD5;
  [[vk::location(6)]] float4 row1 : TEXCOORD6;
  [[vk::location(7)]] float4 row2 : TEXCOORD7;
  [[vk::location(8)]] uint bone_offset : TEXCOORD8;
};

struct VsOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float3 normal : TEXCOORD0;
  [[vk::location(1)]] float4 color : TEXCOORD1;
  [[vk::location(2)]] float3 world : TEXCOORD2;
};

VsOut vs_main(VsIn v) {
  uint a = (v.bone_offset + v.bone_ab.x) * 3, b = (v.bone_offset + v.bone_ab.y) * 3;
  float w = v.bone_w;
  float4 s0 = bones[a] * (1 - w) + bones[b] * w;
  float4 s1 = bones[a + 1] * (1 - w) + bones[b + 1] * w;
  float4 s2 = bones[a + 2] * (1 - w) + bones[b + 2] * w;
  float4 p = float4(v.position, 1.0);
  float4 posed = float4(dot(s0, p), dot(s1, p), dot(s2, p), 1.0);
  float3 n = float3(dot(s0.xyz, v.normal), dot(s1.xyz, v.normal), dot(s2.xyz, v.normal));
  float3 world = float3(dot(v.row0, posed), dot(v.row1, posed), dot(v.row2, posed));
  n = float3(dot(v.row0.xyz, n), dot(v.row1.xyz, n), dot(v.row2.xyz, n));
  VsOut o;
  o.position = mul(view_proj, float4(world, 1.0));
  o.normal = n;
  o.color = v.color;
  o.world = world;
  return o;
}

float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target0 {
  const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
  return float4(shade(i.color.rgb, n, i.world), 1.0);
}

void shadow_ps() {}
