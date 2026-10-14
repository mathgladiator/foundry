// A model's condition (design/planner.md, "Ruins and building"): built whole, then broken down to a
// ruin, cut back to its half-built walls in scaffolding, or cleared to its staked-out plan; and its
// fire points (lit fires, and the points it burns from when it burns). Works on the model's arrays
// alone, so any building's model can take it. Each system carries its own copy (spine.md).
#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "math.h"

namespace wall_build_v1 {

using namespace wall_math;

namespace condition {

inline vec3 unpack(uint32_t c) {
  return v3(static_cast<float>(c & 255u) / 255.0f, static_cast<float>((c >> 8) & 255u) / 255.0f, static_cast<float>((c >> 16) & 255u) / 255.0f);
}
inline uint32_t repack(vec3 c) {
  auto byte = [](float v) { return static_cast<uint32_t>(clampf(v, 0, 1) * 255.0f + 0.5f); };
  return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (255u << 24);
}
inline float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

// A box straight into the model (root bone), so condition needs no builder: centre c, axes ax,
// ay, az (unit, at right angles), half sizes h.
template <typename M>
void box_axes(M &m, vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, vec3 col) {
  const vec3 A[3] = {ax * h.x, ay * h.y, az * h.z};
  const vec3 N[3] = {ax, ay, az};
  for (int f = 0; f < 6; ++f) {
    const int i = f / 2;
    const float s = f % 2 ? -1.0f : 1.0f;
    const vec3 n = N[i] * s, fc = c + A[i] * s, du = A[(i + 1) % 3], dw = A[(i + 2) % 3];
    const float shade = n.y > 0.5f ? 1.0f : (n.y < -0.5f ? 0.6f : 0.82f);
    const uint32_t base = static_cast<uint32_t>(m.positions.size());
    const vec3 corners[4] = {fc - du - dw, fc + du - dw, fc + du + dw, fc - du + dw};
    for (const vec3 &k : corners) {
      m.positions.push_back(k);
      m.normals.push_back(n);
      m.colors.push_back(repack(col * shade));
      m.bone_a.push_back(0);
      m.bone_b.push_back(0);
      m.bone_w.push_back(0);
    }
    // Wind so the face looks out.
    if (dot(cross(corners[1] - corners[0], corners[2] - corners[0]), n) >= 0) {
      for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + k);
    } else {
      for (uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(base + k);
    }
  }
}
template <typename M>
void box(M &m, vec3 c, vec3 h, vec3 col, float yaw = 0) {
  const float cy = foundry::det::cos(yaw), sy = foundry::det::sin(yaw);
  box_axes(m, c, v3(cy, 0, -sy), v3(0, 1, 0), v3(sy, 0, cy), h, col);
}
// A thin square beam from a to b.
template <typename M>
void beam(M &m, vec3 a, vec3 b, float half, vec3 col) {
  const float len = length(b - a);
  if (len < 1e-4f) return;
  const vec3 ax = (b - a) * (1.0f / len);
  const vec3 side = absf(ax.y) > 0.9f ? v3(1, 0, 0) : normalize(cross(v3(0, 1, 0), ax));
  box_axes(m, (a + b) * 0.5f, ax, normalize(cross(ax, side)), side, v3(len * 0.5f, half, half), col);
}

// The building's bounds: what stands above the ground (y over 0.3 m).
struct Bounds {
  vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
  bool any = false;
};
template <typename M>
Bounds bounds(const M &m) {
  Bounds b;
  for (const vec3 &p : m.positions) {
    if (p.y < 0.3f) continue;
    b.lo = v3(minf(b.lo.x, p.x), minf(b.lo.y, p.y), minf(b.lo.z, p.z));
    b.hi = v3(maxf(b.hi.x, p.x), maxf(b.hi.y, p.y), maxf(b.hi.z, p.z));
    b.any = true;
  }
  if (!b.any) {
    b.lo = v3(-1, 0, -1);
    b.hi = v3(1, 1, 1);
  }
  return b;
}

// Moving parts go (triangles on any bone but the root), and so do their bones.
template <typename M>
void drop_moving(M &m) {
  std::vector<uint32_t> kept;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    bool root = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = m.indices[t + static_cast<size_t>(k)];
      root = root && m.bone_a[v] == 0 && (m.bone_w[v] <= 0 || m.bone_b[v] == 0);
    }
    if (root)
      for (int k = 0; k < 3; ++k) kept.push_back(m.indices[t + static_cast<size_t>(k)]);
  }
  m.indices = kept;
  if (m.bones.size() > 1) m.bones.resize(1);
  for (size_t v = 0; v < m.bone_a.size(); ++v) {
    m.bone_a[v] = 0;
    m.bone_b[v] = 0;
    m.bone_w[v] = 0;
  }
}

