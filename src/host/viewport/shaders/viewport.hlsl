// viewport.hlsl: the foundry viewport on the GPU, the same look as the CPU
// renderer (raster.cpp; style.md section 6): cel bands with warm light and cool
// shadow, a rim band, ink on silhouettes and depth edges, and overlay lines that
// fade where the mesh hides them. Drawn at twice the size and box-filtered down.
// Lit by the runtime's light.hlsli, so under an observatory's sky (sky_ps, the
// runtime's sky.hlsli) a model has the same sun, shadows, clouds, and fog as in
// the game; shadow_ps draws the shadow map's depth.
// SDL GPU binding model for SPIR-V: vertex uniforms are set 1; fragment samplers
// set 2, fragment uniforms set 3.

// The mesh pass's light is its second uniform slot (Look is the first); the sky's is its third.
#define LIGHT_BINDING 1
#define LIGHT_REG b1
#define LIGHT_T0 t4
#define LIGHT_S0 s4
#define LIGHT_T1 t5
#define LIGHT_S1 s5
#include "../../../runtime/shaders/light.hlsli"
#define SKY_BINDING 2
#define SKY_REG b2
#define SKY_T0 t6
#define SKY_S0 s6
#define SKY_T1 t7
#define SKY_S1 s7
#define SKY_T2 t8
#define SKY_S2 s8
#include "../../../runtime/shaders/sky.hlsli"

[[vk::binding(0, 1)]] cbuffer Camera : register(b0, space1) {
  float4x4 clip;     // world to clip
  float4 eye;        // xyz: the eye; w: 1 when orthographic
  float4 forward;    // xyz: the view direction
};

struct MeshIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float4 color : TEXCOORD2;
};

struct MeshOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float3 world : TEXCOORD0;
  [[vk::location(1)]] float3 normal : TEXCOORD1;
  [[vk::location(2)]] float4 color : TEXCOORD2;
  [[vk::location(3)]] float depth : TEXCOORD3;  // distance along the view direction
};

MeshOut mesh_vs(MeshIn v) {
  MeshOut o;
  o.position = mul(clip, float4(v.position, 1.0));
  o.world = v.position;
  o.normal = v.normal;
  o.color = v.color;
  o.depth = dot(v.position - eye.xyz, forward.xyz);
  return o;
}

[[vk::binding(0, 3)]] cbuffer Look : register(b0, space3) {
  float4 mode;  // x: 1 draws a silhouette (solid black); y: 1 under a sky (the background is the sky pass's);
                // z: 1 draws backdrop (a sky's ground: no depth for the ink, so it is never inked)
};

struct MeshTargets {
  float4 color : SV_Target0;
  float4 albedo : SV_Target1;
  float depth : SV_Target2;  // linear depth; 0 where nothing was drawn
};

MeshTargets mesh_ps(MeshOut i) {
  MeshTargets o;
  o.depth = mode.z > 0.5 ? 0.0 : max(i.depth, 1e-4);
  if (mode.x > 0.5) {
    o.color = float4(0, 0, 0, 1);
    o.albedo = float4(0, 0, 0, 1);
    return o;
  }
  float3 to_cam = eye.w > 0.5 ? -forward.xyz : normalize(eye.xyz - i.world);
  float3 n = normalize(i.normal);
  if (dot(n, to_cam) < 0) n = -n;  // two-sided
  float3 al = i.color.rgb;
  float3 c = shade(al, n, i.world);
  if (dot(n, to_cam) < 0.28 && mode.z < 0.5) c += float3(0.07, 0.07, 0.08) * (light.key_dir.w > 0.5 ? light.eye.w * dot(light.ambient.rgb, 0.6) : 1.0);  // rim band
  o.color = float4(c, 1);
  o.albedo = float4(al, 1);
  return o;
}

// Depth only, for the shadow map.
void shadow_ps() {}

// The sky, after the mesh at the far plane: where nothing was drawn, its colour, and no depth
// (so the ink pass treats it as background).
struct SkyFull {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 ndc : TEXCOORD0;
};

SkyFull skyfull_vs(uint id : SV_VertexID) {
  SkyFull o;
  const float2 p = float2((id << 1) & 2, id & 2);
  o.ndc = p * float2(2, -2) + float2(-1, 1);
  o.position = float4(o.ndc, 1.0, 1.0);
  return o;
}

