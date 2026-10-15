// The viewport's controls beyond the mouse's orbit (design/playtest.md, "Moving around"): a
// gamepad, panning and raising an orbit's target, and the play test, where the hero's capsule
// walks and jumps through the scene with the camera behind it or at its eyes.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "physics/walker.h"
#include "viewport/raster.h"

namespace app {

// The first gamepad, this frame (the SDL3 backend feeds ImGui whichever is plugged in). Sticks are
// -1 to 1 past ImGui's dead zone, with up positive; triggers 0 to 1.
struct Pad {
  bool connected = false;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
  bool south = false, north = false, lb = false, rb = false, l3 = false;  // held
  bool south_pressed = false, north_pressed = false, back_pressed = false, start_pressed = false;
  bool moving() const;  // a stick or trigger is pushed
};
Pad read_pad();

// Orbit input over a 3D view: shift-drag or right-drag pans the target along the ground, ctrl-drag
// raises or lowers it, and the pad orbits (right stick), pans (left), zooms (triggers), and raises
// (shoulders). `scale` is the view's size in metres, for how far a pixel pans. True when it moved.
bool orbit_input(viewport::Camera &camera, const Pad &pad, float dt, bool hovered, float scale);

// A play test: the walker, and a camera that follows it.
class Play {
 public:
  bool on() const { return on_; }
  // Starts walking in `walker` (owned elsewhere, as the live plotter's, whose runtime fills it), or in
  // a world of its own (own_world()). Feet at `feet`, facing `heading` (radians from north to east).
  void start(physics::Walker *walker, const float feet[3], float heading);
  physics::Walker &own_world();
  // Moves into another world (or the same one, rebuilt), standing where it stood.
  void move_to(physics::Walker *walker);
  void stop();
  // The world it was built for (a selection and its version), to rebuild it when that changes.
  std::string world_key;

  // One step: WASD or the left stick walks, Shift or the right trigger runs, + and - change the speed, Space or A jumps, dragging
  // or the right stick looks, the wheel or Y moves the camera between behind and the eyes, and Escape or
  // Back stops. Keys count while the view is hovered or the play test has the keyboard.
  void update(float dt, bool hovered, const Pad &pad);

  // Where it looks from and at, and a free camera for the viewport.
  void eye_target(float eye[3], float target[3]) const;
  viewport::Camera camera(float fov = 1.05f) const;
  // The capsule as overlay lines (none at the eyes).
  void lines(std::vector<viewport::Line> &out) const;
  bool first_person() const { return distance_ <= 0; }
  float heading() const { return heading_; }
  float pitch() const { return pitch_; }
  // Times the walker's walking and running speeds (the view's speed setting; + and - change it).
  float speed = 1;
  const char *help() const;

 private:
  bool on_ = false;
  physics::Walker *walker_ = nullptr;
  std::unique_ptr<physics::Walker> own_;
  float heading_ = 0, pitch_ = -0.15f, distance_ = 4.5f;
};

}  // namespace app
