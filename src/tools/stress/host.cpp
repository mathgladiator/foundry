// The stress platform's machinery: off-screen frames, sampling, layout,
// statistics, OS memory, the machine, and the journal.
#include <malloc.h>
#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "platform.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace stress {

std::string fmt(const char *f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

Stats stats(std::vector<double> v) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))]; };
  s.p50 = at(0.5);
  s.p95 = at(0.95);
  s.p99 = at(0.99);
  s.max = v.back();
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / v.size();
  return s;
}

json to_json(const Stats &s) {
  auto r = [](double x) { return std::round(x * 1000) / 1000; };
  return {{"p50", r(s.p50)}, {"p95", r(s.p95)}, {"p99", r(s.p99)}, {"max", r(s.max)}, {"mean", r(s.mean)}, {"n", s.n}};
}

// --- memory and the machine ---------------------------------------------------

namespace {
double field_kb(const std::string &text, const char *key) {
  size_t at = text.find(key);
  if (at == std::string::npos) return 0;
  return std::atof(text.c_str() + at + std::strlen(key));
}
std::string slurp(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

Memory memory() {
  Memory m;
  std::string status = slurp("/proc/self/status");
  m.rss_mb = field_kb(status, "VmRSS:") / 1024;
  m.rss_anon_mb = field_kb(status, "RssAnon:") / 1024;
  m.hwm_mb = field_kb(status, "VmHWM:") / 1024;
  m.private_dirty_mb = field_kb(slurp("/proc/self/smaps_rollup"), "Private_Dirty:") / 1024;
  struct mallinfo2 mi = mallinfo2();
  m.heap_mb = (static_cast<double>(mi.uordblks) + static_cast<double>(mi.hblkhd)) / (1024.0 * 1024.0);
  return m;
}

void trim() { malloc_trim(0); }

json to_json(const Memory &m) {
  auto r = [](double x) { return std::round(x * 100) / 100; };
  return {{"rss_mb", r(m.rss_mb)}, {"rss_anon_mb", r(m.rss_anon_mb)}, {"hwm_mb", r(m.hwm_mb)},
          {"private_dirty_mb", r(m.private_dirty_mb)}, {"heap_mb", r(m.heap_mb)}};
}

json machine(const std::string &gpu_name, const std::string &gpu_driver) {
  std::string cpu = "unknown";
  std::string info = slurp("/proc/cpuinfo");
  size_t at = info.find("model name");
  if (at != std::string::npos) {
    size_t colon = info.find(':', at), end = info.find('\n', at);
    cpu = info.substr(colon + 2, end - colon - 2);
  }
  double mem_gb = std::round(field_kb(slurp("/proc/meminfo"), "MemTotal:") / (1024.0 * 1024.0));
  struct utsname u;
  uname(&u);
  unsigned cores = std::thread::hardware_concurrency();
  std::string id = cpu + "|" + std::to_string(cores) + "|" + std::to_string(static_cast<int>(mem_gb)) + "|" + gpu_name +
                   "|" + gpu_driver;
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : id) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return {{"cpu", cpu},
          {"cores", cores},
          {"memory_gb", mem_gb},
          {"os", std::string(u.sysname) + " " + u.release},
          {"gpu", gpu_name.empty() ? "none" : gpu_name},
          {"gpu_driver", gpu_driver},
          {"fingerprint", fmt("%016llx", static_cast<unsigned long long>(h))}};
}

// --- the journal ----------------------------------------------------------------

bool Journal::open(const std::string &path) {
  f_ = std::fopen(path.c_str(), "w");
  return f_ != nullptr;
}

void Journal::close() {
  flush();
  if (f_) std::fclose(f_);
  f_ = nullptr;
}

void Journal::flush_frames() {
  if (!pending_frames_ || !f_) return;
  json j = {{"i", index_++}, {"op", "frames"}, {"n", pending_frames_}, {"dt", pending_dt_}};
  std::fputs((j.dump() + "\n").c_str(), f_);
  pending_frames_ = 0;
}

void Journal::frames(uint32_t n, double dt) {
  if (pending_frames_ && dt != pending_dt_) flush_frames();
  pending_frames_ += n;
  pending_dt_ = dt;
}

void Journal::write(const json &line) {
  if (!f_) return;
  flush_frames();
  json j = line;
  j["i"] = index_++;
  std::fputs((j.dump() + "\n").c_str(), f_);
}

void Journal::flush() {
  flush_frames();
  if (f_) std::fflush(f_);
}

// --- the GPU ----------------------------------------------------------------------

bool Gpu::init(const Plan &plan, std::string &error) {
  if (plan.gpu == "none") return false;
  device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
  if (!device) {
    error = std::string("no GPU device: ") + SDL_GetError();
    return false;
  }
  driver = SDL_GetGPUDeviceDriver(device);
  if (plan.window) {
    window = SDL_CreateWindow("Foundry stress", static_cast<int>(plan.width), static_cast<int>(plan.height),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) {
      error = std::string("can't open a window: ") + SDL_GetError();
      shutdown();
      return false;
    }
  }
  SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
  const char *n = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "");
  name = n ? n : "";
  depth_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
  for (SDL_GPUTextureFormat f : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM})
    if (SDL_GPUTextureSupportsFormat(device, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
      depth_format = f;
      break;
    }
  auto make = [&](SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = f;
    t.usage = usage;
    t.width = plan.width;
    t.height = plan.height;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    return SDL_CreateGPUTexture(device, &t);
  };
  color = make(color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
  depth = make(depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  depth_w = plan.width;
  depth_h = plan.height;
  if (!color || !depth) {
    error = std::string("can't make render targets: ") + SDL_GetError();
    shutdown();
    return false;
  }
  return true;
}

void Gpu::shutdown() {
  if (!device) return;
  if (!in_flight.empty()) {
    SDL_WaitForGPUFences(device, true, in_flight.data(), static_cast<Uint32>(in_flight.size()));
    for (auto *f : in_flight) SDL_ReleaseGPUFence(device, f);
    in_flight.clear();
  }
  if (color) SDL_ReleaseGPUTexture(device, color);
  if (depth) SDL_ReleaseGPUTexture(device, depth);
  if (window) {
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    window = nullptr;
  }
  SDL_DestroyGPUDevice(device);
  device = nullptr;
  color = depth = nullptr;
}

// --- the context ------------------------------------------------------------------

bool Ctx::create(std::string &error) {
  foundry_runtime_desc desc = {};
  desc.device = use_gpu ? gpu.device : nullptr;
  desc.color_format = gpu.color_format;
  desc.depth_format = gpu.depth_format;
  desc.quality = FOUNDRY_QUALITY_HIGH;
  desc.update_threads = plan.update_threads;
  desc.build_threads = plan.build_threads;
  // The stress tool plays the game: it owns a Jolt world and hands it to the runtime.
  physics = toy_physics_create(max_bodies);
  desc.physics = toy_physics_desc(physics);
  rt = foundry_runtime_create(&desc);
  if (!rt) {
    error = std::string("runtime: ") + foundry_last_error();
    return false;
  }
  return true;
}

void Ctx::destroy() {
  if (use_gpu && gpu.device && !gpu.in_flight.empty()) {
    SDL_WaitForGPUFences(gpu.device, true, gpu.in_flight.data(), static_cast<Uint32>(gpu.in_flight.size()));
    for (auto *f : gpu.in_flight) SDL_ReleaseGPUFence(gpu.device, f);
    gpu.in_flight.clear();
  }
  if (rt) foundry_runtime_destroy(rt);
  rt = nullptr;
  if (physics) toy_physics_destroy(physics);
  physics = nullptr;
}

FrameTime Ctx::frame(bool render, bool sync) {
  FrameTime t;
  if (gpu.window) pump();
  const double t0 = now_ms();
  foundry_update(rt, static_cast<float>(plan.dt));
  sim_time += plan.dt;
  ++frames;
  if (use_gpu && render) {
    // At most two frames in flight, as a game with a swapchain would have.
    while (gpu.in_flight.size() >= 2) {
      const double w0 = now_ms();
      SDL_WaitForGPUFences(gpu.device, true, &gpu.in_flight.front(), 1);
      t.gpu_wait += now_ms() - w0;
      SDL_ReleaseGPUFence(gpu.device, gpu.in_flight.front());
      gpu.in_flight.erase(gpu.in_flight.begin());
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    foundry_prepare(rt, cmd);
    // A window draws off screen too (so snapshots and video work), then blits to the swapchain.
    SDL_GPUTexture *swap = nullptr;
    Uint32 sw = 0, sh = 0;
    if (gpu.window) {
      SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu.window, &swap, &sw, &sh);
      if (swap && (sw != gpu.depth_w || sh != gpu.depth_h)) {  // the window was resized
        auto make = [&](SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage) {
          SDL_GPUTextureCreateInfo ti = {};
          ti.type = SDL_GPU_TEXTURETYPE_2D;
          ti.format = f;
          ti.usage = usage;
          ti.width = sw;
          ti.height = sh;
          ti.layer_count_or_depth = 1;
          ti.num_levels = 1;
          return SDL_CreateGPUTexture(gpu.device, &ti);
        };
        SDL_ReleaseGPUTexture(gpu.device, gpu.depth);
        SDL_ReleaseGPUTexture(gpu.device, gpu.color);
        gpu.depth = make(gpu.depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        gpu.color = make(gpu.color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        gpu.depth_w = sw;
        gpu.depth_h = sh;
        plan.width = sw;
        plan.height = sh;
        apply_camera();
      }
    }
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = gpu.color;
    ct.clear_color = {0.55f, 0.68f, 0.82f, 1};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {};
    dt.texture = gpu.depth;
    dt.clear_depth = 1;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    foundry_draw(rt, cmd, pass);
    SDL_EndGPURenderPass(pass);
    if (swap) {
      SDL_GPUBlitInfo blit = {};
      blit.source.texture = gpu.color;
      blit.source.w = gpu.depth_w;
      blit.source.h = gpu.depth_h;
      blit.destination.texture = swap;
      blit.destination.w = sw;
      blit.destination.h = sh;
      blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
      blit.filter = SDL_GPU_FILTER_NEAREST;
      SDL_BlitGPUTexture(cmd, &blit);
    }
    const double s0 = now_ms();
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (sync) {
      // Drain what's ahead of it first, so the measurement is this frame alone.
      for (auto *f : gpu.in_flight) {
        SDL_WaitForGPUFences(gpu.device, true, &f, 1);
        SDL_ReleaseGPUFence(gpu.device, f);
      }
      gpu.in_flight.clear();
      SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
      SDL_ReleaseGPUFence(gpu.device, fence);
      t.gpu_sync = now_ms() - s0;
    } else {
      gpu.in_flight.push_back(fence);
    }
  } else {
    foundry_prepare(rt, nullptr);
  }
  foundry_diag_frame f;
  foundry_diag_frame_get(rt, &f);
  t.update = f.update_ms;
  t.prepare = f.prepare_ms;
  t.draw = f.draw_ms;
  t.integrate = f.integrate_ms;
  t.classify = f.classify_ms;
  t.animate = f.animate_ms;
  t.results = f.results_ms;
  t.total = now_ms() - t0;
  journal.frames(1, plan.dt);
  return t;
}

bool Ctx::snapshot(const std::string &path) {
  std::vector<uint8_t> px;
  if (!read_frame(px)) return false;
  return stbi_write_png(path.c_str(), static_cast<int>(plan.width), static_cast<int>(plan.height), 4, px.data(),
                        static_cast<int>(plan.width * 4)) != 0;
}

bool Ctx::read_frame(std::vector<uint8_t> &px) {
  if (!use_gpu || !gpu.device) return false;
  for (auto *f : gpu.in_flight) {
    SDL_WaitForGPUFences(gpu.device, true, &f, 1);
    SDL_ReleaseGPUFence(gpu.device, f);
  }
  gpu.in_flight.clear();
  const uint32_t w = plan.width, h = plan.height, size = w * h * 4;
  SDL_GPUTransferBufferCreateInfo t = {};
  t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  t.size = size;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu.device, &t);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion src = {};
  src.texture = gpu.color;
  src.w = w;
  src.h = h;
  src.d = 1;
  SDL_GPUTextureTransferInfo dst = {tb, 0, 0, 0};
  SDL_DownloadFromGPUTexture(copy, &src, &dst);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
  SDL_ReleaseGPUFence(gpu.device, fence);
  const auto *src_px = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(gpu.device, tb, false));
  px.assign(src_px, src_px + size);
  SDL_UnmapGPUTransferBuffer(gpu.device, tb);
  SDL_ReleaseGPUTransferBuffer(gpu.device, tb);
  return true;
}

void Ctx::camera(const float eye[3], const float target[3], float far_z) {
  std::memcpy(cam_eye, eye, sizeof cam_eye);
  std::memcpy(cam_target, target, sizeof cam_target);
  cam_far = far_z;
  apply_camera();
}

void Ctx::apply_camera() {
  float eye[3], target[3];
  float far_z = cam_far;
  if (fly.on) {
    const float cy = std::cos(fly.yaw), sy = std::sin(fly.yaw), cp = std::cos(fly.pitch), sp = std::sin(fly.pitch);
    std::memcpy(eye, fly.eye, sizeof eye);
    target[0] = eye[0] + sy * cp;
    target[1] = eye[1] + sp;
    target[2] = eye[2] - cy * cp;
    far_z = std::max(far_z, 2000.0f);
  } else {
    std::memcpy(eye, cam_eye, sizeof eye);
    std::memcpy(target, cam_target, sizeof target);
  }
  float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float &v : f) v /= fl;
  float r[3] = {-f[2], 0, f[0]};  // f x up, with up = +Y
  float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
  for (float &v : r) v /= rl;
  float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  float v[16] = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                 -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                 (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]), 1};
  std::memcpy(view, v, sizeof v);
  const float fov = 50.0f * 3.14159265f / 180.0f, near_z = 0.5f;
  const float aspect = static_cast<float>(plan.width) / plan.height;
  float t = 1.0f / std::tan(fov * 0.5f);
  float p[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, far_z / (near_z - far_z), -1, 0, 0, near_z * far_z / (near_z - far_z), 0};
  std::memcpy(projection, p, sizeof p);
  foundry_set_viewport(rt, plan.width, plan.height);
  foundry_set_camera(rt, view, projection);
}

