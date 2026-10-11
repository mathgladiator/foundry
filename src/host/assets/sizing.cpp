#include "assets/sizing.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "assets/lod.h"
#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace assets {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct P2 {
  double x, z;
};

// Andrew's monotone chain convex hull of the footprint.
std::vector<P2> hull2d(std::vector<P2> pts) {
  std::sort(pts.begin(), pts.end(), [](const P2 &a, const P2 &b) { return a.x != b.x ? a.x < b.x : a.z < b.z; });
  pts.erase(std::unique(pts.begin(), pts.end(), [](const P2 &a, const P2 &b) { return a.x == b.x && a.z == b.z; }),
            pts.end());
  if (pts.size() < 3) return pts;
  auto cross = [](const P2 &o, const P2 &a, const P2 &b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); };
  std::vector<P2> h(2 * pts.size());
  size_t k = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
    h[k++] = pts[i];
  }
  for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
    h[k++] = pts[i - 1];
  }
  h.resize(k - 1);
  return h;
}

struct Circle {
  double x = 0, z = 0, r = 0;
};

bool inside(const Circle &c, const P2 &p) { return std::hypot(p.x - c.x, p.z - c.z) <= c.r * (1 + 1e-9) + 1e-12; }

Circle circle2(const P2 &a, const P2 &b) {
  return {(a.x + b.x) / 2, (a.z + b.z) / 2, std::hypot(a.x - b.x, a.z - b.z) / 2};
}

Circle circle3(const P2 &a, const P2 &b, const P2 &c) {
  double d = 2 * (a.x * (b.z - c.z) + b.x * (c.z - a.z) + c.x * (a.z - b.z));
  if (std::fabs(d) < 1e-18) {  // collinear: the widest pair
    Circle c1 = circle2(a, b), c2 = circle2(a, c), c3 = circle2(b, c);
    return c1.r > c2.r ? (c1.r > c3.r ? c1 : c3) : (c2.r > c3.r ? c2 : c3);
  }
  double a2 = a.x * a.x + a.z * a.z, b2 = b.x * b.x + b.z * b.z, c2 = c.x * c.x + c.z * c.z;
  double ux = (a2 * (b.z - c.z) + b2 * (c.z - a.z) + c2 * (a.z - b.z)) / d;
  double uz = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
  return {ux, uz, std::hypot(a.x - ux, a.z - uz)};
}

// Smallest enclosing circle (Welzl, iterative; hull points in a fixed order).
Circle min_circle(const std::vector<P2> &p) {
  Circle c;
  if (p.empty()) return c;
  c = {p[0].x, p[0].z, 0};
  for (size_t i = 1; i < p.size(); ++i) {
    if (inside(c, p[i])) continue;
    c = {p[i].x, p[i].z, 0};
    for (size_t j = 0; j < i; ++j) {
      if (inside(c, p[j])) continue;
      c = circle2(p[i], p[j]);
      for (size_t k = 0; k < j; ++k)
        if (!inside(c, p[k])) c = circle3(p[i], p[j], p[k]);
    }
  }
  return c;
}

struct Footprint {
  Circle circle;
  size_t hull_points = 0;
  double min_y = 0, max_y = 0;
};

Footprint footprint(const runtime::Mesh &m) {
  Footprint f;
  std::vector<P2> pts;
  pts.reserve(m.positions.size());
  f.min_y = m.positions.empty() ? 0 : m.positions[0].y;
  f.max_y = f.min_y;
  for (const auto &p : m.positions) {
    pts.push_back({p.x, p.z});
    f.min_y = std::min<double>(f.min_y, p.y);
    f.max_y = std::max<double>(f.max_y, p.y);
  }
  std::vector<P2> h = hull2d(pts);
  f.hull_points = h.size();
  f.circle = min_circle(h);
  return f;
}

void transform(runtime::Mesh &m, const double M[3][3], const double t[3]) {
  for (auto &p : m.positions) {
    double x = p.x, y = p.y, z = p.z;
    p = {static_cast<float>(M[0][0] * x + M[0][1] * y + M[0][2] * z + t[0]),
         static_cast<float>(M[1][0] * x + M[1][1] * y + M[1][2] * z + t[1]),
         static_cast<float>(M[2][0] * x + M[2][1] * y + M[2][2] * z + t[2])};
  }
  // Normals transform by the inverse transpose; for rotations and scales the
  // cofactor matrix gives the same direction.
  double C[3][3] = {{M[1][1] * M[2][2] - M[1][2] * M[2][1], M[1][2] * M[2][0] - M[1][0] * M[2][2], M[1][0] * M[2][1] - M[1][1] * M[2][0]},
                    {M[0][2] * M[2][1] - M[0][1] * M[2][2], M[0][0] * M[2][2] - M[0][2] * M[2][0], M[0][1] * M[2][0] - M[0][0] * M[2][1]},
                    {M[0][1] * M[1][2] - M[0][2] * M[1][1], M[0][2] * M[1][0] - M[0][0] * M[1][2], M[0][0] * M[1][1] - M[0][1] * M[1][0]}};
  for (auto &nm : m.normals) {
    double x = nm.x, y = nm.y, z = nm.z;
    double nx = C[0][0] * x + C[0][1] * y + C[0][2] * z, ny = C[1][0] * x + C[1][1] * y + C[1][2] * z,
           nz = C[2][0] * x + C[2][1] * y + C[2][2] * z;
    double l = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (l > 0) nm = {static_cast<float>(nx / l), static_cast<float>(ny / l), static_cast<float>(nz / l)};
  }
  // A mirroring transform flips triangle winding; keep faces outward.
  double det = M[0][0] * C[0][0] + M[0][1] * C[0][1] + M[0][2] * C[0][2];
  if (det < 0)
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) std::swap(m.indices[i + 1], m.indices[i + 2]);
}