// Every vertex above top(x, z) comes down to it. A triangle wholly above goes; so does a sloped or
// level one (a roof, a floor, a cap) that reached above, so a broken wall stands open, not lidded.
template <typename M, typename Top>
void clip(M &m, const Top &top, bool move = true) {
  std::vector<uint8_t> above(m.positions.size(), 0);
  std::vector<float> cut(m.positions.size(), 0);
  for (size_t v = 0; v < m.positions.size(); ++v) {
    cut[v] = top(m.positions[v].x, m.positions[v].z);
    above[v] = m.positions[v].y > cut[v] ? 1 : 0;
  }
  std::vector<uint32_t> kept;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
    const int n = above[a] + above[b] + above[c];
    if (n == 3) continue;
    if (n > 0) {
      const vec3 nn = cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
      const float len = length(nn);
      if (len > 1e-8f && absf(nn.y) > 0.35f * len) continue;
    }
    kept.push_back(a);
    kept.push_back(b);
    kept.push_back(c);
  }
  m.indices = kept;
  if (!move) return;
  for (size_t v = 0; v < m.positions.size(); ++v)
    if (above[v]) m.positions[v].y = cut[v];
}

// Long triangles reaching above `y` split at their longest edge, so a break can be ragged.
template <typename M>
void subdivide(M &m, float y, float edge, int passes) {
  for (int pass = 0; pass < passes; ++pass) {
    std::vector<uint32_t> next;
    bool split = false;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      uint32_t i[3] = {m.indices[t], m.indices[t + 1], m.indices[t + 2]};
      const vec3 p[3] = {m.positions[i[0]], m.positions[i[1]], m.positions[i[2]]};
      int longest = 0;
      float best = 0;
      for (int k = 0; k < 3; ++k) {
        const float l = length(p[(k + 1) % 3] - p[k]);
        if (l > best) {
          best = l;
          longest = k;
        }
      }
      if (best < edge || maxf(p[0].y, maxf(p[1].y, p[2].y)) < y) {
        next.insert(next.end(), {i[0], i[1], i[2]});
        continue;
      }
      // A new vertex at the longest edge's middle.
      const uint32_t a = i[longest], b = i[(longest + 1) % 3], c = i[(longest + 2) % 3];
      const uint32_t mid = static_cast<uint32_t>(m.positions.size());
      m.positions.push_back((m.positions[a] + m.positions[b]) * 0.5f);
      m.normals.push_back(m.normals[a]);
      m.colors.push_back(m.colors[a]);
      m.bone_a.push_back(m.bone_a[a]);
      m.bone_b.push_back(m.bone_b[a]);
      m.bone_w.push_back(m.bone_w[a]);
      next.insert(next.end(), {a, mid, c, mid, b, c});
      split = true;
    }
    m.indices = next;
    if (!split) break;
  }
}

