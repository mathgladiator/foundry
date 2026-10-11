// cel.hlsl: the first cut of the mega shader (design/submit.md): instanced
// meshes with the style.md section 6 cel bands. Compiled to SPIR-V by DXC at
// build time. SDL GPU binding model: vertex uniforms are set 1; fragment
// uniforms would be set 3.

[[vk::binding(0, 1)]] cbuffer Frame : register(b0, space1) {
  float4x4 view_proj;  // column-major, as the C API passes it
};

struct VsIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float4 color : TEXCOORD2;
  // Per instance: the rows of a 3x4 transform.
  [[vk::location(3)]] float4 row0 : TEXCOORD3;
  [[vk::location(4)]] float4 row1 : TEXCOORD4;
  [[vk::location(5)]] float4 row2 : TEXCOORD5;
};

struct VsOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float3 normal : TEXCOORD0;
  [[vk::location(1)]] float4 color : TEXCOORD1;
};

VsOut vs_main(VsIn v) {
  float4 p = float4(v.position, 1.0);
  float3 world = float3(dot(v.row0, p), dot(v.row1, p), dot(v.row2, p));
  float3 n = float3(dot(v.row0.xyz, v.normal), dot(v.row1.xyz, v.normal), dot(v.row2.xyz, v.normal));
  VsOut o;
  o.position = mul(view_proj, float4(world, 1.0));
  o.normal = n;
  o.color = v.color;
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
