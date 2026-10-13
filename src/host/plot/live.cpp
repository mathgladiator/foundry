#include "plot/live.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <malloc.h>

#include "runtime_diag.h"

namespace plot {

namespace {

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t index_of(const nlohmann::json &v, std::initializer_list<const char *> names) {
  if (v.is_number()) return v.get<uint32_t>();
  uint32_t i = 0;
  for (const char *n : names) {
    if (v.is_string() && v.get<std::string>() == n) return i;
    ++i;
  }
  return 0;
}

// The plot view's ground (plot_v1_region values, as JSON) as the runtime's hex regions.
struct Ground {
  std::vector<foundry_hex_region> regions;
  std::vector<std::vector<int32_t>> cells;
};

Ground ground_of(const nlohmann::json &regions) {
  Ground g;
  g.cells.reserve(regions.size());
  for (const auto &r : regions) {
    foundry_hex_region h = {};
    h.shape = static_cast<foundry_hex_shape>(index_of(r.value("shape", nlohmann::json("hex")), {"cells", "hex", "ring", "line"}));
    g.cells.emplace_back();
    for (const auto &c : r.value("cells", nlohmann::json::array())) g.cells.back().push_back(c.get<int32_t>());
    h.q = r.value("q", 0);
    h.r = r.value("r", 0);
    h.q2 = r.value("q2", 0);
    h.r2 = r.value("r2", 0);
    h.radius = r.value("radius", 0u);
    h.priority = r.value("priority", 0);
    h.cut = r.value("cut", false);
    uint32_t set = 0;
    if (r.contains("set")) set = r["set"].get<uint32_t>();
    else {
      if (r.contains("ground")) set |= FOUNDRY_SET_GROUND;
      if (r.contains("available")) set |= FOUNDRY_SET_AVAILABLE;
      if (r.contains("keep_clear_m")) set |= FOUNDRY_SET_KEEP_CLEAR;
      if (r.contains("height")) set |= FOUNDRY_SET_HEIGHT;
      if (r.contains("moisture")) set |= FOUNDRY_SET_MOISTURE;
    }
    h.set = set;
    h.ground = static_cast<foundry_ground>(index_of(r.value("ground", nlohmann::json("soil")), {"soil", "water", "rock", "sand", "path"}));
    h.available = r.value("available", true);
    h.keep_clear_m = r.value("keep_clear_m", 0.0f);
    h.height = r.value("height", 0.0f);
    h.moisture = r.value("moisture", 0.5f);
    g.regions.push_back(h);
  }
  for (size_t i = 0; i < g.regions.size(); ++i) {
    g.regions[i].cells = g.cells[i].empty() ? nullptr : g.cells[i].data();
    g.regions[i].cell_count = static_cast<uint32_t>(g.cells[i].size() / 2);
  }
  return g;
}

SDL_GPUTexture *make_texture(SDL_GPUDevice *device, int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = usage;
  info.width = static_cast<Uint32>(w);
  info.height = static_cast<Uint32>(h);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  return SDL_CreateGPUTexture(device, &info);
}

}  // namespace

Live::~Live() { stop(); }

void Live::stop() {
  if (device_) SDL_WaitForGPUIdle(device_);
  if (rt_) {
    for (foundry_instance i : instances_) foundry_destroy(rt_, i);
    if (region_ != FOUNDRY_NO_REGION) foundry_region_destroy(rt_, region_);
    foundry_runtime_destroy(rt_);
  }
  rt_ = nullptr;
  region_ = FOUNDRY_NO_REGION;
  shared_.clear();
  instances_.clear();
  tree_.clear();
  if (device_) {
    if (color_) SDL_ReleaseGPUTexture(device_, color_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
  }
  color_ = depth_ = nullptr;
  w_ = h_ = 0;
  stats_ = {};
  device_ = nullptr;  // the device may go away after this (stop again does nothing)
  // A runtime's builder and update threads leave their freed memory in glibc's per-thread arenas;
  // hand it back, or every regrow of a large region holds on to the last one's.
  malloc_trim(0);
}

bool Live::start(SDL_GPUDevice *device, const std::string &plotter, const nlohmann::json &regions, uint32_t seed,
                 std::string &error) {
  stop();
  device_ = device;
  const foundry_plotter p = foundry_plotter_find(plotter.c_str());
  if (p == FOUNDRY_NO_PLOTTER) {
    error = "the native runtime has no plotter " + plotter + " (build it, or it isn't generated yet)";
    return false;
  }
  foundry_runtime_desc desc = {};
  desc.device = device;
  desc.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  desc.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  desc.quality = FOUNDRY_QUALITY_HIGH;
  rt_ = foundry_runtime_create(&desc);
  if (!rt_) {
    error = std::string("runtime: ") + foundry_last_error();
    return false;
  }
  Ground g = ground_of(regions);
  foundry_region_desc rd = {};
  rd.plotter = p;
  rd.world_seed = seed;
  rd.regions = g.regions.data();
  rd.region_count = static_cast<uint32_t>(g.regions.size());
  region_ = foundry_region_create(rt_, &rd);
  // Grown to full, every placement spawned.
  const uint32_t kinds = foundry_plotter_kind_count(p);
  std::vector<bool> kind_tree(kinds, false);
  for (uint32_t k = 0; k < kinds; ++k) kind_tree[k] = std::strncmp(foundry_plotter_kind_name(p, k), "tree_", 5) == 0;
  const double t0 = now_ms();
  std::vector<foundry_placement> out(4096);
  uint32_t volume = 1;
  float reach = 1;
  while (volume > 0) {
    const uint32_t n = foundry_region_grow(rt_, region_, FOUNDRY_GROW_SEED, nullptr, 0, nullptr, out.data(),
                                           static_cast<uint32_t>(out.size()), &volume);
    if (n == 0) break;
    for (uint32_t k = 0; k < n; ++k) {
      instances_.push_back(foundry_placement_spawn(rt_, region_, &out[k]));
      tree_.push_back(out[k].kind < kinds && kind_tree[out[k].kind]);
      reach = std::max(reach, std::hypot(out[k].position[0], out[k].position[2]));
    }
  }
  // Its fields (grass): one instance each, over every cell.
  std::vector<foundry_instance> fields(foundry_plotter_field_count(p));
  if (!fields.empty()) {
    foundry_region_spawn_fields(rt_, region_, fields.data(), static_cast<uint32_t>(fields.size()));
    for (foundry_instance f : fields) {
      instances_.push_back(f);
      tree_.push_back(false);
    }
  }
  stats_.grow_ms = now_ms() - t0;
  extent_ = reach + 2;
  return true;
}

void Live::share(const nlohmann::json &shared) {
  if (!rt_ || !shared.is_object()) return;
  const std::string dump = shared.dump();
  if (dump == shared_) return;
  shared_ = dump;
  for (auto it = shared.begin(); it != shared.end(); ++it) {
    const std::string value = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    for (foundry_instance i : instances_) foundry_diag_set_param(rt_, i, it.key().c_str(), value.c_str());
  }
}

SDL_GPUTexture *Live::frame(SDL_GPUCommandBuffer *cmd, int width, int height, const float eye[3], const float target[3],
                            double dt) {
  if (!rt_) return nullptr;
  if (!color_ || w_ != width || h_ != height) {
    if (color_) SDL_ReleaseGPUTexture(device_, color_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    color_ = make_texture(device_, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    depth_ = make_texture(device_, width, height, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    w_ = width;
    h_ = height;
  }
  // The camera: a 50-degree view from eye to target (as the stress tool's), far enough for the region.
  float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  const float fl = std::max(1e-6f, std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]));
  for (float &v : f) v /= fl;
  float r[3] = {-f[2], 0, f[0]};
  const float rl = std::max(1e-6f, std::sqrt(r[0] * r[0] + r[2] * r[2]));
  for (float &v : r) v /= rl;
  const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  const float view[16] = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                          -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                          (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]), 1};
  const float fov = 50.0f * 3.14159265f / 180.0f, near_z = 0.3f;
  const float far_z = std::max(500.0f, fl + 3 * extent_);
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float t = 1.0f / std::tan(fov * 0.5f);
  const float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, far_z / (near_z - far_z), -1, 0, 0, near_z * far_z / (near_z - far_z), 0};
  foundry_set_viewport(rt_, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  foundry_set_camera(rt_, view, proj);
  foundry_update(rt_, static_cast<float>(dt));
  foundry_prepare(rt_, cmd);
  SDL_GPUColorTargetInfo ct = {};
  ct.texture = color_;
  ct.clear_color = {0.55f, 0.68f, 0.82f, 1};
  ct.load_op = SDL_GPU_LOADOP_CLEAR;
  ct.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo dt_info = {};
  dt_info.texture = depth_;
  dt_info.clear_depth = 1;
  dt_info.load_op = SDL_GPU_LOADOP_CLEAR;
  dt_info.store_op = SDL_GPU_STOREOP_DONT_CARE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt_info);
  foundry_draw(rt_, cmd, pass);
  SDL_EndGPURenderPass(pass);

