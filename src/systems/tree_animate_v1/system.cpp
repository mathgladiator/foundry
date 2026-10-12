// tree_animate_v1: one frame of a tree's pose (design/systems.md, Simulator).
//
// Wind: each bone bends toward the wind as a damped oscillator at its natural
// frequency, driven by gusty wind. The response is closed form (the steady
// state of each gust component, plus a resonant sway with a per-instance
// phase), so a frame depends only on its inputs and any time can be shown.
// Gusts depend on time alone, so trees given the same local wind sway together.
//
// Break: the log topples about the cut toward break_heading (a rod pivoting on
// its base), bounces when the crown hits the ground, and the limbs snap off on
// impact as free bodies that fly, spin, and come to rest. This is integrated
// with a fixed step from the break to break_time, so it is deterministic too.
#include <vector>

#include "boundary.h"
#include "channels/tree_v1_motion.h"
#include "channels/tree_v1_pose.h"
#include "math.h"
#include "packer/tree_v1_motion.h"
#include "packer/tree_v1_pose.h"

namespace tree_animate_v1 {

using namespace tree_math;
using channels::tree_v1_piece;

namespace {

constexpr float kGravity = 9.81f;
constexpr float kStep = 1.0f / 120.0f;
constexpr float kMaxBreakTime = 20.0f;

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

Xf rotation(vec3 axis, float angle) {
  Xf o;
  const float c = foundry::det::cos(angle), s = foundry::det::sin(angle), k = 1 - c;
  const vec3 a = axis;
  o.r[0] = c + a.x * a.x * k;       o.r[1] = a.x * a.y * k - a.z * s; o.r[2] = a.x * a.z * k + a.y * s;
  o.r[3] = a.y * a.x * k + a.z * s; o.r[4] = c + a.y * a.y * k;       o.r[5] = a.y * a.z * k - a.x * s;
  o.r[6] = a.z * a.x * k - a.y * s; o.r[7] = a.z * a.y * k + a.x * s; o.r[8] = c + a.z * a.z * k;
  return o;
}

// Rotation by `angle` about `axis` through point `p`.
Xf rotation_about(vec3 p, vec3 axis, float angle) {
  Xf r = rotation(axis, angle);
  r.t = p - r.rotate_only(p);
  return r;
}

void put(const Xf &x, std::vector<float> &out) {
  // Column-major 4x4.
  const float m[16] = {x.r[0], x.r[3], x.r[6], 0, x.r[1], x.r[4], x.r[7], 0,
                       x.r[2], x.r[5], x.r[8], 0, x.t.x,  x.t.y,  x.t.z,  1};
  out.insert(out.end(), m, m + 16);
}

vec3 heading(float degrees) {
  float a = degrees * kDeg;
  return v3(foundry::det::sin(a), 0, foundry::det::cos(a));
}

float unit_random(uint32_t seed, uint32_t k) { return static_cast<float>(hash_u32(seed ^ hash_u32(k)) >> 8) / 16777216.0f; }

// ---- wind --------------------------------------------------------------------------

struct Gust {
  float frequency, amplitude, phase;
};
// Gust periods of roughly 2 to 15 seconds; the same for every tree.
const Gust kGusts[3] = {{0.07f, 0.5f, 0.3f}, {0.19f, 0.3f, 2.1f}, {0.43f, 0.2f, 4.4f}};

// Bend angles (along-wind, cross-wind) of one bone at time t.
void wind_bend(const channels::tree_v1_bone &b, float q, float gust, float sway, float t, float phase,
               float &along, float &across) {
  float response = 1;
  const float fb = maxf(b.frequency, 0.05f), z = maxf(b.damping, 0.01f);
  for (const Gust &g : kGusts) {
    float r = g.frequency / fb;
    float re = 1 - r * r, im = 2 * z * r;
    float d = re * re + im * im;
    float w = 2 * kPi * g.frequency * t + g.phase;
    // Steady state of a driven damped oscillator: Im(H e^{iw}).
    response += gust * g.amplitude * ((re / d) * foundry::det::sin(w) - (im / d) * foundry::det::cos(w));
  }
  float own = sway * foundry::det::sin(2 * kPi * fb * t + phase);
  along = clampf(b.drag * q * (response + own), -0.6f, 0.6f);
  across = clampf(b.drag * q * sway * 0.45f * foundry::det::sin(2 * kPi * fb * 1.13f * t + phase * 1.7f), -0.3f, 0.3f);
}

// ---- break ---------------------------------------------------------------------------

struct Fall {
  float angle = 0;           // log rotation about the hinge
  float impact_time = -1;    // when the crown first hits the ground
  float impact_angle = 0;
  float impact_rate = 0;     // angular velocity at impact
  bool settled = false;
};

Fall simulate_log(const tree_v1_piece &log, float until) {
  Fall f;
  const float d = maxf(length(log.center - log.pivot), 0.3f);
  const float k = 3 * kGravity / (4 * d);  // a rod pivoting on its end
  const float crown_hits = 0.5f * kPi - 0.10f;
  const float lies = 0.5f * kPi + 0.05f;
  float theta = 0.03f, omega = 0.15f;
  bool hit = false;
  for (float t = 0; t < until; t += kStep) {
    float limit = hit ? lies : crown_hits;
    omega += k * foundry::det::sin(theta) * kStep;
    omega *= 0.999f;
    theta += omega * kStep;
    if (theta > limit) {
      theta = limit;
      if (!hit) {
        hit = true;
        f.impact_time = t;
        f.impact_angle = theta;
        f.impact_rate = omega;
        omega *= 0.35f;  // the crown crushes, the log keeps settling
      } else {
        omega = -omega * 0.18f;
        if (omega > -0.05f) {
          omega = 0;
          f.settled = true;
        }
      }
    }
  }
  f.angle = theta;
  return f;
}

}  // namespace

bool run(const channels::tree_v1_motion &in, channels::tree_v1_pose &out, std::string &error) {
  const size_t n = in.bones.size();
  for (size_t i = 0; i < n; ++i) {
    if (in.bones[i].parent >= static_cast<int32_t>(i)) {
      error = "bones must come after their parents";
      return false;
    }
    if (in.bones[i].piece >= in.pieces.size() && !in.pieces.empty()) {
      error = "a bone names a piece that doesn't exist";
      return false;
    }
  }
  std::vector<Xf> world(n);
  out.matrices.clear();
  out.matrices.reserve(n * 16);
  out.broken = in.break_time >= 0 && in.pieces.size() >= 2;

  if (!out.broken) {
    const vec3 wind = heading(in.wind_heading);
    const float q = (in.wind_speed / 10.0f) * (in.wind_speed / 10.0f);
    const float gust = 0.6f * (1 - clampf(in.gust_damping, 0, 1));
    const float sway = 0.25f * (0.4f + 0.6f * (1 - clampf(in.gust_damping, 0, 1)));
    for (size_t i = 0; i < n; ++i) {
      const auto &b = in.bones[i];
      Xf local;
      if (b.parent >= 0 && b.drag > 0 && q > 0) {
        float along, across;
        wind_bend(b, q, gust, sway, in.time, 2 * kPi * unit_random(in.seed, static_cast<uint32_t>(i)), along, across);
        vec3 k1 = cross(b.axis, wind);
        float exposed = length(k1);
        k1 = exposed > 1e-3f ? k1 * (1 / exposed) : perpendicular(b.axis);
        vec3 k2 = normalize(cross(b.axis, k1));  // bends sideways, across the wind
        local = compose(rotation_about(b.pivot, k1, along * (0.3f + 0.7f * exposed)), rotation_about(b.pivot, k2, across));
      }
      world[i] = b.parent >= 0 ? compose(world[static_cast<size_t>(b.parent)], local) : local;
    }
    for (const Xf &x : world) put(x, out.matrices);
    out.settled = in.wind_speed <= 0;
    return true;
  }

  // The break path.
  const float t = minf(in.break_time, kMaxBreakTime);
  const vec3 dir = heading(in.break_heading);
  const vec3 hinge_axis = normalize(cross(v3(0, 1, 0), dir));
  const tree_v1_piece &log = in.pieces[1];
  const Fall fall = simulate_log(log, t);
  const Xf log_xf = rotation_about(log.pivot, hinge_axis, fall.angle);
  std::vector<Xf> piece_xf(in.pieces.size());
  piece_xf[1] = log_xf;
  bool all_rest = fall.settled;
  const Xf at_impact = rotation_about(log.pivot, hinge_axis, fall.impact_angle);
  for (size_t p = 2; p < in.pieces.size(); ++p) {
    const tree_v1_piece &limb = in.pieces[p];
    if (fall.impact_time < 0 || t <= fall.impact_time) {
      piece_xf[p] = log_xf;  // still on the log
      all_rest = false;
      continue;
    }
    // Free flight from the impact: the log's velocity at the limb's centre plus a kick.
    const float dt = t - fall.impact_time;
    const uint32_t s = in.seed ^ static_cast<uint32_t>(p) * 2654435761u;
    vec3 c0 = at_impact.apply(limb.center);
    vec3 v = cross(hinge_axis * fall.impact_rate, c0 - log.pivot) * 0.5f;
    v = v + v3(unit_random(s, 1) * 2 - 1, 1.0f + 1.5f * unit_random(s, 2), unit_random(s, 3) * 2 - 1) * 1.4f;
    vec3 spin_axis = normalize(v3(unit_random(s, 4) - 0.5f, unit_random(s, 5) - 0.5f, unit_random(s, 6) - 0.5f));
    float spin = 1.0f + 3.0f * unit_random(s, 7);
    const float rest_y = 0.2f;
    // Time to reach the ground: c0.y + v.y t - g t^2 / 2 = rest_y.
    float a = 0.5f * kGravity, bq = -v.y, cq = rest_y - c0.y;
    float disc = bq * bq - 4 * a * cq;
    float land = disc > 0 ? (-bq + foundry::det::sqrt(disc)) / (2 * a) : 0;
    float fly = minf(dt, land);
    vec3 c = c0 + v * fly + v3(0, -0.5f * kGravity * fly * fly, 0);
    if (dt > land) {
      // Slide to a stop on the ground.
      float slide = minf(dt - land, 0.5f);
      vec3 vh = v3(v.x, 0, v.z);
      c = c + vh * (slide - slide * slide);
      c.y = rest_y;
    } else {
      all_rest = false;
    }
    Xf spin_xf = rotation_about(c0, spin_axis, spin * fly);
    Xf moved;
    moved.t = c - c0;
    piece_xf[p] = compose(moved, compose(spin_xf, at_impact));
  }
  for (size_t i = 0; i < n; ++i) {
    uint32_t piece = in.bones[i].piece;
    world[i] = piece < piece_xf.size() ? piece_xf[piece] : Xf{};
    put(world[i], out.matrices);
  }
  out.settled = all_rest && t > 1.0f;
  return true;
}

}  // namespace tree_animate_v1

FOUNDRY_SYSTEM(tree_animate_v1, channels::tree_v1_motion, channels::tree_v1_pose)
