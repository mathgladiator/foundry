// field.hlsl: a field's tiles (design/plotter.md, "Fields"; design/submit.md,
// "Cost"): one shared tile mesh per draw, instanced at every cell that shows
// it, turned by sixths, scaled in height, tinted, and swayed by the wind here,
// on the GPU, with no bones and no pose: a vertex moves by its sway (metres in a
// 10 m/s wind) times the wind, in gusts that travel across the field. Lit with
// the same cel bands as skinned.hlsl.
// SDL GPU binding model for SPIR-V: vertex uniforms are set 1.

[[vk::binding(0, 1)]] cbuffer Frame : register(b0, space1) {
  float4x4 view_proj;  // column-major, as the C API passes it
  float4 clock;        // x: the world's time, seconds
};

struct VsIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float4 color : TEXCOORD2;
  [[vk::location(3)]] float sway : TEXCOORD3;
  // Per tile: where it stands and its height scale, the wind (metres per metre of sway), its tint
  // (halved, so up to 2), and its turn (low 3 bits) with a phase for the gusts.
  [[vk::location(4)]] float4 at_scale : TEXCOORD4;
  [[vk::location(5)]] float2 wind : TEXCOORD5;
  [[vk::location(6)]] float4 tint : TEXCOORD6;
  [[vk::location(7)]] uint turn_phase : TEXCOORD7;
};

struct VsOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float3 normal : TEXCOORD0;
  [[vk::location(1)]] float4 color : TEXCOORD1;
};

VsOut vs_main(VsIn v) {
  const float turn = (float)(v.turn_phase & 7u) * 1.04719755;
  const float phase = (float)(v.turn_phase >> 3) * (6.2831853 / 536870912.0);
  const float c = cos(turn), s = sin(turn);
  float3 local = float3(v.position.x * c - v.position.z * s, v.position.y * v.at_scale.w, v.position.x * s + v.position.z * c);
  float3 world = v.at_scale.xyz + local;
  // Gusts: a slow swell that travels with the wind, and a quicker flutter.
  const float t = clock.x;
  const float along = dot(world.xz, v.wind) * 0.35;
  const float gust = 0.65 + 0.35 * sin(t * 1.3 - along + phase) + 0.12 * sin(t * 4.1 + phase * 3.0 + world.x * 0.7);
  const float2 offset = v.wind * (v.sway * v.at_scale.w * gust);
  world.xz += offset;
  world.y -= dot(offset, offset) / max(2.0 * local.y, 0.1);  // the tip swings on an arc
  float3 n = float3(v.normal.x * c - v.normal.z * s, v.normal.y, v.normal.x * s + v.normal.z * c);
  VsOut o;
  o.position = mul(view_proj, float4(world, 1.0));
  o.normal = n;
  o.color = float4(v.color.rgb * lerp(float3(1, 1, 1), v.tint.rgb * 2.0, v.color.a), 1.0);  // the tint is the grass's, not the ground's
  return o;
}

float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target0 {
  const float3 light_dir = normalize(float3(0.62, 0.68, 0.40));
  const float3 light = float3(1.04, 0.98, 0.88);
  const float3 shadow = float3(0.50, 0.52, 0.72);
  float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
  float ndl = dot(n, light_dir);
  float3 c = ndl > 0.02 ? i.color.rgb * light * (ndl > 0.45 ? 1.0 : 0.84) : i.color.rgb * shadow * 1.05;
  return float4(c, 1.0);
}
