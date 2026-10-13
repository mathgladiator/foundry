#include "app/controls.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace app {

namespace {

float analog(ImGuiKey key) {
  return ImGui::GetIO().KeysData[key - ImGuiKey_NamedKey_BEGIN].AnalogValue;
}

}  // namespace

bool Pad::moving() const {
  return std::fabs(lx) + std::fabs(ly) + std::fabs(rx) + std::fabs(ry) + lt + rt > 0.02f;
}

Pad read_pad() {
  Pad p;
  ImGuiIO &io = ImGui::GetIO();
  p.connected = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
  if (!p.connected) return p;
  p.lx = analog(ImGuiKey_GamepadLStickRight) - analog(ImGuiKey_GamepadLStickLeft);
  p.ly = analog(ImGuiKey_GamepadLStickUp) - analog(ImGuiKey_GamepadLStickDown);
  p.rx = analog(ImGuiKey_GamepadRStickRight) - analog(ImGuiKey_GamepadRStickLeft);
  p.ry = analog(ImGuiKey_GamepadRStickUp) - analog(ImGuiKey_GamepadRStickDown);
  p.lt = analog(ImGuiKey_GamepadL2);
  p.rt = analog(ImGuiKey_GamepadR2);
  p.south = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
  p.north = ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp);
  p.lb = ImGui::IsKeyDown(ImGuiKey_GamepadL1);
  p.rb = ImGui::IsKeyDown(ImGuiKey_GamepadR1);
  p.l3 = ImGui::IsKeyDown(ImGuiKey_GamepadL3);
  p.south_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
  p.north_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
  p.back_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false);
  p.start_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
  return p;
}

bool orbit_input(viewport::Camera &camera, const Pad &pad, float dt, bool hovered, float scale) {
  ImGuiIO &io = ImGui::GetIO();
  bool moved = false;
  // Along the ground, as the camera faces: forward and right.
  const float fx = -std::sin(camera.yaw), fz = -std::cos(camera.yaw);
  const float rx = std::cos(camera.yaw), rz = -std::sin(camera.yaw);
  const float per_pixel = scale * 0.0025f / std::max(camera.zoom, 0.05f);
  if (hovered) {
    const ImVec2 d = io.MouseDelta;
    const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Right) || (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    const bool raise = io.KeyCtrl && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (raise) {  // the target follows the cursor up and down
      camera.pan[1] += d.y * per_pixel;
      moved = true;
    } else if (pan) {  // the ground follows the cursor
      camera.pan[0] += (-rx * d.x + fx * d.y) * per_pixel;
      camera.pan[2] += (-rz * d.x + fz * d.y) * per_pixel;
      moved = true;
    }
  }
  if (pad.moving() || pad.lb || pad.rb) {
    camera.view = viewport::View::Orbit;
    camera.yaw -= pad.rx * 2.0f * dt;
    camera.pitch = std::clamp(camera.pitch + pad.ry * 1.5f * dt, -1.4f, 1.4f);
    const float step = scale * 0.8f * dt / std::max(camera.zoom, 0.05f);
    camera.pan[0] += (fx * pad.ly + rx * pad.lx) * step;
    camera.pan[2] += (fz * pad.ly + rz * pad.lx) * step;
    camera.pan[1] += ((pad.rb ? 1.0f : 0.0f) - (pad.lb ? 1.0f : 0.0f)) * step * 0.6f;
    camera.zoom = std::clamp(camera.zoom * std::pow(2.0f, (pad.rt - pad.lt) * dt * 1.5f), 0.05f, 30.0f);
    moved = true;
  }
  return moved;
}

void Play::start(physics::Walker *walker, const float feet[3], float heading) {
  walker_ = walker ? walker : &own_world();
  walker_->spawn(feet);
  heading_ = heading;
  pitch_ = -0.15f;
  on_ = true;
}

physics::Walker &Play::own_world() {
  if (!own_) own_ = std::make_unique<physics::Walker>();
  return *own_;
}

void Play::move_to(physics::Walker *walker) {
  float feet[3] = {0, 0, 0};
  if (walker_) walker_->feet(feet);
  walker_ = walker;
  feet[1] += 0.2f;
  walker_->spawn(feet);
}

void Play::stop() {
  on_ = false;
  walker_ = nullptr;
  world_key.clear();
}

