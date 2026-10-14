// A play test's walker (design/playtest.md, "Walking"): the hero's capsule (1.8 m tall, 0.35 m
// across, as the viewport's hero overlay) moved by Jolt's CharacterVirtual, with gravity, steps,
// slopes, and jumps, in a Jolt world of its own.
//
// The world is a game's: the live plotter hands it to the runtime (foundry_runtime_desc::physics),
// which keeps every instance's collision there, so the walker bumps into trunks as a player would.
// Other views add the ground and their model's collision themselves.
#pragma once

#include <memory>
#include <vector>

#include "core/pipeline.h"
#include "runtime.h"

namespace physics {

struct WalkerWorld;

class Walker {
 public:
  static constexpr float kHeight = 1.8f, kRadius = 0.35f, kEye = 1.62f;

  Walker();
  ~Walker();
  Walker(const Walker &) = delete;
  Walker &operator=(const Walker &) = delete;

  // The world as a game hands it to the runtime: a purpose's layer for each purpose. The walker stands
  // on solid bodies and its ground, pushes through soft ones, and passes query and sensor bodies.
  const foundry_physics *desc() const;

  // The static things it walks on, which it owns: triangles (xyz per vertex, three per triangle, world
  // space), a model's collision bodies at a place, and a floor (a wide plane at a height, so nothing
  // falls forever). clear() removes them, and never the bodies a runtime added.
  void add_triangles(const std::vector<float> &xyz);
  void add_bodies(const std::vector<runtime::CollisionBody> &bodies, const float at[3], float yaw);
  void add_floor(float y);
  void clear();

  // Stands the capsule with its feet at `feet`, still.
  void spawn(const float feet[3]);

  // The first place nearest (x, z) where the capsule fits: searching rings outward (every half metre,
  // out to `radius`), the first open ground (its own ground, with nothing overhead, flat enough to
  // stand on) where it touches nothing. `on_floor`: the wide floor counts as ground (a scene with no ground
  // of its own); `solid_ground`: solid bodies count as ground too (a terrain's columns).
  // Writes the feet and returns true, or false when nothing fits.
  bool find_spawn(float x, float z, float radius, bool on_floor, float feet[3], bool solid_ground = false);

  struct Input {
    float forward = 0, right = 0;  // -1 to 1, along the heading
    float heading = 0;             // radians from north (-Z) toward east (+X)
    bool jump = false;             // pressed this step
    bool run = false;
  };
  void step(float dt, const Input &in);

  void feet(float out[3]) const;
  bool grounded() const;
  bool spawned() const;

 private:
  std::unique_ptr<WalkerWorld> w_;
};

}  // namespace physics