// Floors and lids open: large faces looking up, above `y`, go (a ruin's storeys have fallen in).
template <typename M>
void open_up(M &m, float y, float area) {
  std::vector<uint32_t> kept;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const vec3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
    const vec3 n = cross(b - a, c - a);
    const float len = length(n);
    if (len > 2 * area && absf(n.y) > 0.7f * len && minf(a.y, minf(b.y, c.y)) > y) continue;
    for (int k = 0; k < 3; ++k) kept.push_back(m.indices[t + static_cast<size_t>(k)]);
  }
  m.indices = kept;
}

// Colours weathered: greyer and darker, moss on what faces up and near the ground.
template <typename M>
void weather(M &m, float grey, float dark, float moss) {
  const vec3 green = v3(0.30f, 0.42f, 0.22f);
  for (size_t v = 0; v < m.colors.size(); ++v) {
    vec3 c = unpack(m.colors[v]);
    const float l = 0.3f * c.x + 0.55f * c.y + 0.15f * c.z;
    c = lerp(c, v3(l, l, l), grey) * (1.0f - dark);
    const float up = v < m.normals.size() ? m.normals[v].y : 0.0f;
    const float low = v < m.positions.size() ? clampf(1.0f - m.positions[v].y / 1.2f, 0, 1) : 0.0f;
    c = lerp(c, green, clampf(moss * (maxf(up, 0) * 0.8f + low * 0.4f), 0, 0.85f));
    m.colors[v] = repack(c);
  }
}

// Collision cut down to `top`: boxes and cylinders shortened, anything wholly above it gone.
template <typename M>
void cap_collision(M &m, float top) {
  for (auto &body : m.collision) {
    std::vector<typename std::decay<decltype(body.shapes[0])>::type> kept;
    for (auto sh : body.shapes) {
      const bool box = sh.type == 1, cyl = sh.type == 4;
      if (sh.type == 5) {  // a convex hull: its points brought down
        float lo = 1e30f;
        for (auto &q : sh.points) lo = minf(lo, q.y + sh.position.y);
        if (lo >= top - 0.05f) continue;
        for (auto &q : sh.points) q.y = minf(q.y, top - sh.position.y);
        kept.push_back(sh);
        continue;
      }
      const float h = box ? sh.half_extents.y : (cyl ? sh.half_height : sh.radius);
      const float lo = sh.position.y - h;
      if (lo >= top - 0.05f) continue;
      if (box || cyl) {
        const float hi = minf(sh.position.y + h, top), nh = (hi - lo) * 0.5f;
        sh.position.y = lo + nh;
        if (box) sh.half_extents.y = nh;
        else sh.half_height = nh;
      }
      kept.push_back(sh);
    }
    body.shapes = kept;
  }
  std::vector<typename std::decay<decltype(m.collision[0])>::type> bodies;
  for (auto &body : m.collision)
    if (!body.shapes.empty()) bodies.push_back(body);
  m.collision = bodies;
}

// Fire points: lit 1 always, 0 only while it burns, 2 its smoke only (a chimney).
template <typename M>
void fire(M &m, vec3 at, uint32_t config, uint32_t lit) {
  m.fire_points.push_back(at);
  m.fire_configs.push_back(config);
  m.fire_lit.push_back(lit);
}
// The points a building burns from: up to `count` spots on its upper surfaces and walls, spread
// over its bounds by a seeded grid (unlit: they burn only when the game sets the building alight).
template <typename M>
void blaze_points(M &m, uint32_t config, uint32_t seed, int count) {
  const Bounds b = bounds(m);
  if (!b.any) return;
  int placed = 0;
  for (int k = 0; k < count * 4 && placed < count; ++k) {
    const float u = unit(hash_u32(seed + static_cast<uint32_t>(k) * 7919u)), w = unit(hash_u32(seed ^ (static_cast<uint32_t>(k) * 104729u + 17u)));
    const float x = b.lo.x + (b.hi.x - b.lo.x) * (0.15f + 0.7f * u), z = b.lo.z + (b.hi.z - b.lo.z) * (0.15f + 0.7f * w);
    // The highest surface over (x, z): the nearest vertex's height there.
    float y = -1e30f, best = 1e30f;
    for (size_t v = 0; v < m.positions.size(); v += 3) {
      const float dx = m.positions[v].x - x, dz = m.positions[v].z - z, d = dx * dx + dz * dz;
      if (d < best - 0.25f || (d < best + 0.25f && m.positions[v].y > y)) {
        best = minf(best, d);
        y = maxf(y, m.positions[v].y);
      }
    }
    if (y < 0.5f) continue;
    fire(m, v3(x, y - 0.1f, z), config, 0u);
    ++placed;
  }
}
// Whether a model carries fire points (buildings do; walls, fences, and roads don't).
template <typename M, typename = void>
struct has_fires : std::false_type {};
template <typename M>
struct has_fires<M, std::void_t<decltype(std::declval<M &>().fire_points)>> : std::true_type {};

