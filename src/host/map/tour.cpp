#include "map/tour.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace map {

namespace {

constexpr float kPi = 3.14159265f;

float ease(float s) { return s * s * (3 - 2 * s); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }

void pose_from(const float eye[3], const float target[3], Pose &p) {
  for (int i = 0; i < 3; ++i) p.eye[i] = eye[i];
  const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
  p.heading = std::atan2(dx, -dz);
  p.pitch = std::atan2(dy, std::max(1e-3f, std::hypot(dx, dz)));
}

}  // namespace

Pose Tour::at(double t) const {
  Pose p;
  if (legs_.empty()) return p;
  const Leg *leg = &legs_.back();
  for (const Leg &l : legs_)
    if (t < l.t1) {
      leg = &l;
      break;
    }
  const float s = static_cast<float>(std::clamp((t - leg->t0) / std::max(1e-6, leg->t1 - leg->t0), 0.0, 1.0));
  float eye[3], target[3];
  leg->f(s, eye, target);
  pose_from(eye, target, p);
  return p;
}

bool Tour::sites(const nlohmann::json &plan, const std::vector<std::string> &names, const TourOptions &o, const Ground &ground,
                 std::string &error) {
  legs_.clear();
  seconds_ = 0;
  struct Site {
    float x, y, z, r;
  };
  std::vector<Site> visit;
  const auto &places = plan.value("places", nlohmann::json::array());
  auto add = [&](const nlohmann::json &pl) {
    visit.push_back({pl["position"][0].get<float>(), pl["position"][1].get<float>(), pl["position"][2].get<float>(), pl.value("radius", 20.0f)});
  };
  if (names.empty()) {
    for (const auto &pl : places) add(pl);
  } else {
    for (const std::string &n : names) {
      bool found = false;
      for (const auto &pl : places)
        if (pl.value("name", "") == n) {
          add(pl);
          found = true;
        }
      if (!found) {
        error = "no site named " + n;
        return false;
      }
    }
  }
  if (visit.empty()) {
    error = "the map has no sites";
    return false;
  }
  // Each orbit's radius and height, and where it starts: facing the way the camera comes from.
  const float sweep = o.sweep_deg * kPi / 180.0f;
  float prev_x = 0, prev_z = 0;  // the first orbit starts on the side toward the map's middle
  double t = 0;
  float last_eye[3] = {0, 0, 0}, last_target[3] = {0, 0, 0};
  for (size_t i = 0; i < visit.size(); ++i) {
    const Site s = visit[i];
    const float R = std::clamp(s.r * 2.0f + 40.0f, 60.0f, 220.0f), H = R * 0.5f;
    const float a0 = std::atan2(prev_x - s.x, prev_z - s.z);
    auto orbit = [s, R, H, a0, sweep, ground](float u, float eye[3], float target[3]) {
      const float a = a0 + sweep * u;
      eye[0] = s.x + R * std::sin(a);
      eye[2] = s.z + R * std::cos(a);
      eye[1] = std::max(s.y + H, ground(eye[0], eye[2]) + 15.0f);
      target[0] = s.x;
      target[1] = s.y + 4.0f;
      target[2] = s.z;
    };
    float start_eye[3], start_target[3];
    orbit(0, start_eye, start_target);
    if (i > 0) {  // the flight from the last orbit's end to this one's start, turning toward this site
      float from[3], from_t[3], to[3], to_t[3];
      for (int k = 0; k < 3; ++k) {
        from[k] = last_eye[k];
        from_t[k] = last_target[k];
        to[k] = start_eye[k];
        to_t[k] = start_target[k];
      }
      const float d = std::hypot(to[0] - from[0], to[2] - from[2]);
      const double dur = std::max(3.0, static_cast<double>(d / std::max(5.0f, o.speed)));
      const float bump = std::min(150.0f, 0.12f * d);
      Leg leg;
      leg.t0 = t;
      leg.t1 = t + dur;
      leg.f = [=](float u, float eye[3], float target[3]) {
        const float e = ease(u), turn = ease(std::min(1.0f, u * 1.6f));
        for (int k = 0; k < 3; ++k) {
          eye[k] = mixf(from[k], to[k], e);
          target[k] = mixf(from_t[k], to_t[k], turn);
        }
        eye[1] += bump * std::sin(kPi * u);
        eye[1] = std::max(eye[1], ground(eye[0], eye[2]) + 20.0f);
      };
      legs_.push_back(leg);
      t = leg.t1;
    }
    Leg leg;
    leg.t0 = t;
    leg.t1 = t + o.orbit_s;
    leg.f = orbit;
    legs_.push_back(leg);
    t = leg.t1;
    orbit(1, last_eye, last_target);
    prev_x = last_eye[0];
    prev_z = last_eye[2];
  }
  seconds_ = t;
  return true;
}

