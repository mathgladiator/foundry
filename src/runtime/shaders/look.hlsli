// look.hlsli: the game's look (design/submit.md, "The look"): a per-pixel grade the game sets for
// everything the runtime draws, for accessibility and for effects. Colour vision (daltonizing for
// protanopia, deuteranopia, or tritanopia: what the eye can't separate is moved into channels it
// can), contrast (an S-curve on luminance, with a little more saturation), saturation, and an
// effect (greyscale, or a duotone from black through one hue to white) at a strength. Models never need to agree; the
// identity look (all zero but saturation 1) changes nothing. Mirrored on the CPU by
// src/runtime/render/look.h.
#ifndef LOOK_HLSLI
#define LOOK_HLSLI

struct LookData {
  float4 a;  // x colour vision (0 typical, 1 protan, 2 deutan, 3 tritan), y contrast 0..1, z saturation (1 as is), w effect (0 none, 1 grey, 2 tint)
  float4 b;  // rgb the tint's hue, w the effect's strength 0..1
  float4 c;  // x edges 0..1 (lines between colours, where a pass can see neighbours), yzw unused
};

float look_luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 look_apply(LookData L, float3 c) {
  const int vision = (int)(L.a.x + 0.5);
  if (vision > 0) {
    const float3x3 to_lms = float3x3(17.8824, 43.5161, 4.11935, 3.45565, 27.1554, 3.86714, 0.0299566, 0.184309, 1.46709);
    const float3x3 to_rgb = float3x3(0.0809444479, -0.130504409, 0.116721066, -0.0102485335, 0.0540193266, -0.113614708,
                                     -0.000365296938, -0.00412161469, 0.693511405);
    float3 lms = mul(to_lms, c);
    if (vision == 1) lms.x = 2.02344 * lms.y - 2.52581 * lms.z;
    else if (vision == 2) lms.y = 0.494207 * lms.x + 1.24827 * lms.z;
    else lms.z = -0.395913 * lms.x + 0.801109 * lms.y;
    const float3 err = c - mul(to_rgb, lms);
    c += vision == 3 ? float3(err.r + 0.7 * err.b, err.g + 0.7 * err.b, 0) : float3(0, 0.7 * err.r + err.g, 0.7 * err.r + err.b);
    c = saturate(c);
  }
  float l = look_luma(c);
  if (L.a.y > 0 || abs(L.a.z - 1) > 1e-3) {
    const float s = saturate(l);
    const float l2 = lerp(l, s * s * (3 - 2 * s), saturate(L.a.y));
    const float sat = L.a.z * (1 + 0.35 * saturate(L.a.y));
    c = max(l2 + (c - l) * sat, 0);
    l = l2;
  }
  const int effect = (int)(L.a.w + 0.5);
  if (effect == 1) c = lerp(c, float3(l, l, l), saturate(L.b.w));
  else if (effect == 2) {
    // A duotone: black to the hue to white, by brightness.
    const float t = saturate(l);
    const float3 d = t < 0.5 ? L.b.rgb * (2 * t) : lerp(L.b.rgb, float3(1, 1, 1), 2 * t - 1);
    c = lerp(c, d, saturate(L.b.w));
  }
  return c;
}

#endif