// A ruin keeps no lit fire.
template <typename M>
void douse(M &m) {
  if constexpr (!has_fires<M>::value) {
    (void)m;
    return;
  } else {
  std::vector<vec3> p;
  std::vector<uint32_t> c, l;
  for (size_t i = 0; i < m.fire_points.size(); ++i)
    if (!m.fire_lit[i]) {
      p.push_back(m.fire_points[i]);
      c.push_back(m.fire_configs[i]);
      l.push_back(0);
    }
  m.fire_points = p;
  m.fire_configs = c;
  m.fire_lit = l;
  }
}

// ---- the conditions ----

// A ruin over ground `base(x, z)`: its moving parts and lit fires gone, what stands broken off at a
// ragged height (a little over a tenth to three quarters of `height`), what was a floor or roof
// fallen in (a building's), weathered and mossed, rubble about it. A line (a wall, a fence) keeps
// its rubble beside it rather than across its bounds.
template <typename M, typename Base>
void ruin_over(M &m, uint32_t seed, float moss, const Base &base, float height, bool building) {
  const Bounds b = bounds(m);
  drop_moving(m);
  douse(m);
  const float H = maxf(height, 0.6f);
  auto top = [&](float x, float z) {
    const float n = noise(v3(x * 0.45f, 0.0f, z * 0.45f), seed) * 0.6f + noise(v3(x * 1.9f, 3.0f, z * 1.9f), seed + 7u) * 0.25f +
                    noise(v3(x * 5.0f, 5.0f, z * 5.0f), seed + 13u) * 0.15f;
    return base(x, z) + H * (0.12f + 0.62f * n);
  };
  // Floors and roofs that reached above the break go whole; then the walls break raggedly.
  if (building) open_up(m, 0.6f, 0.8f);
  clip(m, top, false);
  subdivide(m, building ? H * 0.12f : -1e30f, building ? 0.75f : 0.7f, 5);
  clip(m, top);
  weather(m, 0.45f, 0.22f, 0.35f + 0.5f * moss);
  // What collides stands no higher than what's left standing.
  float left = -1e30f;
  for (const vec3 &p : m.positions) left = maxf(left, p.y);
  cap_collision(m, left);
  // Rubble: stones and fallen timbers about it.
  const int n = building ? 6 + static_cast<int>(hash_u32(seed) % 6u) : static_cast<int>(m.positions.size() / 400u) + 4;
  const size_t nv = m.positions.size();
  for (int k = 0; k < n && nv > 0; ++k) {
    const float u = unit(hash_u32(seed ^ (static_cast<uint32_t>(k) * 2654435761u))), w = unit(hash_u32(seed + static_cast<uint32_t>(k) * 40503u));
    const float rs = building ? 1.0f : clampf(H / 4.0f, 0.2f, 1.0f);  // rubble in scale with what fell
    const float s = (0.12f + 0.25f * unit(hash_u32(seed * 31u + static_cast<uint32_t>(k)))) * rs;
    vec3 at;
    if (building) at = v3(b.lo.x + s + (b.hi.x - b.lo.x - 2 * s) * u, s, b.lo.z + s + (b.hi.z - b.lo.z - 2 * s) * w);
    else {
      const vec3 p = m.positions[static_cast<size_t>(u * static_cast<float>(nv - 1))];
      at = v3(p.x + (w - 0.5f) * 1.6f * rs, 0, p.z + (unit(hash_u32(seed + static_cast<uint32_t>(k) * 977u)) - 0.5f) * 1.6f * rs);
      at.y = base(at.x, at.z) + s;
    }
    if (k % 3 == 2) box(m, at + v3(0, -s * 0.6f, 0), v3(0.9f * rs, 0.08f * rs, 0.1f * rs), v3(0.32f, 0.25f, 0.18f), u * 3.0f);
    else box(m, at, v3(s, s * 0.8f, s * 0.9f), v3(0.5f, 0.49f, 0.46f) * (0.85f + 0.2f * w), w * 2.0f);
  }
}