bool Tour::road(const nlohmann::json &plan, int link, const TourOptions &o, const Ground &ground, std::string &error) {
  legs_.clear();
  seconds_ = 0;
  const auto &roads = plan.value("roads", nlohmann::json::array());
  const auto &of = plan.value("road_of", nlohmann::json::array());
  std::vector<std::array<float, 3>> pts;
  for (size_t i = 0; i < roads.size() && i < of.size(); ++i) {
    if (of[i].get<int>() != link || roads[i].value("kind", 0u) == 3u) continue;
    const auto &a = roads[i]["a"], &b = roads[i]["b"];
    if (pts.empty()) pts.push_back({a[0].get<float>(), a[1].get<float>(), a[2].get<float>()});
    pts.push_back({b[0].get<float>(), b[1].get<float>(), b[2].get<float>()});
  }
  if (pts.size() < 2) {
    error = "link " + std::to_string(link) + " has no route";
    return false;
  }
  std::vector<float> arc(pts.size(), 0);
  for (size_t i = 1; i < pts.size(); ++i) arc[i] = arc[i - 1] + std::hypot(pts[i][0] - pts[i - 1][0], pts[i][2] - pts[i - 1][2]);
  const float len = arc.back();
  auto point = [pts, arc](float d, float out[3]) {
    d = std::clamp(d, 0.0f, arc.back());
    size_t i = 1;
    while (i + 1 < arc.size() && arc[i] < d) ++i;
    const float u = (d - arc[i - 1]) / std::max(1e-3f, arc[i] - arc[i - 1]);
    for (int k = 0; k < 3; ++k) out[k] = mixf(pts[i - 1][k], pts[i][k], u);
  };
  Leg leg;
  leg.t0 = 0;
  leg.t1 = std::max(2.0, static_cast<double>(len / std::max(1.0f, o.speed)));
  leg.f = [=](float u, float eye[3], float target[3]) {
    const float d = len * u;
    point(d, eye);
    eye[1] = std::max(eye[1], ground(eye[0], eye[2])) + 2.2f;
    // Looking ahead: the mean of points down the road, so the turns are smooth.
    float sum[3] = {0, 0, 0};
    for (int k = 1; k <= 4; ++k) {
      float p[3];
      point(d + 8.0f * static_cast<float>(k), p);
      for (int j = 0; j < 3; ++j) sum[j] += p[j] * 0.25f;
    }
    if (std::hypot(sum[0] - eye[0], sum[2] - eye[2]) < 1.0f) {  // at the end: keep the last way
      float back[3];
      point(d - 10.0f, back);
      for (int j = 0; j < 3; ++j) sum[j] = eye[j] + (eye[j] - back[j]);
    }
    target[0] = sum[0];
    target[1] = sum[1] + 1.6f;
    target[2] = sum[2];
  };
  legs_.push_back(leg);
  seconds_ = leg.t1;
  return true;
}

bool Tour::keys(const nlohmann::json &keys, const Ground &ground, std::string &error) {
  legs_.clear();
  seconds_ = 0;
  if (!keys.is_array() || keys.size() < 2) {
    error = "keys needs at least two camera keys";
    return false;
  }
  struct Key {
    float x, y, z, heading, pitch;
    double seconds;
  };
  std::vector<Key> ks;
  for (const auto &k : keys) {
    Key c;
    c.x = k.value("x", 0.0f);
    c.z = k.value("z", 0.0f);
    c.y = ground(c.x, c.z) + k.value("y", 20.0f);
    c.heading = k.value("heading", 0.0f) * kPi / 180.0f;
    c.pitch = k.value("pitch", -10.0f) * kPi / 180.0f;
    c.seconds = std::max(0.1, k.value("seconds", 4.0));
    ks.push_back(c);
  }
  double t = 0;
  for (size_t i = 1; i < ks.size(); ++i) {
    const Key a = ks[i - 1], b = ks[i];
    float dh = b.heading - a.heading;
    while (dh > kPi) dh -= 2 * kPi;
    while (dh < -kPi) dh += 2 * kPi;
    Leg leg;
    leg.t0 = t;
    leg.t1 = t + b.seconds;
    leg.f = [a, b, dh](float u, float eye[3], float target[3]) {
      const float e = ease(u);
      eye[0] = mixf(a.x, b.x, e);
      eye[1] = mixf(a.y, b.y, e);
      eye[2] = mixf(a.z, b.z, e);
      const float h = a.heading + dh * e, p = mixf(a.pitch, b.pitch, e);
      target[0] = eye[0] + std::sin(h) * std::cos(p);
      target[1] = eye[1] + std::sin(p);
      target[2] = eye[2] - std::cos(h) * std::cos(p);
    };
    legs_.push_back(leg);
    t = leg.t1;
  }
  seconds_ = t;
  return true;
}

}  // namespace map
