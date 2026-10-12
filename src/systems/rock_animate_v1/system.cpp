// rock_animate_v1: one frame of a rock's pose (design/systems.md, Rocks).
//
// Whole: bone 0 (the whole rock) at rest, every piece hidden (a zero matrix).
// Broken: the whole rock hidden, and each piece on a faux-physics path in closed
// form, so a frame depends only on its inputs and any moment can be shown:
// - Flight: out from the rock's centre and along break_heading, upper pieces
//   faster than the ones near the ground (a rock slumps and scatters rather than
//   exploding), lighter pieces faster; a ballistic arc while the piece tumbles
//   about a random level axis.
// - Landing: from then on the piece rests on its lowest support point, so it
//   never sinks into the ground however it has turned; one small bounce, a
//   slide that friction stops, and a roll on to the nearest whole turn, so it
//   comes to rest lying as it lay in the rock (a stable face down), turned only
//   about the vertical.
// The rock settles (and its instance can sleep) once every piece is still.
#include <vector>

#include "boundary.h"
#include "channels/rock_v1_motion.h"
#include "channels/rock_v1_pose.h"
#include "math.h"
#include "packer/rock_v1_motion.h"
#include "packer/rock_v1_pose.h"

namespace rock_animate_v1 {

using namespace rock_math;

namespace {

constexpr float kGravity = 9.81f;
constexpr float kFriction = 0.6f;
constexpr float kMaxBreakTime = 30.0f;

struct Xf {
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major
  vec3 t = {0, 0, 0};
  vec3 rotate_only(vec3 v) const {
    return v3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
              r[6] * v.x + r[7] * v.y + r[8] * v.z);
  }
};

Xf rotation(vec3 a, float angle) {
  Xf o;
  const float c = foundry::det::cos(angle), s = foundry::det::sin(angle), k = 1 - c;
  o.r[0] = c + a.x * a.x * k;       o.r[1] = a.x * a.y * k - a.z * s; o.r[2] = a.x * a.z * k + a.y * s;
  o.r[3] = a.y * a.x * k + a.z * s; o.r[4] = c + a.y * a.y * k;       o.r[5] = a.y * a.z * k - a.x * s;
  o.r[6] = a.z * a.x * k - a.y * s; o.r[7] = a.z * a.y * k + a.x * s; o.r[8] = c + a.z * a.z * k;
  return o;
}

void put(const Xf &x, std::vector<float> &out) {
  const float m[16] = {x.r[0], x.r[3], x.r[6], 0, x.r[1], x.r[4], x.r[7], 0,
                       x.r[2], x.r[5], x.r[8], 0, x.t.x,  x.t.y,  x.t.z,  1};
  out.insert(out.end(), m, m + 16);
}

void hidden(std::vector<float> &out) { out.insert(out.end(), 16, 0.0f); }

float unit_random(uint32_t seed, uint32_t k) { return static_cast<float>(hash_u32(seed ^ hash_u32(k)) >> 8) / 16777216.0f; }

}  // namespace

