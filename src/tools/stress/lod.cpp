// Detail sizes (design/quality.perf.md, "Detail sizes"): for each coarser level
// of detail, the size on screen below which it looks like the finest, measured
// the way the foundry measures a fixed asset's switch distances.
//
// For sampled configurations at rest (no wind), each level is drawn at a ladder
// of sizes (the projected diameter of the bounding sphere, in pixels of a 1080
// pixel tall view) from two views, and compared with the finest level drawn the
// same way. Each is drawn at four times the size and box-filtered down, so a
// pixel holds the colour of what it covers, as the eye averages it; a pixel
// disagrees when that colour moves by more than a tenth of the colour cube's
// diagonal. A level's switch size for an allowance (a share of the pixels that
// show the thing) is the largest size at which it, and every smaller size,
// stays within it; across configurations, the median. The runtime compiles them
// in (the pipeline's draw.detail_pixels) and picks levels with them, by quality:
// high allows 10% (the foundry's default switch threshold for a fixed asset's
// levels), medium 20%, low 30%.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "platform.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace stress {

Diff image_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, std::vector<uint8_t> *mask) {
  Diff d;
  if (a.size() != b.size() || a.empty()) return d;
  const int sky[3] = {140, 173, 209};  // the clear colour
  auto is_sky = [&](const uint8_t *p) {
    return std::abs(p[0] - sky[0]) <= 2 && std::abs(p[1] - sky[1]) <= 2 && std::abs(p[2] - sky[2]) <= 2;
  };
  const double limit = 0.1 * 255.0 * std::sqrt(3.0);
  uint64_t wrong = 0, object = 0, object_wrong = 0;
  const size_t n = a.size() / 4;
  if (mask) mask->assign(a.size(), 0);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *pa = &a[i * 4], *pb = &b[i * 4];
    const double dr = pa[0] - pb[0], dg = pa[1] - pb[1], db = pa[2] - pb[2];
    const bool bad = std::sqrt(dr * dr + dg * dg + db * db) > limit;
    const bool obj = !is_sky(pa) || !is_sky(pb);
    wrong += bad;
    object += obj;
    object_wrong += bad && obj;
    if (mask) {
      uint8_t *m = &(*mask)[i * 4];
      const uint8_t g = static_cast<uint8_t>((pa[0] + pa[1] + pa[2]) / 6);
      m[0] = bad ? 255 : g;
      m[1] = bad ? 32 : g;
      m[2] = bad ? 32 : g;
      m[3] = 255;
    }
  }
  d.image = 100.0 * static_cast<double>(wrong) / static_cast<double>(n);
  d.object = object ? 100.0 * static_cast<double>(object_wrong) / static_cast<double>(object) : 0;
  d.object_pixels = object;
  return d;
}

namespace {

// A box filter: each output pixel is the mean of a factor x factor block (what the eye averages
// over a pixel, where the renderer takes one sample).
std::vector<uint8_t> downsample(const std::vector<uint8_t> &px, uint32_t w, uint32_t h, uint32_t f, uint32_t &ow,
                                uint32_t &oh) {
  ow = w / f;
  oh = h / f;
  std::vector<uint8_t> out(size_t(ow) * oh * 4);
  for (uint32_t y = 0; y < oh; ++y)
    for (uint32_t x = 0; x < ow; ++x)
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < f; ++dy)
          for (uint32_t dx = 0; dx < f; ++dx) sum += px[(size_t(y * f + dy) * w + (x * f + dx)) * 4 + c];
        out[(size_t(y) * ow + x) * 4 + c] = static_cast<uint8_t>((sum + f * f / 2) / (f * f));
      }
  return out;
}

}  // namespace

