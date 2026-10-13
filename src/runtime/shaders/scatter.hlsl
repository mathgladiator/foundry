// scatter.hlsl: writes the poses that changed this frame into the persistent
// bone buffer (design/submit.md, "GPU skinning for crowds"): sleeping and
// unchanged instances keep their bones on the GPU, so only what moved is
// uploaded, packed, and each bone lands at its instance's slot.
// SDL GPU binding model for SPIR-V compute: read-only storage buffers are
// set 0, read-write storage buffers set 1, uniforms set 2.

[[vk::binding(0, 0)]] StructuredBuffer<float4> rows : register(t0, space0);  // 3 rows per bone, packed
[[vk::binding(1, 0)]] StructuredBuffer<uint> slots : register(t1, space0);   // per bone: where it goes
[[vk::binding(0, 1)]] RWStructuredBuffer<float4> bones : register(u0, space1);

[[vk::binding(0, 2)]] cbuffer Scatter : register(b0, space2) {
  uint count;
};

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
  const uint i = id.x;
  if (i >= count) return;
  const uint d = slots[i] * 3;
  bones[d] = rows[i * 3];
  bones[d + 1] = rows[i * 3 + 1];
  bones[d + 2] = rows[i * 3 + 2];
}
