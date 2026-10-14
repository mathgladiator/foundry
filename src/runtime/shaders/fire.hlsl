// fire.hlsl: the fire system's sprites (design/submit.md, "Fire"; runtime/fire/fire.h): one
// camera-facing quad per particle, drawn after everything solid, depth-tested without writing
// depth. Flames, sparks, and glows add light (a flame's tongue stretched up, a teardrop);
// smoke is blended over, lit by the sky's light, and fogged. Both apply the game's look last, as
// every mesh does (light.hlsli).
// SDL GPU binding model for SPIR-V: vertex uniforms are set 1, fragment samplers set 2, fragment
// uniforms set 3.

#include "light.hlsli"

[[vk::binding(0, 1)]] cbuffer Frame : register(b0, space1) {
  float4x4 view_proj;  // column-major, as the C API passes it
  float4 right;        // the camera's right, world space
  float4 up;           // the camera's up, world space
};

struct VsIn {
  [[vk::location(0)]] float4 pos_size : TEXCOORD0;
  [[vk::location(1)]] float4 color : TEXCOORD1;
  [[vk::location(2)]] float2 stretch_add : TEXCOORD2;
};

struct VsOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float4 color : TEXCOORD0;
  [[vk::location(1)]] float2 uv : TEXCOORD1;
  [[vk::location(2)]] float3 world : TEXCOORD2;
  [[vk::location(3)]] float2 kind : TEXCOORD3;  // x stretch, y additive
};

VsOut vs_main(VsIn v, uint id : SV_VertexID) {
  const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
  const float2 c = corners[id];
  const float s = v.pos_size.w;
  // A tongue rises: stretch it along the world's up as the camera sees it.
  float3 upw = normalize(lerp(up.xyz, float3(0, 1, 0), 0.7));
  const float stretch = v.stretch_add.x;
  const float3 world = v.pos_size.xyz + right.xyz * (c.x * s) + upw * (c.y * s * stretch + (stretch - 1.0) * s * 0.6);
  VsOut o;
  o.position = mul(view_proj, float4(world, 1.0));
  o.color = v.color;
  o.uv = c;
  o.world = world;
  o.kind = float2(stretch, v.stretch_add.y);
  return o;
}

float4 ps_main(VsOut i) : SV_Target0 {
  float2 uv = i.uv;
  // A teardrop for a tongue: narrower toward the top.
  if (i.kind.x > 1.2) uv.x /= max(1.0 - saturate(uv.y * 0.5 + 0.5) * 0.65, 0.2);
  const float r2 = dot(uv, uv);
  if (r2 >= 1.0) discard;
  const float soft = (1.0 - r2) * (1.0 - r2);
  const bool sky = light.key_dir.w > 0.5;
  if (i.kind.y > 0.5) {
    // Light: its colour, brighter at the core, at full exposure (a fire lights the night).
    float3 c = i.color.rgb * (0.6 + 0.8 * soft) * i.color.a * soft;
    if (sky) c *= min(light.eye.w, 3.0);
    return float4(look_apply(light.look, c), 0.0);
  }
  // Smoke: lit by the sky (half the key, half the unlit colour), fogged like the rest.
  float3 c = i.color.rgb * lerp(light.ambient.rgb, light.key.rgb, 0.55);
  float a = i.color.a * soft;
  if (sky) {
    c *= light.eye.w;
    const float d = distance(i.world, light.eye.xyz);
    c = lerp(light.fog.rgb, c, exp(-d * light.fog.w));
  }
  return float4(look_apply(light.look, c), a);
}