bool run(const channels::rock_v1_motion &in, channels::rock_v1_pose &out, std::string &error) {
  out.matrices.clear();
  const size_t n = in.pieces.size();
  for (size_t i = 0; i < n; ++i)
    if (in.pieces[i].bone != i + 1 || in.pieces[i].support.empty()) {
      error = "piece n must ride bone n + 1 and have support points";
      return false;
    }
  out.matrices.reserve((n + 1) * 16);
  if (in.break_time < 0 || n == 0) {
    put(Xf{}, out.matrices);
    for (size_t i = 0; i < n; ++i) hidden(out.matrices);
    out.broken = false;
    out.settled = true;  // a whole rock never moves
    return true;
  }
  const float t = minf(in.break_time, kMaxBreakTime);
  hidden(out.matrices);  // the whole rock gives way to its pieces

  // The rock's centre, size, and typical piece mass.
  float total = 0, reach = 0;
  vec3 centre = v3(0, 0, 0);
  for (const auto &p : in.pieces) {
    total += p.mass;
    centre = centre + p.center * p.mass;
  }
  centre = total > 0 ? centre * (1.0f / total) : v3(0, 0, 0);
  for (const auto &p : in.pieces) reach = maxf(reach, length(p.center - centre) + p.radius);
  const float mean_mass = total / static_cast<float>(n);
  float top = 0;
  for (const auto &p : in.pieces) top = maxf(top, p.center.y);
  const float a = in.break_heading * kDeg;
  const vec3 heading = v3(foundry::det::sin(a), 0, foundry::det::cos(a));
  const float scale = foundry::det::sqrt(clampf(reach, 0.25f, 3.0f));

  bool still = true;
  for (size_t i = 0; i < n; ++i) {
    const auto &p = in.pieces[i];
    const uint32_t s = in.seed ^ static_cast<uint32_t>(i + 1) * 2654435761u;
    // Launch: out from the centre, along the heading, a little up; the higher the piece
    // sat, the faster it goes, so the base slumps where it was and the top scatters.
    vec3 out_dir = p.center - centre;
    out_dir.y = 0;
    const float jx = unit_random(s, 1) - 0.5f;
    const float jz = unit_random(s, 3) - 0.5f;
    out_dir = length(out_dir) > 1e-4f ? normalize(out_dir) : normalize(v3(jx, 0, jz) + heading * 0.1f);
    const vec3 dir = normalize(out_dir + heading * 0.7f + v3(jx, 0, jz) * 0.5f + v3(0, 0.35f + 0.4f * unit_random(s, 2), 0));
    const float height = clampf(p.center.y / maxf(top, 1e-3f), 0, 1);
    const float light = clampf(foundry::det::sqrt(foundry::det::sqrt(mean_mass / maxf(p.mass, 1e-3f))), 0.6f, 1.7f);
    const float speed = (1.5f + 1.7f * unit_random(s, 4)) * scale * light * (0.45f + 0.55f * height);
    const vec3 v = dir * speed;
    const float ax = unit_random(s, 5) - 0.5f;
    const float az = unit_random(s, 7) - 0.5f;
    const vec3 axis = normalize(v3(ax, 0.15f * (unit_random(s, 6) - 0.5f), az));  // tumbling, mostly end over end
    const float spin = (1.5f + 3.5f * unit_random(s, 8)) / (0.5f + p.radius) * light * (0.3f + 0.7f * height);
    const float yaw_rate = 1.2f * (unit_random(s, 9) - 0.5f);
    // When the arc comes down to roughly resting height.
    const float rest = 0.55f * p.radius;
    const float disc = v.y * v.y + 2 * kGravity * (p.center.y - rest);
    const float land = disc > 0 ? (v.y + foundry::det::sqrt(disc)) / kGravity : maxf(v.y / kGravity, 0.05f);
    const float fly = minf(t, land);
    const float after = t > land ? t - land : 0;
    // Spin: free in flight, then a roll on to the nearest whole turn, so the piece lies
    // the way it lay in the rock; a turn about the vertical keeps them from looking alike.
    const float at_land = spin * land;
    const float whole = 2 * kPi * static_cast<float>(static_cast<int>(at_land / (2 * kPi) + 0.5f));
    const float ease = smooth(clampf(after / 0.7f, 0, 1));
    const float angle = after <= 0 ? spin * fly : at_land + (whole - at_land) * ease;
    const float yaw = yaw_rate * (fly + 0.5f * minf(after, 0.7f));
    const Xf turn = rotation(v3(0, 1, 0), yaw), tumble = rotation(axis, angle);
    Xf rot;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        rot.r[r * 3 + c] = turn.r[r * 3] * tumble.r[c] + turn.r[r * 3 + 1] * tumble.r[3 + c] + turn.r[r * 3 + 2] * tumble.r[6 + c];
    // Across the ground: the arc, then a slide that friction stops.
    const vec3 vh = v3(v.x, 0, v.z);
    const float hs = length(vh);
    const float stop = clampf(hs / (kFriction * kGravity), 0.05f, 0.6f);
    const float sl = minf(after, stop);
    vec3 c = p.center + vh * (fly + 0.5f * (sl - sl * sl / (2 * stop)));
    // Up and down: the arc in flight, never below the lowest support point; then resting
    // on that point, after one small bounce.
    float low = 1e9f;
    for (const vec3 &q : p.support) low = minf(low, rot.rotate_only(q).y);
    const float ground = -low;
    if (after <= 0) {
      c.y = maxf(p.center.y + v.y * fly - 0.5f * kGravity * fly * fly, ground);
      still = false;
    } else {
      const float vb = 0.25f * maxf(kGravity * land - v.y, 0.0f);
      const float hop = vb * after - 0.5f * kGravity * after * after;
      c.y = ground + (hop > 0 ? hop : 0);
      if (after < maxf(stop, 0.9f)) still = false;
    }
    Xf x = rot;
    x.t = c - rot.rotate_only(p.center);
    put(x, out.matrices);
  }
  out.broken = true;
  out.settled = still;
  return true;
}

}  // namespace rock_animate_v1

FOUNDRY_SYSTEM(rock_animate_v1, channels::rock_v1_motion, channels::rock_v1_pose)
