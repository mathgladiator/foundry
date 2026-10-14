// The game's look (design/submit.md, "The look"; look.hlsli): a per-pixel grade for accessibility
// and effects, set once for everything the runtime draws. This is its CPU mirror (the foundry's
// software renderer) and the uniform layout the shaders read.
#pragma once

#include <algorithm>
#include <cmath>

namespace runtime {

struct Look {
  int vision = 0;          // 0 typical, 1 protanopia, 2 deuteranopia, 3 tritanopia
  float contrast = 0;      // 0..1
  float saturation = 1;    // 1 as is
  float edges = 0;         // 0..1: lines between colours (passes that see neighbours)
  int effect = 0;          // 0 none, 1 greyscale, 2 tint toward a hue
  float tint[3] = {0.85f, 0.12f, 0.1f};
  float strength = 1;      // the effect's, 0..1
  bool identity() const { return vision == 0 && contrast <= 0 && std::fabs(saturation - 1) < 1e-3f && (effect == 0 || strength <= 0); }
  // look.hlsli's LookData: three float4s.
  void uniform(float out[12]) const {
    const float u[12] = {static_cast<float>(vision), contrast, saturation, static_cast<float>(effect), tint[0], tint[1], tint[2], strength,
                         edges, 0, 0, 0};
    std::copy(u, u + 12, out);
  }
};

inline float look_luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// look.hlsli's look_apply, on one colour (rgb, 0..1 and over).
inline void look_apply(const Look &L, float c[3]) {
  auto sat = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
  if (L.vision > 0) {
    const float m[3][3] = {{17.8824f, 43.5161f, 4.11935f}, {3.45565f, 27.1554f, 3.86714f}, {0.0299566f, 0.184309f, 1.46709f}};
    const float n[3][3] = {{0.0809444479f, -0.130504409f, 0.116721066f},
                           {-0.0102485335f, 0.0540193266f, -0.113614708f},
                           {-0.000365296938f, -0.00412161469f, 0.693511405f}};
    float lms[3], sim[3];
    for (int i = 0; i < 3; ++i) lms[i] = m[i][0] * c[0] + m[i][1] * c[1] + m[i][2] * c[2];
    if (L.vision == 1) lms[0] = 2.02344f * lms[1] - 2.52581f * lms[2];
    else if (L.vision == 2) lms[1] = 0.494207f * lms[0] + 1.24827f * lms[2];
    else lms[2] = -0.395913f * lms[0] + 0.801109f * lms[1];
    for (int i = 0; i < 3; ++i) sim[i] = n[i][0] * lms[0] + n[i][1] * lms[1] + n[i][2] * lms[2];
    const float e[3] = {c[0] - sim[0], c[1] - sim[1], c[2] - sim[2]};
    if (L.vision == 3) {
      c[0] += e[0] + 0.7f * e[2];
      c[1] += e[1] + 0.7f * e[2];
    } else {
      c[1] += 0.7f * e[0] + e[1];
      c[2] += 0.7f * e[0] + e[2];
    }
    for (int i = 0; i < 3; ++i) c[i] = sat(c[i]);
  }
  float l = look_luma(c);
  if (L.contrast > 0 || std::fabs(L.saturation - 1) > 1e-3f) {
    const float s = sat(l);
    const float l2 = l + (s * s * (3 - 2 * s) - l) * sat(L.contrast);
    const float sa = L.saturation * (1 + 0.35f * sat(L.contrast));
    for (int i = 0; i < 3; ++i) c[i] = std::max(l2 + (c[i] - l) * sa, 0.0f);
    l = l2;
  }
  const float k = sat(L.strength);
  if (L.effect == 1) {
    for (int i = 0; i < 3; ++i) c[i] += (l - c[i]) * k;
  } else if (L.effect == 2) {
    // A duotone: black to the hue to white, by brightness.
    const float t = sat(l);
    for (int i = 0; i < 3; ++i) {
      const float d = t < 0.5f ? L.tint[i] * (2 * t) : L.tint[i] + (1 - L.tint[i]) * (2 * t - 1);
      c[i] += (d - c[i]) * k;
    }
  }
}

}  // namespace runtime