void Ctx::overview(float half_extent, float height) {
  // A raised three-quarter view that fills the frame with the scene.
  const float radius = half_extent + height * 0.5f;
  const float dist = radius / std::tan(25.0f * 3.14159265f / 180.0f) * 0.85f;
  const float eye[3] = {dist * 0.5f, dist * 0.45f, dist * 0.55f}, target[3] = {0, height * 0.2f, 0};
  camera(eye, target, dist * 3);
}

void Ctx::pump() {
  pressed.clear();
  const double now = now_ms();
  const float wall_dt = last_wall > 0 ? static_cast<float>(std::min(0.1, (now - last_wall) / 1000)) : 0.0f;
  last_wall = now;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      journal.write({{"op", "window closed"}});
      journal.close();
      std::printf("window closed: run stopped\n");
      std::fflush(stdout);
      std::_Exit(0);
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
      pressed.push_back(e.key.key);
      if (e.key.key == SDLK_TAB) {
        if (!fly.on) {  // start flying from where the phase's camera is
          std::memcpy(fly.eye, cam_eye, sizeof fly.eye);
          const float d[3] = {cam_target[0] - cam_eye[0], cam_target[1] - cam_eye[1], cam_target[2] - cam_eye[2]};
          fly.yaw = std::atan2(d[0], -d[2]);
          fly.pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
        }
        fly.on = !fly.on;
      }
      if (e.key.key == SDLK_ESCAPE) fly.on = false;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT) {
      fly.look = true;
      if (!fly.on) {
        std::memcpy(fly.eye, cam_eye, sizeof fly.eye);
        const float d[3] = {cam_target[0] - cam_eye[0], cam_target[1] - cam_eye[1], cam_target[2] - cam_eye[2]};
        fly.yaw = std::atan2(d[0], -d[2]);
        fly.pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
        fly.on = true;
      }
      SDL_SetWindowRelativeMouseMode(gpu.window, true);
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) {
      fly.look = false;
      SDL_SetWindowRelativeMouseMode(gpu.window, false);
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && fly.look) {
      fly.yaw += e.motion.xrel * 0.003f;
      fly.pitch = std::clamp(fly.pitch - e.motion.yrel * 0.003f, -1.5f, 1.5f);
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL) fly.speed = std::clamp(fly.speed * (e.wheel.y > 0 ? 1.25f : 0.8f), 1.0f, 400.0f);
  }
  if (fly.on) {
    const bool *k = SDL_GetKeyboardState(nullptr);
    const float v = fly.speed * wall_dt * (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] ? 4.0f : 1.0f);
    const float fx = std::sin(fly.yaw), fz = -std::cos(fly.yaw);
    float fwd = 0, side = 0, up = 0;
    if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) fwd += 1;
    if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) fwd -= 1;
    if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) side += 1;
    if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) side -= 1;
    if (k[SDL_SCANCODE_SPACE]) up += 1;
    if (k[SDL_SCANCODE_C]) up -= 1;
    fly.eye[0] += (fx * fwd - fz * side) * v;
    fly.eye[2] += (fz * fwd + fx * side) * v;
    fly.eye[1] = std::max(0.3f, fly.eye[1] + up * v);
  }
  apply_camera();
  if (now - hud_at > 250) {
    hud_at = now;
    foundry_diag_counters c;
    foundry_diag_counters_get(rt, &c);
    foundry_diag_frame f;
    foundry_diag_frame_get(rt, &f);
    std::string title = fmt("%s  %llu instances (%llu awake)  update %.1f ms  prepare %.1f ms  %s  %s",
                            pipeline ? pipeline->name : title_name.c_str(),
                            static_cast<unsigned long long>(c.instances), static_cast<unsigned long long>(c.awake),
                            f.update_ms, f.prepare_ms,
                            fly.on ? fmt("flying %.0f m/s (Tab: follow the test)", fly.speed).c_str()
                                   : "Tab or right mouse: fly",
                            hud.c_str());
    SDL_SetWindowTitle(gpu.window, title.c_str());
  }
}

