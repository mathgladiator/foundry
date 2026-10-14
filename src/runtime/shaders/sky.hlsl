// sky.hlsl: the observatory's sky as the runtime draws it (design/observatory.md):
// a triangle over the whole target at the far plane, drawn after everything else,
// so the depth test leaves only the pixels nothing covers.
// SDL GPU binding model for SPIR-V: fragment samplers set 2, uniforms set 3.

#include "sky.hlsli"

struct SkyOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 ndc : TEXCOORD0;
};

SkyOut sky_vs(uint id : SV_VertexID) {
  SkyOut o;
  const float2 p = float2((id << 1) & 2, id & 2);
  o.ndc = p * float2(2, -2) + float2(-1, 1);
  o.position = float4(o.ndc, 1.0, 1.0);
  return o;
}

float4 sky_ps(SkyOut i) : SV_Target0 {
  const float3 dir = normalize(sky.ray_z.xyz + i.ndc.x * sky.ray_x.xyz + i.ndc.y * sky.ray_y.xyz);
  const float dither = (sky_hash(i.position.xy) - 0.5) / 255.0;
  return float4(look_apply(sky.look, sky_color(dir)) + dither, 1.0);
}