void Play::update(float dt, bool hovered, const Pad &pad) {
  if (!on_ || !walker_) return;
  ImGuiIO &io = ImGui::GetIO();
  const bool keys = !io.WantTextInput;
  if ((keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) || pad.back_pressed) {
    stop();
    return;
  }
  // Looking: drag (either button), or the right stick.
  if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0))) {
    heading_ += io.MouseDelta.x * 0.005f;
    pitch_ -= io.MouseDelta.y * 0.005f;
  }
  heading_ += pad.rx * 2.6f * dt;
  pitch_ += pad.ry * 1.8f * dt;
  pitch_ = std::clamp(pitch_, -1.35f, 1.45f);
  // Behind or at the eyes: the wheel brings the camera in (past 1 m, to the eyes) and out; Y swaps.
  if (hovered && io.MouseWheel != 0) {
    if (distance_ <= 0 && io.MouseWheel < 0) distance_ = 1.2f;
    else if (distance_ > 0) distance_ = std::min(12.0f, distance_ / std::pow(1.15f, io.MouseWheel));
    if (distance_ > 0 && distance_ < 1.0f) distance_ = 0;
  }
  if (pad.north_pressed) distance_ = distance_ > 0 ? 0 : 4.5f;

  physics::Walker::Input in;
  in.heading = heading_;
  if (keys) {
    in.forward = (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f);
    in.right = (ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f);
    in.jump = ImGui::IsKeyPressed(ImGuiKey_Space, false);
    in.run = io.KeyShift;
  }
  in.forward = std::clamp(in.forward + pad.ly, -1.0f, 1.0f);
  in.right = std::clamp(in.right + pad.lx, -1.0f, 1.0f);
  in.jump = in.jump || pad.south_pressed;
  in.run = in.run || pad.rt > 0.4f || pad.l3;
  // Small steps keep a slow frame from tunnelling through a trunk.
  float left = std::min(dt, 0.1f);
  while (left > 1e-4f) {
    const float h = std::min(left, 1.0f / 60.0f);
    walker_->step(h, in);
    in.jump = false;
    left -= h;
  }
}

void Play::eye_target(float eye[3], float target[3]) const {
  float feet[3] = {0, 0, 0};
  if (walker_) walker_->feet(feet);
  const float look[3] = {std::sin(heading_) * std::cos(pitch_), std::sin(pitch_), -std::cos(heading_) * std::cos(pitch_)};
  const float head[3] = {feet[0], feet[1] + physics::Walker::kEye, feet[2]};
  if (distance_ <= 0) {
    for (int i = 0; i < 3; ++i) eye[i] = head[i];
  } else {  // over the shoulder: behind, a little up, never under its feet
    for (int i = 0; i < 3; ++i) eye[i] = head[i] - look[i] * distance_;
    eye[1] = std::max(eye[1] + 0.25f, feet[1] + 0.3f);
  }
  for (int i = 0; i < 3; ++i) target[i] = eye[i] + look[i];
}

viewport::Camera Play::camera(float fov) const {
  viewport::Camera c;
  float target[3];
  eye_target(c.eye, target);
  c.free = true;
  c.azimuth = heading_;
  c.elevation = pitch_;
  c.fov = fov;
  return c;
}

void Play::lines(std::vector<viewport::Line> &out) const {
  if (!walker_ || distance_ <= 0) return;
  float f[3];
  walker_->feet(f);
  const float r = physics::Walker::kRadius, lo = r, hi = physics::Walker::kHeight - r;
  const float cr = 1.0f, cg = 0.78f, cb = 0.3f;
  auto seg = [&](foundry::vec3 a, foundry::vec3 b) { out.push_back({a, b, cr, cg, cb, 0.95f}); };
  const int n = 16;
  for (int i = 0; i < n; ++i) {  // rings at the caps and the waist
    const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
    for (float y : {lo, 0.5f * physics::Walker::kHeight, hi})
      seg({f[0] + r * std::cos(a0), f[1] + y, f[2] + r * std::sin(a0)}, {f[0] + r * std::cos(a1), f[1] + y, f[2] + r * std::sin(a1)});
  }
  for (int k = 0; k < 4; ++k) {  // sides, and arcs over the caps
    const float a = 1.5707963f * k + heading_;
    const float dx = std::sin(a), dz = -std::cos(a);
    seg({f[0] + dx * r, f[1] + lo, f[2] + dz * r}, {f[0] + dx * r, f[1] + hi, f[2] + dz * r});
    for (int i = 0; i < 6; ++i) {
      const float b0 = 1.5707963f * i / 6, b1 = 1.5707963f * (i + 1) / 6;
      seg({f[0] + dx * r * std::cos(b0), f[1] + hi + r * std::sin(b0), f[2] + dz * r * std::cos(b0)},
          {f[0] + dx * r * std::cos(b1), f[1] + hi + r * std::sin(b1), f[2] + dz * r * std::cos(b1)});
      seg({f[0] + dx * r * std::cos(b0), f[1] + lo - r * std::sin(b0), f[2] + dz * r * std::cos(b0)},
          {f[0] + dx * r * std::cos(b1), f[1] + lo - r * std::sin(b1), f[2] + dz * r * std::cos(b1)});
    }
  }
  // Where it faces: a short line from the chest.
  const float hx = std::sin(heading_), hz = -std::cos(heading_);
  seg({f[0], f[1] + 1.3f, f[2]}, {f[0] + hx * 0.7f, f[1] + 1.3f, f[2] + hz * 0.7f});
}

const char *Play::help() const {
  return "WASD or the left stick walks, Shift or RT runs, Space or A jumps, drag or the right stick looks, "
         "the wheel or Y moves between behind and the eyes; Escape or Back stops.";
}

}  // namespace app
