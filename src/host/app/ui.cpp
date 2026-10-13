// The workspace UI (Dear ImGui on SDL3's GPU API).
//
// Layout: the workspace tree on the left (mirroring the project layout), the
// viewport in the middle, the inspector (state, details, notes, approval) on
// the right, and the activity log along the bottom.
#include <SDL3/SDL.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/capture.h"
#include "app/services.h"
#include "plot/tools.h"
#include "assets/view.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"
#include "assets/fracture_item.h"
#include "assets/mesh_item.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "json/registry.h"
#include "perf/stress.h"
#include "pipeline/config.h"
#include "pipeline/flow.h"
#include "pipeline/live.h"
#include "viewport/gpu.h"

namespace app {

namespace {

ImVec4 state_color(const std::string &state) {
  if (state == "final") return {0.45f, 0.85f, 0.5f, 1};
  if (state == "rejected") return {0.95f, 0.45f, 0.4f, 1};
  return {0.95f, 0.8f, 0.4f, 1};
}

const char *kKinds[][2] = {{"pipeline", "Pipelines"}, {"plotter", "Plotters"}, {"system", "Systems"}, {"channel", "Channels"},
                           {"config", "Config"}, {"conformance", "Conformance"}, {"meshy", "Meshy"}, {"fixed", "Fixed assets"},
                           {"fracture", "Fractures"}, {"reference", "Reference"}};

// A GPU texture holding the viewport image, re-uploaded when it changes.
struct ViewTexture {
  SDL_GPUDevice *device = nullptr;
  SDL_GPUTexture *texture = nullptr;
  int width = 0, height = 0;
  // Textures replaced this frame. ImGui's draw data may still reference them,
  // so they're released only after the frame has been submitted.
  std::vector<SDL_GPUTexture *> retired;

  void upload(SDL_GPUCommandBuffer *cmd, const viewport::Image &img) {
    if (img.width <= 0 || img.height <= 0) return;
    if (!texture || width != img.width || height != img.height) {
      if (texture) retired.push_back(texture);
      SDL_GPUTextureCreateInfo info = {};
      info.type = SDL_GPU_TEXTURETYPE_2D;
      info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      info.width = static_cast<Uint32>(img.width);
      info.height = static_cast<Uint32>(img.height);
      info.layer_count_or_depth = 1;
      info.num_levels = 1;
      texture = SDL_CreateGPUTexture(device, &info);
      width = img.width;
      height = img.height;
    }
    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = static_cast<Uint32>(img.rgba.size());
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tinfo);
    void *dst = SDL_MapGPUTransferBuffer(device, tb, false);
    std::memcpy(dst, img.rgba.data(), img.rgba.size());
    SDL_UnmapGPUTransferBuffer(device, tb);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion region = {};
    region.texture = texture;
    region.w = static_cast<Uint32>(img.width);
    region.h = static_cast<Uint32>(img.height);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, tb);
  }
  void release_retired() {
    for (SDL_GPUTexture *t : retired) SDL_ReleaseGPUTexture(device, t);
    retired.clear();
  }
  void release() {
    release_retired();
    if (texture) SDL_ReleaseGPUTexture(device, texture);
    texture = nullptr;
  }
};

// Renders the frame's draw data into an offscreen texture and reads it back.
viewport::Image capture_frame(SDL_GPUDevice *device, SDL_GPUTextureFormat format, ImDrawData *draw) {
  viewport::Image img;
  const Uint32 w = static_cast<Uint32>(draw->DisplaySize.x * draw->FramebufferScale.x);
  const Uint32 h = static_cast<Uint32>(draw->DisplaySize.y * draw->FramebufferScale.y);
  if (w == 0 || h == 0) return img;
  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  info.width = w;
  info.height = h;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  SDL_GPUTexture *target = SDL_CreateGPUTexture(device, &info);
  SDL_GPUTransferBufferCreateInfo tinfo = {};
  tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  tinfo.size = w * h * 4;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tinfo);
  if (!target || !tb) return img;

  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
  ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
  SDL_GPUColorTargetInfo ct = {};
  ct.texture = target;
  ct.clear_color = {0.1f, 0.1f, 0.12f, 1};
  ct.load_op = SDL_GPU_LOADOP_CLEAR;
  ct.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
  ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
  SDL_EndGPURenderPass(pass);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion region = {};
  region.texture = target;
  region.w = w;
  region.h = h;
  region.d = 1;
  SDL_GPUTextureTransferInfo dst = {};
  dst.transfer_buffer = tb;
  SDL_DownloadFromGPUTexture(copy, &region, &dst);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(device, true, &fence, 1);
  SDL_ReleaseGPUFence(device, fence);

  const uint8_t *px = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, tb, false));
  img.width = static_cast<int>(w);
  img.height = static_cast<int>(h);
  img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
  SDL_UnmapGPUTransferBuffer(device, tb);
  const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    if (bgra) std::swap(img.rgba[i], img.rgba[i + 2]);
    img.rgba[i + 3] = 255;
  }
  SDL_ReleaseGPUTransferBuffer(device, tb);
  SDL_ReleaseGPUTexture(device, target);
  return img;
}

struct UiState {
  std::string selected;
  int ws_version = -1;
  assets::ViewContent content;
  viewport::Camera camera;
  viewport::Overlays overlays;
  bool silhouette = false;
  // What the current texture was rendered for.
  std::string rendered_key;
  viewport::Image pending;
  bool has_pending = false;
  char note[1024] = {};
  char reject_reason[512] = {};
  std::string message;
  Uint64 last_scan = 0;
  Uint64 interact_until = 0;  // render at low resolution until this tick
  uint32_t triangles = 0;      // viewport level of detail; 0 = full mesh
  int selected_lod = -1;       // the LOD row being viewed (follows it when it's regenerated); -1 = none
  std::string limits_item;     // the mesh item the LOD limit sliders belong to
  float limits[3] = {1, 5, 20};  // LOD error limits, in percent
  float limits_saved[3] = {0, 0, 0};
  std::string live_key;  // what the live pipeline frame was for (model version and values)
  float share = 2, share_saved = -1;  // the switch threshold, in percent  // the saved limits last frame, to notice the foundry changing them
  int view_tab = 0;  // for pipelines and config entries: 0 view, 1 flow, 2 video
  // The flow graph's canvas: zoom (the text scale), pan (the graph's offset in the canvas), and whether
  // to fit it on the next frame (once, when first shown or asked; never continuously, so it holds still).
  float flow_zoom = 1;
  ImVec2 flow_pan = {8, 8};
  bool flow_fit = true;
  // Reference photos for what the viewport shows (the config entries the live instance uses link them),
  // drawn in the viewport's corner when show_reference is on.
  bool show_reference = false;
  std::vector<std::string> references;  // workspace-relative paths
  std::string references_key;            // what references was found for
  int reference_pick = 0;
  std::string reference_loaded;  // the photo in the reference texture
  viewport::Image reference_pending;
  bool reference_has_pending = false;
  // The flow tab's probe, run in the background.
  struct Probe {
    std::mutex mutex;
    nlohmann::json result;
    bool running = false;
    std::string pipeline;
  };
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  // A fixed asset's fracture: the preview cut, and playback of the generated one.
  struct Fracture {
    std::string asset;            // what the settings and preview are for
    int algorithm = 0;            // index into assets::fracture::algorithms()
    int pieces = 16;
    int seed = 1;
    float explode = 0.35f;
    nlohmann::json recommended;
    bool show_preview = false;
    int version = 0;              // bumps when the preview changes
    // Background work: a preview cut or a generate.
    struct Work {
      std::mutex mutex;
      bool running = false, done = false;
      std::string error, label;
      std::shared_ptr<assets::fracture::Fracture> result;
    };
    std::shared_ptr<Work> work = std::make_shared<Work>();
    std::shared_ptr<assets::fracture::Fracture> preview;  // cut, not baked
    std::shared_ptr<assets::fracture::Fracture> loaded;   // the generated fracture of the selected fracture item
    std::string loaded_for;
    std::string generated_at;  // of the fracture the settings were last read from
    bool playing = true, tint = false;
    float time = 0;
    double origin = 0;
  } fracture;
  // The video tab's settings. Its export jobs live in perf, per pipeline.
  struct Video {
    int floats = 1;
    float seconds = 1.0f;
    int fps = 30;
    int size = 0;  // 0 720p, 1 1080p
    // Per pipeline: parameter -> "" (all) or one value (pipelines share parameter names, such as species).
    std::map<std::string, std::map<std::string, std::string>> pins;
  } video;
  // The plotter inspector's controls (design/plotter.md, "In the foundry").
  struct PlotUi {
    int preset = 0, radius = 12, max = 40, seed = 1;
    float clear_radius = 5, fell_fraction = 0.4f;
    bool verify = false, collision = false;
    std::string message;
    // A region to add.
    int shape = 1, q = 0, r = 0, rad = 2, priority = 3, ground = 1;
    bool available = true;
    float keep = 0, height = 0;
    // The newest scale report (design/quality.perf.md, "Scale"), reread every few seconds.
    nlohmann::json scale;
    std::string scale_path, scale_for;
    double scale_read_at = -100;
    char scale_sizes[64] = "1,2,4,8,12,16";
    bool scale_find = true;  // then grow the grid until the overview drops below 60 fps
  } plot_ui;
};

std::string render_key(const UiState &s, int w, int h) {
  char buf[512];
  std::snprintf(buf, sizeof buf, "%s|%u|%d|%d|%d|%.3f|%.3f|%.3f|%.3f|%d%d%d%d%d|%d|%dx%d", s.selected.c_str(), s.triangles,
                s.ws_version, static_cast<int>(s.camera.view), s.camera.quarter, s.camera.distance, s.camera.yaw, s.camera.pitch, s.camera.zoom, s.overlays.conformance,
                s.overlays.collision, s.overlays.hero, s.overlays.hex, s.overlays.grid, s.silhouette, w, h);
  return buf + s.live_key + (s.plot_ui.collision ? "|pc" : "");
}

// Every item name uses `_` as a path separator, so the tree has virtual folders:
// garden_gnome_concept -> garden / gnome / concept; prop_tiny -> prop / tiny.
struct TreeNode {
  std::map<std::string, TreeNode> children;
  const workspace::Item *item = nullptr;
};

void draw_node(Services &sv, UiState &st, const std::string &label, const TreeNode &node) {
  if (node.item && node.children.empty()) {
    const auto &i = *node.item;
    ImGui::PushID(i.id.c_str());
    ImGui::TextColored(state_color(i.state()), "%s", i.state() == "final" ? "*" : i.state() == "rejected" ? "x" : "o");
    ImGui::SameLine();
    if (ImGui::Selectable(label.c_str(), st.selected == i.id)) sv.ws->select(i.id);
    ImGui::PopID();
    return;
  }
  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNode(label.c_str())) {
    for (const auto &[name, child] : node.children) draw_node(sv, st, name, child);
    ImGui::TreePop();
  }
}

void draw_tree(Services &sv, UiState &st) {
  auto items = sv.ws->items();
  for (const auto &k : kKinds) {
    int count = 0;
    for (const auto &i : items) count += i.kind == k[0];
    char label[64];
    std::snprintf(label, sizeof label, "%s (%d)###%s", k[1], count, k[0]);
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::TreeNode(label)) continue;
    TreeNode root;
    for (const auto &i : items) {
      if (i.kind != k[0]) continue;
      TreeNode *n = &root;
      size_t start = 0;
      while (true) {
        size_t end = i.name.find('_', start);
        n = &n->children[i.name.substr(start, end == std::string::npos ? std::string::npos : end - start)];
        if (end == std::string::npos) break;
        start = end + 1;
      }
      n->item = &i;
    }
    for (const auto &[name, child] : root.children) draw_node(sv, st, name, child);
    ImGui::TreePop();
  }
  if (!sv.ws->preview().label.empty()) {
    ImGui::Separator();
    if (ImGui::Selectable(("Preview: " + sv.ws->preview().label).c_str(), st.selected == "preview"))
      sv.ws->select("preview");
  }
}

UiState *g_ui = nullptr;  // for LOD rows to set the viewport's triangle count

void draw_stats(const nlohmann::json &st) {
  if (!ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Size (w h d)  ").x);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
  auto row = [](const char *k, const std::string &v) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v.c_str());
  };
  char b[128];
  row("Triangles", std::to_string(st.value("triangles", 0)));
  row("Vertices", std::to_string(st.value("vertices", 0)) + " (" + std::to_string(st.value("welded_vertices", 0)) + " positions)");
  if (st.contains("size")) {
    std::snprintf(b, sizeof b, "%.3f x %.3f x %.3f m", st["size"][0].get<double>(), st["size"][1].get<double>(), st["size"][2].get<double>());
    row("Size (w h d)", b);
  }
  std::snprintf(b, sizeof b, "%.4f m2", st.value("surface_area_m2", 0.0));
  row("Surface area", b);
  std::snprintf(b, sizeof b, "%.5f m3", st.value("volume_m3", 0.0));
  row("Volume", b);
  std::snprintf(b, sizeof b, "%d open, %d non-manifold, %d degenerate", st.value("boundary_edges", 0),
                st.value("non_manifold_edges", 0), st.value("degenerate_triangles", 0));
  row("Topology", b);
  std::snprintf(b, sizeof b, "%.1f KB", st.value("gpu_bytes", 0) / 1024.0);
  row("GPU memory", b);
  ImGui::EndTable();
}