template <typename M>
void ruin(M &m, uint32_t seed, float moss) {
  const Bounds b = bounds(m);
  ruin_over(m, seed, moss, [](float, float) { return 0.0f; }, maxf(b.hi.y, 1.0f), true);
}

// Half built: its walls up to half their height (no roof, no moving parts), scaffolding round it,
// ladders, and the stone and timber still to go up.
template <typename M>
void half_built(M &m, uint32_t seed) {
  const Bounds b = bounds(m);
  drop_moving(m);
  if constexpr (has_fires<M>::value) {
    m.fire_points.clear();
    m.fire_configs.clear();
    m.fire_lit.clear();
  }
  const float H = maxf(b.hi.y, 1.5f);
  const float cut = maxf(1.2f, H * 0.4f);
  clip(m, [&](float, float) { return cut; });
  cap_collision(m, cut);
  const vec3 pole = v3(0.55f, 0.42f, 0.28f), plank = v3(0.62f, 0.5f, 0.34f);
  const float x0 = b.lo.x - 0.5f, x1 = b.hi.x + 0.5f, z0 = b.lo.z - 0.5f, z1 = b.hi.z + 0.5f;
  const float top = cut + 1.6f;
  // Poles every two metres round the outside, planks at two levels.
  auto side = [&](vec3 a, vec3 c) {
    const float len = length(c - a);
    const int n = static_cast<int>(len / 2.0f) > 1 ? static_cast<int>(len / 2.0f) : 1;
    for (int k = 0; k <= n; ++k) {
      const vec3 p = a + (c - a) * (static_cast<float>(k) / static_cast<float>(n));
      beam(m, p, p + v3(0, top, 0), 0.05f, pole);
    }
    for (float y : {cut * 0.5f, cut + 0.1f}) beam(m, a + v3(0, y, 0), c + v3(0, y, 0), 0.04f, plank);
  };
  side(v3(x0, 0, z0), v3(x1, 0, z0));
  side(v3(x1, 0, z0), v3(x1, 0, z1));
  side(v3(x1, 0, z1), v3(x0, 0, z1));
  side(v3(x0, 0, z1), v3(x0, 0, z0));
  // A ladder at the front, and the materials.
  for (int s = -1; s <= 1; s += 2) beam(m, v3((x0 + x1) * 0.5f + 0.25f * static_cast<float>(s), 0, z1 + 0.3f), v3((x0 + x1) * 0.5f + 0.25f * static_cast<float>(s), cut + 0.5f, z1 + 0.05f), 0.03f, pole);
  for (int k = 0; k < 5; ++k) box(m, v3(x1 + 0.9f, 0.2f + 0.4f * static_cast<float>(k / 2), z0 + 0.6f + 0.5f * static_cast<float>(k % 2)), v3(0.3f, 0.2f, 0.22f), v3(0.62f, 0.6f, 0.56f));
  for (int k = 0; k < 4; ++k) beam(m, v3(x0 - 0.9f, 0.1f + 0.12f * static_cast<float>(k), z0 + 0.3f), v3(x0 - 0.9f, 0.1f + 0.12f * static_cast<float>(k), z1 - 0.3f), 0.06f, plank * 0.9f);
  (void)seed;
}

