// Tests for src/host/assets/fracture: every algorithm's pieces are closed (their
// volumes add up to the mesh's), on a solid, on a hollow vase whose cut faces
// have holes, and on a mesh with a gap; the bake settles; a save loads back.
// Run with ctest.
#include <cmath>
#include <cstdio>

#include "assets/fracture.h"

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

namespace {

using foundry::vec3;

void quad(runtime::Mesh &m, vec3 a, vec3 b, vec3 c, vec3 d) {
  const uint32_t i = static_cast<uint32_t>(m.positions.size());
  for (const vec3 &p : {a, b, c, d}) {
    m.positions.push_back(p);
    m.normals.push_back({0, 1, 0});
    m.colors.push_back(0xff6080a0u);
  }
  for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(i + k);
}

// A box from lo to hi, faces outward.
runtime::Mesh box(vec3 lo, vec3 hi) {
  runtime::Mesh m;
  quad(m, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});  // +z
  quad(m, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z});  // -z
  quad(m, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z});  // +x
  quad(m, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});  // -x
  quad(m, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z});  // +y
  quad(m, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});  // -y
  return m;
}

// A vase: a closed hollow shell of revolution (outer wall, rim, inner wall, inner floor, base).
runtime::Mesh vase(double &volume) {
  const int sides = 24;
  const float H = 0.5f, base = 0.04f;
  auto outer = [](float y) { return 0.12f + 0.06f * std::sin(y * 6.0f); };
  auto inner = [&](float y) { return outer(y) - 0.015f; };
  std::vector<vec3> profile;  // r, y pairs walked around the cross-section (outside first, then back down inside)
  for (int k = 0; k <= 8; ++k) {
    const float y = H * k / 8;
    profile.push_back({outer(y), y, 0});
  }
  for (int k = 8; k >= 0; --k) {
    const float y = base + (H - base) * k / 8;
    profile.push_back({inner(y), y, 0});
  }
  profile.push_back({0, base, 0});
  runtime::Mesh m;
  auto at = [&](const vec3 &p, int s) {
    const float a = 6.2831853f * s / sides;
    return vec3{p.x * std::cos(a), p.y, p.x * std::sin(a)};
  };
  // Surface of revolution: each profile segment swept around, outward-facing for a profile walked
  // up the outside and down the inside.
  for (size_t k = 0; k + 1 < profile.size(); ++k)
    for (int s = 0; s < sides; ++s)
      quad(m, at(profile[k], s + 1), at(profile[k], s), at(profile[k + 1], s), at(profile[k + 1], s + 1));
  // The bottom.
  for (int s = 0; s < sides; ++s) {
    const uint32_t i = static_cast<uint32_t>(m.positions.size());
    for (const vec3 &p : {vec3{0, 0, 0}, at(profile[0], s), at(profile[0], s + 1)}) {
      m.positions.push_back(p);
      m.normals.push_back({0, -1, 0});
      m.colors.push_back(0xff3060c0u);
    }
    m.indices.insert(m.indices.end(), {i, i + 1, i + 2});
  }
  volume = 0;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const vec3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
    volume += (a.x * (b.y * c.z - b.z * c.y) + a.y * (b.z * c.x - b.x * c.z) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
  }
  return m;
}

double piece_total(const assets::fracture::Fracture &f) {
  double v = 0;
  for (float x : f.volumes) v += x;
  return v;
}

}  // namespace

int main() {
  using namespace assets::fracture;
  // A solid box: every algorithm conserves its volume (0.4 x 0.6 x 0.3 m).
  const runtime::Mesh cube = box({-0.2f, 0, -0.15f}, {0.2f, 0.6f, 0.15f});
  for (const auto &a : algorithms()) {
    Settings st;
    st.algorithm = a.name;
    st.pieces = 12;
    Fracture f;
    std::string error;
    CHECK(cut(cube, st, f, error));
    const double total = piece_total(f);
    std::printf("box %-8s %2zu pieces, %5zu triangles, volume %.6f of 0.072000, open %d\n", a.name, f.pieces(),
                f.mesh.indices.size() / 3, total, f.stats.value("open_sections", -1));
    CHECK(f.pieces() >= 2);
    CHECK(std::fabs(total - 0.072) < 1e-5);
    CHECK(f.stats.value("open_sections", -1) == 0);
  }

  // A hollow vase: cut faces are rings (holes), and the shell's volume is kept.
  double vase_volume = 0;
  const runtime::Mesh v = vase(vase_volume);
  for (const char *a : {"radial", "voronoi", "slab"}) {
    Settings st;
    st.algorithm = a;
    st.pieces = 14;
    Fracture f;
    std::string error;
    CHECK(cut(v, st, f, error));
    const double total = piece_total(f);
    std::printf("vase %-8s %2zu pieces, volume %.6f of %.6f\n", a, f.pieces(), total, vase_volume);
    CHECK(std::fabs(total - vase_volume) < vase_volume * 1e-3);
  }

  // A box with one face missing: the sections don't close, are counted, and are closed straight.
  runtime::Mesh open = cube;
  open.indices.resize(open.indices.size() - 6);
  {
    Settings st;
    st.algorithm = "voronoi";
    st.pieces = 6;
    Fracture f;
    std::string error;
    CHECK(cut(open, st, f, error));
    std::printf("open box: %zu pieces, open sections %d\n", f.pieces(), f.stats.value("open_sections", -1));
    CHECK(f.stats.value("open_sections", 0) > 0);
  }

  // The bake: pieces fly, land, and settle; a save loads back the same.
  {
    Settings st;
    st.pieces = 10;
    Fracture f;
    std::string error;
    CHECK(cut(cube, st, f, error));
    bake(f, st);
    std::printf("bake: %u frames at %.0f fps, settled at %s s, farthest %.2f m\n", f.frames, f.fps,
                f.stats["settled_s"].dump().c_str(), f.stats.value("farthest_m", 0.0));
    CHECK(f.frames == 91);
    CHECK(f.stats["settled_s"].is_number());
    const runtime::Mesh end = pose(f, f.seconds());
    float lowest = 1e9f;
    for (const auto &p : end.positions) lowest = std::fmin(lowest, p.y);
    CHECK(lowest > -0.02f);  // nothing sinks through the ground
    const std::string path = "fracture_test.bin";
    CHECK(save(path, f));
    Fracture g;
    CHECK(load(path, g));
    CHECK(g.track == f.track && g.mesh.positions.size() == f.mesh.positions.size() && g.pieces() == f.pieces());
    std::remove(path.c_str());
  }
  std::printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
