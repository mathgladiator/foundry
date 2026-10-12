// The QA video: every combination of a pipeline's discrete parameters (enums
// and config entries), times a number of samples of its continuous ones, one clip
// each, behind a title slide. Frames are rendered off screen by the native
// runtime, titled here, and piped to ffmpeg as H.264 in MP4 (yuv420p, CRF 20,
// faststart): the codec every player and browser takes, at a fraction of the
// size of a lossless or MJPEG file. ffmpeg runs as its own process, so its
// encoders never link into the foundry.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "platform.h"
#include "text.h"

namespace stress {

namespace {

constexpr float kFov = 50.0f * 3.14159265f / 180.0f;

std::string pick_encoder() {
  std::string out;
  FILE *p = popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
  if (!p) return "";
  char buf[512];
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  if (out.empty()) return "";
  return out.find("libx264") != std::string::npos ? "-c:v libx264 -preset medium -crf 20" : "-c:v mpeg4 -q:v 3";
}

std::string quote(const std::string &s) {
  std::string o = "'";
  for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return o + "'";
}

// The clear colour becomes sky above the horizon and ground below it, so trees stand on something.
void backdrop(Canvas &c, float pitch_down) {
  const float f = (c.height * 0.5f) / std::tan(kFov * 0.5f);
  const float horizon = c.height * 0.5f - f * std::tan(pitch_down);
  for (int y = 0; y < c.height; ++y) {
    Rgba col;
    if (y < horizon) {
      const float t = std::clamp(y / std::max(1.0f, horizon), 0.0f, 1.0f);
      col = {static_cast<uint8_t>(110 + 70 * t), static_cast<uint8_t>(150 + 55 * t), static_cast<uint8_t>(205 + 30 * t), 255};
    } else {
      const float t = std::clamp((y - horizon) / std::max(1.0f, c.height - horizon), 0.0f, 1.0f);
      col = {static_cast<uint8_t>(150 - 45 * t), static_cast<uint8_t>(160 - 40 * t), static_cast<uint8_t>(110 - 35 * t), 255};
    }
    uint8_t *row = &c.px[static_cast<size_t>(y) * c.width * 4];
    for (int x = 0; x < c.width; ++x) {
      uint8_t *p = row + x * 4;
      // The clear colour (0.55, 0.68, 0.82) as RGBA8.
      if (std::abs(p[0] - 140) <= 1 && std::abs(p[1] - 173) <= 1 && std::abs(p[2] - 209) <= 1) {
        p[0] = col.r;
        p[1] = col.g;
        p[2] = col.b;
      }
    }
  }
}

std::string value_text(const stress_param &p, stress_value v) {
  if (p.kind == STRESS_ENUM) return p.values[std::min(v.u, p.count - 1)];
  if (p.kind == STRESS_U32) return std::to_string(v.u);
  return fmt("%.2f", v.f);
}

}  // namespace

int run_video(Ctx &ctx, const VideoOptions &o) {
  const stress_pipeline &pl = *ctx.pipeline;
  const std::string encoder = pick_encoder();
  if (encoder.empty()) {
    std::fprintf(stderr, "video: ffmpeg isn't on the PATH; install it to export video\n");
    return 2;
  }
  // The variations: every discrete combination (honouring the plan's fixed and ranges), times `floats`.
  std::vector<uint32_t> discrete;
  std::vector<std::vector<uint32_t>> choices;
  for (uint32_t i = 0; i < pl.param_count; ++i) {
    const auto &p = pl.params[i];
    if (p.kind != STRESS_ENUM) continue;
    std::vector<uint32_t> vals;
    if (ctx.plan.fixed.contains(p.name)) {
      Rng r(1);
      vals.push_back(ctx.random_value(i, r).u);
    } else if (ctx.plan.ranges.contains(p.name)) {
      for (const auto &v : ctx.plan.ranges[p.name])
        for (uint32_t k = 0; k < p.count; ++k)
          if (v == p.values[k]) vals.push_back(k);
    }
    if (vals.empty())
      for (uint32_t k = 0; k < p.count; ++k) vals.push_back(k);
    discrete.push_back(i);
    choices.push_back(vals);
  }
  std::vector<Config> variations;
  Rng rng(ctx.plan.seed, 71);
  std::vector<size_t> at(discrete.size(), 0);
  for (bool done = false; !done;) {
    for (int k = 0; k < o.floats; ++k) {
      Config c = ctx.fallback();
      for (uint32_t i = 0; i < pl.param_count; ++i) {
        const auto kind = pl.params[i].kind;
        if (kind == STRESS_U32 || kind == STRESS_F32) c[i] = ctx.random_value(i, rng);
      }
      for (size_t d = 0; d < discrete.size(); ++d) c[discrete[d]].u = choices[d][at[d]];
      variations.push_back(c);
    }
    done = true;
    for (size_t d = discrete.size(); d-- > 0;) {
      if (++at[d] < choices[d].size()) {
        done = false;
        break;
      }
      at[d] = 0;
    }
    if (discrete.empty()) break;
  }
  const int W = static_cast<int>(ctx.plan.width), H = static_cast<int>(ctx.plan.height);
  const int clip_frames = std::max(1, static_cast<int>(o.seconds * o.fps + 0.5));
  const int title_frames = static_cast<int>(o.title_seconds * o.fps + 0.5);
  const double total_seconds = o.title_seconds + variations.size() * o.seconds;
  std::printf("video: %zu variations (%zu discrete combinations x %d), %.0f s at %d fps, %dx%d\n", variations.size(),
              variations.size() / std::max(1, o.floats), o.floats, total_seconds, o.fps, W, H);
  std::fflush(stdout);

  const std::string cmd = fmt("ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - %s "
                              "-pix_fmt yuv420p -movflags +faststart ",
                              W, H, o.fps, encoder.c_str()) + quote(o.path);
  FILE *ff = popen(cmd.c_str(), "w");
  if (!ff) {
    std::fprintf(stderr, "video: can't start ffmpeg\n");
    return 2;
  }
  Canvas c;
  c.width = W;
  c.height = H;

  // The title slide.
  {
    c.px.assign(static_cast<size_t>(W) * H * 4, 255);
    for (int y = 0; y < H; ++y) {
      const float t = static_cast<float>(y) / H;
      const Rgba col = {static_cast<uint8_t>(24 + 16 * t), static_cast<uint8_t>(28 + 22 * t), static_cast<uint8_t>(40 + 30 * t), 255};
      c.fill_rect(0, y, W, 1, col);
    }
    const float big = H * 0.13f, mid = H * 0.055f, small = H * 0.03f;
    const std::string title = "Foundry QA", sub = std::string("Pipeline '") + pl.name + "' Demo";
    draw_text(c, (W - text_width(big, title)) / 2, static_cast<int>(H * 0.32f), big, title, {255, 255, 255, 255});
    draw_text(c, (W - text_width(mid, sub)) / 2, static_cast<int>(H * 0.32f + big * 1.25f), mid, sub, {255, 205, 120, 255});
    char date[32];
    std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    std::string info = fmt("%zu variations, %.1f s each  |  %s  |  %s", variations.size(), o.seconds, pl.state, date);
    draw_text(c, (W - text_width(small, info)) / 2, static_cast<int>(H * 0.75f), small, info, {190, 198, 215, 255});
    std::string sys;
    for (uint32_t i = 0; i < pl.system_count; ++i) sys += (i ? "  >  " : "") + std::string(pl.systems[i]);
    draw_text(c, (W - text_width(small * 0.85f, sys)) / 2, static_cast<int>(H * 0.75f + small * 1.6f), small * 0.85f, sys,
              {150, 158, 178, 255});
    for (int f = 0; f < title_frames; ++f) fwrite(c.px.data(), 1, c.px.size(), ff);
  }

  // One clip per variation.
  json index = json::array();
  const float pitch = 0.22f;
  for (size_t v = 0; v < variations.size(); ++v) {
    const Config &cfg = variations[v];
    float m[16];
    translation(0, 0, 0, m);
    foundry_instance h = ctx.spawn(cfg, m);
    foundry_diag_instance info = {};
    for (int w = 0; w < 6000; ++w) {  // wait for the build without moving time
      foundry_diag_instance_get(ctx.rt, h, &info);
      if (info.ready || info.failed) break;
      foundry_update(ctx.rt, 0);
      SDL_Delay(1);
    }
    // Frame the bounding sphere, orbiting a quarter turn over the clip.
    const float R = std::max(0.5f, info.radius), cy = info.height * 0.5f;
    const float dist = R / std::sin(kFov * 0.5f) * 1.08f;
    std::string title;
    for (uint32_t d : discrete) title += (title.empty() ? "" : "  |  ") + value_text(pl.params[d], cfg[d]);
    std::vector<std::string> lines;
    for (uint32_t i = 0; i < pl.param_count; ++i) {
      const auto &p = pl.params[i];
      if (p.kind == STRESS_CLOCK || p.kind == STRESS_EVENT) continue;
      lines.push_back(std::string(p.name) + ": " + value_text(p, cfg[i]));
    }
    lines.push_back(fmt("height: %.1f m   triangles: %u", info.height, info.triangles));
    if (info.failed) lines.push_back(std::string("FAILED: ") + info.error);
    index.push_back({{"variation", v + 1}, {"start_seconds", o.title_seconds + v * o.seconds}, {"values", ctx.describe(cfg)}});
    for (int f = 0; f < clip_frames; ++f) {
      const float yaw = 0.6f + 1.4f * static_cast<float>(v) + 0.5f * static_cast<float>(f) / clip_frames;
      const float eye[3] = {dist * std::sin(yaw) * std::cos(pitch), cy + dist * std::sin(pitch), dist * std::cos(yaw) * std::cos(pitch)};
      const float target[3] = {0, cy, 0};
      ctx.camera(eye, target, dist * 4 + 50);
      ctx.frame();
      ctx.read_frame(c.px);
      backdrop(c, pitch);
      const float big = H * 0.05f, small = H * 0.026f;
      const int pad = static_cast<int>(H * 0.03f);
      draw_text(c, pad, pad, big, title, {255, 255, 255, 255});
      const int box_y = pad + static_cast<int>(big * 1.5f);
      int box_w = 0;
      for (const auto &l : lines) box_w = std::max(box_w, text_width(small, l));
      c.fill_rect(pad - 8, box_y - 6, box_w + 16, static_cast<int>(lines.size() * small * 1.3f) + 12, {0, 0, 0, 110});
      for (size_t k = 0; k < lines.size(); ++k)
        draw_text(c, pad, box_y + static_cast<int>(k * small * 1.3f), small, lines[k], {235, 238, 245, 255}, false);
      const std::string counter = fmt("%zu / %zu", v + 1, variations.size());
      draw_text(c, W - pad - text_width(small, counter), H - pad - static_cast<int>(small), small, counter, {220, 225, 235, 255});
      const std::string name = std::string("Foundry QA  |  ") + pl.name;
      draw_text(c, pad, H - pad - static_cast<int>(small), small, name, {200, 205, 220, 255});
      fwrite(c.px.data(), 1, c.px.size(), ff);
    }
    ctx.destroy(h);
    if ((v + 1) % 10 == 0 || v + 1 == variations.size()) {
      std::printf("video: %zu/%zu variations\n", v + 1, variations.size());
      std::fflush(stdout);
    }
  }
  const int status = pclose(ff);
  if (status != 0) {
    std::fprintf(stderr, "video: ffmpeg failed (%d)\n", status);
    return 1;
  }
  // The index beside the video: what each second shows, and how it was made.
  json sidecar = {{"kind", "qa_video"},
                  {"pipeline", pl.name},
                  {"state", pl.state},
                  {"variations", variations.size()},
                  {"floats_per_combination", o.floats},
                  {"seconds_per_variation", o.seconds},
                  {"title_seconds", o.title_seconds},
                  {"fps", o.fps},
                  {"resolution", {W, H}},
                  {"codec", encoder.find("libx264") != std::string::npos ? "H.264 (libx264, CRF 20), yuv420p, MP4" : "MPEG-4 Part 2, MP4"},
                  {"seed", ctx.plan.seed},
                  {"index", index}};
  std::ofstream(o.path + ".json") << sidecar.dump(1) << "\n";
  std::printf("video: wrote %s (%.0f s) and its index %s.json\n", o.path.c_str(), total_seconds, o.path.c_str());
  return 0;
}

}  // namespace stress
