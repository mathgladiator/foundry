#include "assets/quality.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "assets/lod.h"

namespace assets {

QualityJudge::QualityJudge(const runtime::Mesh &reference, int resolution) : res_(resolution) {
  std::vector<std::array<float, 3>> fwd = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const float d = 1.0f / std::sqrt(3.0f);
  for (int sx : {-1, 1})
    for (int sy : {-1, 1})
      for (int sz : {-1, 1}) fwd.push_back({sx * d, sy * d, sz * d});
  for (const auto &f : fwd) {
    std::array<float, 3> up = std::fabs(f[1]) > 0.9f ? std::array<float, 3>{0, 0, 1} : std::array<float, 3>{0, 1, 0};
    std::array<float, 3> r = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float &v : r) v /= rl;
    std::array<float, 3> u = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    dirs_.push_back({r[0], r[1], r[2], u[0], u[1], u[2], f[0], f[1], f[2]});
  }
  if (!reference.positions.empty()) {
    foundry::vec3 lo = reference.positions[0], hi = lo;
    for (const auto &p : reference.positions) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    cx_ = 0.5f * (lo.x + hi.x);
    cy_ = 0.5f * (lo.y + hi.y);
    cz_ = 0.5f * (lo.z + hi.z);
    float radius = 0;
    for (const auto &p : reference.positions)
      radius = std::max(radius, std::sqrt((p.x - cx_) * (p.x - cx_) + (p.y - cy_) * (p.y - cy_) + (p.z - cz_) * (p.z - cz_)));
    scale_ = 0.5f * res_ / std::max(radius * 1.05f, 1e-6f);
  }
  for (size_t v = 0; v < dirs_.size(); ++v) {
    ref_.push_back(render(reference, v));
    for (uint8_t b : ref_.back().mask) ref_pixels_ += b;
  }
}

QualityJudge::Frame QualityJudge::render(const runtime::Mesh &mesh, size_t view) const {
  Frame f;
  const size_t px = static_cast<size_t>(res_) * res_;
  f.mask.assign(px, 0);
  f.color.assign(px, {0, 0, 0});
  std::vector<float> depth(px, std::numeric_limits<float>::infinity());
  const runtime::Mesh &list = mesh;
  const auto &d = dirs_[view];
  const size_t n = list.positions.size();
  const bool has_c = list.colors.size() == n;
  std::vector<float> sx(n), sy(n), sz(n);
  const float half = 0.5f * res_;
  for (size_t i = 0; i < n; ++i) {
    float x = list.positions[i].x - cx_, y = list.positions[i].y - cy_, z = list.positions[i].z - cz_;
    sx[i] = half + (x * d[0] + y * d[1] + z * d[2]) * scale_;
    sy[i] = half - (x * d[3] + y * d[4] + z * d[5]) * scale_;
    sz[i] = x * d[6] + y * d[7] + z * d[8];  // depth along the view direction (the camera looks along +forward)
  }
  auto rgb = [&](uint32_t i) {
    uint32_t c = has_c ? list.colors[i] : 0xffb4b4b4u;
    return std::array<float, 3>{(c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f};
  };
  for (size_t t = 0; t + 2 < list.indices.size(); t += 3) {
    uint32_t i0 = list.indices[t], i1 = list.indices[t + 1], i2 = list.indices[t + 2];
    if (i0 >= n || i1 >= n || i2 >= n) continue;
    float ax = sx[i0], ay = sy[i0], bx = sx[i1], by = sy[i1], cx = sx[i2], cy = sy[i2];
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(area) < 1e-12f) continue;
    auto c0 = rgb(i0), c1 = rgb(i1), c2 = rgb(i2);
    int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    int x1 = std::min(res_ - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    int y1 = std::min(res_ - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        float pxf = x + 0.5f, pyf = y + 0.5f;
        float w0 = ((bx - pxf) * (cy - pyf) - (by - pyf) * (cx - pxf)) / area;
        float w1 = ((cx - pxf) * (ay - pyf) - (cy - pyf) * (ax - pxf)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        size_t i = static_cast<size_t>(y) * res_ + x;
        float z = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2];
        if (z >= depth[i]) continue;
        depth[i] = z;
        f.mask[i] = 1;
        for (int k = 0; k < 3; ++k) f.color[i][k] = w0 * c0[k] + w1 * c1[k] + w2 * c2[k];
      }
  }
  return f;
}

std::vector<QualityJudge::Frame> QualityJudge::render_views(const runtime::Mesh &m) const {
  std::vector<Frame> out;
  for (size_t v = 0; v < dirs_.size(); ++v) out.push_back(render(m, v));
  return out;
}

ImageError QualityJudge::error(const runtime::Mesh &candidate) const {
  ImageError e;
  if (ref_pixels_ <= 0) return e;
  const double kNorm = 1.0 / std::sqrt(3.0);
  for (size_t v = 0; v < dirs_.size(); ++v) {
    Frame f = render(candidate, v);
    const Frame &r = ref_[v];
    for (size_t i = 0; i < f.mask.size(); ++i) {
      if (f.mask[i] != r.mask[i]) {
        e.silhouette += 1;
      } else if (f.mask[i]) {
        double dr = f.color[i][0] - r.color[i][0], dg = f.color[i][1] - r.color[i][1], db = f.color[i][2] - r.color[i][2];
        double dist = std::sqrt(dr * dr + dg * dg + db * db) * kNorm;
        e.color += std::min(1.0, dist / kColorScale);
      }
    }
  }
  e.silhouette /= ref_pixels_;
  e.color /= ref_pixels_;
  return e;
}

DistanceProfile::DistanceProfile(const runtime::Mesh &full, double fov_y, int screen_height) : full_(full) {
  if (full.positions.empty()) return;
  foundry::vec3 lo = full.positions[0], hi = lo;
  for (const auto &p : full.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const double cx = 0.5 * (lo.x + hi.x), cy = 0.5 * (lo.y + hi.y), cz = 0.5 * (lo.z + hi.z);
  double r = 0;
  for (const auto &p : full.positions)
    r = std::max(r, std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) + (p.z - cz) * (p.z - cz)));
  frame_m_ = 2 * r * 1.05;
  height_m_ = hi.y - lo.y;
  px_per_m_at_1m_ = screen_height / (2 * std::tan(fov_y / 2));
  judge_ = std::make_unique<QualityJudge>(full, 512);
  ref_ = chain(full);
  if (ref_.empty()) return;
  for (const Level &l : ref_[0]) {
    double distance = frame_m_ * px_per_m_at_1m_ / l.res;
    distances_.push_back(distance);
    pixels_.push_back(height_m_ * px_per_m_at_1m_ / distance);
  }
}