foundry_instance Ctx::spawn(const Config &c, const float transform[16]) {
  foundry_instance i = pipeline->spawn(rt, c.data(), transform);
  json t = json::array();
  for (int k = 0; k < 16; ++k) t.push_back(transform[k]);
  journal.write({{"op", "spawn"}, {"handle", i}, {"values", describe(c)}, {"transform", t}});
  return i;
}

foundry_status Ctx::set(foundry_instance i, uint32_t param, stress_value v) {
  const stress_param &p = pipeline->params[param];
  json value = p.kind == STRESS_F32 ? json(v.f) : p.kind == STRESS_ENUM ? json(p.values[v.u]) : json(v.u);
  journal.write({{"op", "set"}, {"handle", i}, {"param", p.name}, {"value", value}});
  return pipeline->set(rt, i, param, v);
}

foundry_status Ctx::trigger(foundry_instance i, uint32_t param) {
  journal.write({{"op", "trigger"}, {"handle", i}, {"param", pipeline->params[param].name}});
  return pipeline->trigger(rt, i, param);
}

foundry_status Ctx::destroy(foundry_instance i) {
  journal.write({{"op", "destroy"}, {"handle", i}});
  return foundry_destroy(rt, i);
}

std::vector<uint32_t> Ctx::update_params() const {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < pipeline->param_count; ++i) {
    const auto &p = pipeline->params[i];
    if (p.kind != STRESS_CLOCK && p.kind != STRESS_EVENT && p.phase == STRESS_UPDATE && !plan.fixed.contains(p.name))
      out.push_back(i);
  }
  return out;
}

