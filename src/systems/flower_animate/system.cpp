// flower_animate: one frame of a flower clump's pose (design/systems.md, Simulator).
//
// Each stem bone bends toward the wind as a damped oscillator at its natural
// frequency, driven by gusty wind: the steady state of each gust component plus
// a resonant sway with a per-stem phase, in closed form, so a frame depends only
// on its inputs and any time can be shown. The gusts are the same as the trees'
// (tree_animate), so flowers and trees given the same local wind move together.
#include <vector>

#include "boundary.h"
#include "channels/flower_motion.h"
#include "channels/flower_pose.h"
#include "math.h"
#include "packer/flower_motion.h"
#include "packer/flower_pose.h"

namespace flower_animate {

using namespace flower_math;

namespace {

// A rigid transform: x' = R x + t, with R row-major.
struct Xf {
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  vec3 t = {0, 0, 0};
  vec3 apply(vec3 v) const {
    return v3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
              r[6] * v.x + r[7] * v.y + r[8] * v.z) + t;
  }
  vec3 rotate_only(vec3 v) const { return apply(v) - t; }
};

Xf compose(const Xf &a, const Xf &b) {  // a after b
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      o.r[i * 3 + j] = a.r[i * 3] * b.r[j] + a.r[i * 3 + 1] * b.r[3 + j] + a.r[i * 3 + 2] * b.r[6 + j];
  o.t = a.apply(b.t);
  return o;
}

Xf rotation_about(vec3 p, vec3 a, float angle) {
  Xf o;
  const float c = foundry::det::cos(angle), s = foundry::det::sin(angle), k = 1 - c;
  o.r[0] = c + a.x * a.x * k;       o.r[1] = a.x * a.y * k - a.z * s; o.r[2] = a.x * a.z * k + a.y * s;
  o.r[3] = a.y * a.x * k + a.z * s; o.r[4] = c + a.y * a.y * k;       o.r[5] = a.y * a.z * k - a.x * s;
  o.r[6] = a.z * a.x * k - a.y * s; o.r[7] = a.z * a.y * k + a.x * s; o.r[8] = c + a.z * a.z * k;
  o.t = p - o.rotate_only(p);
  return o;
}

void put(const Xf &x, std::vector<float> &out) {
  const float m[16] = {x.r[0], x.r[3], x.r[6], 0, x.r[1], x.r[4], x.r[7], 0,
                       x.r[2], x.r[5], x.r[8], 0, x.t.x,  x.t.y,  x.t.z,  1};
  out.insert(out.end(), m, m + 16);
}

float unit_random(uint32_t seed, uint32_t k) { return static_cast<float>(hash_u32(seed ^ hash_u32(k)) >> 8) / 16777216.0f; }

struct Gust {
  float frequency, amplitude, phase;
};
// The trees' gusts: periods of roughly 2 to 15 seconds.
const Gust kGusts[3] = {{0.07f, 0.5f, 0.3f}, {0.19f, 0.3f, 2.1f}, {0.43f, 0.2f, 4.4f}};

}  // namespace

bool run(const channels::flower_motion &in, channels::flower_pose &out, std::string &error) {
  const size_t n = in.bones.size();
  for (size_t i = 0; i < n; ++i)
    if (in.bones[i].parent >= static_cast<int32_t>(i)) {
      error = "bones must come after their parents";
      return false;
    }
  out.matrices.clear();
  out.matrices.reserve(n * 16);
  const float a = in.wind_heading * kDeg;
  const vec3 wind = v3(foundry::det::sin(a), 0, foundry::det::cos(a));
  const float q = (in.wind_speed / 10.0f) * (in.wind_speed / 10.0f);
  const float steady = clampf(in.gust_damping, 0, 1);
  const float gust = 0.6f * (1 - steady);
  const float sway = 0.3f * (0.4f + 0.6f * (1 - steady));
  std::vector<Xf> world(n);
  for (size_t i = 0; i < n; ++i) {
    const auto &b = in.bones[i];
    Xf local;
    if (b.parent >= 0 && b.drag > 0 && q > 0) {
      const float fb = maxf(b.frequency, 0.05f), z = maxf(b.damping, 0.01f);
      float response = 1;
      for (const Gust &g : kGusts) {
        const float r = g.frequency / fb;
        const float re = 1 - r * r, im = 2 * z * r;
        const float d = re * re + im * im;
        const float w = 2 * kPi * g.frequency * in.time + g.phase;
        response += gust * g.amplitude * ((re / d) * foundry::det::sin(w) - (im / d) * foundry::det::cos(w));
      }
      const float phase = 2 * kPi * unit_random(in.seed, static_cast<uint32_t>(i));
      const float own = sway * foundry::det::sin(2 * kPi * fb * in.time + phase);
      const float along = clampf(b.drag * q * (response + own), -0.9f, 0.9f);
      const float across =
          clampf(b.drag * q * sway * 0.5f * foundry::det::sin(2 * kPi * fb * 1.17f * in.time + phase * 1.7f), -0.4f, 0.4f);
      vec3 k1 = cross(b.axis, wind);
      const float exposed = length(k1);
      k1 = exposed > 1e-3f ? k1 * (1 / exposed) : perpendicular(b.axis);
      const vec3 k2 = normalize(cross(b.axis, k1));
      local = compose(rotation_about(b.pivot, k1, along * (0.3f + 0.7f * exposed)), rotation_about(b.pivot, k2, across));
    }
    world[i] = b.parent >= 0 ? compose(world[static_cast<size_t>(b.parent)], local) : local;
  }
  for (const Xf &x : world) put(x, out.matrices);
  out.settled = in.wind_speed <= 0;
  return true;
}

}  // namespace flower_animate

FOUNDRY_SYSTEM(flower_animate, channels::flower_motion, channels::flower_pose)