MeshTargets sky_ps(SkyFull i) {
  const float3 dir = normalize(sky.ray_z.xyz + i.ndc.x * sky.ray_x.xyz + i.ndc.y * sky.ray_y.xyz);
  MeshTargets o;
  o.color = float4(sky_color(dir) + (sky_hash(i.position.xy) - 0.5) / 255.0, 1);
  o.albedo = float4(0, 0, 0, 1);
  o.depth = 0;
  return o;
}

// A triangle over the whole target.
struct Full {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

Full full_vs(uint id : SV_VertexID) {
  Full o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p;
  return o;
}

// Each pass's textures start at binding 0 (SDL binds a stage's samplers in order); resources a pass
// doesn't use are dropped from its module, so the passes can share bindings.
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] Texture2D<float4> color_tex : register(t0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState color_smp : register(s0, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] Texture2D<float4> albedo_tex : register(t1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]] SamplerState albedo_smp : register(s1, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]] Texture2D<float> depth_tex : register(t2, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]] SamplerState depth_smp : register(s2, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] Texture2D<float> line_depth_tex : register(t3, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]] SamplerState line_depth_smp : register(s3, space2);

// Ink on silhouettes and depth edges (a pixel two supersampled pixels out, so the ink is about one
// final pixel wide), and the background's vertical gradient.
float4 ink_ps(Full i) : SV_Target0 {
  uint w, h;
  depth_tex.GetDimensions(w, h);
  int2 p = int2(i.position.xy);
  float d = depth_tex.Load(int3(p, 0));
  bool mesh = d > 0;
  bool silhouette = mode.x > 0.5;
  if (!mesh) {
    if (mode.y > 0.5) return color_tex.Load(int3(p, 0));
    float s = (float)p.y / (float)h;
    return float4(0.20 + 0.06 * s, 0.22 + 0.05 * s, 0.27 + 0.02 * s, 1);
  }
  bool edge = false;
  if (p.x >= 1 && p.y >= 1 && p.x + 1 < (int)w && p.y + 1 < (int)h) {
    const int2 dirs[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    [unroll] for (int k = 0; k < 8; ++k) {
      int2 q = p + dirs[k % 4] * (k < 4 ? 1 : 2);
      if (q.x < 0 || q.y < 0 || q.x >= (int)w || q.y >= (int)h) continue;
      float dq = depth_tex.Load(int3(q, 0));
      if (dq <= 0) edge = true;
      else if (abs(d - dq) > 0.03 * max(d, 1.0) && d <= dq) edge = true;
    }
  }
  if (!edge) return color_tex.Load(int3(p, 0));
  float3 a = albedo_tex.Load(int3(p, 0)).rgb;
  return float4(silhouette ? float3(0, 0, 0) : a * 0.28, 1);
}

// Half the size: the bilinear sample at the corner of each 2x2 block is their average.
float4 down_ps(Full i) : SV_Target0 { return color_tex.Sample(color_smp, i.uv); }

// Overlay lines, at the final size: full alpha where they are in front of the mesh, `hidden` where
// it hides them (the linear depth, at twice the size, says which).
struct LineIn {
  [[vk::location(0)]] float3 position : TEXCOORD0;
  [[vk::location(1)]] float4 color : TEXCOORD1;  // rgb, alpha
  [[vk::location(2)]] float hidden : TEXCOORD2;
};

struct LineOut {
  float4 position : SV_Position;
  [[vk::location(0)]] float4 color : TEXCOORD0;
  [[vk::location(1)]] float hidden : TEXCOORD1;
  [[vk::location(2)]] float depth : TEXCOORD2;
};

LineOut line_vs(LineIn v) {
  LineOut o;
  o.position = mul(clip, float4(v.position, 1.0));
  o.color = v.color;
  o.hidden = v.hidden;
  o.depth = dot(v.position - eye.xyz, forward.xyz);
  return o;
}

float4 line_ps(LineOut i) : SV_Target0 {
  int2 p = int2(i.position.xy) * 2;
  float d = line_depth_tex.Load(int3(p, 0));
  bool seen = d <= 0 || i.depth <= d * 1.002 + 0.002;
  return float4(i.color.rgb, seen ? i.color.a : i.hidden);
}