namespace {

// Box-filters a frame by an integer factor: coverage averages; color averages over the covered part.
template <typename Level, typename Frame>
Level box(const Frame &f, int res_in, int factor) {
  Level n;
  n.res = res_in / factor;
  n.coverage.assign(static_cast<size_t>(n.res) * n.res, 0);
  n.color.assign(n.coverage.size(), {0, 0, 0});
  for (int y = 0; y < n.res; ++y)
    for (int x = 0; x < n.res; ++x) {
      float cov = 0;
      std::array<float, 3> col = {0, 0, 0};
      for (int dy = 0; dy < factor; ++dy)
        for (int dx = 0; dx < factor; ++dx) {
          size_t i = static_cast<size_t>(y * factor + dy) * res_in + (x * factor + dx);
          float c = f.coverage_at(i);
          cov += c;
          for (int k = 0; k < 3; ++k) col[k] += f.color[i][k] * c;
        }
      size_t o = static_cast<size_t>(y) * n.res + x;
      n.coverage[o] = cov / static_cast<float>(factor * factor);
      if (cov > 0)
        for (int k = 0; k < 3; ++k) n.color[o][k] = col[k] / cov;
    }
  return n;
}

// Adapters so box() reads both a point-sampled frame and a filtered level.
struct FrameView {
  const QualityJudge::Frame &f;
  const std::vector<std::array<float, 3>> &color;
  float coverage_at(size_t i) const { return f.mask[i]; }
};
template <typename Level>
struct LevelView {
  const Level &l;
  const std::vector<std::array<float, 3>> &color;
  float coverage_at(size_t i) const { return l.coverage[i]; }
};

}  // namespace

std::vector<std::vector<DistanceProfile::Level>> DistanceProfile::chain(const runtime::Mesh &m) const {
  std::vector<std::vector<Level>> out;
  for (const auto &f : judge_->render_views(m)) {
    std::vector<Level> levels;
    levels.push_back(box<Level>(FrameView{f, f.color}, judge_->resolution(), 1));
    while (levels.back().res > 4) {
      const Level &p = levels.back();
      levels.push_back(box<Level>(LevelView<Level>{p, p.color}, p.res, 2));
    }
    out.push_back(std::move(levels));
  }
  return out;
}

void DistanceProfile::compare(const Level &a, const Level &b, double &wrong, double &disagree, double &object) {
  const double kNorm = 1.0 / std::sqrt(3.0);
  for (size_t i = 0; i < a.coverage.size(); ++i) {
    double d = std::fabs(a.coverage[i] - b.coverage[i]);
    float both = std::min(a.coverage[i], b.coverage[i]);
    if (both > 0) {
      double dr = a.color[i][0] - b.color[i][0], dg = a.color[i][1] - b.color[i][1], db = a.color[i][2] - b.color[i][2];
      d += both * std::min(1.0, std::sqrt(dr * dr + dg * dg + db * db) * kNorm / QualityJudge::kColorScale);
    }
    wrong += d;
    if (a.coverage[i] > 0 || b.coverage[i] > 0) {
      object += 1;
      if (d > kDisagree) disagree += 1;
    }
  }
}