// Planning: the building is only its plan: stakes at its corners, string between them, its
// foundation trench marked, a surveyor's table, and piles of stone and timber waiting.
template <typename M>
void planning(M &m, uint32_t seed) {
  const Bounds b = bounds(m);
  m.positions.clear();
  m.normals.clear();
  m.colors.clear();
  m.indices.clear();
  m.bone_a.clear();
  m.bone_b.clear();
  m.bone_w.clear();
  if (m.bones.size() > 1) m.bones.resize(1);
  m.collision.clear();
  if constexpr (has_fires<M>::value) {
    m.fire_points.clear();
    m.fire_configs.clear();
    m.fire_lit.clear();
  }
  const vec3 stake = v3(0.6f, 0.48f, 0.3f), string = v3(0.95f, 0.92f, 0.8f), earth = v3(0.42f, 0.33f, 0.24f);
  const float x0 = b.lo.x, x1 = b.hi.x, z0 = b.lo.z, z1 = b.hi.z;
  const vec3 c[4] = {v3(x0, 0, z0), v3(x1, 0, z0), v3(x1, 0, z1), v3(x0, 0, z1)};
  for (int k = 0; k < 4; ++k) {
    beam(m, c[k] + v3(0, -0.1f, 0), c[k] + v3(0, 0.55f, 0), 0.03f, stake);
    beam(m, c[k] + v3(0, 0.45f, 0), c[(k + 1) % 4] + v3(0, 0.45f, 0), 0.008f, string);
    // The trench, dug along the line.
    beam(m, c[k] + v3(0, 0.01f, 0), c[(k + 1) % 4] + v3(0, 0.01f, 0), 0.18f, earth);
  }
  // Inside the staked rectangle (the plot is clear): the surveyor's table and a sheet on it, a
  // ranging rod, and piles of stone and timber waiting.
  const float ix0 = x0 + 0.5f, ix1 = x1 - 0.5f, iz0 = z0 + 0.5f, iz1 = z1 - 0.5f;
  const vec3 t = v3(ix1 - 0.4f, 0, iz1 - 0.3f);
  for (int s = 0; s < 4; ++s) beam(m, t + v3(s & 1 ? 0.35f : -0.35f, 0, s & 2 ? 0.25f : -0.25f), t + v3(s & 1 ? 0.35f : -0.35f, 0.8f, s & 2 ? 0.25f : -0.25f), 0.025f, stake);
  box(m, t + v3(0, 0.82f, 0), v3(0.42f, 0.02f, 0.3f), stake * 1.1f);
  box(m, t + v3(0.05f, 0.845f, 0), v3(0.3f, 0.004f, 0.22f), v3(0.92f, 0.88f, 0.75f));
  beam(m, v3(ix0 + 0.2f, 0, iz1 - 0.2f), v3(ix0 + 0.2f, 2.2f, iz1 - 0.2f), 0.02f, v3(0.85f, 0.2f, 0.18f));  // the ranging rod
  for (int k = 0; k < 6; ++k) box(m, v3(ix0 + 0.3f + 0.5f * static_cast<float>(k % 3), 0.18f + 0.36f * static_cast<float>(k / 3), iz0 + 0.3f), v3(0.25f, 0.18f, 0.2f), v3(0.62f, 0.6f, 0.56f));
  for (int k = 0; k < 4; ++k) {
    const float y = 0.08f + 0.13f * static_cast<float>(k), zl = minf(iz0 + 2.6f, iz1 - 0.8f);
    beam(m, v3(ix0 + 0.4f, y, iz0 + 1.0f), v3(ix0 + 0.4f, y, maxf(zl, iz0 + 1.2f)), 0.06f, stake * 0.9f);
  }
  (void)seed;
}

}  // namespace condition

}  // namespace wall_build_v1
