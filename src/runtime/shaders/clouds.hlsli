// clouds.hlsli: the sky's cloud map (design/observatory.md), shared by the sky
// pass and the light on the ground, so a cloud's shadow is under the cloud. Two
// tiling noises (broad shapes in R, detail in G) drift with the wind, cut by the
// cover.
//   cloud: x cover (0 clear to 1 overcast), y 1 / the map's tile (4 clouds across, per metre),
//          z their height (m), w how dark their shadow is
//   wind:  xy drift (m/s, x east, y south), z the time (s)

#ifndef FOUNDRY_CLOUDS_HLSLI
#define FOUNDRY_CLOUDS_HLSLI

float cloud_density(Texture2D<float4> tex, SamplerState smp, float2 xz, float4 cloud, float4 wind) {
  if (cloud.x <= 0.001) return 0.0;
  const float2 drift = wind.xy * wind.z;
  const float2 uv = (xz - drift) * cloud.y;
  const float a = tex.SampleLevel(smp, uv, 0).r;
  // A broader layer, turned, so the tile doesn't repeat toward the horizon; and finer detail.
  const float2 turned = float2(uv.x * 0.83 - uv.y * 0.56, uv.x * 0.56 + uv.y * 0.83) * 0.37 + float2(0.21, 0.63);
  const float a2 = tex.SampleLevel(smp, turned, 0).r;
  const float b = tex.SampleLevel(smp, uv * 3.1 + float2(0.37, 0.71) - drift * cloud.y * 0.6, 0).g;
  const float n = a * 0.5 + a2 * 0.32 + b * 0.18;
  const float edge = 1.02 - cloud.x * 1.05;
  return smoothstep(edge - 0.04, edge + 0.22, n);
}

#endif
