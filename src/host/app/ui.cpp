// The workspace UI (Dear ImGui on SDL3's GPU API).
//
// Layout: the workspace tree on the left (mirroring the project layout), the
// viewport in the middle, the inspector (state, details, notes, approval) on
// the right, and the activity log along the bottom.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "app/capture.h"
#include "app/services.h"
#include "assets/view.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"
#include "assets/mesh_item.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

namespace app {

namespace {

ImVec4 state_color(const std::string &state) {
  if (state == "final") return {0.45f, 0.85f, 0.5f, 1};
  if (state == "rejected") return {0.95f, 0.45f, 0.4f, 1};
  return {0.95f, 0.8f, 0.4f, 1};
}

const char *kKinds[][2] = {{"pipeline", "Pipelines"}, {"system", "Systems"}, {"channel", "Channels"},
                           {"conformance", "Conformance"}, {"meshy", "Meshy"}, {"fixed", "Fixed assets"}};

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
  float share = 2, share_saved = -1;  // the switch threshold, in percent  // the saved limits last frame, to notice the foundry changing them
};

std::string render_key(const UiState &s, int w, int h) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s|%u|%d|%d|%d|%.3f|%.3f|%.3f|%.3f|%d%d%d%d|%d|%dx%d", s.selected.c_str(), s.triangles,
                s.ws_version, static_cast<int>(s.camera.view), s.camera.quarter, s.camera.distance, s.camera.yaw, s.camera.pitch, s.camera.zoom, s.overlays.conformance,
                s.overlays.hero, s.overlays.hex, s.overlays.grid, s.silhouette, w, h);
  return buf;
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
  } else if (item.kind == "conformance") {
    ImGui::Text("Shape: %s   radius %.2f m   height %.2f m", d.value("shape", "").c_str(), d.value("radius", 0.0f),
                d.value("height", 0.0f));
  } else if (item.kind == "pipeline" && d.contains("nodes")) {
    for (const auto &n : d["nodes"])
      ImGui::BulletText("%s: %s", n.value("id", "").c_str(),
                        n.contains("system") ? n["system"].get<std::string>().c_str() : n.value("doc", "").c_str());
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
  ImGui::TextColored(state_color(item.state()), "%s", item.state().c_str());
  if (item.data.contains("approved_at"))
    ImGui::TextDisabled("approved %s", item.data["approved_at"].get<std::string>().c_str());
  ImGui::TextDisabled("%s", item.path.c_str());
  ImGui::Separator();

  std::string error;
  const bool is_mesh = item.kind == "meshy" && item.data.value("kind", "") == "mesh";
  if (item.state() == "final") {
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
  ImGui::Checkbox("Hero", &st.overlays.hero);
  ImGui::SameLine();
  ImGui::Checkbox("Hex", &st.overlays.hex);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &st.overlays.grid);
  ImGui::SameLine();
  ImGui::Checkbox("Silhouette", &st.silhouette);
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

  ViewTexture tex;
  tex.device = device;
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
      st.content = assets::load_view(*sv.ws, st.selected);
      // A viewed LOD follows its row: after a regeneration, show its new triangle count.
      workspace::Item item;
      if (st.selected_lod >= 0 && sv.ws->find(st.selected, item) && item.data.contains("recommended")) {
        const auto &lods = item.data["recommended"].value("lods", nlohmann::json::array());
        if (st.selected_lod < static_cast<int>(lods.size()))
          st.triangles = lods[st.selected_lod].value("triangles", st.triangles);
      }
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
    draw_viewport_controls(st);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int side = std::max(64, static_cast<int>(std::min(avail.x, avail.y)));
    int px = std::min(side, 900);
    if (st.content.empty()) {
      ImGui::TextDisabled("Nothing to show. Select a fixed asset, concept, or conformance shape.");
    } else {
      // While dragging or zooming, render a quarter of the pixels; sharpen once idle.
      const bool interacting = SDL_GetTicks() < st.interact_until;
      const int rpx = interacting ? std::max(64, px / 2) : px;
      std::string key = render_key(st, rpx, rpx);
      if (key != st.rendered_key) {
        st.pending = assets::render_view(st.content, st.camera, st.overlays, st.silhouette, rpx, rpx, st.triangles);
        st.has_pending = true;
        st.rendered_key = key;
      }
      if (tex.texture) {
        float aspect = tex.height > 0 ? static_cast<float>(tex.width) / static_cast<float>(tex.height) : 1;
        ImVec2 size = aspect >= 1 ? ImVec2(static_cast<float>(side), side / aspect) : ImVec2(side * aspect, static_cast<float>(side));
        ImGui::Image(reinterpret_cast<ImTextureID>(tex.texture), size);
        if (ImGui::IsItemHovered() && !st.content.image) {
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
  }

  capture().set_ui_running(false);
  SDL_WaitForGPUIdle(device);
  tex.release();
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
  SDL_ReleaseWindowFromGPUDevice(device, window);
  SDL_DestroyGPUDevice(device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace app