void draw_lods(const nlohmann::json &lods) {
  if (!lods.is_array() || lods.empty()) return;
  ImGui::Text("Levels of detail");
  if (ImGui::BeginTable("lods", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableSetupColumn("LOD", ImGuiTableColumnFlags_WidthStretch, 1.5f);  // View button + name
    for (const char *h : {"Triangles", "GPU KB", "Error", "Shape", "Color"})
      ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Switch at", ImGuiTableColumnFlags_WidthStretch, 2.6f);  // Zoom button + distance + pixels
    ImGui::TableHeadersRow();
    auto line = [](const char *label, const nlohmann::json &l, bool selectable) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      uint32_t tris = l.value("triangles", 0u);
      const int level = l.value("level", -1);
      if (g_ui && g_ui->selected_lod == level)
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
      ImGui::PushID(level);
      if (selectable && g_ui) {
        if (ImGui::SmallButton("View")) {  // show this LOD in the viewport
          g_ui->triangles = tris;
          g_ui->selected_lod = level;
        }
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(label);
      ImGui::TableNextColumn();
      ImGui::Text("%u", tris);
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", l.value("gpu_bytes", 0) / 1024.0);
      const nlohmann::json e = l.value("error", nlohmann::json::object());
      ImGui::TableNextColumn();
      double max = l.value("max_error", -1.0);
      if (e.contains("total")) {
        if (max > 0) ImGui::Text("%.2f%% <%.0f%%", e.value("total", 0.0) * 100, max * 100);
        else ImGui::Text("%.2f%%", e.value("total", 0.0) * 100);
      }
      ImGui::TableNextColumn();
      if (e.contains("silhouette")) ImGui::Text("%.2f%%", e.value("silhouette", 0.0) * 100);
      ImGui::TableNextColumn();
      if (e.contains("color")) ImGui::Text("%.2f%%", e.value("color", 0.0) * 100);
      ImGui::TableNextColumn();
      if (l.contains("switch")) {
        const auto &sw = l["switch"];
        double d = sw.value("distance_m", 0.0);
        // Where Zoom goes: the switch distance, or for LOD0 (used from 0) the
        // distance from which it matches the full mesh, else the closest sample.
        double zoom = d;
        if (zoom <= 0) zoom = sw.value("matches_full_from_m", 0.0);
        if (zoom <= 0 && sw.contains("curve") && !sw["curve"].empty()) zoom = sw["curve"][0].value("distance_m", 0.0);
        if (zoom > 0 && g_ui) {
          // Zoom: view this LOD as the game would from that distance.
          if (ImGui::SmallButton("Zoom")) {
            g_ui->camera.view = viewport::View::Orbit;
            g_ui->camera.distance = static_cast<float>(zoom);
            g_ui->selected_lod = level;
            g_ui->triangles = tris;
          }
          ImGui::SameLine();
        }
        if (sw.value("never_reached", false)) ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, ">%.0f m", d);
        else if (d <= 0 && sw.value("matches_full_from_m", 0.0) > 0)
          ImGui::Text("0 m (as full from %.1f m)", sw.value("matches_full_from_m", 0.0));
        else if (d <= 0) ImGui::Text("0 m");
        else ImGui::Text("%.1f m (%.0f px)", d, sw.value("pixels", 0.0));
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The first distance, going outward (0.9 rad FOV, 1080 lines), at which under %.0f%% of this\n"
                            "LOD's pixels visibly disagree with the full mesh, and the object's height on screen there.\n"
                            "LOD0 starts at 0; its Zoom goes to where it matches the full mesh. Zoom views this LOD from\n"
                            "that distance with the game camera. The game multiplies switch\n"
                            "distances by its quality factor.", sw.value("disagree_share", 0.1) * 100);
      }
      ImGui::PopID();
    };
    for (const auto &l : lods) {
      char label[16];
      std::snprintf(label, sizeof label, "LOD%d", l.value("level", 0));
      line(label, l, true);
    }
    ImGui::EndTable();
  }
}

// A log-log graph of FPS against instance count, one series per LOD, with
// the 60 FPS floor marked.
void draw_perf(const nlohmann::json &perf) {
  const auto &runs = perf["runs"];
  double max_inst = 1, min_fps = 1e9, max_fps = 1;
  for (const auto &l : runs)
    for (const auto &smp : l["samples"]) {
      max_inst = std::max(max_inst, smp.value("instances", 1.0));
      min_fps = std::min(min_fps, smp.value("fps", 1.0));
      max_fps = std::max(max_fps, smp.value("fps", 1.0));
    }
  const double floor_fps = perf.value("fps_floor", 60.0);
  min_fps = std::min(min_fps, floor_fps * 0.5);
  // Start the instance axis where the first LOD drops to 1k FPS (interpolated
  // in log-log); below that the GPU is idle and the curves are flat.
  const double kStartFps = 1000;
  double start = max_inst;
  for (const auto &l : runs) {
    double pi = 0, pf = 0;
    for (const auto &smp : l["samples"]) {
      double in = smp.value("instances", 1.0), f = smp.value("fps", 1.0);
      if (f <= kStartFps) {
        double at = in;
        if (pi > 0 && pf > kStartFps)
          at = std::exp(std::log(pi) + (std::log(pf) - std::log(kStartFps)) / (std::log(pf) - std::log(f)) *
                                           (std::log(in) - std::log(pi)));
        start = std::min(start, at);
        break;
      }
      pi = in;
      pf = f;
    }
  }
  if (start >= max_inst) start = 1;
  max_fps = std::min(max_fps, kStartFps);
  const double x0 = std::log10(start), x1 = std::log10(max_inst * 1.5), y0 = std::log10(min_fps * 0.8),
               y1 = std::log10(max_fps * 1.5);
  ImGui::TextDisabled("FPS (log)");
  ImVec2 size(ImGui::GetContentRegionAvail().x, 240);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float left = 46, bottom = 22;
  ImVec2 a(p0.x + left, p0.y + 4), b(p0.x + size.x - 6, p0.y + size.y - bottom);
  auto px = [&](double inst, double fps) {
    float x = a.x + static_cast<float>((std::log10(std::max(inst, 1.0)) - x0) / (x1 - x0)) * (b.x - a.x);
    float y = b.y - static_cast<float>((std::log10(std::max(fps, 1e-3)) - y0) / (y1 - y0)) * (b.y - a.y);
    return ImVec2(x, y);
  };
  dl->AddRectFilled(a, b, IM_COL32(30, 32, 38, 255));
  for (double d = 1; d <= max_inst * 1.5; d *= 10) {
    if (d < start) continue;
    ImVec2 q = px(d, 1);
    dl->AddLine(ImVec2(q.x, a.y), ImVec2(q.x, b.y), IM_COL32(70, 72, 80, 255));
    char t[16];
    std::snprintf(t, sizeof t, d >= 1e6 ? "%.0fM" : d >= 1e3 ? "%.0fk" : "%.0f", d >= 1e6 ? d / 1e6 : d >= 1e3 ? d / 1e3 : d);
    dl->AddText(ImVec2(q.x - 6, b.y + 3), IM_COL32(150, 150, 160, 255), t);
  }
  for (double f = 1; f <= max_fps * 1.5; f *= 10) {
    if (f < min_fps * 0.8) continue;
    ImVec2 q = px(start, f);
    dl->AddLine(ImVec2(a.x, q.y), ImVec2(b.x, q.y), IM_COL32(70, 72, 80, 255));
    char t[16];
    std::snprintf(t, sizeof t, f >= 1e3 ? "%.0fk" : "%.0f", f >= 1e3 ? f / 1e3 : f);
    dl->AddText(ImVec2(p0.x + 4, q.y - 7), IM_COL32(150, 150, 160, 255), t);
  }
  ImVec2 fl = px(start, floor_fps);
  dl->AddLine(ImVec2(a.x, fl.y), ImVec2(b.x, fl.y), IM_COL32(230, 90, 80, 255), 1.5f);
  dl->AddText(ImVec2(b.x - 52, fl.y - 15), IM_COL32(230, 90, 80, 255), "60 FPS");
  const ImU32 colors[] = {IM_COL32(110, 200, 255, 255), IM_COL32(120, 230, 140, 255), IM_COL32(250, 200, 90, 255),
                          IM_COL32(230, 130, 230, 255)};
  dl->PushClipRect(a, b, true);  // the curves start off the left edge
  int k = 0;
  for (const auto &l : runs) {
    ImU32 c = colors[k++ % 4];
    ImVec2 prev;
    bool first = true;
    for (const auto &smp : l["samples"]) {
      ImVec2 q = px(smp.value("instances", 1.0), smp.value("fps", 1.0));
      if (!first) dl->AddLine(prev, q, c, 2);
      dl->AddCircleFilled(q, 2.5f, c);
      prev = q;
      first = false;
    }
    if (l.value("budget_instances", 0) > 0) dl->AddCircle(px(l.value("budget_instances", 1.0), floor_fps), 5, c, 0, 2);
  }
  dl->PopClipRect();
  ImGui::Dummy(size);
  float title_w = ImGui::CalcTextSize("instances (log)").x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + size.x - title_w - 6);
  ImGui::TextDisabled("instances (log)");
  for (size_t i = 0; i < runs.size(); ++i) {
    const auto &l = runs[i];
    ImGui::ColorButton(("##c" + std::to_string(i)).c_str(), ImGui::ColorConvertU32ToFloat4(colors[i % 4]),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
    ImGui::SameLine();
    ImGui::TextWrapped("%s (%d tris, %d indices): %d%s instances at 60 FPS", l.value("label", "").c_str(),
                       l.value("triangles", 0), l.value("index_count", 0), l.value("budget_instances", 0),
                       l.value("budget_is_lower_bound", false) ? "+" : "");
  }
  ImGui::TextDisabled("%s, %s, %dx%d off screen", perf.value("device", "").c_str(), perf.value("driver", "").c_str(),
                      perf["resolution"][0].get<int>(), perf["resolution"][1].get<int>());
}

