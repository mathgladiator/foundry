#include "perf/bench.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include "core/log.h"
#include "core/util.h"
#include "render/cel_renderer.h"

namespace perf {

namespace {

std::mutex g_bench_mutex;  // one benchmark at a time: they compete for the GPU

constexpr float kPi = 3.14159265358979f;

std::vector<runtime::Instance> grid(uint32_t count, float spacing, float &extent) {
  std::vector<runtime::Instance> out(count);
  uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
  extent = side * spacing;
  float origin = -0.5f * (side - 1) * spacing;
  for (uint32_t i = 0; i < count; ++i) {
    float x = origin + static_cast<float>(i % side) * spacing, z = origin + static_cast<float>(i / side) * spacing;
    float yaw = static_cast<float>((i * 2654435761u) >> 8) / 16777216.0f * 2 * kPi;  // deterministic variety
    float c = std::cos(yaw), s = std::sin(yaw);
    out[i] = {{c, 0, s, x}, {0, 1, 0, 0}, {-s, 0, c, z}};
  }
  return out;
}

}  // namespace

Result benchmark(const runtime::Mesh &mesh, float footprint_radius, const Options &o) {
  std::lock_guard<std::mutex> lock(g_bench_mutex);
  Result r;
  if (mesh.indices.empty()) {
    r.error = "mesh has no triangles";
    return r;
  }
  SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
  if (!device) {
    r.error = std::string("no GPU device: ") + SDL_GetError();
    return r;
  }
  r.driver = SDL_GetGPUDeviceDriver(device);
  SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
  const char *name = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "");
  r.device = name ? name : "";

  SDL_GPUTextureFormat depth = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
  for (SDL_GPUTextureFormat f : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM}) {
    if (SDL_GPUTextureSupportsFormat(device, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
      depth = f;
      break;
    }
  }
  const SDL_GPUTextureFormat color = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  runtime::CelRenderer renderer;
  if (!renderer.init(device, color, depth, r.error)) {
    SDL_DestroyGPUDevice(device);
    return r;
  }
  auto make_target = [&](SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = f;
    t.usage = usage;
    t.width = o.width;
    t.height = o.height;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    return SDL_CreateGPUTexture(device, &t);
  };
  SDL_GPUTexture *color_tex = make_target(color, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
  SDL_GPUTexture *depth_tex = make_target(depth, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  runtime::GpuMesh gpu_mesh = renderer.upload(mesh);

  float height = 0;
  for (const auto &p : mesh.positions) height = std::max(height, p.y);
  const float spacing = std::max(2.5f * footprint_radius, 0.1f);

  // Renders `frames` frames of `count` instances; returns seconds.
  auto run = [&](uint32_t count, uint32_t frames) {
    float extent = 0;
    renderer.set_instances(grid(count, spacing, extent));
    const float fov = 50.0f * kPi / 180.0f;
    float radius = 0.75f * extent + height + 0.5f;
    float dist = radius / std::sin(fov * 0.5f);
    float eye[3] = {dist * 0.6f, dist * 0.55f, dist * 0.6f}, target[3] = {0, height * 0.5f, 0};
    float vp[16];
    runtime::view_projection(eye, target, fov, static_cast<float>(o.width) / o.height, std::max(0.01f, dist * 0.01f),
                             dist * 4, vp);
    auto frame = [&](bool fence) -> SDL_GPUFence * {
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
      SDL_GPUColorTargetInfo ct = {};
      ct.texture = color_tex;
      ct.clear_color = {0.2f, 0.22f, 0.27f, 1};
      ct.load_op = SDL_GPU_LOADOP_CLEAR;
      ct.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPUDepthStencilTargetInfo dt = {};
      dt.texture = depth_tex;
      dt.clear_depth = 1;
      dt.load_op = SDL_GPU_LOADOP_CLEAR;
      dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
      renderer.draw(cmd, pass, gpu_mesh, count, vp);
      SDL_EndGPURenderPass(pass);
      if (fence) return SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
      SDL_SubmitGPUCommandBuffer(cmd);
      return nullptr;
    };
    // Warm up, and wait for the GPU to drain.
    SDL_GPUFence *warm = nullptr;
    for (int i = 0; i < 3; ++i) warm = frame(i == 2);
    SDL_WaitForGPUFences(device, true, &warm, 1);
    SDL_ReleaseGPUFence(device, warm);
    auto start = std::chrono::steady_clock::now();
    SDL_GPUFence *last = nullptr;
    for (uint32_t i = 0; i < frames; ++i) last = frame(i + 1 == frames);
    SDL_WaitForGPUFences(device, true, &last, 1);
    SDL_ReleaseGPUFence(device, last);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  auto sample = [&](uint32_t count, bool refine) {
    // Keep each sample under about two seconds, with at least 3 frames.
    double probe = run(count, 1);
    uint32_t frames = static_cast<uint32_t>(std::clamp(2.0 / std::max(probe, 1e-6), 3.0, static_cast<double>(o.frames)));
    double seconds = run(count, frames);
    Sample s;
    s.instances = count;
    s.frames = frames;
    s.ms = seconds * 1000.0 / frames;
    s.fps = frames / seconds;
    s.refine = refine;
    r.samples.push_back(s);
    return s.fps;
  };

  uint32_t good = 0, bad = 0;
  for (uint32_t n = 1; n <= o.max_instances; n *= 2) {
    if (sample(n, false) >= o.fps_floor) {
      good = n;
    } else {
      bad = n;
      break;
    }
  }
  if (bad == 0) {
    r.budget_is_lower_bound = true;
  } else {
    for (int i = 0; i < o.refine_steps && bad - good > 1; ++i) {
      uint32_t mid = good + (bad - good) / 2;
      if (sample(mid, true) >= o.fps_floor) good = mid;
      else bad = mid;
    }
  }
  r.budget = good;
  std::sort(r.samples.begin(), r.samples.end(), [](const Sample &a, const Sample &b) { return a.instances < b.instances; });

  renderer.release(gpu_mesh);
  renderer.shutdown();
  SDL_ReleaseGPUTexture(device, color_tex);
  SDL_ReleaseGPUTexture(device, depth_tex);
  SDL_DestroyGPUDevice(device);
  r.ok = true;
  return r;
}

nlohmann::json to_json(const Result &r, const Options &o) {
  nlohmann::json samples = nlohmann::json::array();
  for (const auto &s : r.samples)
    samples.push_back({{"instances", s.instances}, {"fps", s.fps}, {"ms", s.ms}, {"frames", s.frames}, {"refine", s.refine}});
  return {{"driver", r.driver},
          {"device", r.device},
          {"resolution", {o.width, o.height}},
          {"fps_floor", o.fps_floor},
          {"samples", samples},
          {"budget_instances", r.budget},
          {"budget_is_lower_bound", r.budget_is_lower_bound}};
}

}  // namespace perf