std::vector<DistanceSample> DistanceProfile::curve(const runtime::Mesh &lod) const {
  std::vector<DistanceSample> out;
  if (!judge_) return out;
  auto cand = chain(lod);
  for (size_t k = 0; k < distances_.size(); ++k) {
    double wrong = 0, disagree = 0, object = 0, ref_cover = 0;
    for (size_t v = 0; v < ref_.size(); ++v) {
      compare(ref_[v][k], cand[v][k], wrong, disagree, object);
      for (float c : ref_[v][k].coverage) ref_cover += c;
    }
    out.push_back({distances_[k], pixels_[k], ref_cover > 0 ? wrong / ref_cover : 0,
                   wrong / static_cast<double>(ref_.size()), object > 0 ? disagree / object : 0});
  }
  return out;
}

double DistanceProfile::share_at(const runtime::Mesh &lod, int res) const {
  // Supersample to about the same 512-pixel render the curve filters down from,
  // so bisection and the curve measure the same way.
  const int ss = std::max(1, (512 + res - 1) / res);
  QualityJudge judge(full_, res * ss);
  auto ref = judge.render_views(full_), cand = judge.render_views(lod);
  double wrong = 0, disagree = 0, object = 0;
  for (size_t v = 0; v < ref.size(); ++v) {
    Level a = box<Level>(FrameView{ref[v], ref[v].color}, res * ss, ss);
    Level b = box<Level>(FrameView{cand[v], cand[v].color}, res * ss, ss);
    compare(a, b, wrong, disagree, object);
  }
  return object > 0 ? disagree / object : 0;
}

double DistanceProfile::switch_distance(const runtime::Mesh &lod, const std::vector<DistanceSample> &c, double share,
                                        double &pixels, bool &never_reached) const {
  never_reached = false;
  pixels = 0;
  if (c.empty()) return 0;
  // The first sample, going outward, under the share.
  size_t first = c.size();
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i].disagree < share) {
      first = i;
      break;
    }
  if (first == 0) {
    pixels = c[0].pixels;
    return 0;
  }
  if (first == c.size()) {
    never_reached = true;
    pixels = c.back().pixels;
    return c.back().distance_m;
  }
  // Bisect (in log distance) between the last sample over the share and the first under it.
  double near = c[first - 1].distance_m, far = c[first].distance_m;
  for (int step = 0; step < 6; ++step) {
    double mid = std::sqrt(near * far);
    int res = std::max(4, static_cast<int>(std::lround(frame_m_ * px_per_m_at_1m_ / mid)));
    if (share_at(lod, res) < share) far = mid;
    else near = mid;
  }
  pixels = height_m_ * px_per_m_at_1m_ / far;
  return far;
}

std::vector<QualityLod> quality_lods(const ProgressiveMesh &pm, const std::vector<double> &thresholds,
                                     const std::function<void(float, const std::string &)> &progress) {
  std::vector<QualityLod> out;
  if (pm.level_triangles.empty()) return out;
  QualityJudge judge(extract_level(pm, 0));
  const size_t last = pm.level_triangles.size() - 1;
  std::vector<ImageError> cache(last + 1);
  std::vector<bool> known(last + 1, false);
  auto err = [&](size_t level) {
    if (!known[level]) {
      cache[level] = level == 0 ? ImageError{} : judge.error(extract_level(pm, level));
      known[level] = true;
    }
    return cache[level].total();
  };
  size_t lo_bound = 0;
  for (size_t k = 0; k < thresholds.size(); ++k) {
    const double t = thresholds[k];
    size_t lo = lo_bound, hi = last;
    if (err(hi) <= t) lo = hi;
    while (hi - lo > 1) {
      size_t mid = lo + (hi - lo) / 2;
      if (err(mid) <= t) lo = mid;
      else hi = mid;
      if (progress) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "LOD%zu (<%.0f%%): %u triangles is %.2f%% (shape %.2f%%, color %.2f%%)", k,
                      t * 100, pm.level_triangles[mid], cache[mid].total() * 100, cache[mid].silhouette * 100,
                      cache[mid].color * 100);
        progress((static_cast<float>(k) + 0.5f) / thresholds.size(), buf);
      }
    }
    QualityLod q;
    q.max_error = t;
    q.level = lo;
    q.triangles = pm.level_triangles[lo];
    err(lo);
    q.error = cache[lo];
    out.push_back(q);
    lo_bound = lo;
  }
  return out;
}

}  // namespace assets
