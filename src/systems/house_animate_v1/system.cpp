// house_animate_v1: one frame of a building's moving parts (design/systems.md, Village houses).
//
// Every motion is closed form, so a frame depends only on its inputs and any time can be shown:
// - sway: bends toward the wind as a damped oscillator driven by the same gusts as the trees
//   (tree_animate_v1), so signs, banners, and laundry move with the trees around them;
// - spin: turns at its rate (faster with the wind when it has drag: sails); with no rate it is a
//   weathervane, turned to point where the wind blows, wobbling in the gusts;
// - rise: a smoke puff rising, swelling, drifting downwind, and shrinking away, on a loop;
// - hinge: the door, swinging open when door_time starts, staying for a visit, and closing;
// - flicker: a glow pulsing.
#include <vector>

#include "boundary.h"
#include "channels/house_v1_motion.h"
#include "channels/house_v1_pose.h"
#include "math.h"
#include "packer/house_v1_motion.h"
#include "packer/house_v1_pose.h"

namespace house_animate_v1 {

using namespace house_math;
using channels::house_v1_bone;

namespace {

// A transform x' = R x + t, with R row-major (rotation and scale).
struct Xf {
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  vec3 t = {0, 0, 0};
  vec3 apply(vec3 v) const {
    return v3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z) + t;
  }
  vec3 linear(vec3 v) const { return apply(v) - t; }
};

Xf compose(const Xf &a, const Xf &b) {  // a after b
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i * 3 + j] = a.r[i * 3] * b.r[j] + a.r[i * 3 + 1] * b.r[3 + j] + a.r[i * 3 + 2] * b.r[6 + j];
  o.t = a.apply(b.t);
  return o;
}

Xf rotation_about(vec3 p, vec3 a, float angle) {
  Xf o;
  const float c = foundry::det::cos(angle), s = foundry::det::sin(angle), k = 1 - c;
  o.r[0] = c + a.x * a.x * k;       o.r[1] = a.x * a.y * k - a.z * s; o.r[2] = a.x * a.z * k + a.y * s;
  o.r[3] = a.y * a.x * k + a.z * s; o.r[4] = c + a.y * a.y * k;       o.r[5] = a.y * a.z * k - a.x * s;
  o.r[6] = a.z * a.x * k - a.y * s; o.r[7] = a.z * a.y * k + a.x * s; o.r[8] = c + a.z * a.z * k;
  o.t = p - o.linear(p);
  return o;
}

// Scales by s about p, then moves by d.
Xf scale_about(vec3 p, float s, vec3 d) {
  Xf o;
  o.r[0] = o.r[4] = o.r[8] = s;
  o.t = p - p * s + d;
  return o;
}

void put(const Xf &x, std::vector<float> &out) {
  const float m[16] = {x.r[0], x.r[3], x.r[6], 0, x.r[1], x.r[4], x.r[7], 0, x.r[2], x.r[5], x.r[8], 0, x.t.x, x.t.y, x.t.z, 1};
  out.insert(out.end(), m, m + 16);
}

float frac(float x) { return x - std::floor(x); }

struct Gust {
  float frequency, amplitude, phase;
};
// The trees' gusts: periods of roughly 2 to 15 seconds.
const Gust kGusts[3] = {{0.07f, 0.5f, 0.3f}, {0.19f, 0.3f, 2.1f}, {0.43f, 0.2f, 4.4f}};

// How far a door is open, 0 to 1, door_time seconds after it opened: 0.6 s to open, a 3 s visit,
// 0.8 s to close.
float door_open(float t) {
  if (t < 0) return 0;
  if (t < 0.6f) return smooth(t / 0.6f);
  if (t < 3.6f) return 1;
  if (t < 4.4f) return 1 - smooth((t - 3.6f) / 0.8f);
  return 0;
}

}  // namespace

