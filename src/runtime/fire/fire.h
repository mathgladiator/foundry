// The runtime's fire (design/submit.md, "Fire"): buildings' fire points become emitters, and a
// particle system on the CPU turns them into flames, sparks, embers' glow, and smoke the wind
// carries, as camera-facing sprites (render/fire_renderer.h draws them; the foundry's viewport
// draws the same sprites). Each fire config (fire_v1_config, compiled in) sets a fire's colours,
// size, flicker, smoke, and the light it throws, which the mega shader adds (light.hlsli).
//
// Fire is for looks: it never feeds back into a pipeline's output, so it needn't match bit for bit
// across machines, only from frame to frame.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace runtime {
namespace fire {

// fire_v1_config, as the runtime keeps it.
struct Config {
  uint32_t id = 0;
  uint32_t kind = 0;  // kFlame, kEmber, kSpirit
  float core[3] = {1, 0.6f, 0.2f}, tip[3] = {1, 0.25f, 0.05f};
  float size = 0.3f, height = 0.5f, rate = 40, flicker = 0.5f, sparks = 2;
  float smoke[3] = {0.4f, 0.4f, 0.42f};
  float smoke_rate = 4, smoke_size = 0.35f, smoke_life = 7, smoke_rise = 1;
  float light[3] = {1, 0.65f, 0.35f};
  float light_radius = 6, light_strength = 1.2f;
};
constexpr uint32_t kFlame = 0, kEmber = 1, kSpirit = 2;

// What an emitter shows: its flames (with sparks and glow) and its smoke, or its smoke alone (a
// chimney).
enum class Show : uint8_t { fire, smoke };

// One emitter this frame, in world space. `key` names it from frame to frame (its instance and
// point), so its particles and their timing carry on; `strength` scales its rate and size (a
// building's blaze grows with how much burns).
struct Emitter {
  uint64_t key = 0;
  float pos[3] = {0, 0, 0};
  uint32_t config = 0;
  float strength = 1;
  Show show = Show::fire;
};

// A sprite to draw: a disc of `size` metres (stretched up by `stretch` for a flame's tongue),
// its colour and alpha; additive for light (flames, sparks, glow), else blended over (smoke).
struct Sprite {
  float pos[3];
  float size;
  float color[4];
  float stretch;
  uint32_t additive;
};

// A fire's light this frame: where, its colour times its strength (flickering), and its reach.
struct Light {
  float pos[3];
  float radius;
  float color[3];
  float strength;
};

class Sim {
 public:
  void set_configs(const std::vector<Config> &configs);
  const Config *find(uint32_t id) const;
  const std::vector<Config> &configs() const { return configs_; }
  // The wind: metres per second, toward a heading (degrees from +Z toward +X), as the pipelines'.
  void set_wind(float speed, float heading_deg);
  // Advances by dt with this frame's emitters (an emitter missing from the list stops: its
  // particles live out their lives).
  void step(float dt, const Emitter *emitters, size_t count);
  // The sprites to draw now, smoke first (back to front isn't needed: smoke is soft), then light.
  void sprites(std::vector<Sprite> &out) const;
  // The fires' lights, strongest and nearest `eye` first, at most `max`.
  void lights(const float eye[3], size_t max, std::vector<Light> &out) const;
  size_t particles() const { return parts_.size(); }
  size_t emitters() const { return live_.size(); }
  double time() const { return time_; }
  void clear();

  // A still: the sprites `seconds` after these emitters were lit, stepped from nothing (the
  // foundry's views and screenshots).
  static void snapshot(const std::vector<Config> &configs, const Emitter *emitters, size_t count, float wind_speed,
                       float wind_heading, float seconds, std::vector<Sprite> &out, std::vector<Light> *lights = nullptr);

 private:
  enum Type : uint8_t { kTongue, kSpark, kSmoke };
  struct Particle {
    float p[3], v[3];
    float age, life, size0, size1;
    float c0[3], c1[3];
    float alpha;
    uint32_t config;
    Type type;
  };
  struct Live {
    Emitter e;
    float acc_flame = 0, acc_spark = 0, acc_smoke = 0;
    float flicker = 1;  // this frame's
    uint32_t rng = 1;
    bool seen = false;
  };
  float rnd(uint32_t &s) const;
  std::vector<Config> configs_;
  std::map<uint32_t, size_t> by_id_;
  std::map<uint64_t, Live> live_;
  std::vector<Particle> parts_;
  float wind_[2] = {0, 0};
  double time_ = 0;
};

}  // namespace fire
}  // namespace runtime