  // Stats: the frame rate over the last second, the runtime's own times and counters.
  ++frames_;
  ++fps_frames_;
  fps_window_ += dt;
  if (fps_window_ >= 1.0) {
    stats_.fps = static_cast<double>(fps_frames_) / fps_window_;
    fps_frames_ = 0;
    fps_window_ = 0;
  }
  foundry_diag_frame fr;
  foundry_diag_frame_get(rt_, &fr);
  const double k = frames_ < 10 ? 1.0 : 0.1;  // smoothed
  stats_.update_ms += (fr.update_ms - stats_.update_ms) * k;
  stats_.prepare_ms += (fr.prepare_ms - stats_.prepare_ms) * k;
  stats_.draw_ms += (fr.draw_ms - stats_.draw_ms) * k;
  foundry_diag_counters c;
  foundry_diag_counters_get(rt_, &c);
  stats_.instances = c.instances;
  stats_.drawn = c.drawn;
  stats_.triangles = c.triangles;
  stats_.draws = c.draws;
  stats_.animated = c.animated;
  stats_.field_tiles = c.field_tiles;
  stats_.builds_pending = c.cache_pending;
  if (frames_ % 30 == 1) {  // trees drawn, now and then (it asks about every instance)
    uint64_t trees = 0;
    for (size_t i = 0; i < instances_.size(); ++i) {
      if (!tree_[i]) continue;
      foundry_diag_instance info;
      if (foundry_diag_instance_get(rt_, instances_[i], &info) == FOUNDRY_OK && info.drawn) ++trees;
    }
    stats_.trees = trees;
  }
  return color_;
}

}  // namespace plot