std::vector<uint32_t> Ctx::build_params() const {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < pipeline->param_count; ++i) {
    const auto &p = pipeline->params[i];
    if (p.kind != STRESS_CLOCK && p.kind != STRESS_EVENT && p.phase == STRESS_BUILD && !plan.fixed.contains(p.name))
      out.push_back(i);
  }
  return out;
}

std::vector<uint32_t> Ctx::events() const {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < pipeline->param_count; ++i)
    if (pipeline->params[i].kind == STRESS_EVENT) out.push_back(i);
  return out;
}

int Ctx::param(const std::string &name) const {
  for (uint32_t i = 0; i < pipeline->param_count; ++i)
    if (name == pipeline->params[i].name) return static_cast<int>(i);
  return -1;
}

json Ctx::describe(const Config &c) const {
  json j = json::object();
  for (uint32_t i = 0; i < pipeline->param_count; ++i) {
    const auto &p = pipeline->params[i];
    if (p.kind == STRESS_ENUM) j[p.name] = p.values[std::min(c[i].u, p.count - 1)];
    else if (p.kind == STRESS_U32) j[p.name] = c[i].u;
    else if (p.kind == STRESS_F32) j[p.name] = c[i].f;
  }
  return j;
}

namespace {
bool value_of(const stress_param &p, const json &v, stress_value &out) {
  if (p.kind == STRESS_ENUM) {
    if (v.is_number_unsigned() && v.get<uint32_t>() < p.count) {
      out.u = v.get<uint32_t>();
      return true;
    }
    if (!v.is_string()) return false;
    for (uint32_t k = 0; k < p.count; ++k)
      if (v.get<std::string>() == p.values[k]) {
        out.u = k;
        return true;
      }
    return false;
  }
  if (!v.is_number()) return false;
  if (p.kind == STRESS_U32) out.u = v.get<uint32_t>();
  else out.f = v.get<float>();
  return true;
}
}  // namespace