bool run(const channels::house_v1_motion &in, channels::house_v1_pose &out, std::string &error) {
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
  const float speed = maxf(in.wind_speed, 0);
  const float q = (speed / 10.0f) * (speed / 10.0f);
  const float steady = clampf(in.gust_damping, 0, 1);
  const float gust = 0.6f * (1 - steady);
  const float time = in.time;
  bool moving = false;
  std::vector<Xf> world(n);
  for (size_t i = 0; i < n; ++i) {
    const house_v1_bone &b = in.bones[i];
    Xf local;
    // The gusts' sum at this bone's frequency (1 in still air).
    auto response = [&](float fb, float z) {
      float r = 1;
      for (const Gust &g : kGusts) {
        const float x = g.frequency / maxf(fb, 0.05f);
        const float re = 1 - x * x, im = 2 * z * x, d = re * re + im * im;
        const float w = 2 * kPi * g.frequency * time + g.phase;
        r += gust * g.amplitude * ((re / d) * foundry::det::sin(w) - (im / d) * foundry::det::cos(w));
      }
      return r;
    };
    switch (b.motion) {
      case house_v1_bone::kMotionSway: {
        if (b.drag <= 0 || q <= 0) break;
        moving = true;
        const float phase = 2 * kPi * b.phase;
        const float own = 0.3f * foundry::det::sin(2 * kPi * b.rate * time + phase);
        const float along = clampf(b.drag * q * (response(b.rate, 0.15f) + own), -b.reach, b.reach);
        const float across = clampf(b.drag * q * 0.25f * foundry::det::sin(2 * kPi * b.rate * 1.31f * time + phase * 1.7f), -b.reach * 0.5f,
                                    b.reach * 0.5f);
        vec3 k1 = cross(b.axis, wind);
        const float exposed = length(k1);
        k1 = exposed > 1e-3f ? k1 * (1 / exposed) : perpendicular(b.axis);
        const vec3 k2 = normalize(cross(b.axis, k1));
        local = compose(rotation_about(b.pivot, k1, along * (0.3f + 0.7f * exposed)), rotation_about(b.pivot, k2, across));
        break;
      }
      case house_v1_bone::kMotionSpin: {
        if (b.rate > 0) {
          moving = true;
          // Sails turn with the wind (a breeze at 8 m/s turns them at their rate); a wheel turns at its own.
          const float k = b.drag > 0 ? clampf(0.15f + speed / 8.0f, 0.15f, 2.5f) : 1.0f;
          local = rotation_about(b.pivot, b.axis, 2 * kPi * (b.rate * k * time + b.phase));
        } else {
          // A weathervane points where the wind blows (its rest points +Z), wobbling in the gusts.
          if (speed > 0) moving = true;
          const float wobble = speed > 0 ? 0.25f * (response(0.6f, 0.3f) - 1) + 0.06f * foundry::det::sin(2 * kPi * 0.9f * time + 6.28f * b.phase) : 0;
          local = rotation_about(b.pivot, b.axis, a + wobble);
        }
        break;
      }
      case house_v1_bone::kMotionRise: {
        moving = true;
        const float u = frac(time * b.rate + b.phase);
        const float grow = 0.35f + 1.5f * u;
        const float fade = u < 0.1f ? u / 0.1f : (u > 0.75f ? (1 - u) / 0.25f : 1.0f);
        const vec3 lift = b.axis * (b.reach * u) + wind * (b.drag * b.reach * 0.6f * u * minf(speed / 6.0f, 1.5f));
        local = scale_about(b.pivot, grow * fade, lift);
        break;
      }
      case house_v1_bone::kMotionHinge: {
        const float open = door_open(in.door_time);
        if (in.door_time >= 0 && in.door_time < 4.4f) moving = true;
        if (open > 0) local = rotation_about(b.pivot, b.axis, b.reach * open);
        break;
      }
      case house_v1_bone::kMotionFlicker: {
        moving = true;
        const float p = 6.2831853f * b.phase, w = 2 * kPi * b.rate * time;
        const float s = 1 + b.reach * (0.5f * foundry::det::sin(w + p) + 0.3f * foundry::det::sin(2.3f * w + 1.7f * p) +
                                       0.2f * foundry::det::sin(4.1f * w + 0.6f));
        local = scale_about(b.pivot, s, v3(0, 0, 0));
        break;
      }
      default:
        break;
    }
    world[i] = b.parent >= 0 ? compose(world[static_cast<size_t>(b.parent)], local) : local;
  }
  for (const Xf &x : world) put(x, out.matrices);
  out.settled = !moving;
  return true;
}

}  // namespace house_animate_v1

FOUNDRY_SYSTEM(house_animate_v1, channels::house_v1_motion, channels::house_v1_pose)