int run_lod(Ctx &ctx, const LodOptions &o) {
  const stress_pipeline &pl = *ctx.pipeline;
  const int detail = ctx.param("detail"), wind = ctx.param("wind_speed");
  if (detail < 0 || pl.params[detail].kind != STRESS_ENUM || pl.params[detail].count < 2) {
    std::cerr << "stress: " << pl.name << " has no detail levels\n";
    return 2;
  }
  // The pipeline's levels, then the runtime's far level (one past the coarsest).
  const uint32_t levels = std::min<uint32_t>(pl.params[detail].count, 3) + 1;
  if (wind >= 0) ctx.plan.fixed["wind_speed"] = 0.0;  // at rest: only the level differs
  foundry_diag_set_cost(ctx.rt, FOUNDRY_DIAG_COST_DETAIL);  // each instance is forced to its level
  Rng rng(ctx.plan.seed, 23);
  auto configs = sample(ctx, static_cast<size_t>(o.configs), rng);
  // Drawn at four times the size and filtered down by four, so a pixel holds what it covers.
  const std::vector<double> sizes = {8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256};
  const uint32_t ss = 4;
  const double allowances[3] = {10, 20, 30};
  const char *quality_names[3] = {"high", "medium", "low"};  // which allowance each quality uses
  // Two views: low and side-on, and raised three-quarters.
  const float views[2][2] = {{0.17f, 0.5f}, {0.6f, 2.4f}};  // elevation, azimuth (radians)
  if (!o.snapshot_dir.empty()) fs::create_directories(o.snapshot_dir);

  std::cout << pl.name << "  detail sizes  " << configs.size() << " configs x " << sizes.size() << " sizes x 2 views\n";
  // switch_px[level][allowance]: per config.
  std::vector<std::vector<std::vector<double>>> switch_px(levels, std::vector<std::vector<double>>(3));
  json curves = json::array();
  const double dt = ctx.plan.dt;
  ctx.plan.dt = 0;
  for (size_t ci = 0; ci < configs.size(); ++ci) {
    // One instance per level, far apart, so each is drawn alone.
    std::vector<foundry_instance> inst(levels);
    Config c = configs[ci];
    c[detail].u = 0;  // the game asks for the finest; the runtime draws each at its forced level
    // A field pipeline is measured as a field: a patch of 37 cells (a hex of radius 3), each showing
    // the tile its cell chooses, sized as one tile (its radius, 1.5 m, as the runtime judges fields).
    std::vector<foundry_field_cell> patch;
    if (pl.spawn_field)
      for (int q = -3; q <= 3; ++q)
        for (int rr = -3; rr <= 3; ++rr)
          if (std::abs(q) + std::abs(rr) + std::abs(q + rr) <= 6) patch.push_back({q, rr, FOUNDRY_GROUND_SOIL, 0.55f, 0.0f});
    for (uint32_t l = 0; l < levels; ++l) {
      float t[16];
      translation(5000.0f * static_cast<float>(l), 0, 0, t);
      inst[l] = pl.spawn_field ? pl.spawn_field(ctx.rt, c.data(), t, patch.data(), static_cast<uint32_t>(patch.size()))
                               : ctx.spawn(c, t);
      foundry_diag_force_level(ctx.rt, inst[l], static_cast<int>(l));
    }
    float r = 0, cy = 0;
    std::vector<uint32_t> tris(levels, 0);
    // Each one is drawn at its level once its build is ready (and checked to pose like the finest):
    // look at each in turn until all are.
    for (int k = 0; k < 600; ++k) {
      bool ready = true;
      for (uint32_t l = 0; l < levels; ++l) {
        foundry_diag_instance info;
        if (foundry_diag_instance_get(ctx.rt, inst[l], &info) != FOUNDRY_OK || !info.ready) {
          ready = false;
          continue;
        }
        if (l == 0) {
          r = pl.spawn_field ? 1.5f : info.radius;
          cy = pl.spawn_field ? 0.3f : info.height * 0.5f;
        }
        const float ox = 5000.0f * static_cast<float>(l);
        const float eye[3] = {ox, cy + 2 * r, 4 * r + 2}, target[3] = {ox, cy, 0};
        ctx.camera(eye, target, 10 * r + 20);
        ctx.frame(true);
        foundry_diag_instance_get(ctx.rt, inst[l], &info);
        ready = ready && info.drawn && info.level == l;
        if (info.drawn) tris[l] = pl.spawn_field ? info.drawn_triangles / static_cast<uint32_t>(patch.size()) : info.drawn_triangles;
      }
      if (ready) break;
      SDL_Delay(2);
      ctx.frame(false);  // builds finish in the update
    }
    std::vector<std::vector<double>> dis(levels, std::vector<double>(sizes.size(), 0));
    for (size_t si = 0; si < sizes.size(); ++si) {
      for (const auto &v : views) {
        // The distance at which the bounding sphere's diameter is this many pixels.
        const float p11 = 1.0f / std::tan(25.0f * 3.14159265f / 180.0f);
        const float d = static_cast<float>(ctx.plan.height) * r * p11 / static_cast<float>(sizes[si] * ss);
        std::vector<std::vector<uint8_t>> px(levels);
        for (uint32_t l = 0; l < levels; ++l) {
          const float ox = 5000.0f * static_cast<float>(l);
          const float eye[3] = {ox + d * std::cos(v[0]) * std::sin(v[1]), cy + d * std::sin(v[0]), d * std::cos(v[0]) * std::cos(v[1])};
          const float target[3] = {ox, cy, 0};
          ctx.camera(eye, target, d + 4 * r + 10);
          ctx.frame(true);
          std::vector<uint8_t> full;
          ctx.read_frame(full);
          uint32_t ow = 0, oh = 0;
          px[l] = downsample(full, ctx.plan.width, ctx.plan.height, ss, ow, oh);
        }
        for (uint32_t l = 1; l < levels; ++l) {
          std::vector<uint8_t> mask;
          const bool keep = !o.snapshot_dir.empty() && ci < 4;
          const Diff df = image_diff(px[0], px[l], keep ? &mask : nullptr);
          dis[l][si] += 0.5 * df.object;
          if (keep && &v == &views[1])
            stbi_write_png(fmt("%s/lod-%zu-%dpx-level%u.png", o.snapshot_dir.c_str(), ci, static_cast<int>(sizes[si]), l).c_str(),
                           static_cast<int>(ctx.plan.width / ss), static_cast<int>(ctx.plan.height / ss), 4, mask.data(),
                           static_cast<int>(ctx.plan.width / ss * 4));
        }
      }
    }
    json cj = {{"config", ctx.describe(configs[ci])}, {"radius", r}, {"triangles", json::array()}, {"disagree_pct", json::array()}};
    for (uint32_t l = 0; l < levels; ++l) {
      cj["triangles"].push_back(tris[l]);
      json row = json::array();
      for (double x : dis[l]) row.push_back(std::round(x * 100) / 100);
      cj["disagree_pct"].push_back(row);
      if (l == 0) continue;
      for (int a = 0; a < 3; ++a) {
        // The largest size at which it, and every smaller size, stays within the allowance.
        double best = 0;
        for (size_t si = 0; si < sizes.size(); ++si) {
          if (dis[l][si] > allowances[a]) break;
          best = sizes[si];
        }
        switch_px[l][static_cast<size_t>(a)].push_back(best);
      }
    }
    curves.push_back(cj);
    for (auto h : inst) ctx.destroy(h);
    for (int k = 0; k < 4; ++k) ctx.frame(false);
    std::cout << fmt("  %2zu/%zu  r %.2f m  ", ci + 1, configs.size(), r);
    for (uint32_t l = 1; l < levels; ++l)
      std::cout << fmt("level %u at 10%%: %4.0f px  ", l, switch_px[l][0].back());
    std::cout << "\n" << std::flush;
  }
  ctx.plan.dt = dt;
  // What the median configuration reaches, per level and allowance; then per quality, the size below
  // which each level gives way to the next (a coarser level never switches in above a finer one).
  json out = json::object();
  for (int a = 0; a < 3; ++a) {
    json row = json::array();
    double above = 1e9;
    for (uint32_t l = 1; l < levels; ++l) {
      auto v = switch_px[l][static_cast<size_t>(a)];
      std::sort(v.begin(), v.end());
      double p50 = v.empty() ? 0 : v[(v.size() - 1) / 2];
      p50 = std::min(p50, above);
      above = p50;
      row.push_back(p50);
    }
    out[quality_names[a]] = row;
  }
  json report = {{"pipeline", pl.name},
                 {"kind", "detail_sizes"},
                 {"started", stamp_now(true)},
                 {"configs", configs.size()},
                 {"sizes_px", sizes},
                 {"levels", "the pipeline's levels, finest first, then the runtime's far level"},
                 {"allowance_pct", {{"high", 10}, {"medium", 20}, {"low", 30}}},
                 {"detail_pixels", out},
                 {"curves", curves}};
  std::cout << "detail_pixels: " << out.dump() << "\n";
  if (!o.report_path.empty()) {
    std::ofstream(o.report_path) << report.dump(1) << "\n";
    std::cout << "report: " << o.report_path << "\n";
  }
  return 0;
}

}  // namespace stress