bool Ctx::parse(const json &j, Config &c) const {
  c = fallback();
  for (uint32_t i = 0; i < pipeline->param_count; ++i) {
    const auto &p = pipeline->params[i];
    if (j.contains(p.name) && !value_of(p, j[p.name], c[i])) return false;
  }
  return true;
}

Config Ctx::fallback() const {
  Config c(pipeline->param_count);
  for (uint32_t i = 0; i < pipeline->param_count; ++i) c[i] = pipeline->params[i].fallback;
  return c;
}

stress_value Ctx::random_value(uint32_t i, Rng &rng) const {
  const auto &p = pipeline->params[i];
  stress_value v = p.fallback;
  if (plan.fixed.contains(p.name)) {
    value_of(p, plan.fixed[p.name], v);
    return v;
  }
  const json *range = plan.ranges.contains(p.name) ? &plan.ranges[p.name] : nullptr;
  if (p.kind == STRESS_ENUM) {
    if (range && range->is_array() && !range->empty()) {
      value_of(p, (*range)[rng.below(static_cast<uint32_t>(range->size()))], v);
    } else {
      v.u = rng.below(p.count);
    }
    return v;
  }
  double lo = p.min, hi = p.max;
  if (range && range->is_array() && range->size() == 2) {
    lo = (*range)[0].get<double>();
    hi = (*range)[1].get<double>();
  }
  if (p.kind == STRESS_U32) v.u = static_cast<uint32_t>(lo) + rng.below(static_cast<uint32_t>(hi - lo) + 1);
  else v.f = static_cast<float>(lo + (hi - lo) * rng.unit());
  return v;
}