void draw_fit(const nlohmann::json &d) {
  if (!d.contains("fit") || !d["fit"].contains("report")) {
    ImGui::TextDisabled("Not sized yet: fit it to a conformance shape (mesh_transform).");
    return;
  }
  const auto &f = d["fit"];
  if (f.contains("conformance"))
    ImGui::Text("Conformance: %s (capsule r %.2f m, h %.2f m)", f["conformance"].value("name", "").c_str(),
                f["conformance"].value("radius", 0.0f), f["conformance"].value("height", 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, f.value("fits", false) ? ImVec4(0.45f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
  ImGui::TextWrapped("%s", f.value("report", "").c_str());
  ImGui::PopStyleColor();
}

// Sliders for each LOD's error limit, with a button that regenerates and
// re-benchmarks just that LOD.
void draw_lod_limits(Services &sv, UiState &st, const workspace::Item &item) {
  if (!item.data.contains("recommended") || item.state() == "final") return;
  std::vector<double> stored = assets::item_thresholds(item.data);
  if (stored.size() < 3) return;
  // Follow the saved limits: all of them when the item changes, and any one
  // the foundry changed (a regeneration, or its cascade) since last frame.
  for (int i = 0; i < 3; ++i) {
    const float saved = static_cast<float>(stored[i] * 100);
    if (st.limits_item != item.id || std::fabs(saved - st.limits_saved[i]) > 1e-4f) st.limits[i] = saved;
    st.limits_saved[i] = saved;
  }
  st.limits_item = item.id;
  ImGui::Text("LOD error limits");
  const float gap = static_cast<float>(assets::kLimitGap * 100);
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(i);
    // Each slider has its own fixed range; moving it pushes the others so the
    // limits always stack (the same cascade regeneration applies).
    float lo = static_cast<float>(assets::kLimitMin * 100) + gap * i;
    float hi = static_cast<float>(assets::kLimitMax * 100) - gap * (2 - i);
    char label[16];
    std::snprintf(label, sizeof label, "LOD%d", i);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::SliderFloat(label, &st.limits[i], lo, hi, "%.1f%%", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
      std::vector<double> t = {st.limits[0] / 100.0, st.limits[1] / 100.0, st.limits[2] / 100.0};
      assets::cascade_limits(t, static_cast<size_t>(i), t[i]);
      for (int k = 0; k < 3; ++k) st.limits[k] = static_cast<float>(t[k] * 100);
    }
    ImGui::SameLine();
    const bool changed = std::fabs(st.limits[i] - stored[i] * 100) > 0.05;
    if (!changed) ImGui::BeginDisabled();
    if (ImGui::Button("Regenerate")) {
      st.selected_lod = i;  // view this LOD now; the view follows it when the regeneration lands
      const auto &lods = item.data["recommended"].value("lods", nlohmann::json::array());
      if (i < static_cast<int>(lods.size())) st.triangles = lods[i].value("triangles", st.triangles);
      std::string root = sv.paths.workspace, name = item.name;
      double limit = st.limits[i] / 100.0;
      size_t lod = static_cast<size_t>(i);
      std::thread([root, name, lod, limit] {
        nlohmann::json j;
        std::string err;
        if (!assets::mesh_regenerate_lod(root, name, lod, limit, j, err)) core::log().add("mesh", "regenerate failed: " + err);
      }).detach();
    }
    if (!changed) ImGui::EndDisabled();
    ImGui::PopID();
  }

  // The switch threshold: the share of pixels allowed to disagree at a switch distance.
  const float saved_share = static_cast<float>(assets::item_switch_share(item.data) * 100);
  if (std::fabs(saved_share - st.share_saved) > 1e-4f) st.share = saved_share;
  st.share_saved = saved_share;
  ImGui::Text("Switch threshold (pixels that may disagree)");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  ImGui::SliderFloat("##share", &st.share, static_cast<float>(assets::kSwitchShareMin * 100),
                     static_cast<float>(assets::kSwitchShareMax * 100), "%.1f%%",
                     ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(ImGui::CalcTextSize("LOD0").x, 0));
  ImGui::SameLine();
  const bool share_changed = std::fabs(st.share - saved_share) > 0.05f;
  if (!share_changed) ImGui::BeginDisabled();
  if (ImGui::Button("Recompute")) {
    std::string root = sv.paths.workspace, name = item.name;
    double share = st.share / 100.0;
    std::thread([root, name, share] {
      nlohmann::json j;
      std::string err;
      if (!assets::mesh_set_switch_share(root, name, share, j, err)) core::log().add("mesh", "switch distances failed: " + err);
    }).detach();
  }
  if (!share_changed) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Recompute every LOD's switch distance with this threshold. The LODs don't change.");
}

bool is_live(const std::string &id) { return id.rfind("pipeline:", 0) == 0 || id.rfind("config:", 0) == 0; }
bool is_plot(const std::string &id) { return id.rfind("plotter:", 0) == 0; }

double now_seconds() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

// What the viewport shows for fractures: a fixed asset's preview cut, or a fracture item's break at
// this moment. Returns false when neither applies.
bool update_fracture_view(Services &sv, UiState &st) {
  auto &fr = st.fracture;
  std::shared_ptr<const runtime::Mesh> mesh;
  std::string key;
  float frame = 0;
  const bool previewing = fr.show_preview && fr.preview &&
                          (st.selected == "fixed:" + fr.asset || st.selected == "fracture:" + fr.asset);
  if (previewing) {
    key = "|preview" + std::to_string(fr.version);
    if (key != st.live_key) mesh = std::make_shared<runtime::Mesh>(assets::fracture::exploded(*fr.preview, fr.explode, true));
  } else if (st.selected.rfind("fracture:", 0) == 0) {
    const std::string name = st.selected.substr(9);
    if (fr.loaded_for != name) {
      auto f = std::make_shared<assets::fracture::Fracture>();
      fr.loaded = assets::fracture_load(sv.paths.workspace, name, *f) ? f : nullptr;
      fr.loaded_for = name;
      fr.origin = now_seconds();
      fr.playing = true;
    }
    if (!fr.loaded) return false;
    const float end = fr.loaded->seconds();
    if (fr.playing) {
      fr.time = static_cast<float>(now_seconds() - fr.origin);
      if (fr.time > end + 1.0f) {  // hold the settled pieces a moment, then break again
        fr.origin = now_seconds();
        fr.time = 0;
      }
    }
    const float t = std::min(fr.time, end);
    char buf[96];
    std::snprintf(buf, sizeof buf, "|fracture%s%d%d", name.c_str(), fr.tint, static_cast<int>(t * fr.loaded->fps));
    key = buf;
    if (key != st.live_key)
      mesh = std::make_shared<runtime::Mesh>(fr.tint ? assets::fracture::exploded(*fr.loaded, 0.35f, true)
                                                     : assets::fracture::pose(*fr.loaded, t));
    frame = std::max(st.content.height, 0.4f) * 1.6f;
  } else {
    return false;
  }
  if (key != st.live_key) {
    st.live_key = key;
    st.content.mesh = mesh;
    st.content.progressive = nullptr;
    st.content.lods.clear();
    st.camera.frame = frame;
  }
  return true;
}

// The live pipeline instance's parameters, clock, break, and last build.
void draw_live(Services &sv) {
  pipeline::Live &live = *sv.live;
  const pipeline::Definition def = live.definition();
  const nlohmann::json values = live.values();
  nlohmann::json patch = nlohmann::json::object();
  ImGui::Text("Pipeline %s", def.name.c_str());
  for (const auto &p : def.params) {
    ImGui::PushID(p.name.c_str());
    const nlohmann::json &v = values[p.name];
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (p.type == "enum" || p.type == "config") {
      std::string cur = v.is_string() ? v.get<std::string>() : "";
      if (ImGui::BeginCombo(p.name.c_str(), cur.c_str())) {
        for (const auto &choice : p.values)
          if (ImGui::Selectable(choice.c_str(), choice == cur)) patch[p.name] = choice;
        ImGui::EndCombo();
      }
    } else if (p.type == "bool") {
      // A gated parameter that doesn't apply here (fruit on a species without any) is shown off and disabled.
      const bool applies = def.applies(p, values);
      bool x = applies && v.is_boolean() && v.get<bool>();
      ImGui::BeginDisabled(!applies);
      if (ImGui::Checkbox(p.name.c_str(), &x)) patch[p.name] = x;
      ImGui::EndDisabled();
      if (!applies) {
        ImGui::SameLine();
        const std::string gate = values.value(p.when_param, std::string());
        ImGui::TextDisabled("disabled: %s is off for %s", p.when_field.c_str(), gate.c_str());
      }
    } else if (p.type == "u32") {
      int x = v.get<int>();
      if (ImGui::SliderInt(p.name.c_str(), &x, static_cast<int>(p.min), static_cast<int>(p.max))) patch[p.name] = x;
    } else if (p.type == "f32") {
      float x = v.get<float>();
      std::string fmt = "%.2f" + (p.unit.empty() ? "" : " " + p.unit);
      if (ImGui::SliderFloat(p.name.c_str(), &x, static_cast<float>(p.min), static_cast<float>(p.max), fmt.c_str()))
        patch[p.name] = x;
    } else if (p.type == "time" || p.type == "event") {
      const bool running = live.running(p.name);
      const double x = v.get<double>();
      if (p.type == "time") {
        if (ImGui::Button(running ? "Pause" : "Play")) {
          if (running) live.set({{p.name, x}});
          else live.start(p.name, now_seconds(), x);
        }
        ImGui::SameLine();
        ImGui::Text("time %.1f s", x);
      } else if (x < 0) {
        if (ImGui::Button(("Start " + p.name).c_str())) live.start(p.name, now_seconds());
        if (!p.doc.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%s)", p.doc.c_str());
        }
      } else {
        if (ImGui::Button("Reset")) live.set({{p.name, -1.0}});
        ImGui::SameLine();
        ImGui::Text("%s %.1f s", p.name.c_str(), x);
      }
    }
    if (!p.doc.empty() && p.type != "time" && p.type != "event" && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.doc.c_str());
    ImGui::PopID();
  }
  if (!patch.empty()) live.set(patch);
  if (live.building()) ImGui::TextColored({0.95f, 0.8f, 0.4f, 1}, "building...");
  if (!live.error().empty()) ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", live.error().c_str());
  const auto stages = live.build_stages();
  if (!stages.empty() && ImGui::BeginTable("stages", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Stage");
    ImGui::TableSetupColumn("System");
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Output");
    ImGui::TableSetupColumn("Native");
    ImGui::TableHeadersRow();
    for (const auto &s : stages) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s.id.c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton(s.system.c_str())) sv.ws->select("system:" + s.system);
      ImGui::TableNextColumn();
      if (s.cached) ImGui::TextDisabled("cached");
      else ImGui::Text("%.1f ms", s.milliseconds);
      ImGui::TableNextColumn();
      ImGui::Text("%zu KB", s.output_bytes / 1024);
      ImGui::TableNextColumn();
      if (s.native_checked) ImGui::TextColored(s.native_match ? ImVec4(0.45f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1),
                                               s.native_match ? "matches" : "DIFFERS");
      else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("Rebuild and verify native")) {
    std::thread([&live] { live.build_now(true); }).detach();
  }
}

// One field of a config entry, edited with a widget chosen from its schema type.
bool draw_config_field(const nlohmann::json &f, nlohmann::json &value, bool color) {
  const std::string name = f.value("name", ""), type = f.value("type", "");
  nlohmann::json &x = value[name];
  bool changed = false;
  ImGui::PushID(name.c_str());
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  const float lo = f.value("min", 0.0f), hi = f.value("max", 1.0f);
  const bool ranged = f.contains("min") && f.contains("max");
  if (f.contains("enum")) {
    const std::string cur = x.is_string() ? x.get<std::string>() : "";
    if (ImGui::BeginCombo(name.c_str(), cur.c_str())) {
      for (const auto &e : f["enum"])
        if (ImGui::Selectable(e.get<std::string>().c_str(), e == x)) {
          x = e;
          changed = true;
        }
      ImGui::EndCombo();
    }
  } else if (type == "f32") {
    float v = x.get<float>();
    if (ranged ? ImGui::SliderFloat(name.c_str(), &v, lo, hi) : ImGui::DragFloat(name.c_str(), &v, 0.01f)) x = v;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
  } else if (type == "u32") {
    int v = x.get<int>();
    if (ranged ? ImGui::SliderInt(name.c_str(), &v, static_cast<int>(lo), static_cast<int>(hi))
               : ImGui::DragInt(name.c_str(), &v, 0.2f, 0, INT32_MAX))
      x = v;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
  } else if (type == "bool") {
    bool v = x.get<bool>();
    if (ImGui::Checkbox(name.c_str(), &v)) {
      x = v;
      changed = true;
    }
  } else if (type == "string") {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", x.is_string() ? x.get<std::string>().c_str() : "");
    if (ImGui::InputText(name.c_str(), buf, sizeof buf)) x = std::string(buf);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
  } else if (type == "vec3") {
    float c[3] = {x[0].get<float>(), x[1].get<float>(), x[2].get<float>()};
    if (color ? ImGui::ColorEdit3(name.c_str(), c) : ImGui::DragFloat3(name.c_str(), c, 0.01f)) x = {c[0], c[1], c[2]};
    changed |= ImGui::IsItemDeactivatedAfterEdit();
  } else if (type == "f32[]" && x.is_array() && !x.empty()) {
    std::vector<float> a = x.get<std::vector<float>>();
    const int n = static_cast<int>(a.size());
    if (ranged ? ImGui::SliderScalarN(name.c_str(), ImGuiDataType_Float, a.data(), n, &lo, &hi)
               : ImGui::DragScalarN(name.c_str(), ImGuiDataType_Float, a.data(), n, 0.01f))
      x = a;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
  } else {
    ImGui::Text("%s: %s", name.c_str(), x.dump().c_str());
  }
  if (f.contains("doc") && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f["doc"].get<std::string>().c_str());
  ImGui::PopID();
  return changed;
}

// A config entry's fields, edited in place. The editor is generic: the table's channel schema gives each
// field's type and range, and its table.json may group the fields and name which vec3s are colours.
void draw_config(Services &sv, const workspace::Item &item) {
  std::string table_name, entry;
  if (!pipeline::config::split(item.path, table_name, entry)) return;
  nlohmann::json file = core::read_json(item.path);
  const nlohmann::json table = pipeline::config::table(sv.paths, table_name);
  if (!file.is_object() || !file.contains("value") || table.is_null()) return;
  const nlohmann::json fields = pipeline::config::fields(table);
  if (fields.empty()) {
    ImGui::TextDisabled("unknown channel %s", table.value("channel", "").c_str());
    return;
  }
  ImGui::TextDisabled("config/%s (%s), entry %s", table_name.c_str(), table.value("channel", "").c_str(), entry.c_str());
  const nlohmann::json editor = table.value("editor", nlohmann::json::object());
  std::set<std::string> colors;
  for (const auto &c : editor.value("colors", nlohmann::json::array())) colors.insert(c.get<std::string>());
  // Groups from table.json; fields it doesn't name go under "Other".
  std::vector<std::pair<std::string, std::vector<std::string>>> groups;
  std::set<std::string> placed;
  for (const auto &g : editor.value("groups", nlohmann::json::array())) {
    groups.push_back({g.value("label", ""), g.value("fields", std::vector<std::string>())});
    for (const auto &f : groups.back().second) placed.insert(f);
  }
  std::vector<std::string> rest;
  for (const auto &f : fields)
    if (!placed.count(f["name"].get<std::string>())) rest.push_back(f["name"]);
  if (!rest.empty()) groups.push_back({groups.empty() ? "" : "Other", rest});

  nlohmann::json &value = file["value"];
  const bool editable = item.state() != "final";
  bool changed = false;
  if (!editable) ImGui::BeginDisabled();
  if (ImGui::BeginChild("config", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 16), ImGuiChildFlags_Borders)) {
    for (const auto &[label, names] : groups) {
      if (!label.empty() && !ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
      for (const auto &n : names)
        for (const auto &f : fields)
          if (f["name"] == n && value.contains(n)) changed |= draw_config_field(f, value, colors.count(n) > 0);
    }
  }
  ImGui::EndChild();
  if (!editable) ImGui::EndDisabled();
  // Live editing: keeping the file current while dragging is too slow, so it saves on release.
  if (changed && core::write_json(item.path, file)) {
    core::log().add("config", table_name + "/" + entry + " edited");
    sv.live->reload();
  }
}

// Fracture (design/transforms.md, "Fracture"): on a fixed asset, pick an algorithm and preview its
// cut on LOD1 (pieces pushed apart and tinted), then generate the fracture item.
void draw_fracture_controls(Services &sv, UiState &st, const workspace::Item &item) {
  auto &fr = st.fracture;
  const auto &algos = assets::fracture::algorithms();
  const std::string root = sv.paths.workspace;
  if (fr.asset != item.name) {
    const std::string generated_at = fr.generated_at;
    fr = {};
    fr.asset = item.name;
    fr.generated_at = generated_at;
    runtime::Mesh lod1;
    nlohmann::json manifest;
    std::string error;
    if (assets::fracture_baseline(root, item.name, lod1, manifest, error)) {
      fr.recommended = assets::fracture::recommend(lod1, manifest.value("provenance", nlohmann::json::object()).value("prompt", ""));
      // Start from the generated fracture's settings if there is one, else the recommendation.
      const nlohmann::json saved = core::read_json(root + "/fixed/" + item.name + "/fracture.json");
      const nlohmann::json from = saved.is_object() && saved.contains("settings") ? saved["settings"] : fr.recommended;
      for (size_t k = 0; k < algos.size(); ++k)
        if (from.value("algorithm", "") == algos[k].name) fr.algorithm = static_cast<int>(k);
      fr.pieces = from.value("pieces", 16);
      fr.seed = from.value("seed", 1);
    }
  }
  ImGui::Text("Fracture");
  ImGui::TextDisabled("The break, cut from LOD1 and baked. Preview each algorithm, then generate.");
  if (!fr.recommended.is_null())
    ImGui::TextWrapped("Recommended: %s (%s)", fr.recommended.value("algorithm", "").c_str(),
                       fr.recommended.value("reason", "").c_str());
  bool changed = false;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  if (ImGui::BeginCombo("algorithm", algos[static_cast<size_t>(fr.algorithm)].name)) {
    for (size_t k = 0; k < algos.size(); ++k) {
      if (ImGui::Selectable(algos[k].name, static_cast<int>(k) == fr.algorithm)) {
        fr.algorithm = static_cast<int>(k);
        changed = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", algos[k].doc);
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::SliderInt("pieces", &fr.pieces, 2, 64);
  changed |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  if (ImGui::InputInt("seed", &fr.seed)) changed = true;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  if (ImGui::SliderFloat("explode", &fr.explode, 0, 1)) ++fr.version;
  assets::fracture::Settings settings;
  settings.algorithm = algos[static_cast<size_t>(fr.algorithm)].name;
  settings.pieces = fr.pieces;
  settings.seed = static_cast<uint32_t>(std::max(fr.seed, 1));
  auto work = fr.work;
  bool running;
  {
    std::lock_guard<std::mutex> lock(work->mutex);
    running = work->running;
    if (work->done) {
      work->done = false;
      if (!work->error.empty()) st.message = work->error;
      else if (work->label == "preview") {
        fr.preview = work->result;
        fr.show_preview = true;
        ++fr.version;
      } else {
        fr.show_preview = false;
        sv.ws->scan();
        sv.ws->select("fracture:" + fr.asset);
        st.ws_version = -1;  // reload the viewport even if the fracture item was already selected
      }
    }
  }
  auto start = [&](const std::string &label) {
    {
      std::lock_guard<std::mutex> lock(work->mutex);
      work->running = true;
      work->label = label;
    }
    std::thread([work, label, root, asset = fr.asset, settings] {
      auto f = std::make_shared<assets::fracture::Fracture>();
      std::string error;
      bool ok;
      if (label == "preview") {
        runtime::Mesh lod1;
        nlohmann::json manifest;
        ok = assets::fracture_baseline(root, asset, lod1, manifest, error) && assets::fracture::cut(lod1, settings, *f, error);
      } else {
        ok = assets::fracture_generate(root, asset, settings, *f, error);
      }
      std::lock_guard<std::mutex> lock(work->mutex);
      work->running = false;
      work->done = true;
      work->error = ok ? "" : error;
      work->result = ok ? f : nullptr;
    }).detach();
  };
  if (running) ImGui::BeginDisabled();
  if (ImGui::Button("Preview cut") || (changed && fr.show_preview && !running)) start("preview");
  ImGui::SameLine();
  if (ImGui::Button(st.selected.rfind("fracture:", 0) == 0 ? "Regenerate" : "Generate fracture")) start("generate");
  if (running) ImGui::EndDisabled();
  if (fr.show_preview) {
    ImGui::SameLine();
    if (ImGui::Button("Hide preview")) {
      fr.show_preview = false;
      ++fr.version;
    }
  }
  if (running) ImGui::TextDisabled("Working...");
  if (fr.preview && fr.show_preview) {
    const auto &x = fr.preview->stats;
    ImGui::Text("%d pieces, %d triangles (%d cap), largest %.0f%%, smallest %.2f%%", x.value("pieces", 0),
                x.value("triangles", 0), x.value("cap_triangles", 0), x.value("largest_share", 0.0) * 100,
                x.value("smallest_share", 0.0) * 100);
  }
}

// A generated fracture: its stats, and its break played in the viewport.
void draw_fracture_item(Services &sv, UiState &st, const workspace::Item &item) {
  // Regenerated (here or through MCP): read its settings into the controls again.
  if (st.fracture.generated_at != item.data.value("generated_at", "")) {
    st.fracture.asset.clear();
    st.fracture.generated_at = item.data.value("generated_at", "");
  }
  auto &fr = st.fracture;
  const auto &d = item.data;
  const auto set = d.value("settings", nlohmann::json::object());
  const auto x = d.value("stats", nlohmann::json::object());
  ImGui::Text("%s, %d pieces from LOD1 (%d triangles)", set.value("algorithm", "").c_str(), x.value("pieces", 0),
              x.value("source_triangles", 0));
  ImGui::Text("%d triangles (%d on cut faces), largest piece %.0f%%", x.value("triangles", 0), x.value("cap_triangles", 0),
              x.value("largest_share", 0.0) * 100);
  if (x.contains("settled_s") && x["settled_s"].is_number())
    ImGui::Text("Settles at %.2f s; the farthest piece lands %.2f m away", x["settled_s"].get<double>(), x.value("farthest_m", 0.0));
  else
    ImGui::TextColored({0.95f, 0.8f, 0.4f, 1}, "Still moving when the bake ends (%.1f s)", x.value("seconds", 0.0));
  if (x.value("open_sections", 0) > 0)
    ImGui::TextColored({0.95f, 0.8f, 0.4f, 1}, "%d cut faces crossed gaps in the mesh and were closed straight", x.value("open_sections", 0));
  ImGui::Spacing();
  if (!fr.loaded || fr.loaded_for != item.name) {
    ImGui::TextDisabled("No fracture.bin");
    return;
  }
  if (ImGui::Button(fr.playing ? "Pause" : "Play")) {
    fr.playing = !fr.playing;
    fr.origin = now_seconds() - fr.time;
  }
  ImGui::SameLine();
  if (ImGui::Button("Break again")) {
    fr.playing = true;
    fr.origin = now_seconds();
  }
  ImGui::SameLine();
  ImGui::Checkbox("pieces tinted, pushed apart", &fr.tint);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  if (ImGui::SliderFloat("time", &fr.time, 0, fr.loaded->seconds(), "%.2f s")) fr.playing = false;
  ImGui::Separator();
  draw_fracture_controls(sv, st, item);
}

// The reference photos linked by the config entries the live instance uses (a config entry's
// "reference" list), found again only when the selection, the workspace, or a config choice changes.
void update_references(Services &sv, UiState &st) {
  std::vector<std::string> refs;
  if (is_live(st.selected)) {
    const pipeline::Definition def = sv.live->definition();
    const nlohmann::json values = sv.live->values();
    std::string key = st.selected + "|" + std::to_string(st.ws_version);
    std::vector<std::pair<std::string, std::string>> picks;  // (table, entry)
    for (const auto &p : def.params)
      if (!p.table.empty() && values.contains(p.name) && values[p.name].is_string()) {
        picks.push_back({p.table, values[p.name].get<std::string>()});
        key += "|" + p.table + "/" + picks.back().second;
      }
    if (key == st.references_key) return;
    st.references_key = key;
    for (const auto &[table, entry] : picks) {
      const nlohmann::json file = pipeline::config::entry(sv.paths, table, entry);
      if (file.is_object())
        for (const auto &r : file.value("reference", nlohmann::json::array()))
          if (r.is_string()) refs.push_back(r);
    }
  } else {
    if (st.references_key.empty()) return;
    st.references_key.clear();
  }
  if (refs != st.references) {
    st.references = refs;
    st.reference_pick = 0;
  }
}

// The workspace-relative path of a file in the workspace.
std::string workspace_relative(const Services &sv, const std::string &path) {
  const std::string root = sv.paths.workspace + "/";
  return path.rfind(root, 0) == 0 ? path.substr(root.size()) : path;
}

// A reference photo: where it came from, its license, and the config entries that link it.
void draw_reference(Services &sv, UiState &st, const workspace::Item &item) {
  const auto &d = item.data;
  const nlohmann::json prov = d.value("provenance", nlohmann::json::object());
  ImGui::TextWrapped("%s", d.value("title", "").c_str());
  if (!d.value("description", "").empty()) ImGui::TextWrapped("%s", d.value("description", "").c_str());
  ImGui::Spacing();
  ImGui::Text("License: %s%s", prov.value("license", "?").c_str(),
              prov.value("attribution_required", true) ? "" : " (no attribution required)");
  if (!prov.value("author", "").empty()) ImGui::TextWrapped("Author: %s", prov.value("author", "").c_str());
  ImGui::TextWrapped("Source: %s, %s", prov.value("service", "").c_str(), prov.value("page", "").c_str());
  ImGui::TextDisabled("Retrieved %s; %s", prov.value("retrieved", "").c_str(), prov.value("size", "").c_str());
  ImGui::Spacing();
  const std::string rel = workspace_relative(sv, item.path);
  ImGui::Text("Linked from:");
  bool any = false;
  for (const auto &i : sv.ws->items()) {
    if (i.kind != "config") continue;
    for (const auto &r : i.data.value("reference", nlohmann::json::array()))
      if (r.is_string() && r.get<std::string>() == rel) {
        ImGui::SameLine();
        if (ImGui::SmallButton(i.name.c_str())) sv.ws->select(i.id);
        any = true;
      }
  }
  if (!any) {
    ImGui::SameLine();
    ImGui::TextDisabled("nothing yet");
  }
  (void)st;
}

// The reference photo in the viewport's top-right corner (over the image between a and b): click it
// for the next photo.
void draw_reference_corner(UiState &st, SDL_GPUTexture *texture, int tw, int th, ImVec2 a, ImVec2 b) {
  if (!st.show_reference || st.references.empty() || !texture || tw <= 0 || th <= 0) return;
  const float w = (b.x - a.x) * 0.34f, h = w * th / tw;
  const ImVec2 lo = {b.x - w - 10, a.y + 10}, hi = {b.x - 10, a.y + 10 + h};
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled({lo.x - 3, lo.y - 3}, {hi.x + 3, hi.y + 3 + ImGui::GetTextLineHeightWithSpacing()},
                    IM_COL32(20, 22, 28, 230), 4);
  dl->AddImage(reinterpret_cast<ImTextureID>(texture), lo, hi);
  dl->AddRect(lo, hi, IM_COL32(200, 150, 230, 255), 0, 0, 1.5f);
  const int n = static_cast<int>(st.references.size());
  const std::string &path = st.references[st.reference_pick % n];
  std::string label = path.substr(0, path.rfind('/'));
  label = label.substr(label.rfind('/') + 1) + "  " + std::to_string(st.reference_pick % n + 1) + "/" + std::to_string(n);
  if (n > 1) label += "  (click for next)";
  dl->AddText({lo.x + 2, hi.y + 2}, IM_COL32(220, 210, 235, 255), label.c_str());
  const ImVec2 m = ImGui::GetIO().MousePos;
  if (n > 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m.x >= lo.x && m.x <= hi.x && m.y >= lo.y && m.y <= hi.y)
    st.reference_pick = (st.reference_pick + 1) % n;
}

// Loads the picked reference photo when it changes (uploaded to the GPU by the frame loop).
void load_reference(Services &sv, UiState &st) {
  if (!st.show_reference || st.references.empty()) return;
  const std::string &path = st.references[st.reference_pick % st.references.size()];
  if (path == st.reference_loaded) return;
  st.reference_loaded = path;
  st.reference_has_pending = viewport::load_image(sv.paths.workspace + "/" + path, st.reference_pending);
}

// A plotter: its region's ground, seed, shared values, growth, clearing, and metrics (design/plotter.md).
// The plotter's performance in the native runtime: the newest scale report, summarized, and a button
// that runs the scale plan (plotter_perf does the same through MCP).
void draw_plotter_performance(Services &sv, UiState::PlotUi &u, const std::string &name) {
  if (!ImGui::CollapsingHeader("Performance (native runtime)", ImGuiTreeNodeFlags_DefaultOpen)) return;
  const int job = perf::latest_plotter_scale(name);
  const nlohmann::json status = job ? perf::job_status(job) : nlohmann::json();
  const bool running = job && !status.value("done", true);
  if (u.scale_for != name || ImGui::GetTime() - u.scale_read_at > (running ? 1.0 : 5.0)) {
    u.scale = perf::latest_scale_report(sv.paths, name, &u.scale_path);
    u.scale_for = name;
    u.scale_read_at = ImGui::GetTime();
  }
  ImGui::SetNextItemWidth(160);
  ImGui::InputText("grid sides", u.scale_sizes, sizeof u.scale_sizes);
  ImGui::SameLine();
  ImGui::Checkbox("then find 60 fps", &u.scale_find);
  ImGui::SameLine();
  ImGui::BeginDisabled(running);
  if (ImGui::Button(running ? "Running..." : "Run the scale plan"))
    perf::start_plotter_scale(sv.paths, name, u.scale_sizes, 240, true, u.scale_find ? 60.0 : 0.0);
  ImGui::EndDisabled();
  if (running) {
    const auto &out = status["output"];
    for (size_t i = out.size() > 3 ? out.size() - 3 : 0; i < out.size(); ++i) ImGui::TextDisabled("%s", out[i].get<std::string>().c_str());
  } else if (job && status.value("exit", 0) != 0) {
    ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "the last run failed: %s", status.value("error", std::string("?")).c_str());
  }
  if (u.scale.is_null()) {
    ImGui::TextDisabled("No scale report yet: run the scale plan.");
    return;
  }
  ImGui::TextDisabled("%s", u.scale_path.c_str());
  ImGui::TextDisabled("%s, %s", u.scale.value("started", std::string()).c_str(),
                      u.scale.value("machine", nlohmann::json::object()).value("gpu", std::string()).c_str());
  const nlohmann::json h = u.scale.value("headline", nlohmann::json());
  if (h.is_object())
    ImGui::Text("%d trees visible (%d things drawn, %dx%d regions): %.1f fps, p95 frame %.1f ms", h.value("trees_visible", 0),
                h.value("instances", 0), h.value("grid", 0), h.value("grid", 0), h.value("fps", 0.0), h.value("frame_ms_p95", 0.0));
  const nlohmann::json most = u.scale.value("most_trees", nlohmann::json());
  if (most.is_object()) {
    const nlohmann::json vista = most.value("vista", nlohmann::json::object());
    ImGui::Text("Most trees at %.0f fps (%dx%d regions): vista %d in view at %.1f fps; overview %d at %.1f fps",
                most.value("target_fps", 0.0), most.value("grid", 0), most.value("grid", 0), vista.value("trees_visible", 0),
                vista.value("fps", 0.0), most.value("trees_visible", 0), most.value("fps", 0.0));
  }
  auto row = [](const std::string &label, const nlohmann::json &c) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label.c_str());
    ImGui::TableNextColumn();
    ImGui::Text("%.0f", c.value("fps", 0.0));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", c["frame_ms"].value("p95", 0.0));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", c["update_ms"].value("mean", 0.0));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", c["prepare_ms"].value("mean", 0.0));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", c["gpu_frame_ms"].value("mean", 0.0));
    ImGui::TableNextColumn();
    ImGui::Text("%d", c.value("drawn", 0));
    ImGui::TableNextColumn();
    ImGui::Text("%d", c.value("trees_visible", 0));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f M", c.value("triangles", 0.0) / 1e6);
  };
  auto header = [](const char *id) {
    if (!ImGui::BeginTable(id, 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return false;
    int col = 0;
    for (const char *c : {"scene", "fps", "p95 ms", "update", "prepare", "GPU ms", "drawn", "trees", "triangles"})
      ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthStretch, col++ == 0 ? 4.0f : 1.0f);
    ImGui::TableHeadersRow();
    return true;
  };
  if (header("scale")) {
    for (const auto &s : u.scale.value("sizes", nlohmann::json::array())) {
      const int g = s.value("grid", 0);
      for (auto it = s["cameras"].begin(); it != s["cameras"].end(); ++it)
        row(std::to_string(g) + "x" + std::to_string(g) + " " + it.key(), it.value());
    }
    ImGui::EndTable();
  }
  const nlohmann::json ladder = u.scale.value("ladder", nlohmann::json::array());
  if (!ladder.empty()) {
    ImGui::TextDisabled("The largest listed scene, with the runtime's cost decisions turned on one at a time:");
    if (header("ladder")) {
      for (const auto &e : ladder) row(e.value("camera", std::string()) + " " + e.value("step", std::string()), e);
      ImGui::EndTable();
    }
    const nlohmann::json looks = u.scale.value("looks", nlohmann::json::object());
    if (!looks.empty() && ImGui::BeginTable("looks", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      for (const char *c : {"what shows that changed", "rate", "detail", "too small", "all"}) ImGui::TableSetupColumn(c);
      ImGui::TableHeadersRow();
      for (auto it = looks.begin(); it != looks.end(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(it.key().c_str());
        for (const char *k : {"rate", "detail", "too_small", "all"}) {
          ImGui::TableNextColumn();
          ImGui::Text("%.2f%%", it.value().value(k, nlohmann::json::object()).value("object_pct", 0.0));
        }
      }
      ImGui::EndTable();
    }
  }
  const nlohmann::json ex = u.scale.value("experiments", nlohmann::json::array());
  if (!ex.empty()) {
    ImGui::TextDisabled("The largest scene, from the overview, changed one way at a time:");
    if (header("experiments")) {
      for (const auto &e : ex) row(e.value("experiment", std::string()), e);
      ImGui::EndTable();
    }
  }
}

void draw_plotter(Services &sv, UiState &st, const workspace::Item &item) {
  auto &u = st.plot_ui;
  plot::View &view = *sv.plot;
  std::string error;
  if (!view.show(item.name, error)) {
    ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", error.c_str());
    return;
  }
  const plot::Plotter pl = view.plotter();
  ImGui::Text("Plot system: %s   distinct seeds per kind: %u", pl.system.c_str(), pl.distinct_seeds);
  if (ImGui::CollapsingHeader("Kinds", ImGuiTreeNodeFlags_None)) {
    if (ImGui::BeginTable("kinds", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("layer");
      ImGui::TableSetupColumn("kind");
      ImGui::TableSetupColumn("weight");
      ImGui::TableSetupColumn("footprint");
      ImGui::TableHeadersRow();
      for (const auto &k : pl.kinds) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted((k.layer + (k.regrow_only ? " (regrow)" : "")).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(k.label().c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", k.weight);
        ImGui::TableNextColumn();
        if (k.footprint_cells) ImGui::Text("%u cells (%.2f m)", k.footprint_cells, k.footprint_m);
        else ImGui::TextDisabled("none");
      }
      ImGui::EndTable();
    }
  }
  draw_plotter_performance(sv, u, item.name);
  ImGui::SeparatorText("Ground");
  const auto &presets = plot::View::presets();
  ImGui::SetNextItemWidth(140);
  if (ImGui::BeginCombo("##preset", presets[u.preset].c_str())) {
    for (int i = 0; i < static_cast<int>(presets.size()); ++i)
      if (ImGui::Selectable(presets[i].c_str(), i == u.preset)) u.preset = i;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  ImGui::SliderInt("radius (cells)", &u.radius, 1, 40);
  ImGui::SameLine();
  if (ImGui::Button("Use shape")) {
    nlohmann::json out;
    if (!view.set_regions(plot::View::preset(presets[u.preset], u.radius, view.seed()), out, error)) u.message = error;
    else u.message = "ground: " + out.dump();
  }
  ImGui::SetNextItemWidth(120);
  u.seed = static_cast<int>(view.seed());
  if (ImGui::InputInt("world seed", &u.seed)) view.set_seed(static_cast<uint32_t>(std::max(1, u.seed)));
  // The regions, highest priority last; each can be removed.
  nlohmann::json regions = view.regions();
  int remove = -1;
  for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
    const auto &r = regions[i];
    ImGui::PushID(i);
    if (ImGui::SmallButton("x")) remove = i;
    ImGui::SameLine();
    std::string text = r.value("shape", std::string("?")) + " (" + std::to_string(r.value("q", 0)) + ", " +
                       std::to_string(r.value("r", 0)) + ")";
    if (r.value("shape", std::string()) == "line")
      text += " to (" + std::to_string(r.value("q2", 0)) + ", " + std::to_string(r.value("r2", 0)) + ")";
    else text += " radius " + std::to_string(r.value("radius", 0));
    text += "  priority " + std::to_string(r.value("priority", 0));
    if (r.value("cut", false)) text += "  cut";
    if (r.contains("ground")) text += "  " + r["ground"].get<std::string>();
    if (r.contains("available") && !r["available"].get<bool>()) text += "  unavailable";
    if (r.value("keep_clear_m", 0.0f) > 0) text += "  keep clear " + std::to_string(r["keep_clear_m"].get<float>()).substr(0, 4) + " m";
    if (r.contains("height")) text += "  height " + std::to_string(r["height"].get<float>()).substr(0, 4) + " m";
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopID();
  }
  if (remove >= 0) {
    regions.erase(regions.begin() + remove);
    nlohmann::json out;
    if (!view.set_regions(regions, out, error)) u.message = error;
  }
  if (ImGui::TreeNode("Add a region")) {
    static const char *shapes[] = {"cells", "hex", "ring", "line"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("shape", &u.shape, shapes, 4);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("q", &u.q);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("r", &u.r);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(u.shape == 3 ? "to q (line)" : "radius", &u.rad, u.shape == 3 ? -40 : 0, 40);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("priority", &u.priority);
    ImGui::SetNextItemWidth(100);
    std::vector<const char *> gs;
    for (const auto &g : plot::grounds()) gs.push_back(g.c_str());
    ImGui::Combo("ground", &u.ground, gs.data(), static_cast<int>(gs.size()));
    ImGui::Checkbox("available", &u.available);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("keep clear (m)", &u.keep, 0, 6);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("height (m)", &u.height, -2, 4);
    if (ImGui::Button("Add")) {
      nlohmann::json r = {{"shape", shapes[u.shape]}, {"q", u.q}, {"r", u.r}, {"priority", u.priority},
                          {"ground", plot::grounds()[u.ground]}, {"available", u.available}};
      if (u.shape == 3) {
        r["q2"] = u.rad;
        r["r2"] = u.r;
      } else {
        r["radius"] = std::max(0, u.rad);
      }
      if (u.shape == 0) r["cells"] = {u.q, u.r};
      if (u.keep > 0) r["keep_clear_m"] = u.keep;
      if (u.height != 0) r["height"] = u.height;
      regions.push_back(r);
      nlohmann::json out;
      if (!view.set_regions(regions, out, error)) u.message = error;
      else u.message = "ground: " + out.dump();
    }
    ImGui::TreePop();
  }
  // Shared values: set on every occupant shown (the game sets them per instance).
  ImGui::SeparatorText("Shared");
  nlohmann::json shared = view.shared();
  for (auto it = shared.begin(); it != shared.end(); ++it) {
    ImGui::PushID(it.key().c_str());
    ImGui::SetNextItemWidth(160);
    if (it.value().is_string()) {
      std::vector<std::string> choices;
      for (const auto &k : pl.kinds) {
        pipeline::Definition d;
        std::string e;
        if (pipeline::load(sv.paths, k.pipeline, d, e))
          if (const pipeline::Param *p = d.param(it.key())) {
            choices = p->values;
            break;
          }
      }
      const std::string cur = it.value().get<std::string>();
      if (ImGui::BeginCombo(it.key().c_str(), cur.c_str())) {
        for (const auto &c : choices)
          if (ImGui::Selectable(c.c_str(), c == cur)) view.set_shared({{it.key(), c}});
        ImGui::EndCombo();
      }
    } else if (it.value().is_number()) {
      float v = it.value().get<float>();
      if (ImGui::SliderFloat(it.key().c_str(), &v, 0, it.key().find("heading") != std::string::npos ? 360.0f : 25.0f))
        view.set_shared({{it.key(), v}});
    }
    ImGui::PopID();
  }
  ImGui::SeparatorText("Grow and clear");
  ImGui::SetNextItemWidth(160);
  ImGui::SliderInt("max per grow", &u.max, 1, 500);
  ImGui::SameLine();
  ImGui::Checkbox("verify native", &u.verify);
  auto grow = [&](bool regrow, uint32_t max) {
    nlohmann::json out;
    if (!view.grow(regrow, max, nlohmann::json::array(), false, nullptr, u.verify, out, error)) u.message = error;
    else u.message = (regrow ? "regrow: " : "grow: ") + out.dump();
  };
  if (ImGui::Button("Grow")) grow(false, static_cast<uint32_t>(u.max));
  ImGui::SameLine();
  if (ImGui::Button("Grow to full")) grow(false, 100000);
  ImGui::SameLine();
  if (ImGui::Button("Regrow")) grow(true, static_cast<uint32_t>(u.max));
  ImGui::SameLine();
  if (ImGui::Button("Regrow to full")) grow(true, 100000);
  if (ImGui::Button("Clear all")) u.message = "clear: " + view.clear({{"all", true}}).dump();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100);
  ImGui::SliderFloat("##clear_radius", &u.clear_radius, 1, 20, "%.0f m");
  ImGui::SameLine();
  if (ImGui::Button("Clear circle at centre")) u.message = "clear: " + view.clear({{"center", {0, 0}}, {"radius", u.clear_radius}}).dump();
  ImGui::SetNextItemWidth(100);
  ImGui::SliderFloat("##fell", &u.fell_fraction, 0.05f, 1.0f, "%.0f%%");
  ImGui::SameLine();
  if (ImGui::Button("Fell that share of the canopy"))
    u.message = "fell: " + view.clear({{"layer", "canopy"}, {"fraction", u.fell_fraction}, {"fell", true},
                                       {"seed", static_cast<uint32_t>(SDL_GetTicks())}})
                               .dump();
  ImGui::SameLine();
  // Stumps are felled trees still standing on their footprints; clearing them frees the slots.
  if (ImGui::Button("Clear stumps")) u.message = "stumps: " + view.clear({{"stumps", true}}).dump();
  ImGui::Checkbox("Show collision and footprints", &st.plot_ui.collision);
  if (!u.message.empty()) ImGui::TextWrapped("%s", u.message.c_str());
  // Metrics.
  ImGui::SeparatorText("Metrics");
  const plot::Picture pic = view.picture();
  if (pic.building) ImGui::TextDisabled("composing the region...");
  if (!pic.error.empty()) ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", pic.error.c_str());
  const nlohmann::json m = view.metrics();
  ImGui::Text("%d cells (%.3f ha), %d occupants, %d distinct builds, %d footprint overlaps", m.value("cells", 0),
              m.value("hectares", 0.0), m.value("occupants", 0), m.value("distinct_builds", 0), m.value("footprint_overlaps", 0));
  if (ImGui::BeginTable("layers", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("layer");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("per hectare");
    ImGui::TableSetupColumn("spacing");
    ImGui::TableSetupColumn("nearest");
    ImGui::TableHeadersRow();
    for (auto it = m["layers"].begin(); it != m["layers"].end(); ++it) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(it.key().c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%d", it.value().value("count", 0));
      ImGui::TableNextColumn();
      ImGui::Text("%.0f", it.value().value("per_hectare", 0.0));
      ImGui::TableNextColumn();
      ImGui::Text("%.1f m", it.value().value("spacing_m", 0.0));
      ImGui::TableNextColumn();
      ImGui::Text("%.2f m", it.value().value("nearest_m", 0.0));
    }
    ImGui::EndTable();
  }
  const nlohmann::json by_kind = m.value("by_kind", nlohmann::json::object());
  for (auto it = by_kind.begin(); it != by_kind.end(); ++it) ImGui::BulletText("%s: %d", it.key().c_str(), it.value().get<int>());
}

void draw_details(Services &sv, UiState &st, const workspace::Item &item) {
  const auto &d = item.data;
  if (d.contains("doc")) ImGui::TextWrapped("%s", d["doc"].get<std::string>().c_str());
  ImGui::Spacing();
  const std::string sub = d.value("kind", "");
  const nlohmann::json prov = d.value("provenance", nlohmann::json::object());
  if (item.kind == "system") {
    ImGui::Text("Input:  %s", d.value("input", "?").c_str());
    ImGui::Text("Output: %s", d.value("output", "?").c_str());
  } else if (item.kind == "meshy" && sub == "concept") {
    ImGui::TextWrapped("Prompt: %s", prov.value("prompt", "").c_str());
    ImGui::Text("Model: %s   Cost: %d credits", prov.value("ai_model", "").c_str(), prov.value("cost_credits", 0));
  } else if (item.kind == "meshy" && sub == "mesh") {
    ImGui::Text("From %s (Meshy, %d credits)", d.value("concept", "").c_str(), prov.value("cost_credits", 0));
    draw_fit(d);
    ImGui::Text("Sizing ops:");
    for (const auto &op : d.value("ops", nlohmann::json::array())) ImGui::BulletText("%s", op.dump().c_str());
    if (d.contains("stats")) draw_stats(d["stats"]);
    ImGui::Spacing();
    if (d.contains("progressive"))
      ImGui::TextWrapped("Progressive mesh: %s (%.0f ms)", d["progressive"].value("report", "").c_str(),
                         d["progressive"].value("ms", 0.0));
    if (d.contains("recommended")) {
      draw_lods(d["recommended"]["lods"]);
      draw_lod_limits(sv, st, item);
    }
    else
      ImGui::TextDisabled("No LODs recommended yet.");
    ImGui::Spacing();
    ImGui::Text("Performance");
    if (d.contains("perf") && d["perf"].contains("runs")) draw_perf(d["perf"]);
    else ImGui::TextDisabled("Not benchmarked yet.");
    if (d.contains("stamped")) ImGui::Text("Stamped into fixed/%s", d["stamped"].value("fixed", "").c_str());
  } else if (item.kind == "fixed") {
    ImGui::Text("Stamped from %s at %s", d.value("stamped_from", "").c_str(), d.value("stamped_at", "").c_str());
    draw_fit(d);
    if (d.contains("stats")) draw_stats(d["stats"]);
    draw_lods(d.value("lods", nlohmann::json::array()));
    if (d.contains("perf") && d["perf"].contains("runs")) draw_perf(d["perf"]);
    ImGui::Separator();
    draw_fracture_controls(sv, st, item);
  } else if (item.kind == "fracture") {
    draw_fracture_item(sv, st, item);
  } else if (item.kind == "reference") {
    draw_reference(sv, st, item);
  } else if (item.kind == "conformance") {
    ImGui::Text("Shape: %s   radius %.2f m   height %.2f m", d.value("shape", "").c_str(), d.value("radius", 0.0f),
                d.value("height", 0.0f));
  } else if (item.kind == "plotter") {
    draw_plotter(sv, st, item);
  } else if (item.kind == "pipeline") {
    std::string error;
    if (sv.live->show(item.name, error)) draw_live(sv);
    else ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", error.c_str());
  } else if (item.kind == "config") {
    std::string error;
    const nlohmann::json refs = item.data.value("reference", nlohmann::json::array());
    if (!refs.empty()) {
      ImGui::Checkbox("Show reference photo in the viewport corner", &st.show_reference);
      ImGui::SameLine();
      ImGui::TextDisabled("open:");
      int k = 0;
      for (const auto &r : refs) {
        if (!r.is_string()) continue;
        ImGui::SameLine();
        ImGui::PushID(k++);
        // reference/trees/pine/01.jpg is the item reference:trees_pine_01.
        std::string id = r.get<std::string>().substr(std::string("reference/").size());
        id = id.substr(0, id.rfind('.'));
        std::replace(id.begin(), id.end(), '/', '_');
        if (ImGui::SmallButton(std::to_string(k).c_str())) sv.ws->select("reference:" + id);
        ImGui::PopID();
      }
    } else {
      ImGui::TextDisabled("No reference photos linked (config_set reference).");
    }
    draw_config(sv, item);
    ImGui::Separator();
    if (sv.live->show_config_file(item.path, error)) draw_live(sv);
    else ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", error.c_str());
  } else if (item.kind == "channel") {
    nlohmann::json schema = core::read_json(item.path);
    if (schema.is_object()) {
      if (schema.contains("doc")) ImGui::TextWrapped("%s", schema["doc"].get<std::string>().c_str());
      for (const auto &f : schema.value("fields", nlohmann::json::array()))
        ImGui::BulletText("%s: %s  %s", f.value("name", "").c_str(), f.value("type", "").c_str(),
                          f.value("doc", "").c_str());
    }
  }
}

void draw_inspector(Services &sv, UiState &st) {
  if (st.selected.empty()) {
    ImGui::TextDisabled("Select an item in the workspace.");
    return;
  }
  if (st.selected == "preview") {
    ImGui::Text("Preview: %s", sv.ws->preview().label.c_str());
    ImGui::TextDisabled("A system's output; not an approvable item.");
    return;
  }
  workspace::Item item;
  if (!sv.ws->find(st.selected, item)) return;
  ImGui::Text("%s", item.id.c_str());
  const bool approvable = item.kind != "reference";
  if (approvable) ImGui::TextColored(state_color(item.state()), "%s", item.state().c_str());
  if (item.data.contains("approved_at"))
    ImGui::TextDisabled("approved %s", item.data["approved_at"].get<std::string>().c_str());
  ImGui::TextDisabled("%s", item.path.c_str());
  ImGui::Separator();

  std::string error;
  const bool is_mesh = item.kind == "meshy" && item.data.value("kind", "") == "mesh";
  if (!approvable) {
    ImGui::TextDisabled("Reference material: what assets are compared against, not approved itself.");
  } else if (item.state() == "final") {
    ImGui::TextDisabled(item.kind == "fixed" ? "Stamped and final." : "Final: immutable. Changes need a fork.");
  } else {
    const bool ready = !is_mesh || item.data.contains("recommended");
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button(is_mesh ? "Approve (stamps LODs)" : "Approve")) {
      if (!sv.ws->approve(item.id, error)) st.message = error;
    }
    if (!ready) {
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("(recommend LODs first)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reject")) {
      if (!sv.ws->reject(item.id, st.reject_reason, error)) st.message = error;
      st.reject_reason[0] = 0;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##reason", "reason for rejecting (optional)", st.reject_reason, sizeof st.reject_reason);
    if (is_mesh && ImGui::Button("Recommend LODs")) {
      std::string root = sv.paths.workspace, name = item.name;
      std::thread([root, name] {
        nlohmann::json j;
        std::string err;
        if (!assets::mesh_recommend(root, name, nullptr, j, err)) core::log().add("mesh", "recommend failed: " + err);
      }).detach();
    }
    if (is_mesh) {
      ImGui::SameLine();
      ImGui::TextDisabled("(progressive mesh, LOD search, GPU benchmark)");
    }
  }
  if (item.kind == "system") {
    if (ImGui::Button("Build")) {
      std::string sys = item.name;
      forge::Paths paths = sv.paths;
      std::thread([paths, sys] { forge::build(paths, sys); }).detach();
    }
  }
  if (!st.message.empty()) ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", st.message.c_str());
  ImGui::Separator();
  draw_details(sv, st, item);
  ImGui::Separator();

  ImGui::Text("Notes");
  if (item.data.contains("notes"))
    for (const auto &n : item.data["notes"]) {
      ImGui::TextColored(n.value("author", "") == "human" ? ImVec4(0.6f, 0.8f, 1, 1) : ImVec4(0.85f, 0.7f, 1, 1),
                         "%s", n.value("author", "").c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%s", n.value("time", "").c_str());
      ImGui::TextWrapped("%s", n.value("text", "").c_str());
    }
  ImGui::InputTextMultiline("##note", st.note, sizeof st.note, ImVec2(-1, 60));
  if (ImGui::Button("Add note") && st.note[0]) {
    if (sv.ws->add_note(item.id, "human", st.note, error)) st.note[0] = 0;
    else st.message = error;
  }
}

// The flow tab: parameters on the left, stages in columns by depth, edges for
// what each reads. Hovering a parameter lights what reruns when it changes;
// hovering a stage lights its cache key. Below, the probe matrix.
void draw_flow(Services &sv, UiState &st) {
  pipeline::Live &live = *sv.live;
  const pipeline::Definition def = live.definition();
  const pipeline::Flow flow = pipeline::analyze(def);
  const auto runs = live.build_stages();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Scroll to zoom, drag to pan. Hover a parameter or lookup to see what reruns when it changes; hover a stage to see its cache key. Red lines are what the renderer uploads to the GPU.");
  ImGui::PopStyleColor();
  if (ImGui::SmallButton("Fit")) st.flow_fit = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("100%")) {
    st.flow_zoom = 1;
    st.flow_pan = {8, 8};
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.0f%%", st.flow_zoom * 100);
  // The canvas: a fixed share of the tab. The graph's zoom and pan change only when the human scrolls,
  // drags, or asks to fit, so resizing the window never moves it.
  const ImVec2 canvas = ImGui::GetCursorScreenPos();
  const ImVec2 canvas_size = {std::max(100.0f, ImGui::GetContentRegionAvail().x),
                              std::max(240.0f, ImGui::GetContentRegionAvail().y * 0.62f)};
  ImGui::InvisibleButton("flow_canvas", canvas_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool canvas_hovered = ImGui::IsItemHovered();
  {
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
      st.flow_pan.x += io.MouseDelta.x;
      st.flow_pan.y += io.MouseDelta.y;
    }
    if (canvas_hovered && io.MouseWheel != 0) {
      // Zoom about the mouse: the point under it stays under it.
      const float z = std::clamp(st.flow_zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 3.0f);
      const float f = z / st.flow_zoom;
      st.flow_pan.x = io.MousePos.x - canvas.x - (io.MousePos.x - canvas.x - st.flow_pan.x) * f;
      st.flow_pan.y = io.MousePos.y - canvas.y - (io.MousePos.y - canvas.y - st.flow_pan.y) * f;
      st.flow_zoom = z;
    }
  }
  const ImVec2 after_canvas = ImGui::GetCursorScreenPos();
  ImGui::SetWindowFontScale(st.flow_zoom);
  const float row = ImGui::GetTextLineHeightWithSpacing();
  // Nodes are as wide as their longest line.
  float pw = 0, sw = 0, cw = 0;
  for (const auto &p : flow.params) pw = std::max(pw, ImGui::CalcTextSize((p.name + "  (" + p.cardinality_text + ")").c_str()).x);
  for (const auto &s : flow.stages)
    for (const std::string &t : {s.id + "   [every frame]", s.system, s.in + " -> " + s.out, "distinct: " + s.distinct_text,
                                 std::string("last build: 000.0 ms")})
      sw = std::max(sw, ImGui::CalcTextSize(t.c_str()).x);
  // Config lookups: a box each, right of the parameter that picks from it, since the parameter's value is
  // the name the lookup finds; the entry it finds is what reaches the stages.
  std::map<std::string, std::pair<std::string, std::string>> config_text;  // parameter -> (title, detail)
  for (const auto &p : flow.params) {
    if (p.table.empty()) continue;
    const nlohmann::json t = pipeline::config::table(sv.paths, p.table);
    const std::string title = "lookup config/" + p.table;
    const std::string detail = std::to_string(p.entries) + " entries of " + (t.is_object() ? t.value("channel", "?") : "?");
    config_text[p.name] = {title, detail};
    cw = std::max({cw, ImGui::CalcTextSize(title.c_str()).x, ImGui::CalcTextSize(detail.c_str()).x});
  }
  // GPU destinations: a box each in a last column, fed by red lines from the stages whose output the
  // renderer uploads (the draw model, the pose).
  float gw = 0;
  for (const auto &g : flow.gpu)
    gw = std::max({gw, ImGui::CalcTextSize(g.destination.c_str()).x, ImGui::CalcTextSize(g.detail.c_str()).x});
  // Padding scales with the text, so the graph's width is proportional to the scale and the fit below
  // settles in one step (fixed padding made it flip between two scales near the smallest).
  const float k = st.flow_zoom;
  pw += 14 * k;
  sw += 18 * k;
  if (cw > 0) cw += 16 * k;
  if (gw > 0) gw += 16 * k;
  const float sh = row * 5.2f, gapx = ImGui::CalcTextSize("bones, pieces").x + 40 * k, gapy = 22 * k, prow = row * 1.6f;
  const float gapc = cw > 0 ? 36 * k : 0.0f, ch = row * 2.4f;
  int max_depth = 0;
  std::map<int, int> per_depth;
  for (const auto &s : flow.stages) {
    max_depth = std::max(max_depth, s.depth);
    per_depth[s.depth]++;
  }
  int tallest = 0;
  for (auto &[d, n] : per_depth) tallest = std::max(tallest, n);
  const ImVec2 o = {canvas.x + st.flow_pan.x, canvas.y + st.flow_pan.y};
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(canvas, {canvas.x + canvas_size.x, canvas.y + canvas_size.y}, true);
  dl->AddRectFilled(canvas, {canvas.x + canvas_size.x, canvas.y + canvas_size.y}, IM_COL32(22, 24, 30, 255), 4);
  const float top = o.y + 6 + (cw > 0 ? (ch - row * 1.3f) * 0.5f : 0.0f);
  const float px = o.x + 4 * k, cx = px + pw + gapc, sx = cx + cw + (cw > 0 ? gapx : gapx - gapc);
  const float gh = row * 2.4f;
  const float graph_h = std::max({top - o.y + flow.params.size() * prow, tallest * (sh + gapy),
                                  flow.gpu.size() * (gh + gapy)}) + 20;
  // The GPU column, past a gap wide enough for the longest field list on its lines.
  float gpu_gap = gapx;
  for (const auto &g : flow.gpu) gpu_gap = std::max(gpu_gap, ImGui::CalcTextSize(g.fields.c_str()).x + 40 * k);
  const float gx = sx + (max_depth + 1) * (sw + gapx) - gapx + gpu_gap;

  // Layout: parameters down the left in the pipeline's order, stages in columns by depth, each column
  // centred and in the pipeline's order.
  std::map<std::string, float> pyc;  // parameter centre heights
  std::map<std::string, ImVec2> ppos, spos;
  for (size_t i = 0; i < flow.params.size(); ++i) {
    ppos[flow.params[i].name] = {px, top + i * prow};
    pyc[flow.params[i].name] = top + i * prow + row * 0.65f;
  }
  std::map<int, int> placed;
  for (const auto &s : flow.stages) {
    const int k = placed[s.depth]++;
    const float colh = per_depth[s.depth] * (sh + gapy);
    spos[s.id] = {sx + s.depth * (sw + gapx), o.y + 6 + (graph_h - colh) * 0.5f + k * (sh + gapy)};
  }

  float stages_bottom = o.y;
  for (auto &[n, p] : spos) stages_bottom = std::max(stages_bottom, p.y + sh);
  // GPU boxes, in the order of their sources' heights, so lines cross as little as they can.
  std::vector<size_t> gorder(flow.gpu.size());
  for (size_t i = 0; i < gorder.size(); ++i) gorder[i] = i;
  std::stable_sort(gorder.begin(), gorder.end(), [&](size_t a, size_t b) {
    return spos[flow.gpu[a].stage].y < spos[flow.gpu[b].stage].y;
  });
  std::vector<ImVec2> gpos(flow.gpu.size());
  {
    const float colh = flow.gpu.size() * (gh + gapy);
    for (size_t j = 0; j < gorder.size(); ++j) gpos[gorder[j]] = {gx, o.y + 6 + (graph_h - colh) * 0.5f + j * (gh + gapy)};
  }

  // Hover.
  const ImVec2 mouse = canvas_hovered ? ImGui::GetIO().MousePos : ImVec2(-1e9f, -1e9f);
  std::string hp, hs;
  for (auto &[n, p] : ppos) {
    if (mouse.x >= p.x && mouse.x <= p.x + pw && mouse.y >= p.y && mouse.y <= p.y + row * 1.3f) hp = n;
    if (config_text.count(n) && mouse.x >= cx && mouse.x <= cx + cw && mouse.y >= pyc[n] - ch * 0.5f &&
        mouse.y <= pyc[n] + ch * 0.5f)
      hp = n;
  }
  for (auto &[n, p] : spos)
    if (mouse.x >= p.x && mouse.x <= p.x + sw && mouse.y >= p.y && mouse.y <= p.y + sh) hs = n;
  auto lit_stage = [&](const pipeline::FlowStage &s) {
    if (!hp.empty()) return std::find(s.all.begin(), s.all.end(), hp) != s.all.end();
    return hs == s.id;
  };
  auto lit_param = [&](const std::string &p) {
    if (!hs.empty())
      for (const auto &s : flow.stages)
        if (s.id == hs) return std::find(s.all.begin(), s.all.end(), p) != s.all.end();
    return hp == p;
  };
  const ImU32 dim = IM_COL32(110, 115, 130, 255), bright = IM_COL32(255, 196, 90, 255), cache = IM_COL32(120, 190, 255, 255);
  const ImU32 config_col = IM_COL32(200, 150, 230, 255);

  // Edges. Each stage takes its incoming lines at points spread down its left side in the order of their
  // sources, and its outgoing ones down its right side in the order of their targets.
  struct In {
    float from_y;
    ImVec2 a;
    bool param;
    std::string source, label;
  };
  std::map<std::string, std::vector<In>> ins;
  std::map<std::string, std::vector<std::pair<float, std::string>>> outs;  // stage -> (target y, target)
  for (const auto &s : flow.stages) {
    for (const auto &p : s.direct) {
      std::string fields;
      for (const auto &[param, field] : s.binds)
        if (param == p && field != p) fields += (fields.empty() ? "" : ", ") + field;
      // A config parameter's line leaves from its lookup, carrying the entry it found.
      const float ax = config_text.count(p) ? cx + cw : px + pw;
      ins[s.id].push_back({pyc[p], {ax, pyc[p]}, true, p, fields});
    }
    std::map<std::string, std::string> reads;  // upstream stage -> the fields read, one edge each
    for (const auto &e : s.upstream) {
      std::string &l = reads[e.from];
      l += (l.empty() ? "" : ", ") + (e.field.empty() ? std::string("all") : e.field);
    }
    for (const auto &[from, label] : reads) {
      if (!spos.count(from)) continue;
      ins[s.id].push_back({spos[from].y, {}, false, from, label});
      outs[from].push_back({spos[s.id].y, s.id});
    }
  }
  for (size_t i = 0; i < flow.gpu.size(); ++i)
    if (spos.count(flow.gpu[i].stage)) outs[flow.gpu[i].stage].push_back({gpos[i].y + gh * 0.5f, "gpu:" + std::to_string(i)});
  std::map<std::pair<std::string, std::string>, float> out_y;  // (from, to) -> where it leaves the box
  for (auto &[from, list] : outs) {
    std::sort(list.begin(), list.end());
    for (size_t k = 0; k < list.size(); ++k)
      out_y[{from, list[k].second}] = spos[from].y + sh * (k + 1) / (list.size() + 1);
  }
  for (const auto &s : flow.stages) {
    auto &list = ins[s.id];
    std::stable_sort(list.begin(), list.end(), [](const In &a, const In &b) { return a.from_y < b.from_y; });
    const ImVec2 sp = spos[s.id];
    for (size_t k = 0; k < list.size(); ++k) {
      const In &e = list[k];
      const ImVec2 b = {sp.x, sp.y + sh * (k + 1) / (list.size() + 1)};
      if (e.param) {
        const bool lit = (!hp.empty() && hp == e.source) || (!hs.empty() && hs == s.id);
        const float bend = std::min((b.x - e.a.x) * 0.45f, gapx * 0.9f);
        const ImU32 c = lit ? bright : (config_text.count(e.source) ? config_col : dim);
        dl->AddBezierCubic(e.a, {e.a.x + bend, e.a.y}, {b.x - bend, b.y}, b, c, lit ? 2.5f : 1.0f);
        if (!e.label.empty()) {
          const float lw = ImGui::CalcTextSize(e.label.c_str()).x;
          dl->AddText({b.x - lw - 6, b.y - row * 0.95f}, lit ? bright : IM_COL32(170, 180, 200, 255), e.label.c_str());
        }
      } else {
        const ImVec2 a = {spos[e.source].x + sw, out_y[{e.source, s.id}]};
        const bool lit = (!hs.empty() && (hs == s.id || hs == e.source));
        dl->AddBezierCubic(a, {a.x + gapx * 0.5f, a.y}, {b.x - gapx * 0.5f, b.y}, b, lit ? cache : IM_COL32(150, 160, 180, 255), 2.0f);
        // The fields read, just above where the line leaves its source (lines leave level, so labels don't sit on them).
        dl->AddText({a.x + 8, a.y - row}, IM_COL32(170, 180, 200, 255), e.label.c_str());
      }
      dl->AddTriangleFilled({b.x, b.y}, {b.x - 7, b.y - 4}, {b.x - 7, b.y + 4}, IM_COL32(170, 180, 200, 255));
    }
  }
  // GPU lines (red) and their destinations.
  const ImU32 gpu_col = IM_COL32(230, 80, 70, 255), gpu_lit = IM_COL32(255, 130, 110, 255);
  for (size_t i = 0; i < flow.gpu.size(); ++i) {
    const auto &g = flow.gpu[i];
    if (!spos.count(g.stage)) continue;
    const bool lit = hs == g.stage;
    const ImVec2 a = {spos[g.stage].x + sw, out_y[{g.stage, "gpu:" + std::to_string(i)}]};
    const ImVec2 b = {gpos[i].x, gpos[i].y + gh * 0.5f};
    const ImU32 lc = lit ? gpu_lit : gpu_col;
    const float lw = lit ? 3.0f : 2.0f;
    int depth = 0;
    for (const auto &fs : flow.stages)
      if (fs.id == g.stage) depth = fs.depth;
    if (depth < max_depth) {
      // A stage before the last column: drop to a lane under the stages, run right, and rise to the box,
      // so the line never passes behind a later stage.
      const float lane = stages_bottom + gapy * (1.0f + static_cast<float>(i)) + row * 0.4f * static_cast<float>(i);
      const float x1 = a.x + gapx * 0.5f, x2 = gx - gpu_gap * 0.5f;
      dl->AddBezierCubic(a, {x1, a.y}, {a.x, lane}, {x1 + gapx * 0.2f, lane}, lc, lw);
      dl->AddLine({x1 + gapx * 0.2f, lane}, {x2 - gapx * 0.2f, lane}, lc, lw);
      dl->AddBezierCubic({x2 - gapx * 0.2f, lane}, {b.x - gpu_gap * 0.1f, lane}, {x2, b.y}, b, lc, lw);
    } else {
      const float bend = std::max(gapx * 0.5f, (b.x - a.x) * 0.35f);
      dl->AddBezierCubic(a, {a.x + bend, a.y}, {b.x - bend, b.y}, b, lc, lw);
    }
    dl->AddTriangleFilled(b, {b.x - 8, b.y - 5}, {b.x - 8, b.y + 5}, lc);
    // The fields it carries, just before the box, where the gap was made wide enough for them.
    const float tw = ImGui::CalcTextSize(g.fields.c_str()).x;
    dl->AddText({b.x - tw - 12, b.y - row}, IM_COL32(240, 150, 140, 255), g.fields.c_str());
    const ImVec2 c = gpos[i], d = {gpos[i].x + gw, gpos[i].y + gh};
    dl->AddRectFilled(c, d, lit ? IM_COL32(90, 40, 36, 255) : IM_COL32(58, 30, 30, 255), 6);
    dl->AddRect(c, d, lit ? gpu_lit : gpu_col, 6.0f, 0, 2.0f);
    dl->AddText({c.x + 8 * k, c.y + 4}, IM_COL32(255, 230, 226, 255), g.destination.c_str());
    dl->AddText({c.x + 8 * k, c.y + 4 + row}, IM_COL32(215, 175, 170, 255), g.detail.c_str());
  }
  // Lookup boxes, fed by the parameter that names the entry.
  for (const auto &[param, text] : config_text) {
    const ImVec2 a = {cx, pyc[param] - ch * 0.5f}, b = {cx + cw, pyc[param] + ch * 0.5f};
    const bool lit = lit_param(param);
    dl->AddLine({px + pw, pyc[param]}, {cx, pyc[param]}, lit ? bright : config_col, lit ? 2.5f : 1.5f);
    dl->AddTriangleFilled({cx, pyc[param]}, {cx - 7, pyc[param] - 4}, {cx - 7, pyc[param] + 4}, IM_COL32(170, 180, 200, 255));
    dl->AddRectFilled(a, b, lit ? IM_COL32(90, 70, 30, 255) : IM_COL32(52, 40, 62, 255), 6);
    dl->AddRect(a, b, lit ? bright : config_col, 6.0f, 0, 2.0f);
    dl->AddText({a.x + 8 * k, a.y + 4}, IM_COL32(240, 230, 250, 255), text.first.c_str());
    dl->AddText({a.x + 8 * k, a.y + 4 + row}, IM_COL32(190, 180, 205, 255), text.second.c_str());
  }
  // Nodes.
  for (const auto &p : flow.params) {
    const ImVec2 a = ppos[p.name];
    const bool lit = lit_param(p.name);
    dl->AddRectFilled(a, {a.x + pw, a.y + row * 1.3f}, lit ? IM_COL32(90, 70, 30, 255) : IM_COL32(45, 48, 58, 255), 4);
    dl->AddRect(a, {a.x + pw, a.y + row * 1.3f}, lit ? bright : dim, 4.0f);
    const std::string t = p.name + "  (" + p.cardinality_text + ")";
    dl->AddText({a.x + 6 * k, a.y + 3}, IM_COL32(230, 232, 240, 255), t.c_str());
  }
  for (const auto &s : flow.stages) {
    const ImVec2 a = spos[s.id];
    const bool lit = lit_stage(s);
    const bool update = s.phase == "update";
    dl->AddRectFilled(a, {a.x + sw, a.y + sh}, lit ? IM_COL32(90, 70, 30, 255) : IM_COL32(40, 46, 60, 255), 6);
    dl->AddRect(a, {a.x + sw, a.y + sh}, lit ? bright : (update ? IM_COL32(120, 200, 140, 255) : cache), 6.0f, 0, 2.0f);
    float y = a.y + 4;
    auto line = [&](ImU32 c, const std::string &t) {
      dl->AddText({a.x + 8 * k, y}, c, t.c_str());
      y += row;
    };
    line(IM_COL32(255, 255, 255, 255), s.id + (update ? "   [every frame]" : "   [built, cached]"));
    line(IM_COL32(190, 200, 220, 255), s.system);
    line(IM_COL32(160, 170, 190, 255), s.in + " -> " + s.out);
    line(IM_COL32(160, 170, 190, 255), "distinct: " + s.distinct_text);
    std::string last = "-";
    for (const auto &r : runs)
      if (r.id == s.id) {
        char buf[64];
        std::snprintf(buf, sizeof buf, r.cached ? "last build: cached" : "last build: %.1f ms", r.milliseconds);
        last = buf;
      }
    line(IM_COL32(140, 150, 170, 255), last);
  }
  const float graph_w = flow.gpu.empty() ? sx - o.x + (max_depth + 1) * (sw + gapx) - gapx + 8 * k : gx + gw - o.x + 8 * k;
  dl->PopClipRect();
  ImGui::SetWindowFontScale(1);
  ImGui::SetCursorScreenPos(after_canvas);
  // Fit once: the zoom that shows the whole graph (its size is proportional to the zoom), then left at that.
  // The height includes the lanes GPU lines take under the stages.
  const float fit_h = std::max(graph_h, stages_bottom - o.y + (gapy + row * 0.4f) * (flow.gpu.size() + 1));
  if (st.flow_fit && graph_w > 0 && fit_h > 0) {
    const float f = std::min((canvas_size.x - 16) / graph_w, (canvas_size.y - 16) / fit_h);
    st.flow_zoom = std::clamp(st.flow_zoom * f, 0.25f, 1.5f);
    st.flow_pan = {8, 8};
    st.flow_fit = false;
  }
  ImGui::Separator();

  // The probe: which declared dependencies actually change each stage's output.
  auto &pr = *st.probe;
  bool running;
  nlohmann::json result;
  {
    std::lock_guard<std::mutex> lock(pr.mutex);
    running = pr.running;
    if (pr.pipeline == def.name) result = pr.result;
  }
  ImGui::BeginDisabled(running);
  if (ImGui::Button(running ? "Probing..." : "Probe dependencies")) {
    auto probe = st.probe;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->running = true;
      probe->pipeline = def.name;
    }
    pipeline::Runner *runner = &live.runner();
    const nlohmann::json base = live.values();
    std::thread([probe, runner, def, base] {
      nlohmann::json r = pipeline::probe(*runner, def, base, 3);
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->result = r;
      probe->running = false;
    }).detach();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("varies each parameter from the current values and compares every stage's output");
  if (result.contains("params") &&
      ImGui::BeginTable("probe", static_cast<int>(flow.stages.size()) + 1,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableSetupColumn("parameter");
    for (const auto &s : flow.stages) ImGui::TableSetupColumn(s.id.c_str());
    ImGui::TableHeadersRow();
    for (const auto &p : result["params"]) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p["name"].get<std::string>().c_str());
      for (const auto &s : flow.stages) {
        ImGui::TableNextColumn();
        const bool declared = std::find(s.all.begin(), s.all.end(), p["name"].get<std::string>()) != s.all.end();
        const bool changed = p["changes"].value(s.id, false);
        if (changed) ImGui::TextColored({0.45f, 0.85f, 0.5f, 1}, "changes");
        else if (declared) ImGui::TextColored({0.95f, 0.8f, 0.4f, 1}, "reaches, no change");
        else ImGui::TextDisabled("-");
      }
    }
    ImGui::EndTable();
    ImGui::TextDisabled("\"reaches, no change\": the stage reruns for this parameter but its output didn't move in the "
                       "values tried; a cache keyed without it could be shared.");
  }
}

// The video tab: export a QA video of every variation of the pipeline
// (design/quality.perf.md, "The QA video").
void draw_video(Services &sv, UiState &st) {
  const pipeline::Definition def = sv.live->definition();
  auto &v = st.video;
  ImGui::TextWrapped("A QA video of pipeline '%s': a 5-second title slide, then one clip per variation, titled with its "
                     "parameters. Every combination of the discrete parameters is enumerated; continuous ones are sampled.",
                     def.name.c_str());
  ImGui::Spacing();
  std::map<std::string, std::vector<std::string>> chosen;  // what the video enumerates, per parameter
  int continuous = 0;
  for (const auto &p : def.params) {
    if (p.type == "u32" || p.type == "f32") ++continuous;
    if (p.type != "enum" && p.type != "config" && p.type != "bool") continue;
    ImGui::PushID(p.name.c_str());
    std::string &pin = v.pins[def.name][p.name];
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo(p.name.c_str(), pin.empty() ? "all" : pin.c_str())) {
      if (ImGui::Selectable("all", pin.empty())) pin.clear();
      for (const auto &c : p.values)
        if (ImGui::Selectable(c.c_str(), pin == c)) pin = c;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(pin.empty() ? "%zu values" : "pinned", p.values.size());
    chosen[p.name] = pin.empty() ? p.values : std::vector<std::string>{pin};
    if (!p.when_param.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("(only where %s.%s: %zu entries)", p.when_param.c_str(), p.when_field.c_str(), p.when_on.size());
    }
    ImGui::PopID();
  }
  const double combos = def.combinations(chosen);
  ImGui::SetNextItemWidth(220);
  ImGui::SliderInt("samples of continuous values", &v.floats, 1, 10);
  ImGui::SameLine();
  ImGui::TextDisabled("(%d continuous parameters, sampled at random per variation)", continuous);
  ImGui::SetNextItemWidth(220);
  ImGui::SliderFloat("seconds per variation", &v.seconds, 0.5f, 3.0f, "%.1f s");
  ImGui::SetNextItemWidth(220);
  ImGui::SliderInt("fps", &v.fps, 15, 60);
  ImGui::SetNextItemWidth(220);
  ImGui::Combo("size", &v.size, "1280x720\0" "1920x1080\0");
  const double variations = combos * v.floats, seconds = 5 + variations * v.seconds;
  ImGui::Text("%.0f variations, %d:%02d long", variations, static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
  // Progress for a job: the last "video: a/b" line it printed.
  auto progress_of = [](const nlohmann::json &status) {
    float progress = 0;
    for (const auto &l : status.value("output", nlohmann::json::array())) {
      size_t a = 0, b = 0;
      if (std::sscanf(l.get<std::string>().c_str(), "video: %zu/%zu", &a, &b) == 2 && b) progress = static_cast<float>(a) / b;
    }
    return progress;
  };
  // This pipeline's latest export, and whatever export is running (one at a time, of any pipeline).
  const int job = perf::latest_video(def.name), busy = perf::running_video();
  const nlohmann::json status = job ? perf::job_status(job) : nlohmann::json();
  const nlohmann::json busy_status = busy ? perf::job_status(busy) : nlohmann::json();
  const bool running = busy && busy == job;
  const std::string blocker = busy && !running ? busy_status.value("pipeline", std::string()) : "";
  ImGui::BeginDisabled(busy != 0 || variations < 1);
  if (ImGui::Button("Export video")) {
    perf::VideoRequest q;
    q.pipeline = def.name;
    q.floats = v.floats;
    q.seconds = v.seconds;
    q.fps = v.fps;
    q.size = v.size == 1 ? "1920x1080" : "1280x720";
    for (const auto &[name, pin] : v.pins[def.name])
      if (!pin.empty()) q.fixed[name] = pin;
    perf::start_video(sv.paths, q);
  }
  ImGui::EndDisabled();
  if (!blocker.empty()) {
    // Another pipeline's export holds the exporter: say which, and how far along it is.
    ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Blocked: pipeline '%s' is exporting its video; this one can start when it finishes.",
                       blocker.c_str());
    char overlay[96];
    std::snprintf(overlay, sizeof overlay, "%s: %.0f%%", blocker.c_str(), progress_of(busy_status) * 100);
    ImGui::ProgressBar(progress_of(busy_status), ImVec2(-1, 0), overlay);
  }
  if (status.is_object()) {
    const auto lines = status.value("output", nlohmann::json::array());
    if (running) {
      ImGui::ProgressBar(progress_of(status), ImVec2(-1, 0));
    } else if (status.contains("video")) {
      const std::string path = status["video"];
      ImGui::TextColored({0.45f, 0.85f, 0.5f, 1}, "Wrote %s", path.c_str());
      if (ImGui::Button("Play")) SDL_OpenURL(("file://" + path).c_str());
      ImGui::SameLine();
      if (ImGui::Button("Open folder")) SDL_OpenURL(("file://" + std::filesystem::path(path).parent_path().string()).c_str());
    } else if (status.value("done", false)) {
      ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "Export failed: %s", status.value("error", std::string("")).c_str());
      for (const auto &l : lines) ImGui::TextDisabled("%s", l.get<std::string>().c_str());
    }
  }
}

void draw_viewport_controls(UiState &st) {
  const char *views[] = {"Orbit", "Front", "Side", "Top", "Iso"};
  for (int v = 0; v < 5; ++v) {
    if (v) ImGui::SameLine();
    if (ImGui::RadioButton(views[v], static_cast<int>(st.camera.view) == v)) st.camera.view = static_cast<viewport::View>(v);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate -90")) {
    st.camera.quarter = (st.camera.quarter + 3) % 4;
    st.camera.yaw -= 1.5707963f;
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate +90")) {
    st.camera.quarter = (st.camera.quarter + 1) % 4;
    st.camera.yaw += 1.5707963f;
  }
  ImGui::SameLine();
  ImGui::Text("|");
  ImGui::SameLine();
  ImGui::Checkbox("Conformance", &st.overlays.conformance);
  ImGui::SameLine();
  ImGui::Checkbox("Collision", &st.overlays.collision);
  ImGui::SameLine();
  ImGui::Checkbox("Hero", &st.overlays.hero);
  ImGui::SameLine();
  ImGui::Checkbox("Hex", &st.overlays.hex);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &st.overlays.grid);
  ImGui::SameLine();
  ImGui::Checkbox("Silhouette", &st.silhouette);
  if (!st.references.empty()) {
    ImGui::SameLine();
    ImGui::Checkbox("Reference", &st.show_reference);
    ImGui::SetItemTooltip("Show the photos the config entries in use link to, in the corner (click it for the next)");
  }
  if (st.camera.distance > 0) {
    ImGui::SameLine();
    ImGui::Text("| camera at %.1f m (game FOV)", st.camera.distance);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) st.camera.distance = 0;
  }
  if (st.content.progressive && !st.content.progressive->level_triangles.empty()) {
    const auto &lt = st.content.progressive->level_triangles;
    int full = static_cast<int>(lt.front()), lo = static_cast<int>(lt.back());
    int t = st.triangles == 0 ? full : static_cast<int>(st.triangles);
    ImGui::SetNextItemWidth(320);
    if (ImGui::SliderInt("Triangles", &t, lo, full, "%d", ImGuiSliderFlags_Logarithmic)) {
      st.triangles = t >= full ? 0 : static_cast<uint32_t>(t);
      st.selected_lod = -1;
      st.interact_until = SDL_GetTicks() + 200;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Full")) {
      st.triangles = 0;
      st.selected_lod = -1;
    }
  }
}

}  // namespace

int run_ui(Services &sv) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    core::log().add("ui", std::string("SDL_Init failed: ") + SDL_GetError());
    return 1;
  }
  float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0) scale = 1;
  SDL_Window *window = SDL_CreateWindow("Foundry", static_cast<int>(1600 * scale), static_cast<int>(950 * scale),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    core::log().add("ui", std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    return 1;
  }
  SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                                  SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
                                              false, nullptr);
  if (!device || !SDL_ClaimWindowForGPUDevice(device, window)) {
    core::log().add("ui", std::string("GPU setup failed: ") + SDL_GetError());
    return 1;
  }
  SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
  core::log().add("ui", std::string("GPU driver: ") + SDL_GetGPUDeviceDriver(device));
  // The viewport draws on the GPU from here on (screenshots and sheets too); without it, the CPU.
  static viewport::GpuRenderer gpu_view;
  if (std::string gerr; gpu_view.init(device, gerr)) {
    viewport::set_gpu_renderer(&gpu_view);
    core::log().add("ui", "viewport: drawn on the GPU");
  } else {
    core::log().add("ui", "viewport: drawn on the CPU (" + gerr + ")");
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init = {};
  init.Device = device;
  init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
  init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init(&init);

  ViewTexture tex, ref_tex;
  tex.device = device;
  ref_tex.device = device;
  capture().set_ui_running(true);
  UiState st;
  g_ui = &st;
  bool done = false;
  while (!done) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT) {
        core::log().add("ui", "quit requested");
        done = true;
      }
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)) {
        core::log().add("ui", "window closed");
        done = true;
      }
    }
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(20);
      continue;
    }
    // Pick up changes made on disk or by the agent.
    if (SDL_GetTicks() - st.last_scan > 1000) {
      sv.ws->scan();
      st.last_scan = SDL_GetTicks();
    }
    int version = sv.ws->version();
    std::string selected = sv.ws->selected();
    if (version != st.ws_version || selected != st.selected) {
      if (selected != st.selected) {
        st.message.clear();
        st.triangles = 0;
        st.selected_lod = -1;
        st.camera.distance = 0;
      }
      st.ws_version = version;
      st.selected = selected;
      // A plotter's region composes in the background (below), so it never blocks here.
      st.content = is_plot(st.selected) ? assets::ViewContent{} : assets::load_view(*sv.ws, st.selected);
      st.live_key.clear();  // whatever replaces the content (live or fracture) sets it again
      st.fracture.loaded_for.clear();  // a regenerated fracture loads again
      // A viewed LOD follows its row: after a regeneration, show its new triangle count.
      workspace::Item item;
      if (st.selected_lod >= 0 && sv.ws->find(st.selected, item) && item.data.contains("recommended")) {
        const auto &lods = item.data["recommended"].value("lods", nlohmann::json::array());
        if (st.selected_lod < static_cast<int>(lods.size()))
          st.triangles = lods[st.selected_lod].value("triangles", st.triangles);
      }
    }
    // A pipeline or config entry shows the live instance: pose it for this moment.
    if (update_fracture_view(sv, st)) {
      // a fracture preview or break
    } else if (is_plot(st.selected)) {
      // A plotter shows its region: the latest composed picture.
      std::string err;
      if (sv.plot->name() != st.selected.substr(8) && !sv.plot->show(st.selected.substr(8), err)) st.plot_ui.message = err;
      plot::Picture pic = sv.plot->picture();
      if (pic.mesh) {
        const std::string key = "|plot" + std::to_string(pic.version);
        if (key != st.live_key) {
          st.live_key = key;
          st.content.mesh = pic.mesh;
          st.content.lines = pic.lines;
          st.content.collision = pic.collision;
          st.content.has_conformance = false;
          st.content.progressive = nullptr;
          st.content.lods.clear();
          st.camera.frame = 0;
        }
      }
    } else if (is_live(st.selected)) {
      pipeline::Frame f = sv.live->frame(now_seconds());
      if (f.mesh) {
        std::string key = "|live" + std::to_string(f.model_version) + sv.live->values().dump();
        if (key != st.live_key) {
          // A stable frame per model, so swaying and falling don't refit the camera.
          if (st.live_key.rfind("|live" + std::to_string(f.model_version) + "{", 0) != 0)
            st.camera.frame = std::max(f.height, 2.0f) * 1.15f;
          st.live_key = key;
          st.content.mesh = f.mesh;
          st.content.has_conformance = f.conformance_radius > 0;
          st.content.radius = f.conformance_radius;
          st.content.height = f.conformance_height;
          st.content.collision.clear();
          if (f.collision) physics::wireframe(*f.collision, st.content.collision);
          st.content.progressive = nullptr;
          st.content.lods.clear();
        }
      }
    } else {
      st.live_key.clear();
      st.camera.frame = 0;
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float left = std::round(W * 0.18f), right = std::round(W * 0.36f), bottom = std::round(H * 0.2f);
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize({left, H - bottom});
    ImGui::Begin("Workspace", nullptr, fixed);
    draw_tree(sv, st);
    ImGui::End();

    ImGui::SetNextWindowPos({vp->WorkPos.x + W - right, vp->WorkPos.y});
    ImGui::SetNextWindowSize({right, H - bottom});
    ImGui::Begin("Inspector", nullptr, fixed);
    draw_inspector(sv, st);
    ImGui::End();

    ImGui::SetNextWindowPos({vp->WorkPos.x + left, vp->WorkPos.y});
    ImGui::SetNextWindowSize({W - left - right, H - bottom});
    ImGui::Begin("Viewport", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    const std::string want_tab = sv.ws->take_tab();
    if (const int r = sv.ws->take_reference(); r >= 0) st.show_reference = r == 1;
    auto tab_flags = [&](const char *name) {
      return want_tab == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (is_live(st.selected) && ImGui::BeginTabBar("viewtabs")) {
      if (ImGui::BeginTabItem("View", nullptr, tab_flags("view"))) {
        st.view_tab = 0;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Flow", nullptr, tab_flags("flow"))) {
        st.view_tab = 1;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Video", nullptr, tab_flags("video"))) {
        st.view_tab = 2;
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    const int tab = is_live(st.selected) ? st.view_tab : 0;
    if (tab == 1) {
      ImGui::BeginChild("flow", ImVec2(0, 0));
      draw_flow(sv, st);
      ImGui::EndChild();
    } else if (tab == 2) {
      draw_video(sv, st);
    } else {
    update_references(sv, st);
    load_reference(sv, st);
    draw_viewport_controls(st);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int side = std::max(64, static_cast<int>(std::min(avail.x, avail.y)));
    int px = std::min(side, 900);
    if (st.content.empty()) {
      ImGui::TextDisabled("Nothing to show. Select a fixed asset, concept, or conformance shape.");
    } else {
      // While dragging or zooming, render a quarter of the pixels; sharpen once idle.
      const bool interacting = SDL_GetTicks() < st.interact_until ||
                               (is_live(st.selected) && sv.live->any_running()) ||
                               (st.selected.rfind("fracture:", 0) == 0 && st.fracture.playing);
      const int rpx = interacting && !viewport::gpu_renderer() ? std::max(64, px / 2) : px;  // the GPU keeps up
      std::string key = render_key(st, rpx, rpx);
      if (key != st.rendered_key) {
        // A plotter's region draws its own lines; its collision has its own toggle, off at first.
        const viewport::Overlays ov = is_plot(st.selected) ? plot::overlays(st.plot_ui.collision) : st.overlays;
        st.pending = assets::render_view(st.content, st.camera, ov, st.silhouette, rpx, rpx, st.triangles);
        st.has_pending = true;
        st.rendered_key = key;
      }
      if (tex.texture) {
        float aspect = tex.height > 0 ? static_cast<float>(tex.width) / static_cast<float>(tex.height) : 1;
        ImVec2 size = aspect >= 1 ? ImVec2(static_cast<float>(side), side / aspect) : ImVec2(side * aspect, static_cast<float>(side));
        ImGui::Image(reinterpret_cast<ImTextureID>(tex.texture), size);
        const bool over_reference = st.show_reference && !st.references.empty() && ref_tex.texture &&
                                    ImGui::GetIO().MousePos.x >= ImGui::GetItemRectMax().x - size.x * 0.34f - 10 &&
                                    ImGui::GetIO().MousePos.y <= ImGui::GetItemRectMin().y + 10 + size.x * 0.34f * ref_tex.height / std::max(1, ref_tex.width);
        draw_reference_corner(st, ref_tex.texture, ref_tex.width, ref_tex.height, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered() && !st.content.image && !over_reference) {
          if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || io.MouseWheel != 0) st.interact_until = SDL_GetTicks() + 200;
          if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 d = io.MouseDelta;
            st.camera.view = viewport::View::Orbit;
            st.camera.yaw -= d.x * 0.01f;
            st.camera.pitch = std::clamp(st.camera.pitch + d.y * 0.01f, -1.4f, 1.4f);
          }
          if (io.MouseWheel != 0) {
            if (st.camera.distance > 0) st.camera.distance = std::max(0.2f, st.camera.distance / std::pow(1.1f, io.MouseWheel));
            else st.camera.zoom = std::clamp(st.camera.zoom * std::pow(1.1f, io.MouseWheel), 0.2f, 8.0f);
          }
        }
      }
    }
    }  // the view tab
    ImGui::End();

    ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + H - bottom});
    ImGui::SetNextWindowSize({W, bottom});
    ImGui::Begin("Activity", nullptr, fixed);
    nlohmann::json ledger = sv.meshy->ledger();
    ImGui::Text("MCP: %s  (%d requests)   Meshy: %d of %d credits left%s", sv.server->url().c_str(),
                sv.server->requests(), ledger.value("remaining", 0), ledger.value("budget", 0),
                sv.meshy->has_key() ? "" : "   (no API key)");
    // Background jobs: one progress bar per running task.
    for (const auto &job : core::jobs().snapshot()) {
      char overlay[256];
      std::snprintf(overlay, sizeof overlay, "%s  %s  (%.0f s)", job.title.c_str(), job.status.c_str(),
                    core::jobs().now() - job.started);
      float p = job.progress < 0 ? static_cast<float>(-0.3 * ImGui::GetTime()) : job.progress;  // indeterminate: animate
      ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
    }
    ImGui::Separator();
    ImGui::BeginChild("log");
    for (const auto &l : core::log().snapshot()) {
      ImGui::TextDisabled("%s", l.time.c_str());
      ImGui::SameLine();
      ImGui::TextColored({0.6f, 0.8f, 1, 1}, "%-8s", l.source.c_str());
      ImGui::SameLine();
      ImGui::TextUnformatted(l.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    ImDrawData *draw = ImGui::GetDrawData();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    if (st.reference_has_pending) {
      ref_tex.upload(cmd, st.reference_pending);
      st.reference_has_pending = false;
    }
    if (st.has_pending) {
      tex.upload(cmd, st.pending);
      st.has_pending = false;
    }
    if (capture().wanted()) {
      // Submit the texture upload first, so the capture sees this frame's viewport.
      SDL_SubmitGPUCommandBuffer(cmd);
      capture().fulfil(capture_frame(device, init.ColorTargetFormat, draw));
      cmd = SDL_AcquireGPUCommandBuffer(device);
    }
    SDL_GPUTexture *swap = nullptr;
    SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, nullptr, nullptr);
    if (swap && draw->DisplaySize.x > 0 && draw->DisplaySize.y > 0) {
      ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
      SDL_GPUColorTargetInfo target = {};
      target.texture = swap;
      target.clear_color = {0.1f, 0.1f, 0.12f, 1};
      target.load_op = SDL_GPU_LOADOP_CLEAR;
      target.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
      ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
      SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    tex.release_retired();
    ref_tex.release_retired();
  }

  capture().set_ui_running(false);
  SDL_WaitForGPUIdle(device);
  tex.release();
  ref_tex.release();
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
  viewport::set_gpu_renderer(nullptr);
  gpu_view.shutdown();
  SDL_ReleaseWindowFromGPUDevice(device, window);
  SDL_DestroyGPUDevice(device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace app
