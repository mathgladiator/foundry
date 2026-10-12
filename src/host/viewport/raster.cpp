#include "viewport/raster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace viewport {

namespace {

using foundry::vec3;
constexpr float kPi = 3.14159265358979f;
constexpr int kSupersample = 2;

vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
vec3 normalize(vec3 v) {
  float l = std::sqrt(dot(v, v));
  return l > 1e-12f ? v * (1.0f / l) : vec3{0, 1, 0};
}

struct Rgb {
  float r = 0, g = 0, b = 0;
};

Rgb unpack_color(uint32_t c) {
  return {(c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f};
}

// style.md section 6.
const vec3 kLightDir = normalize({0.62f, 0.68f, 0.40f});
const Rgb kLight = {1.04f, 0.98f, 0.88f};
const Rgb kShadow = {0.50f, 0.52f, 0.72f};
const Rgb kDefaultAlbedo = {0.72f, 0.70f, 0.66f};

struct Projector {
  vec3 eye, right, up, forward;
  bool ortho = true;
  float scale = 1;  // pixels per metre (ortho) or focal length in pixels (perspective)
  float cx = 0, cy = 0;

  // Returns screen x, y and view depth.
  vec3 project(vec3 p) const {
    vec3 d = p - eye;
    float x = dot(d, right), y = dot(d, up), z = dot(d, forward);
    if (ortho) return {cx + x * scale, cy - y * scale, z};
    float iz = z > 1e-4f ? 1.0f / z : 1e4f;
    return {cx + x * scale * iz, cy - y * scale * iz, z};
  }
  vec3 to_camera(vec3 p) const { return ortho ? forward * -1.0f : normalize(eye - p); }
};

struct Bounds {
  vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  void add(vec3 p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  bool empty() const { return lo.x > hi.x; }
};

struct Target {
  int w, h;
  std::vector<Rgb> color;
  std::vector<float> depth;
  std::vector<uint8_t> id;  // 0 = background, 1 = mesh
  std::vector<Rgb> albedo;  // for ink color
  Target(int w_, int h_)
      : w(w_), h(h_), color(static_cast<size_t>(w_) * h_), depth(static_cast<size_t>(w_) * h_, std::numeric_limits<float>::infinity()),
        id(static_cast<size_t>(w_) * h_, 0), albedo(static_cast<size_t>(w_) * h_) {}
};

float hero_x(float asset_radius) { return -(std::max(asset_radius, 0.3f) + 0.35f + 0.35f); }

void draw_line(Target &t, const Projector &pr, vec3 a, vec3 b, Rgb c, float alpha, float hidden_alpha) {
  vec3 pa = pr.project(a), pb = pr.project(b);
  if (pa.z <= 1e-3f || pb.z <= 1e-3f) return;
  float len = std::hypot(pb.x - pa.x, pb.y - pa.y);
  int steps = std::max(1, static_cast<int>(len));
  for (int i = 0; i <= steps; ++i) {
    float s = static_cast<float>(i) / static_cast<float>(steps);
    float x = pa.x + (pb.x - pa.x) * s, y = pa.y + (pb.y - pa.y) * s, z = pa.z + (pb.z - pa.z) * s;
    for (int oy = 0; oy < kSupersample; ++oy)
      for (int ox = 0; ox < kSupersample; ++ox) {
        int px = static_cast<int>(x) + ox - kSupersample / 2, py = static_cast<int>(y) + oy - kSupersample / 2;
        if (px < 0 || py < 0 || px >= t.w || py >= t.h) continue;
        size_t i2 = static_cast<size_t>(py) * t.w + px;
        float a2 = z <= t.depth[i2] * 1.002f + 0.002f ? alpha : hidden_alpha;
        Rgb &d = t.color[i2];
        d = {d.r + (c.r - d.r) * a2, d.g + (c.g - d.g) * a2, d.b + (c.b - d.b) * a2};
      }
  }
}

void draw_circle(Target &t, const Projector &pr, vec3 centre, float r, Rgb c, float alpha, float hidden) {
  const int n = 48;
  for (int i = 0; i < n; ++i) {
    float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
    draw_line(t, pr, centre + vec3{r * std::cos(a0), 0, r * std::sin(a0)},
              centre + vec3{r * std::cos(a1), 0, r * std::sin(a1)}, c, alpha, hidden);
  }
}

// A capsule standing on the ground at `base`: rings, four sides, and the caps' arcs. When it is wider
// than it is tall (a rock, a shrub), the caps are squashed so it stays between the ground and h.
void draw_capsule(Target &t, const Projector &pr, vec3 base, float r, float h, Rgb c, float alpha, float hidden) {
  const float rv = std::min(r, h * 0.5f);  // the caps' height
  float lo = rv, hi = h - rv;
  draw_circle(t, pr, base + vec3{0, lo, 0}, r, c, alpha, hidden);
  draw_circle(t, pr, base + vec3{0, hi, 0}, r, c, alpha, hidden);
  for (int k = 0; k < 4; ++k) {
    float a = kPi * 0.5f * k;
    vec3 d{std::cos(a), 0, std::sin(a)};
    draw_line(t, pr, base + d * r + vec3{0, lo, 0}, base + d * r + vec3{0, hi, 0}, c, alpha, hidden);
    const int n = 12;
    for (int i = 0; i < n; ++i) {
      float b0 = 0.5f * kPi * i / n, b1 = 0.5f * kPi * (i + 1) / n;
      // Top and bottom quarter arcs.
      draw_line(t, pr, base + vec3{0, hi, 0} + d * (r * std::cos(b0)) + vec3{0, rv * std::sin(b0), 0},
                base + vec3{0, hi, 0} + d * (r * std::cos(b1)) + vec3{0, rv * std::sin(b1), 0}, c, alpha, hidden);
      draw_line(t, pr, base + vec3{0, lo, 0} + d * (r * std::cos(b0)) - vec3{0, rv * std::sin(b0), 0},
                base + vec3{0, lo, 0} + d * (r * std::cos(b1)) - vec3{0, rv * std::sin(b1), 0}, c, alpha, hidden);
    }
  }
}

void raster_mesh(Target &t, const Projector &pr, const runtime::Mesh &m, bool silhouette) {
  const size_t nv = m.positions.size();
  std::vector<vec3> sp(nv);
  for (size_t i = 0; i < nv; ++i) sp[i] = pr.project(m.positions[i]);
  const bool has_normals = m.normals.size() == nv;
  const bool has_colors = m.colors.size() == nv;

  for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
    uint32_t i0 = m.indices[k], i1 = m.indices[k + 1], i2 = m.indices[k + 2];
    if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
    vec3 a = sp[i0], b = sp[i1], c = sp[i2];
    if (a.z <= 1e-3f || b.z <= 1e-3f || c.z <= 1e-3f) continue;
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-9f) continue;
    vec3 face_n = normalize(cross(m.positions[i1] - m.positions[i0], m.positions[i2] - m.positions[i0]));
    int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    int x1 = std::min(t.w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    int y1 = std::min(t.h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    Rgb ca = has_colors ? unpack_color(m.colors[i0]) : kDefaultAlbedo;
    Rgb cb = has_colors ? unpack_color(m.colors[i1]) : kDefaultAlbedo;
    Rgb cc = has_colors ? unpack_color(m.colors[i2]) : kDefaultAlbedo;
    vec3 na = has_normals ? m.normals[i0] : face_n, nb = has_normals ? m.normals[i1] : face_n,
         nc = has_normals ? m.normals[i2] : face_n;
    vec3 centroid = (m.positions[i0] + m.positions[i1] + m.positions[i2]) * (1.0f / 3.0f);
    vec3 to_cam = pr.to_camera(centroid);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
        float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float z = w0 * a.z + w1 * b.z + w2 * c.z;
        size_t i = static_cast<size_t>(y) * t.w + x;
        if (z >= t.depth[i]) continue;
        t.depth[i] = z;
        t.id[i] = 1;
        if (silhouette) {
          t.color[i] = {0, 0, 0};
          t.albedo[i] = {0, 0, 0};
          continue;
        }
        Rgb al = {w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g, w0 * ca.b + w1 * cb.b + w2 * cc.b};
        vec3 n = normalize(na * w0 + nb * w1 + nc * w2);
        if (dot(n, to_cam) < 0) n = n * -1.0f;  // two-sided: imported meshes may have flipped winding
        float ndl = dot(n, kLightDir);
        Rgb out;
        if (ndl > 0.02f) {
          float k = ndl > 0.45f ? 1.0f : 0.84f;
          out = {al.r * kLight.r * k, al.g * kLight.g * k, al.b * kLight.b * k};
        } else {
          out = {al.r * kShadow.r * 1.05f, al.g * kShadow.g * 1.05f, al.b * kShadow.b * 1.05f};
        }
        if (dot(n, to_cam) < 0.28f) out = {out.r + 0.07f, out.g + 0.07f, out.b + 0.08f};  // rim band
        t.color[i] = out;
        t.albedo[i] = al;
      }
  }
}

// Ink: darken pixels on silhouettes and depth discontinuities (style.md section 6).
void ink(Target &t, bool silhouette) {
  std::vector<Rgb> out = t.color;
  for (int y = 1; y + 1 < t.h; ++y)
    for (int x = 1; x + 1 < t.w; ++x) {
      size_t i = static_cast<size_t>(y) * t.w + x;
      if (!t.id[i] && silhouette) continue;
      bool edge = false;
      // Look two supersampled pixels out, so the ink is about one final pixel wide.
      for (int k = 0; k < 8 && !edge; ++k) {
        int reach = k < 4 ? 1 : kSupersample;
        int dx = (k % 4 == 0 ? 1 : k % 4 == 1 ? -1 : 0) * reach, dy = (k % 4 == 2 ? 1 : k % 4 == 3 ? -1 : 0) * reach;
        if (x + dx < 0 || x + dx >= t.w || y + dy < 0 || y + dy >= t.h) continue;
        size_t j = static_cast<size_t>(y + dy) * t.w + (x + dx);
        if (t.id[i] != t.id[j]) edge = t.id[i] != 0;
        else if (t.id[i] && std::fabs(t.depth[i] - t.depth[j]) > 0.03f * std::max(t.depth[i], 1.0f)) edge = t.depth[i] > t.depth[j] ? false : true;
      }
      if (!edge) continue;
      Rgb a = t.albedo[i];
      out[i] = silhouette ? Rgb{0, 0, 0} : Rgb{a.r * 0.28f, a.g * 0.28f, a.b * 0.28f};
    }
  t.color = std::move(out);
}

}  // namespace

View parse_view(const std::string &name) {
  if (name == "front") return View::Front;
  if (name == "side") return View::Side;
  if (name == "top") return View::Top;
  if (name == "iso") return View::Iso;
  return View::Orbit;
}

const char *view_name(View v) {
  switch (v) {
    case View::Front: return "front";
    case View::Side: return "side";
    case View::Top: return "top";
    case View::Iso: return "iso";
    default: return "orbit";
  }
}

Image render(const Scene &scene, int width, int height) {
  const int W = width * kSupersample, H = height * kSupersample;
  Target t(W, H);

  // Frame everything that will be drawn.
  Bounds bounds;
  float asset_radius = 0;
  if (scene.mesh) {
    for (const auto &p : scene.mesh->positions) {
      bounds.add(p);
      asset_radius = std::max(asset_radius, std::hypot(p.x, p.z));
    }
  }
  if (scene.has_conformance && scene.overlays.conformance) {
    asset_radius = std::max(asset_radius, scene.conformance_radius);
    bounds.add({-scene.conformance_radius, 0, -scene.conformance_radius});
    bounds.add({scene.conformance_radius, scene.conformance_height, scene.conformance_radius});
  }
  for (const Line &l : scene.lines) {
    bounds.add(l.a);
    bounds.add(l.b);
  }
  if (scene.overlays.hero) {
    float hx = hero_x(asset_radius);
    bounds.add({hx - 0.35f, 0, -0.35f});
    bounds.add({hx + 0.35f, 1.8f, 0.35f});
  }
  if (scene.overlays.hex) {
    bounds.add({-1, 0, -1});
    bounds.add({1, 0, 1});
  }
  if (bounds.empty()) {
    bounds.add({-1, 0, -1});
    bounds.add({1, 1.8f, 1});
  }
  if (scene.camera.frame > 0) {
    const float f = scene.camera.frame;
    bounds = Bounds();
    bounds.add({-0.5f * f, 0, -0.5f * f});
    bounds.add({0.5f * f, f, 0.5f * f});
  }
  vec3 centre = (bounds.lo + bounds.hi) * 0.5f;
  vec3 ext = bounds.hi - bounds.lo;
  float radius = 0.5f * std::sqrt(dot(ext, ext)) + 0.05f;

  Projector pr;
  pr.cx = W * 0.5f;
  pr.cy = H * 0.5f;
  const float zoom = std::max(scene.camera.zoom, 0.05f);
  vec3 dir, up{0, 1, 0};
  switch (scene.camera.view) {
    case View::Front: dir = {0, 0, 1}; break;
    case View::Side: dir = {1, 0, 0}; break;
    case View::Top: dir = {0, 1, 0}; up = {0, 0, -1}; break;
    case View::Iso: {
      // True isometric: 45 degrees around, and atan(1/sqrt(2)) (35.26 degrees) down.
      const float c = 1.0f / std::sqrt(3.0f);
      dir = {c, c, c};
      break;
    }
    case View::Orbit:
      dir = {std::sin(scene.camera.yaw) * std::cos(scene.camera.pitch), std::sin(scene.camera.pitch),
             std::cos(scene.camera.yaw) * std::cos(scene.camera.pitch)};
      break;
  }
  if (scene.camera.view != View::Orbit) {
    // Quarter turns around +Y (the orbit view folds them into its yaw).
    const int q = ((scene.camera.quarter % 4) + 4) % 4;
    for (int k = 0; k < q; ++k) {
      dir = {dir.z, dir.y, -dir.x};
      up = {up.z, up.y, -up.x};
    }
  }
  pr.forward = normalize(dir * -1.0f);
  pr.right = normalize(cross(pr.forward, up));
  pr.up = cross(pr.right, pr.forward);
  if (scene.camera.view == View::Orbit && scene.camera.distance > 0) {
    // The game camera at a set distance from the asset (style.md section 3).
    pr.ortho = false;
    const float fov = 0.9f;
    vec3 target = centre;
    if (scene.mesh && !scene.mesh->positions.empty()) {
      Bounds mb;
      for (const auto &p : scene.mesh->positions) mb.add(p);
      target = (mb.lo + mb.hi) * 0.5f;
    }
    pr.eye = target + normalize(dir) * scene.camera.distance;
    pr.forward = normalize(target - pr.eye);
    pr.right = normalize(cross(pr.forward, up));
    pr.up = cross(pr.right, pr.forward);
    pr.scale = 0.5f * std::min(W, H) / std::tan(fov * 0.5f);
  } else if (scene.camera.view == View::Orbit) {
    pr.ortho = false;
    const float fov = 35.0f * kPi / 180.0f;
    float dist = radius / std::sin(fov * 0.5f) / zoom;
    pr.eye = centre + normalize(dir) * dist;
    pr.scale = 0.5f * std::min(W, H) / std::tan(fov * 0.5f);
  } else {
    pr.ortho = true;
    pr.eye = centre + normalize(dir) * (radius * 4 + 10);
    // Fit the projected extent of the bounds.
    float half_w = 0, half_h = 0;
    for (int k = 0; k < 8; ++k) {
      vec3 c{k & 1 ? bounds.hi.x : bounds.lo.x, k & 2 ? bounds.hi.y : bounds.lo.y, k & 4 ? bounds.hi.z : bounds.lo.z};
      vec3 d = c - centre;
      half_w = std::max(half_w, std::fabs(dot(d, pr.right)));
      half_h = std::max(half_h, std::fabs(dot(d, pr.up)));
    }
    pr.scale = zoom * 0.88f * std::min(W * 0.5f / std::max(half_w, 0.05f), H * 0.5f / std::max(half_h, 0.05f));
  }

  // Background: a soft vertical gradient.
  for (int y = 0; y < H; ++y) {
    float s = static_cast<float>(y) / static_cast<float>(H);
    Rgb c = {0.20f + 0.06f * s, 0.22f + 0.05f * s, 0.27f + 0.02f * s};
    for (int x = 0; x < W; ++x) t.color[static_cast<size_t>(y) * W + x] = c;
  }

  if (scene.mesh) raster_mesh(t, pr, *scene.mesh, scene.silhouette);
  ink(t, scene.silhouette);

  if (!scene.silhouette) {
    if (scene.overlays.grid) {
      float g = std::ceil(radius * 1.5f);
      for (float v = -g; v <= g + 1e-3f; v += 0.5f) {
        float a = std::fabs(std::fmod(v, 1.0f)) < 1e-3f ? 0.22f : 0.1f;
        draw_line(t, pr, {v, 0, -g}, {v, 0, g}, {0.6f, 0.65f, 0.7f}, a, 0.0f);
        draw_line(t, pr, {-g, 0, v}, {g, 0, v}, {0.6f, 0.65f, 0.7f}, a, 0.0f);
      }
    }
    if (scene.overlays.hex) {
      // Pointy-top hex, circumradius 1 m (style.md section 2).
      for (int k = 0; k < 6; ++k) {
        float a0 = kPi / 2 + kPi / 3 * k, a1 = kPi / 2 + kPi / 3 * (k + 1);
        draw_line(t, pr, {std::cos(a0), 0, std::sin(a0)}, {std::cos(a1), 0, std::sin(a1)}, {0.95f, 0.8f, 0.35f}, 0.9f, 0.3f);
      }
    }
    if (scene.overlays.hero)
      draw_capsule(t, pr, {hero_x(asset_radius), 0, 0}, 0.35f, 1.8f, {0.55f, 0.85f, 1.0f}, 0.85f, 0.25f);
    if (scene.has_conformance && scene.overlays.conformance)
      draw_capsule(t, pr, {0, 0, 0}, scene.conformance_radius, scene.conformance_height, {0.45f, 1.0f, 0.55f}, 0.9f, 0.3f);
    for (const Line &l : scene.lines) draw_line(t, pr, l.a, l.b, {l.r, l.g, l.bl}, l.alpha, l.alpha * 0.25f);
    if (scene.overlays.collision)
      for (const Line &l : scene.collision) draw_line(t, pr, l.a, l.b, {l.r, l.g, l.bl}, l.alpha, l.alpha * 0.35f);
  }

  Image img;
  img.width = W;
  img.height = H;
  img.rgba.resize(static_cast<size_t>(W) * H * 4);
  for (size_t i = 0; i < t.color.size(); ++i) {
    auto q = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    img.rgba[i * 4 + 0] = q(t.color[i].r);
    img.rgba[i * 4 + 1] = q(t.color[i].g);
    img.rgba[i * 4 + 2] = q(t.color[i].b);
    img.rgba[i * 4 + 3] = 255;
  }
  return downsample(img, kSupersample);
}

}  // namespace viewport
