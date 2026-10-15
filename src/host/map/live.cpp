#include "map/live.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <malloc.h>

#include "json/registry.h"
#include "map/map.h"
#include "plot/live.h"
#include "runtime_diag.h"

namespace map {

namespace {

constexpr float kSqrt3 = 1.7320508f;
constexpr float kLoad = 330.0f, kUnload = 460.0f;  // chunk streaming, metres from the focus to a chunk's edge
constexpr float kSiteLoad = 650.0f, kSiteUnload = 820.0f;
constexpr float kFill = 210.0f, kUnfill = 280.0f;  // nature: grown on chunks this near, cleared beyond
constexpr float kGrass = 90.0f, kUngrass = 130.0f;  // grass fields: only the nearest chunks (they cost the most each frame)
constexpr int kSpawnsPerFrame = 6;

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

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

// A pipeline spawned by name, its parameters by name from a JSON object (numbers, booleans, names).
foundry_instance spawn_named(foundry_runtime *rt, const std::string &pipeline, const nlohmann::json &values, float x, float y, float z,
                             float yaw) {
  std::vector<std::string> names, texts;
  if (values.is_object())
    for (auto it = values.begin(); it != values.end(); ++it) {
      if (it.value().is_object() || it.value().is_array()) continue;  // inline entries can't be named
      names.push_back(it.key());
      texts.push_back(it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
  std::vector<const char *> n, v;
  for (size_t i = 0; i < names.size(); ++i) {
    n.push_back(names[i].c_str());
    v.push_back(texts[i].c_str());
  }
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float t[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  return foundry_diag_spawn(rt, pipeline.c_str(), n.data(), v.data(), static_cast<uint32_t>(n.size()), t);
}

}  // namespace

Live::~Live() { stop(); }

void Live::stop() {
  if (device_) SDL_WaitForGPUIdle(device_);
  if (rt_) {
    for (Chunk &c : chunks_) clear(c);
    for (Site &s : sites_)
      for (foundry_instance i : s.things) foundry_destroy(rt_, i);
    foundry_runtime_destroy(rt_);
  }
  rt_ = nullptr;
  chunks_.clear();
  sites_.clear();
  far_ = boundary_ = FOUNDRY_NO_INSTANCE;
  sky_.reset();
  if (device_) {
    if (color_) SDL_ReleaseGPUTexture(device_, color_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
  }
  color_ = depth_ = nullptr;
  w_ = h_ = 0;
  stats_ = {};
  device_ = nullptr;
  malloc_trim(0);
}

void Live::clear(Chunk &c) {
  if (!rt_) return;
  for (foundry_instance i : c.things) foundry_destroy(rt_, i);
  for (foundry_instance i : c.fields) foundry_destroy(rt_, i);
  for (foundry_region r : c.regions) foundry_region_destroy(rt_, r);
  if (c.ground != FOUNDRY_NO_INSTANCE) foundry_destroy(rt_, c.ground);
  c.things.clear();
  c.fields.clear();
  c.regions.clear();
  c.region_plotters.clear();
  c.ground = FOUNDRY_NO_INSTANCE;
  c.filled = false;
}

bool Live::start(SDL_GPUDevice *device, const nlohmann::json &file, const nlohmann::json &plan, const nlohmann::json &far_spec,
                 const std::map<std::string, std::shared_ptr<const plan::Settlement>> &settlements, std::string &error) {
  stop();
  const double t0 = now_ms();
  device_ = device;
  foundry_runtime_desc desc = {};
  desc.device = device;
  desc.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  desc.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  desc.quality = FOUNDRY_QUALITY_HIGH;
  desc.physics = walker_->desc();
  rt_ = foundry_runtime_create(&desc);
  if (!rt_) {
    error = std::string("runtime: ") + foundry_last_error();
    return false;
  }
  plan_ = plan;
  file_ = file;
  seed_ = file.value("seed", 1u);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // The far land: the map's plan, packed.
  {
    const auto *ch = foundry::json::find_channel("map_far_spec");
    std::vector<uint8_t> bytes;
    try {
      ch->from_json(far_spec, bytes);
    } catch (const std::exception &e) {
      error = std::string("the far land's spec doesn't pack: ") + e.what();
      return false;
    }
    foundry_map_far_params p;
    foundry_map_far_default_params(&p);
    far_ = foundry_map_far_spawn_packed(rt_, &p, identity, bytes.data(), static_cast<uint32_t>(bytes.size()));
  }
  // The boundary round the map's hull, with its exits.
  {
    const nlohmann::json &h = plan["hull"];
    std::vector<foundry_hull_cell> cells;
    const auto &qr = h["cells"];
    for (size_t i = 0; i + 1 < qr.size(); i += 2) {
      foundry_hull_cell c = {};
      c.q = qr[i].get<int32_t>();
      c.r = qr[i + 1].get<int32_t>();
      c.edge = static_cast<foundry_edge>(h["edges"][i / 2].get<uint32_t>());
      c.height = h["heights"][i / 2].get<float>();
      c.rise = h["rises"][i / 2].get<float>();
      cells.push_back(c);
    }
    const auto &oc = h.value("opening_cells", nlohmann::json::array());
    for (size_t k = 0; k + 1 < oc.size(); k += 2)
      for (auto &c : cells)
        if (c.q == oc[k].get<int32_t>() && c.r == oc[k + 1].get<int32_t>()) {
          c.opening = h["opening_kinds"][k / 2].get<uint32_t>() + 1;
          c.opening_width = h["opening_widths"][k / 2].get<uint32_t>();
        }
    foundry_boundary_params bp;
    foundry_boundary_default_params(&bp);
    const int biome = foundry_boundary_biome_find(file.value("biome", "temperate").c_str());
    if (biome >= 0) bp.biome = static_cast<foundry_boundary_biome>(biome);
    bp.detail = FOUNDRY_BOUNDARY_DETAIL_MEDIUM;
    bp.view_distance = 4000;
    bp.figure_range = 0;  // the map's own edge woods: the boundary draws them
    bp.seed = 1 + seed_ % 1000000u;
    boundary_ = foundry_boundary_spawn_hull(rt_, &bp, identity, cells.data(), static_cast<uint32_t>(cells.size()));
  }
  // The chunks, by their middles; the sites; each parcel's plotter.
  const int S = plan.value("chunk_size", 64);
  const auto &cq = plan["chunk_q"], &cr = plan["chunk_r"];
  for (size_t c = 0; c < cq.size(); ++c) {
    Chunk ch;
    const float q = static_cast<float>(cq[c].get<int>()) + S * 0.5f, r = static_cast<float>(cr[c].get<int>()) + S * 0.5f;
    ch.x = kSqrt3 * (q + r * 0.5f);
    ch.z = 1.5f * r;
    chunks_.push_back(ch);
  }
  const auto &sites = file.value("sites", nlohmann::json::array());
  for (const auto &pl : plan.value("places", nlohmann::json::array())) {
    Site s;
    s.name = pl.value("name", "");
    s.x = pl["position"][0].get<float>();
    s.y = pl["position"][1].get<float>();
    s.z = pl["position"][2].get<float>();
    s.yaw = pl.value("yaw", 0.0f) * 3.14159265f / 180.0f;
    s.radius = pl.value("radius", 4.0f);
    s.pipeline = pl.value("pipeline", "");
    s.params = pl.value("params", "");
    auto it = settlements.find(s.name);
    if (it != settlements.end() && it->second && it->second->error.empty()) s.town = it->second;
    (void)sites;
    sites_.push_back(s);
  }
  for (const auto &p : plan.value("parcels", nlohmann::json::array())) plotters_.push_back(p.value("plotter", ""));
  stats_.start_ms = now_ms() - t0;
  stats_.chunks_total = static_cast<uint32_t>(chunks_.size());
  return true;
}

void Live::fill(Chunk &c, size_t index) {
  const uint32_t n = foundry_ground_nature(rt_, c.ground, nullptr, nullptr, 0);
  std::vector<foundry_hex_region> regions(n);
  std::vector<uint32_t> parcels(n);
  foundry_ground_nature(rt_, c.ground, regions.data(), parcels.data(), n);
  std::map<uint32_t, std::vector<foundry_hex_region>> by_parcel;
  for (uint32_t i = 0; i < n; ++i) by_parcel[parcels[i]].push_back(regions[i]);
  for (auto &[parcel, rs] : by_parcel) {
    if (parcel >= plotters_.size() || plotters_[parcel].empty()) continue;
    const foundry_plotter p = foundry_plotter_find(plotters_[parcel].c_str());
    if (p == FOUNDRY_NO_PLOTTER) continue;
    foundry_region_desc rd = {};
    rd.plotter = p;
    rd.world_seed = seed_ * 2654435761u ^ static_cast<uint32_t>(index) * 40503u ^ parcel;
    rd.regions = rs.data();
    rd.region_count = static_cast<uint32_t>(rs.size());
    const foundry_region region = foundry_region_create(rt_, &rd);
    if (region == FOUNDRY_NO_REGION) continue;
    c.regions.push_back(region);
    c.region_plotters.push_back(p);
    std::vector<foundry_placement> out(4096);
    uint32_t volume = 1;
    while (volume > 0) {
      const uint32_t got = foundry_region_grow(rt_, region, FOUNDRY_GROW_SEED, nullptr, 0, nullptr, out.data(), static_cast<uint32_t>(out.size()), &volume);
      if (got == 0) break;
      for (uint32_t k = 0; k < got; ++k) c.things.push_back(foundry_placement_spawn(rt_, region, &out[k]));
      stats_.nature += got;
    }
  }
  c.filled = true;
}

void Live::stream(const float focus[3]) {
  // Chunks: the nearest unspawned within reach, a few a frame; those far behind, gone.
  std::vector<std::pair<float, size_t>> want;
  for (size_t i = 0; i < chunks_.size(); ++i) {
    Chunk &c = chunks_[i];
    const float d = std::hypot(c.x - focus[0], c.z - focus[2]) - 75.0f;
    if (c.ground == FOUNDRY_NO_INSTANCE && d < kLoad * reach) want.push_back({d, i});
    else if (c.ground != FOUNDRY_NO_INSTANCE && d > kUnload * reach) clear(c);
  }
  std::sort(want.begin(), want.end());
  size_t pending = want.size();
  const nlohmann::json biome = file_.value("biome", "temperate");
  for (size_t k = 0; k < want.size() && k < static_cast<size_t>(kSpawnsPerFrame); ++k) {
    Chunk &c = chunks_[want[k].second];
    const nlohmann::json spec = chunk_spec(plan_, want[k].second, seed_);
    std::vector<uint8_t> bytes;
    foundry::json::find_channel("map_chunk")->from_json(spec, bytes);
    foundry_map_chunk_params p;
    foundry_map_chunk_default_params(&p);
    const int b = foundry_map_chunk_biome_find(biome.get<std::string>().c_str());
    if (b >= 0) p.biome = static_cast<foundry_map_chunk_biome>(b);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    c.ground = foundry_map_chunk_spawn_packed(rt_, &p, identity, bytes.data(), static_cast<uint32_t>(bytes.size()));
  }
  // Nature on each near chunk once its ground is built (a few a frame), cleared when it falls behind.
  int fills = 0;
  for (size_t i = 0; i < chunks_.size(); ++i) {
    Chunk &c = chunks_[i];
    if (c.ground == FOUNDRY_NO_INSTANCE) continue;
    const float d = std::hypot(c.x - focus[0], c.z - focus[2]) - 75.0f;
    if (c.filled && d > kUnfill * reach) {
      for (foundry_instance t : c.things) foundry_destroy(rt_, t);
      for (foundry_instance t : c.fields) foundry_destroy(rt_, t);
      for (foundry_region r : c.regions) foundry_region_destroy(rt_, r);
      c.things.clear();
      c.fields.clear();
      c.regions.clear();
      c.region_plotters.clear();
      c.filled = false;
      continue;
    }
    // Grass on the nearest filled chunks only.
    if (c.filled && !c.fields.empty() && d > kUngrass) {
      for (foundry_instance t : c.fields) foundry_destroy(rt_, t);
      c.fields.clear();
    } else if (c.filled && c.fields.empty() && grass && d < kGrass) {
      for (size_t k = 0; k < c.regions.size(); ++k) {
        std::vector<foundry_instance> fields(foundry_plotter_field_count(c.region_plotters[k]));
        if (fields.empty()) continue;
        foundry_region_spawn_fields(rt_, c.regions[k], fields.data(), static_cast<uint32_t>(fields.size()));
        for (foundry_instance f : fields) {
          foundry_diag_set_param(rt_, f, "bed", "none");  // the chunk's terrain draws the ground; grass only grows on it
          c.fields.push_back(f);
        }
      }
      if (c.fields.empty()) c.fields.push_back(FOUNDRY_NO_INSTANCE);  // none to grow: don't ask again
    }
    if (!c.filled && d <= kFill * reach) ++pending;
    if (c.filled && grass && d < kGrass && c.fields.empty()) ++pending;
    if (c.filled || d > kFill * reach || fills >= 2) continue;
    foundry_diag_instance info;
    if (foundry_diag_instance_get(rt_, c.ground, &info) == FOUNDRY_OK && info.ready) {
      fill(c, i);
      ++fills;
    }
  }
  // Sites near: their settlements' pieces, or their one pipeline.
  for (Site &s : sites_) {
    const float d = std::hypot(s.x - focus[0], s.z - focus[2]) - s.radius;
    if (!s.things.empty() && d > kSiteUnload * reach) {
      for (foundry_instance i : s.things) foundry_destroy(rt_, i);
      s.things.clear();
      continue;
    }
    if (!s.things.empty() || d > kSiteLoad * reach) continue;
    ++pending;
    if (s.town) {
      for (const plan::Fill &f : s.town->fills) {
        if (f.what == "walls" || f.what == "fences") continue;  // whole networks: not by name yet
        const foundry_instance i = spawn_named(rt_, f.pipeline, f.values, s.x + f.at.x, s.y + f.at.y, s.z + f.at.z, f.yaw);
        if (i != FOUNDRY_NO_INSTANCE) s.things.push_back(i);
      }
    } else if (!s.pipeline.empty()) {
      nlohmann::json values = nlohmann::json::parse(s.params.empty() ? "{}" : s.params, nullptr, false);
      if (values.is_discarded()) values = nlohmann::json::object();
      const foundry_instance i = spawn_named(rt_, s.pipeline, values, s.x, s.y, s.z, s.yaw);
      if (i != FOUNDRY_NO_INSTANCE) s.things.push_back(i);
    }
    if (s.things.empty()) s.things.push_back(FOUNDRY_NO_INSTANCE);  // tried: don't retry every frame
  }
  pending_ = pending;
}

float Live::ground_at(float x, float z) const {
  if (!plan_.is_object()) return 0;
  const float gx = plan_["gx"].get<float>(), gz = plan_["gz"].get<float>(), sp = plan_["spacing"].get<float>();
  const int nx = plan_["nx"].get<int>(), nz = plan_["nz"].get<int>();
  const int i = std::clamp(static_cast<int>(std::lround((x - gx) / sp)), 0, nx - 1), k = std::clamp(static_cast<int>(std::lround((z - gz) / sp)), 0, nz - 1);
  return plan_["heights"][static_cast<size_t>(k) * static_cast<size_t>(nx) + static_cast<size_t>(i)].get<float>();
}

void Live::set_look(const runtime::Look &l) {
  if (!rt_) return;
  foundry_look g;
  foundry_look_default(&g);
  g.vision = static_cast<foundry_vision>(l.vision);
  g.contrast = l.contrast;
  g.saturation = l.saturation;
  g.edges = l.edges;
  g.effect = static_cast<foundry_look_effect>(l.effect);
  std::memcpy(g.tint, l.tint, sizeof g.tint);
  g.strength = l.strength;
  foundry_set_look(rt_, &g);
}

void Live::set_sky(const std::shared_ptr<const runtime::sky::Observatory> &o, const runtime::sky::State &state, const float centre[3],
                   float radius) {
  if (rt_) plot::hand_sky(rt_, sky_, o, state, centre, radius);
}

SDL_GPUTexture *Live::frame(SDL_GPUCommandBuffer *cmd, int width, int height, const float eye[3], const float target[3], const float focus[3],
                            double dt) {
  if (!rt_) return nullptr;
  const double s0 = now_ms();
  stream(focus);
  stats_.stream_ms += (now_ms() - s0 - stats_.stream_ms) * 0.1;
  if (!color_ || w_ != width || h_ != height) {
    if (color_) SDL_ReleaseGPUTexture(device_, color_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    color_ = make_texture(device_, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    depth_ = make_texture(device_, width, height, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    w_ = width;
    h_ = height;
  }
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
  const float fov = 50.0f * 3.14159265f / 180.0f, near_z = 0.3f, far_z = 12000.0f;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float t = 1.0f / std::tan(fov * 0.5f);
  const float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, far_z / (near_z - far_z), -1, 0, 0, near_z * far_z / (near_z - far_z), 0};
  foundry_set_viewport(rt_, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  foundry_set_camera(rt_, view, proj);
  foundry_update(rt_, static_cast<float>(dt));
  walker_->tidy();  // the bodies the update added and removed (a game steps its world; this one only walks)
  foundry_prepare(rt_, cmd);
  SDL_GPUColorTargetInfo ct = {};
  ct.texture = color_;
  ct.clear_color = {0.55f, 0.68f, 0.82f, 1};
  ct.load_op = SDL_GPU_LOADOP_CLEAR;
  ct.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo dti = {};
  dti.texture = depth_;
  dti.clear_depth = 1;
  dti.load_op = SDL_GPU_LOADOP_CLEAR;
  dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dti);
  foundry_draw(rt_, cmd, pass);
  SDL_EndGPURenderPass(pass);
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
  const double k = frames_ < 10 ? 1.0 : 0.1;
  stats_.update_ms += (fr.update_ms - stats_.update_ms) * k;
  stats_.prepare_ms += (fr.prepare_ms - stats_.prepare_ms) * k;
  foundry_diag_counters c;
  foundry_diag_counters_get(rt_, &c);
  stats_.instances = c.instances;
  stats_.drawn = c.drawn;
  stats_.triangles = c.triangles;
  stats_.builds_pending = c.cache_pending;
  stats_.terrain_nodes = c.terrain_nodes;
  stats_.field_tiles = c.field_tiles;
  stats_.gpu_ms = fr.draw_ms;  // the CPU recording the draws
  uint32_t loaded = 0, filled = 0, sites = 0, pieces = 0;
  for (const Chunk &ch : chunks_) {
    loaded += ch.ground != FOUNDRY_NO_INSTANCE;
    filled += ch.filled;
  }
  for (const Site &s : sites_) {
    sites += !s.things.empty();
    pieces += static_cast<uint32_t>(s.things.size());
  }
  stats_.chunks = loaded;
  stats_.chunks_filled = filled;
  stats_.sites = sites;
  stats_.pieces = pieces;
  return color_;
}

}  // namespace map