void uniform_scale(runtime::Mesh &m, double s) {
  double M[3][3] = {{s, 0, 0}, {0, s, 0}, {0, 0, s}}, t[3] = {0, 0, 0};
  transform(m, M, t);
}
void translate(runtime::Mesh &m, double x, double y, double z) {
  double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, t[3] = {x, y, z};
  transform(m, M, t);
}
void ground(runtime::Mesh &m) {
  Footprint f = footprint(m);
  translate(m, 0, -f.min_y, 0);
}
void center(runtime::Mesh &m) {
  Footprint f = footprint(m);
  translate(m, -f.circle.x, 0, -f.circle.z);
}

}  // namespace

nlohmann::json hull_report(const std::string &root, const runtime::Mesh &mesh) {
  nlohmann::json r = mesh_stats(mesh);
  if (mesh.positions.empty()) return r;
  Footprint f = footprint(mesh);
  const double height = f.max_y - f.min_y;
  r["height_m"] = height;
  r["lowest_y"] = f.min_y;
  r["grounded"] = std::fabs(f.min_y) < 1e-4;
  r["footprint"] = {{"centre", {f.circle.x, f.circle.z}}, {"radius_m", f.circle.r}, {"hull_points", f.hull_points},
                    {"centred", std::hypot(f.circle.x, f.circle.z) < 1e-4}};
  nlohmann::json shapes = nlohmann::json::array();
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto &e : fs::directory_iterator(root + "/conformance", ec)) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto &p : files) {
    if (p.extension() != ".json") continue;
    nlohmann::json c = core::read_json(p.string());
    if (!c.is_object()) continue;
    double ch = c.value("height", 0.0), cr = c.value("radius", 0.0);
    bool fits = height <= ch + 1e-4 && f.circle.r <= cr + 1e-4;
    double s = std::min(height > 0 ? ch / height : 1e30, f.circle.r > 0 ? cr / f.circle.r : 1e30);
    shapes.push_back({{"conformance", p.stem().string()}, {"height_m", ch}, {"radius_m", cr}, {"size_fits", fits},
                      {"max_uniform_scale", s}});
  }
  r["conformance"] = shapes;
  return r;
}

bool apply_ops(const std::string &root, runtime::Mesh &m, const nlohmann::json &ops, nlohmann::json &stage,
               std::string &error) {
  for (const auto &op : ops) {
    const std::string kind = op.value("op", "");
    if (kind == "scale") {
      const auto &by = op.contains("by") ? op["by"] : nlohmann::json(1.0);
      double sx, sy, sz;
      if (by.is_array() && by.size() == 3) {
        sx = by[0];
        sy = by[1];
        sz = by[2];
      } else if (by.is_number()) {
        sx = sy = sz = by.get<double>();
      } else {
        error = "scale needs \"by\": a number or [x, y, z]";
        return false;
      }
      if (sx == 0 || sy == 0 || sz == 0) {
        error = "scale by zero";
        return false;
      }
      double M[3][3] = {{sx, 0, 0}, {0, sy, 0}, {0, 0, sz}}, t[3] = {0, 0, 0};
      transform(m, M, t);
    } else if (kind == "rotate") {
      double a = op.value("degrees", 0.0) * kPi / 180.0, c = std::cos(a), s = std::sin(a);
      std::string axis = op.value("axis", "y");
      double t[3] = {0, 0, 0};
      if (axis == "x") {
        double M[3][3] = {{1, 0, 0}, {0, c, -s}, {0, s, c}};
        transform(m, M, t);
      } else if (axis == "y") {
        double M[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
        transform(m, M, t);
      } else if (axis == "z") {
        double M[3][3] = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
        transform(m, M, t);
      } else {
        error = "rotate axis must be x, y, or z";
        return false;
      }
    } else if (kind == "translate") {
      const auto &by = op.value("by", nlohmann::json::array());
      if (!by.is_array() || by.size() != 3) {
        error = "translate needs \"by\": [x, y, z]";
        return false;
      }
      translate(m, by[0], by[1], by[2]);
    } else if (kind == "ground") {
      ground(m);
    } else if (kind == "center") {
      center(m);
    } else if (kind == "height") {
      Footprint f = footprint(m);
      double h = f.max_y - f.min_y, want = op.value("meters", 0.0);
      if (h <= 0 || want <= 0) {
        error = "height needs a positive \"meters\" and a mesh with height";
        return false;
      }
      uniform_scale(m, want / h);
      ground(m);
    } else if (kind == "fit") {
      const std::string name = op.value("conformance", "");
      nlohmann::json c = core::read_json(root + "/conformance/" + name + ".json");
      if (!c.is_object()) {
        error = "no conformance shape '" + name + "'";
        return false;
      }
      center(m);
      ground(m);
      Footprint f = footprint(m);
      double h = f.max_y - f.min_y, r = f.circle.r;
      double ch = c.value("height", 0.0), cr = c.value("radius", 0.0), fill = op.value("fill", 0.95);
      double s;
      if (op.contains("height")) {
        s = h > 0 ? op["height"].get<double>() / h : 1;
      } else {
        s = std::min(h > 0 ? fill * ch / h : 1e30, r > 0 ? fill * cr / r : 1e30);
      }
      uniform_scale(m, s);
      ground(m);
      stage["conformance"] = name;
    } else {
      error = "unknown op '" + kind + "' (scale, rotate, translate, ground, center, height, fit)";
      return false;
    }
  }
  return true;
}

}  // namespace assets