std::vector<Config> sample(const Ctx &ctx, size_t n, Rng &rng) {
  const stress_pipeline &pl = *ctx.pipeline;
  std::vector<Config> out(n, ctx.fallback());
  for (uint32_t i = 0; i < pl.param_count; ++i) {
    const auto &p = pl.params[i];
    if (p.kind == STRESS_CLOCK || p.kind == STRESS_EVENT) continue;
    std::vector<stress_value> vals(n);
    if (ctx.plan.fixed.contains(p.name)) {
      for (auto &v : vals) v = ctx.random_value(i, rng);
    } else if (p.kind == STRESS_ENUM) {
      std::vector<uint32_t> allowed;
      if (ctx.plan.ranges.contains(p.name)) {
        for (const auto &x : ctx.plan.ranges[p.name]) {
          stress_value v;
          if (value_of(p, x, v)) allowed.push_back(v.u);
        }
      }
      if (allowed.empty())
        for (uint32_t k = 0; k < p.count; ++k) allowed.push_back(k);
      for (size_t k = 0; k < n; ++k) vals[k].u = allowed[k % allowed.size()];
    } else {
      for (size_t k = 0; k < n; ++k) vals[k] = ctx.random_value(i, rng);
      // The two ends of every range.
      double lo = p.min, hi = p.max;
      if (ctx.plan.ranges.contains(p.name)) {
        lo = ctx.plan.ranges[p.name][0].get<double>();
        hi = ctx.plan.ranges[p.name][1].get<double>();
      }
      if (n > 0) {
        if (p.kind == STRESS_U32) vals[0].u = static_cast<uint32_t>(lo);
        else vals[0].f = static_cast<float>(lo);
      }
      if (n > 1) {
        if (p.kind == STRESS_U32) vals[1].u = static_cast<uint32_t>(hi);
        else vals[1].f = static_cast<float>(hi);
      }
    }
    for (size_t k = n; k > 1; --k) std::swap(vals[k - 1], vals[rng.below(static_cast<uint32_t>(k))]);
    for (size_t k = 0; k < n; ++k) out[k][i] = vals[k];
  }
  return out;
}

std::vector<std::pair<float, float>> hex_layout(size_t n, float spacing, Rng &rng, float jitter) {
  std::vector<std::pair<float, float>> out;
  static const int dirs[6][2] = {{1, 0}, {0, 1}, {-1, 1}, {-1, 0}, {0, -1}, {1, -1}};
  auto put = [&](int q, int r) {
    float x = spacing * (q + r * 0.5f), z = spacing * (r * 0.8660254f);
    float jx = static_cast<float>((rng.unit() * 2 - 1) * jitter * spacing);
    float jz = static_cast<float>((rng.unit() * 2 - 1) * jitter * spacing);
    out.push_back({x + jx, z + jz});
  };
  if (n) put(0, 0);
  for (int ring = 1; out.size() < n; ++ring) {
    int q = 0, r = -ring;  // start at a corner, walk the six sides
    for (int side = 0; side < 6 && out.size() < n; ++side)
      for (int step = 0; step < ring && out.size() < n; ++step) {
        put(q, r);
        q += dirs[side][0];
        r += dirs[side][1];
      }
  }
  return out;
}

void translation(float x, float z, float yaw, float out[16]) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float m[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, 0, z, 1};
  std::memcpy(out, m, sizeof m);
}

}  // namespace stress
