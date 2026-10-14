#include "fire/fire.h"

#include <algorithm>
#include <cmath>

namespace runtime {
namespace fire {

namespace {

constexpr float kPi = 3.14159265f;
// The most particles the runtime keeps; past it the oldest smoke goes first.
constexpr size_t kMaxParticles = 60000;

uint32_t mix32(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdull;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ull;
  k ^= k >> 33;
  return static_cast<uint32_t>(k) | 1u;
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

float Sim::rnd(uint32_t &s) const {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
}

void Sim::set_configs(const std::vector<Config> &configs) {
  configs_ = configs;
  by_id_.clear();
  for (size_t i = 0; i < configs_.size(); ++i) by_id_[configs_[i].id] = i;
}

const Config *Sim::find(uint32_t id) const {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : &configs_[it->second];
}

void Sim::set_wind(float speed, float heading_deg) {
  const float h = heading_deg * kPi / 180.0f;
  wind_[0] = std::sin(h) * speed;
  wind_[1] = std::cos(h) * speed;
}

void Sim::clear() {
  live_.clear();
  parts_.clear();
  time_ = 0;
}

void Sim::step(float dt, const Emitter *emitters, size_t count) {
  if (dt <= 0) return;
  dt = std::min(dt, 0.1f);
  time_ += dt;
  for (auto &kv : live_) kv.second.seen = false;
  for (size_t i = 0; i < count; ++i) {
    const Emitter &e = emitters[i];
    const Config *c = find(e.config);
    if (!c || e.strength <= 0) continue;
    Live &l = live_[e.key];
    if (!l.seen && l.rng == 1) l.rng = mix32(e.key);
    l.e = e;
    l.seen = true;
    const float s = e.strength;
    // Flicker: two incommensurate wobbles and a little noise, deeper as the config says.
    const float ph = static_cast<float>(e.key % 997u) * 0.37f;
    const float t = static_cast<float>(time_);
    const float wob = 0.5f * std::sin(t * 9.1f + ph) + 0.3f * std::sin(t * 15.7f + ph * 2.3f) + 0.2f * (rnd(l.rng) * 2 - 1);
    l.flicker = std::max(0.2f, 1.0f + c->flicker * 0.35f * wob);
    const bool flames = e.show == Show::fire;
    const float size = c->size * std::sqrt(s), height = c->height * std::sqrt(s);
    if (flames && c->rate > 0) {
      l.acc_flame += c->rate * s * dt * l.flicker;
      while (l.acc_flame >= 1) {
        l.acc_flame -= 1;
        Particle p{};
        const float a = rnd(l.rng) * 2 * kPi, r = std::sqrt(rnd(l.rng)) * size * 0.45f;
        p.p[0] = e.pos[0] + std::cos(a) * r;
        p.p[1] = e.pos[1];
        p.p[2] = e.pos[2] + std::sin(a) * r;
        const float life = (c->kind == kEmber ? 0.3f : 0.45f) + 0.35f * rnd(l.rng);
        // Rising to the flame's height over its life, drawn in toward the middle.
        const float rise = height / life;
        p.v[0] = -std::cos(a) * r / life * 0.6f + wind_[0] * 0.05f;
        p.v[1] = rise * (0.8f + 0.4f * rnd(l.rng));
        p.v[2] = -std::sin(a) * r / life * 0.6f + wind_[1] * 0.05f;
        p.life = life;
        p.size0 = size * (0.55f + 0.3f * rnd(l.rng)) * l.flicker;
        p.size1 = size * 0.08f;
        for (int k = 0; k < 3; ++k) {
          p.c0[k] = c->core[k];
          p.c1[k] = c->tip[k];
        }
        p.alpha = c->kind == kSpirit ? 0.85f : 0.75f;
        p.config = e.config;
        p.type = kTongue;
        parts_.push_back(p);
      }
    }
    if (flames && c->sparks > 0) {
      l.acc_spark += c->sparks * s * dt;
      while (l.acc_spark >= 1) {
        l.acc_spark -= 1;
        Particle p{};
        for (int k = 0; k < 3; ++k) p.p[k] = e.pos[k];
        p.p[1] += height * 0.3f;
        const float a = rnd(l.rng) * 2 * kPi, out = 0.4f + 0.8f * rnd(l.rng);
        p.v[0] = std::cos(a) * out + wind_[0] * 0.2f;
        p.v[1] = 1.5f + 2.5f * rnd(l.rng);
        p.v[2] = std::sin(a) * out + wind_[1] * 0.2f;
        p.life = 0.6f + 0.9f * rnd(l.rng);
        p.size0 = 0.035f + 0.02f * rnd(l.rng);
        p.size1 = 0.01f;
        for (int k = 0; k < 3; ++k) {
          p.c0[k] = std::min(1.0f, c->core[k] * 1.2f + 0.2f);
          p.c1[k] = c->tip[k];
        }
        p.alpha = 1;
        p.config = e.config;
        p.type = kSpark;
        parts_.push_back(p);
      }
    }
    if (c->kind != kSpirit && c->smoke_rate > 0) {
      l.acc_smoke += c->smoke_rate * (flames ? s : std::max(s, 1.0f)) * dt;
      while (l.acc_smoke >= 1) {
        l.acc_smoke -= 1;
        Particle p{};
        const float a = rnd(l.rng) * 2 * kPi, r = rnd(l.rng) * size * 0.3f;
        p.p[0] = e.pos[0] + std::cos(a) * r;
        p.p[1] = e.pos[1] + (flames ? height * 0.85f : 0.0f);
        p.p[2] = e.pos[2] + std::sin(a) * r;
        p.v[0] = (rnd(l.rng) - 0.5f) * 0.2f;
        p.v[1] = c->smoke_rise * (0.8f + 0.4f * rnd(l.rng)) * (flames ? 1.0f + 0.3f * s : 1.0f);
        p.v[2] = (rnd(l.rng) - 0.5f) * 0.2f;
        p.life = c->smoke_life * (0.7f + 0.6f * rnd(l.rng));
        const float sz = c->smoke_size * (flames ? std::sqrt(s) : 1.0f);
        p.size0 = sz * (0.8f + 0.4f * rnd(l.rng));
        p.size1 = sz * (3.5f + 2.0f * rnd(l.rng));
        const float shade = 0.9f + 0.2f * rnd(l.rng);
        for (int k = 0; k < 3; ++k) {
          p.c0[k] = std::min(1.0f, c->smoke[k] * shade);
          p.c1[k] = std::min(1.0f, lerpf(c->smoke[k], 0.75f, 0.35f) * shade);
        }
        p.alpha = 0.55f;
        p.config = e.config;
        p.type = kSmoke;
        parts_.push_back(p);
      }
    }
  }
  // Emitters gone this frame stop; forget them once their particles could have died.
  for (auto it = live_.begin(); it != live_.end();) {
    if (!it->second.seen) it = live_.erase(it);
    else ++it;
  }
  // Move and age. Smoke drifts toward the wind's velocity (it's carried) and slows its rise;
  // sparks fall back a little; tongues sway in the wind.
  const float drag = 1.0f - std::exp(-0.8f * dt);
  size_t w = 0;
  for (size_t i = 0; i < parts_.size(); ++i) {
    Particle p = parts_[i];
    p.age += dt;
    if (p.age >= p.life) continue;
    if (p.type == kSmoke) {
      p.v[0] += (wind_[0] - p.v[0]) * drag;
      p.v[2] += (wind_[1] - p.v[2]) * drag;
      p.v[1] *= 1.0f - 0.08f * dt;
    } else if (p.type == kSpark) {
      p.v[1] -= 2.5f * dt;
      p.v[0] += (wind_[0] - p.v[0]) * drag * 0.5f;
      p.v[2] += (wind_[1] - p.v[2]) * drag * 0.5f;
    } else {
      p.v[0] += wind_[0] * 0.15f * dt;
      p.v[2] += wind_[1] * 0.15f * dt;
    }
    for (int k = 0; k < 3; ++k) p.p[k] += p.v[k] * dt;
    parts_[w++] = p;
  }
  parts_.resize(w);
  if (parts_.size() > kMaxParticles) parts_.erase(parts_.begin(), parts_.begin() + static_cast<long>(parts_.size() - kMaxParticles));
}

void Sim::sprites(std::vector<Sprite> &out) const {
  out.clear();
  out.reserve(parts_.size() + live_.size());
  // Smoke first, blended; then the light: glows, tongues, and sparks, added.
  for (int pass = 0; pass < 2; ++pass)
    for (const Particle &p : parts_) {
      if ((p.type == kSmoke) != (pass == 0)) continue;
      const float t = p.age / p.life;
      Sprite s{};
      for (int k = 0; k < 3; ++k) s.pos[k] = p.p[k];
      s.size = lerpf(p.size0, p.size1, p.type == kSmoke ? std::sqrt(t) : t);
      for (int k = 0; k < 3; ++k) s.color[k] = lerpf(p.c0[k], p.c1[k], t);
      if (p.type == kSmoke) s.color[3] = p.alpha * std::min(1.0f, t * 6.0f) * (1.0f - t) * (1.0f - t);
      else s.color[3] = p.alpha * (1.0f - t * t);
      s.stretch = p.type == kTongue ? 1.6f : (p.type == kSpark ? 1.0f : 1.0f);
      s.additive = p.type == kSmoke ? 0u : 1u;
      out.push_back(s);
    }
  // Each burning emitter's glow at its root: a wide, soft light that flickers.
  for (const auto &kv : live_) {
    const Live &l = kv.second;
    if (l.e.show != Show::fire) continue;
    const Config *c = find(l.e.config);
    if (!c) continue;
    Sprite s{};
    for (int k = 0; k < 3; ++k) s.pos[k] = l.e.pos[k];
    s.pos[1] += c->height * 0.15f;
    s.size = c->size * std::sqrt(l.e.strength) * (c->kind == kEmber ? 2.2f : 1.6f) * l.flicker;
    for (int k = 0; k < 3; ++k) s.color[k] = c->core[k];
    s.color[3] = (c->kind == kEmber ? 0.55f : 0.3f) * std::min(1.0f, l.flicker);
    s.stretch = 0.8f;
    s.additive = 1;
    out.push_back(s);
  }
}

void Sim::lights(const float eye[3], size_t max, std::vector<Light> &out) const {
  out.clear();
  struct Cand {
    float score;
    Light l;
  };
  std::vector<Cand> cands;
  for (const auto &kv : live_) {
    const Live &l = kv.second;
    if (l.e.show != Show::fire) continue;
    const Config *c = find(l.e.config);
    if (!c || c->light_strength <= 0 || c->light_radius <= 0) continue;
    Light li{};
    for (int k = 0; k < 3; ++k) li.pos[k] = l.e.pos[k];
    li.pos[1] += c->height * 0.4f * std::sqrt(l.e.strength);
    li.radius = c->light_radius * std::sqrt(std::max(l.e.strength, 0.1f));
    for (int k = 0; k < 3; ++k) li.color[k] = c->light[k];
    li.strength = c->light_strength * std::min(l.e.strength, 2.0f) * l.flicker;
    const float dx = li.pos[0] - eye[0], dy = li.pos[1] - eye[1], dz = li.pos[2] - eye[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    cands.push_back({li.strength * li.radius / (1.0f + d), li});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
  for (size_t i = 0; i < cands.size() && i < max; ++i) out.push_back(cands[i].l);
}

void Sim::snapshot(const std::vector<Config> &configs, const Emitter *emitters, size_t count, float wind_speed, float wind_heading,
                   float seconds, std::vector<Sprite> &out, std::vector<Light> *lights) {
  Sim s;
  s.set_configs(configs);
  s.set_wind(wind_speed, wind_heading);
  // Long enough for smoke to fill its trail, then the moment asked for.
  float longest = 1.5f;
  for (const Config &c : configs) longest = std::max(longest, c.smoke_life);
  const float total = std::min(longest * 1.2f, 12.0f) + std::fmod(std::max(seconds, 0.0f), 4.0f);
  for (float t = 0; t < total; t += 1.0f / 30.0f) s.step(1.0f / 30.0f, emitters, count);
  s.sprites(out);
  if (lights) {
    const float eye[3] = {0, 0, 0};
    s.lights(eye, 8, *lights);
  }
}

}  // namespace fire
}  // namespace runtime
