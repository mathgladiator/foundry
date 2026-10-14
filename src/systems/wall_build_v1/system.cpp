// wall_build_v1: a wall network (design/planner.md, "Walls, fences, and gates"). A wall runs on the
// hex grid's diagonal lattice (line_v1_network): from node to node along lines through opposite
// corners, vertex to vertex through each cell and along the edge between the next two, so it stays
// straight however long it runs, and its lines meet at 60 and 120 degrees. This builds a whole
// network (or one chunk of it) as one model, at its nodes' heights.
//
// 1. Runs: each unit link (3 m) is a run from its node to the next, rising with the ground, less
//    what a gate opens. By material: a palisade of pointed logs with rails behind, a stone or brick
//    wall (battered, with a coping, a brick string course, a few proud stones), or an earth bank
//    with a timber crown; battlements or a wall-walk with a parapet (a palisade's: a fighting
//    platform on posts) on its inside (+z: a counter-clockwise network's inside); buttresses out.
// 2. Joins, by each node's arms: an end gets a square pier; a bend a pier wide enough to cover its
//    mitre, or a tower when it turns at least tower_turn_deg; a junction (three arms or more) a
//    tower; a straight node a tower every tower_every_m along its run, or where the network says.
//    Towers: square or round, slits, a crenellated top or a cone or pyramid roof, a banner.
// 3. Gates: a gate node opens 3 m centred on it, a gate link its 3 m; touching openings merge. The
//    model reports each opening's middle, turn, and width, where gate_v1 goes.
// 4. Chunks: with `chunks` above one, unit links are split by angle round the network's middle and
//    only `chunk`'s (and the nodes it owns) are built.
// 5. Guides for the viewer: the lattice's links and the cells they run through, the openings.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "condition.h"
#include "network.h"
#include "channels/wall_v1_model.h"
#include "channels/wall_v1_spec.h"
#include "packer/wall_v1_model.h"
#include "packer/wall_v1_spec.h"

namespace wall_build_v1 {

using channels::house_v1_bone;
using channels::wall_v1_kind;
using channels::wall_v1_model;
using channels::wall_v1_spec;

namespace {

using B = Builder<wall_v1_model>;

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }
const vec3 kSlit = {0.08f, 0.07f, 0.07f};

struct Wall {
  const wall_v1_kind &k;
  float H, t, bt, F;
  float half() const { return t * 0.5f + bt; }
};

// A sheared slab: x from x0 to x1, its bottom at y0 and top at y1 (plus rise * x / len), half
// widths hb at the bottom and ht at the top, centred on z = zc.
void slab(B &b, float x0, float x1, float y0, float y1, float hb, float ht, float zc, float rise, float len, vec3 col, bool snowy = true) {
  auto P = [&](float x, float y, float z) { return v3(x, y + (len > 0 ? rise * x / len : 0), z); };
  const vec3 a0 = P(x0, y0, zc - hb), a1 = P(x1, y0, zc - hb), a2 = P(x1, y1, zc - ht), a3 = P(x0, y1, zc - ht);
  const vec3 c0 = P(x0, y0, zc + hb), c1 = P(x1, y0, zc + hb), c2 = P(x1, y1, zc + ht), c3 = P(x0, y1, zc + ht);
  b.quad(a0, a1, a2, a3, col * 0.82f, 0, false, v3(0, 0, -1));
  b.quad(c0, c1, c2, c3, col * 0.95f, 0, false, v3(0, 0, 1));
  b.quad(a3, a2, c2, c3, col, 0, snowy, v3(0, 1, 0));
  b.quad(a0, a3, c3, c0, col * 0.75f, 0, false, v3(-1, 0, 0));
  b.quad(a1, a2, c2, c1, col * 0.75f, 0, false, v3(1, 0, 0));
}

// A palisade log at x from y0 up to its tip.
void log_(B &b, const Wall &w, float x, float z, float y0, float top, float r, vec3 col) {
  const int n = b.detail == 0 ? 6 : (b.detail == 1 ? 5 : 4);
  b.frustum(v3(x, y0, z), v3(x, top, z), r, r * 0.95f, n, col, 0, false, false);
  b.frustum(v3(x, top, z), v3(x, top + r * 2.4f, z), r * 0.95f, 0, n, lerp(col, w.k.trim_color, 0.5f), 0, false, true);
}

void run_piece(B &b, const Wall &w, float len, float rise, uint32_t seed) {
  const wall_v1_kind &k = w.k;
  auto pick = [&](uint32_t salt) { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); };
  auto yat = [&](float x) { return len > 0 ? rise * x / len : 0.0f; };
  const float H = w.H, F = w.F, t = w.t;
  if (k.material == wall_v1_kind::kMaterialLogs) {
    if (b.detail == 2) {
      slab(b, 0, len, -F, H, t * 0.5f, t * 0.5f, 0, rise, len, k.color, false);
      // A sawtooth of tips along the top.
      const int n = static_cast<int>(len / 0.6f) + 1;
      for (int i = 0; i < n; ++i) {
        const float x0 = len * static_cast<float>(i) / static_cast<float>(n), x1 = len * static_cast<float>(i + 1) / static_cast<float>(n);
        const float xm = (x0 + x1) * 0.5f;
        for (int s = -1; s <= 1; s += 2)
          b.triangle(v3(x0, H + yat(x0), t * 0.5f * static_cast<float>(s)), v3(x1, H + yat(x1), t * 0.5f * static_cast<float>(s)),
                     v3(xm, H + yat(xm) + 0.5f, 0), k.trim_color, 0, true, v3(0, 0.3f, static_cast<float>(s)));
      }
    } else {
      const int n = maxi_(1, round_to_int(len / (t * 1.05f)));
      const float r = len / static_cast<float>(n) * 0.5f;
      for (int i = 0; i < n; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
        const float top = H * (0.94f + 0.06f * pick(10 + static_cast<uint32_t>(i))) + yat(x);
        log_(b, w, x, 0, -F + yat(x), top, r, k.color * (0.9f + 0.15f * pick(30 + static_cast<uint32_t>(i))));
      }
    }
    // Rails behind (inside), and a fighting platform on posts.
    for (float y : {0.7f, H * 0.72f}) b.box(v3(len * 0.5f, y + yat(len * 0.5f), t * 0.5f + 0.06f), v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1), v3(len * 0.5f, 0.06f, 0.05f), k.trim_color * 0.85f);
    if (k.walkway) {
      const float py = H - 1.35f;
      b.box(v3(len * 0.5f, py + yat(len * 0.5f), t * 0.5f + 0.55f), v3(len * 0.5f, 0.05f, 0.45f), k.trim_color);
      b.beam(v3(len * 0.5f, -0.2f + yat(len * 0.5f), t * 0.5f + 0.95f), v3(len * 0.5f, py + yat(len * 0.5f), t * 0.5f + 0.95f), 0.07f, k.trim_color * 0.8f);
    }
  } else if (k.material == wall_v1_kind::kMaterialMegalith) {
    // Standing stones every couple of metres on a low bank, lintels across pairs (trilithons).
    slab(b, 0, len, -F, 0.35f, t * 0.6f + 0.3f, t * 0.4f, 0, rise, len, lerp(k.color, v3(0.42f, 0.55f, 0.3f), 0.6f));
    const int n = maxi_(1, round_to_int(len / 2.2f));
    float tops[16];
    for (int i = 0; i < n && i < 16; ++i) {
      const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
      const float h = H * (0.82f + 0.18f * pick(10 + static_cast<uint32_t>(i)));
      const float lean = (pick(20 + static_cast<uint32_t>(i)) - 0.5f) * 0.08f;
      const vec3 ay = normalize(v3(lean, 1, (pick(30 + static_cast<uint32_t>(i)) - 0.5f) * 0.06f));
      const vec3 ax = normalize(cross(ay, v3(0, 0, 1)) * -1.0f);
      b.box(v3(x, (h - F) * 0.5f + yat(x), 0), ax, ay, cross(ax, ay), v3(0.45f, (h + F) * 0.5f, t * 0.4f),
            k.color * (0.85f + 0.2f * pick(40 + static_cast<uint32_t>(i))), 0, true);
      tops[i] = h;
    }
    for (int i = 0; i + 1 < n && i + 1 < 16; i += 2) {
      const float xa = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n) - 0.45f, xb = (static_cast<float>(i) + 1.5f) * len / static_cast<float>(n) + 0.45f;
      const float y = minf(tops[i], tops[i + 1]) - 0.05f;
      b.box(v3((xa + xb) * 0.5f, y + 0.25f + yat((xa + xb) * 0.5f), 0), v3((xb - xa) * 0.5f, 0.25f, t * 0.42f), k.trim_color, 0, true);
    }
  } else if (k.material == wall_v1_kind::kMaterialLiving) {
    // Willow and hawthorn grown and woven: trunks, a woven core, leafy crowns along the top.
    const vec3 leaf = b.winter ? lerp(k.color, v3(0.55f, 0.5f, 0.42f), 0.7f) : k.color;
    slab(b, 0, len, -F, H * 0.75f, t * 0.3f, t * 0.25f, 0, rise, len, k.trim_color * 0.9f, false);
    const int n = maxi_(1, round_to_int(len / 1.0f));
    for (int i = 0; i < n; ++i) {
      const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
      b.frustum(v3(x, -F + yat(x), 0), v3(x, H * 0.8f + yat(x), 0), 0.12f, 0.08f, 5, k.trim_color, 0, false, false);
      const float h = H * (0.9f + 0.15f * pick(50 + static_cast<uint32_t>(i)));
      b.frustum(v3(x, H * 0.45f + yat(x), 0), v3(x, h + yat(x), 0), t * 0.65f, t * 0.2f, 6, leaf * (0.9f + 0.2f * pick(60 + static_cast<uint32_t>(i))), 0, true, true);
    }
    if (b.detail < 2)  // the weave: diagonals across the core
      for (int i = 0; i < n; ++i)
        for (int s = -1; s <= 1; s += 2) {
          const float xa = static_cast<float>(i) * len / static_cast<float>(n), xb = static_cast<float>(i + 1) * len / static_cast<float>(n);
          b.beam(v3(xa, 0.3f + yat(xa), static_cast<float>(s) * t * 0.3f), v3(xb, H * 0.7f + yat(xb), static_cast<float>(s) * t * 0.3f), 0.03f,
                 k.trim_color * 1.1f, 0, false);
        }
  } else if (k.material == wall_v1_kind::kMaterialEarth) {
    // A bank, grass on its faces, and a timber crown along its top.
    const vec3 bank = b.winter ? b.snow : v3(0.42f, 0.55f, 0.3f);
    slab(b, 0, len, -F, H * 0.65f, t * 0.5f + w.bt + 0.6f, t * 0.5f, 0, rise, len, lerp(k.color, bank, 0.5f));
    const int n = maxi_(1, round_to_int(len / 1.2f));
    for (int i = 0; i < n; ++i) {
      const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
      b.beam(v3(x, H * 0.6f + yat(x), 0), v3(x, H + 0.2f + yat(x), 0), 0.07f, k.trim_color);
    }
    for (float y : {H * 0.8f, H}) b.box(v3(len * 0.5f, y + yat(len * 0.5f), -0.08f), v3(len * 0.5f, 0.05f, 0.04f), k.trim_color * 0.9f);
  } else {
    // Stone or brick: a battered wall, its coping, and its crown.
    const vec3 col = k.color;
    slab(b, 0, len, -F, H, t * 0.5f + w.bt, t * 0.5f, 0, rise, len, col, false);
    if (k.walkway) {
      // A parapet on the outer edge, a low lip on the inner; the walk between them.
      slab(b, 0, len, H, H + 1.0f, 0.2f, 0.2f, -t * 0.5f + 0.2f, rise, len, col * 0.97f);
      if (t > 1.2f) slab(b, 0, len, H, H + 0.3f, 0.12f, 0.12f, t * 0.5f - 0.12f, rise, len, col * 0.95f);
      if (k.crenellated && b.detail < 2) {
        const float cw = 0.55f;
        const int n = static_cast<int>(len / (2 * cw));
        const float pad = (len - static_cast<float>(n) * 2 * cw + cw) * 0.5f;
        for (int i = 0; i < n; ++i) {
          const float x = pad + 2 * cw * static_cast<float>(i) + cw * 0.5f;
          b.box(v3(x, H + 1.0f + 0.32f + yat(x), -t * 0.5f + 0.2f), v3(cw * 0.5f, 0.32f, 0.2f), col * 0.97f);
        }
      }
    } else if (k.crenellated && b.detail < 2) {
      const float cw = 0.5f;
      const int n = static_cast<int>(len / (2 * cw));
      const float pad = (len - static_cast<float>(n) * 2 * cw + cw) * 0.5f;
      for (int i = 0; i < n; ++i) {
        const float x = pad + 2 * cw * static_cast<float>(i) + cw * 0.5f;
        b.box(v3(x, H + 0.35f + yat(x), 0), v3(cw * 0.5f, 0.35f, t * 0.5f), col * 0.97f);
      }
    } else {
      slab(b, 0, len, H, H + 0.08f, t * 0.5f + 0.05f, t * 0.5f + 0.05f, 0, rise, len, k.trim_color);
    }
    if (k.material == wall_v1_kind::kMaterialDrystone && b.detail < 2) {
      // Unmortared courses, and a row of upright cap stones along the top.
      for (float y = 0.28f; y < H - 0.1f; y += 0.28f)
        for (int s = -1; s <= 1; s += 2) {
          const float face = t * 0.5f + w.bt * (1.0f - y / H) + 0.01f;
          slab(b, 0, len, y, y + 0.025f, 0.012f, 0.012f, static_cast<float>(s) * face, rise, len, col * 0.72f, false);
        }
      const int n = static_cast<int>(len / 0.32f);
      for (int i = 0; i < n; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
        const float lean = (pick(200 + static_cast<uint32_t>(i)) - 0.5f) * 0.3f;
        const vec3 ay = normalize(v3(lean, 1, 0)), ax = normalize(v3(1, -lean, 0));
        b.box(v3(x, H + 0.22f + yat(x), 0), ax, ay, v3(0, 0, 1), v3(0.12f, 0.22f, t * 0.45f), col * (0.85f + 0.2f * pick(220 + static_cast<uint32_t>(i))), 0, true);
      }
    }
    if (k.material == wall_v1_kind::kMaterialBrick && b.detail < 2)
      for (int s = -1; s <= 1; s += 2) {
        const float zc = static_cast<float>(s) * (t * 0.5f + w.bt * 0.6f + 0.02f);
        slab(b, 0, len, H * 0.38f, H * 0.42f, 0.03f, 0.03f, zc, rise, len, k.trim_color, false);
      }
    if (b.detail == 0) {
      // A few proud stones on each face.
      for (int i = 0; i < 6; ++i) {
        const float x = 0.3f + (len - 0.6f) * pick(100 + static_cast<uint32_t>(i)), y = 0.3f + (H - 0.8f) * pick(120 + static_cast<uint32_t>(i));
        const float s = i % 2 ? 1.0f : -1.0f;
        const float face = t * 0.5f + w.bt * (1.0f - y / H);
        b.box(v3(x, y + yat(x), s * (face + 0.01f)), v3(0.22f + 0.1f * pick(140 + static_cast<uint32_t>(i)), 0.12f, 0.03f), col * (0.86f + 0.2f * pick(160 + static_cast<uint32_t>(i))), 0, false);
      }
    }
    if (k.buttresses && len > 1.5f) {
      const float x = len * 0.5f;
      const float face = -(t * 0.5f + w.bt);
      b.box(v3(x, (H * 0.75f - F) * 0.5f + yat(x), face - 0.3f), v3(0.35f, (H * 0.75f + F) * 0.5f, 0.32f), col * 0.92f);
      b.box(v3(x, H * 0.75f + 0.2f + yat(x), face - 0.15f), v3(0.35f, 0.2f, 0.17f), k.trim_color);
    }
  }
  const float mid_y = (H + rise) * 0.5f;
  b.solid(v3(len * 0.5f, mid_y * 0.5f + H * 0.25f, 0), v3(len * 0.5f, (H + (rise < 0 ? -rise : rise)) * 0.5f, t * 0.5f + w.bt * 0.5f));
}

// A pier on the bisector of a turn (corners) or square to the line (ends).
void pier(B &b, const Wall &w, float turn_deg, bool end) {
  const wall_v1_kind &k = w.k;
  const float half_turn = turn_deg * 0.5f * kDeg;
  const float c = foundry::det::cos(half_turn);
  const float r = (w.half() + 0.08f) / maxf(c, 0.3f) + (end ? 0.12f : 0.0f);
  const float top = w.H + (k.material == wall_v1_kind::kMaterialLogs ? 0.25f : 0.15f) + (k.walkway && k.material != wall_v1_kind::kMaterialLogs ? 1.0f : 0.0f);
  if (k.material == wall_v1_kind::kMaterialLogs) {
    log_(b, w, 0, 0, -w.F, top, r * 0.75f, k.color * 0.95f);
  } else if (k.material == wall_v1_kind::kMaterialMegalith) {
    b.box(v3(0, (top - w.F) * 0.5f, 0), v3(r * 0.8f, (top + w.F) * 0.5f, r * 0.8f), k.color * 0.95f);
  } else if (k.material == wall_v1_kind::kMaterialLiving) {
    b.frustum(v3(0, -w.F, 0), v3(0, top, 0), 0.22f, 0.15f, 6, k.trim_color, 0, false, false);
    b.frustum(v3(0, w.H * 0.4f, 0), v3(0, top + 0.4f, 0), r + 0.2f, 0.2f, 7, k.color, 0, true, true);
  } else if (k.material == wall_v1_kind::kMaterialEarth) {
    const vec3 bank = b.winter ? b.snow : v3(0.42f, 0.55f, 0.3f);
    b.frustum(v3(0, -w.F, 0), v3(0, w.H * 0.65f, 0), r + 0.6f, w.t * 0.5f + 0.05f, 8, lerp(k.color, bank, 0.5f));
    b.beam(v3(0, w.H * 0.6f, 0), v3(0, w.H + 0.3f, 0), 0.09f, k.trim_color);
  } else {
    const int n = end ? 4 : 8;
    const float turn = end ? kPi * 0.25f : -half_turn + kPi / 8.0f;
    b.frustum(v3(0, -w.F, 0), v3(0, top, 0), r * (end ? 1.42f : 1.0f), r * (end ? 1.42f : 1.0f), n, k.color * 0.96f, 0, true, true, turn);
    b.frustum(v3(0, top, 0), v3(0, top + 0.1f, 0), r * (end ? 1.5f : 1.06f), r * (end ? 1.5f : 1.06f), n, k.trim_color, 0, true, true, turn);
  }
  b.solid_cylinder(v3(0, 0, 0), r, top);
}

void tower(B &b, const Wall &w, float turn_deg, uint32_t seed) {
  const wall_v1_kind &k = w.k;
  if (k.tower == wall_v1_kind::kTowerNone) {
    pier(b, w, turn_deg, true);
    return;
  }
  auto pick = [&](uint32_t salt) { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); };
  const float phi = turn_deg * 0.5f * kDeg;
  const vec3 ax = v3(foundry::det::cos(phi), 0, -foundry::det::sin(phi)), az = cross(ax, v3(0, 1, 0)) * -1.0f;
  const float R = maxf(k.tower_radius, w.half() + 0.3f);
  const float top = w.H + maxf(k.tower_height, 0.5f);
  const bool round = k.tower == wall_v1_kind::kTowerRound;
  const vec3 col = k.color;
  const bool logs = k.material == wall_v1_kind::kMaterialLogs;
  if (round) {
    const int n = b.detail == 0 ? 14 : (b.detail == 1 ? 10 : 7);
    b.frustum(v3(0, -w.F, 0), v3(0, top, 0), R + w.bt * 0.6f, R, n, col, 0, true, false);
  } else {
    b.box(v3(0, (top - w.F) * 0.5f, 0), ax, v3(0, 1, 0), az, v3(R, (top + w.F) * 0.5f, R), col, 0, false);
  }
  // Slits on the outer faces.
  if (b.detail < 2 && !logs)
    for (int lvl = 0; lvl < 2; ++lvl)
      for (int f = 0; f < 3; ++f) {
        const float a = (static_cast<float>(f) - 1.0f) * kPi * 0.5f;
        const vec3 n = az * -foundry::det::cos(a) + ax * foundry::det::sin(a);  // facing out (-az), and the sides
        const vec3 c = n * (R + 0.01f) + v3(0, top * (0.35f + 0.35f * static_cast<float>(lvl)), 0);
        b.window(c, n, 0.14f, 0.7f, k.trim_color, kSlit, false);
      }
  // The top.
  if (k.tower_roof == wall_v1_kind::kTowerRoofCone) {
    const int n = round ? (b.detail == 0 ? 14 : 8) : 4;
    b.frustum(v3(0, top, 0), v3(0, top + R * 1.7f, 0), R * (round ? 1.12f : 1.5f), 0, n, k.roof_color, 0, true, true, round ? 0.0f : -phi + kPi * 0.25f);
  } else if (k.tower_roof == wall_v1_kind::kTowerRoofPyramid) {
    b.pyramid(v3(0, top, 0), ax, az, R + 0.2f, R + 0.2f, R * 1.3f, k.roof_color);
  } else if (k.tower_roof == wall_v1_kind::kTowerRoofDome) {
    // A corbelled dome: courses stepping in.
    const int rings = b.detail == 2 ? 3 : 6, n = b.detail == 0 ? 14 : 8;
    for (int i = 0; i < rings; ++i) {
      const float a0 = kPi * 0.5f * static_cast<float>(i) / static_cast<float>(rings), a1 = kPi * 0.5f * static_cast<float>(i + 1) / static_cast<float>(rings);
      const float r0 = (R + 0.1f) * foundry::det::cos(a0), r1 = (R + 0.1f) * foundry::det::cos(a1);
      b.frustum(v3(0, top + R * foundry::det::sin(a0), 0), v3(0, top + R * foundry::det::sin(a1), 0), r0, i == rings - 1 ? 0.0f : r1, n,
                k.roof_color * (0.95f + 0.05f * static_cast<float>(i % 2)), 0, false, true);
    }
  } else if (k.material == wall_v1_kind::kMaterialMegalith) {
    b.box(v3(0, top + 0.2f, 0), ax, v3(0, 1, 0), az, v3(R + 0.25f, 0.2f, R + 0.25f), k.trim_color);  // a capstone
  } else if (logs) {
    b.box(v3(0, top + 0.05f, 0), ax, v3(0, 1, 0), az, v3(R + 0.1f, 0.05f, R + 0.1f), k.trim_color);
    for (int s = 0; s < 4; ++s) {
      const vec3 c = (s & 1 ? ax : ax * -1.0f) * (R * 0.85f) + (s & 2 ? az : az * -1.0f) * (R * 0.85f);
      b.beam(c + v3(0, top, 0), c + v3(0, top + 1.1f, 0), 0.06f, k.trim_color);
    }
  } else if (b.detail < 2) {
    // A crenellated top: merlons round its edge.
    const int n = round ? 8 : 4;
    for (int s = 0; s < n; ++s) {
      if (round) {
        const float a = 2 * kPi * (static_cast<float>(s) + 0.5f) / static_cast<float>(n);
        const vec3 d = ax * foundry::det::cos(a) + az * foundry::det::sin(a);
        b.box(d * (R - 0.2f) + v3(0, top + 0.35f, 0), cross(v3(0, 1, 0), d), v3(0, 1, 0), d, v3(0.35f, 0.35f, 0.2f), col * 0.97f);
      } else {
        const vec3 e0 = (s & 1 ? ax : ax * -1.0f) * R, side = s & 1 ? az : ax;
        for (int m = -1; m <= 1; ++m) b.box(e0 + side * (static_cast<float>(m) * R * 0.6f) + v3(0, top + 0.35f, 0), v3(0.25f, 0.35f, 0.25f), col * 0.97f);
        (void)side;
      }
    }
  }
  if (k.banners) {
    const float roof = k.tower_roof == wall_v1_kind::kTowerRoofCone ? R * 1.7f
                       : (k.tower_roof == wall_v1_kind::kTowerRoofPyramid ? R * 1.3f : (k.tower_roof == wall_v1_kind::kTowerRoofDome ? R : 0.0f));
    b.banner(v3(0, top + roof - 0.1f, 0), 1.6f, ax, k.accent, pick(7));
  }
  if (round) b.solid_cylinder(v3(0, 0, 0), R, top);
  else b.solid(v3(0, top * 0.5f, 0), v3(R, top * 0.5f, R), -phi);
}

}  // namespace

bool run(const wall_v1_spec &in, wall_v1_model &out, std::string &error) {
  const wall_v1_kind &k = in.kind;
  if (in.season > 3 || in.detail > 2 || k.material > wall_v1_kind::kMaterialLiving || k.tower > 2 || k.tower_roof > wall_v1_kind::kTowerRoofDome ||
      in.condition > wall_v1_spec::kConditionRuin) {
    error = "season, detail, or the kind's material, tower, or tower roof out of range";
    return false;
  }
  out = wall_v1_model();
  Network net;
  if (!expand(in.network, in.chunks, net, error)) return false;
  B b{out};
  b.winter = in.season == wall_v1_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 10 : (in.detail == 1 ? 7 : 5);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_v1_bone::kMotionFixed, 0, 0, 0, 0);
  const Wall w{k, clampf(k.height, 1.5f, 14.0f), clampf(k.thickness, 0.25f, 4.0f), clampf(k.batter, 0, 1.5f), clampf(k.footing, 0.2f, 3.0f)};
  const uint32_t base_seed = hash_u32(in.seed * 2654435761u ^ hash_str(k.name.c_str()));
  // A chunk past the last wraps round, so every chunk asked for has something in it.
  const bool all = net.parts <= 1;
  const uint32_t chunk = in.chunk % net.parts;
  auto mine_u = [&](const Unit &u) { return all || u.chunk == chunk; };
  auto mine_n = [&](int i) { return all || net.node_chunk[i] == chunk; };
  const float L = 3.0f;  // a unit link
  // ---- 1. runs ----
  for (size_t ui = 0; ui < net.units.size(); ++ui) {
    const Unit &u = net.units[ui];
    if (!mine_u(u)) continue;
    const float x0 = u.cut0 * L, x1 = L - u.cut1 * L;
    if (x1 - x0 < 0.05f) continue;
    const vec3 A = net.pos(u.a), Bp = net.pos(u.b);
    const float rise = Bp.y - A.y;
    const size_t v0 = out.positions.size(), c0 = out.collision.size(), b0 = out.bones.size();
    run_piece(b, w, x1 - x0, rise * (x1 - x0) / L, base_seed ^ static_cast<uint32_t>(ui * 7919u));
    to_world(out, v0, c0, b0, A + dir(u.k) * x0 + v3(0, rise * x0 / L, 0), u.k);
  }
  // ---- 2. joins ----
  // Towers along straight runs: walk each chain between joins, a tower once tower_every_m has passed
  // (never on a gate or beside one).
  std::vector<uint8_t> walked(net.units.size(), 0);
  auto join_node = [&](int i) { return !net.straight(i) || net.gate[i]; };
  auto beside_gate = [&](int i) {
    for (const Unit &u : net.units)
      if ((u.a == i && (net.gate[u.b] || u.cut1 > 0 || u.cut0 > 0)) || (u.b == i && (net.gate[u.a] || u.cut0 > 0 || u.cut1 > 0))) return true;
    return false;
  };
  if (k.tower != wall_v1_kind::kTowerNone && k.tower_every_m > 0)
    for (size_t pass = 0; pass < 2; ++pass)  // chains from joins first, then loops with none
      for (size_t s = 0; s < net.q.size(); ++s) {
        const int start = static_cast<int>(s);
        if (pass == 0 && !join_node(start)) continue;
        for (size_t ui = 0; ui < net.units.size(); ++ui) {
          if (walked[ui] || (net.units[ui].a != start && net.units[ui].b != start)) continue;
          float since = 0;
          int at = start;
          size_t cur = ui;
          for (size_t guard = 0; guard < net.units.size(); ++guard) {
            walked[cur] = 1;
            at = net.units[cur].a == at ? net.units[cur].b : net.units[cur].a;
            since += L;
            if (join_node(at) || at == start) break;
            if (since >= k.tower_every_m && !beside_gate(at)) {
              net.tower[at] = 1;
              since = 0;
            }
            size_t next = net.units.size();
            for (size_t vj = 0; vj < net.units.size(); ++vj)
              if (!walked[vj] && (net.units[vj].a == at || net.units[vj].b == at)) next = vj;
            if (next == net.units.size()) break;
            cur = next;
          }
        }
      }
  uint32_t joins = 0;
  for (size_t s = 0; s < net.q.size(); ++s) {
    const int i = static_cast<int>(s);
    if (!mine_n(i) || net.arms[i] == 0 || net.gate[i]) continue;
    const int deg = net.degree(i);
    int k0 = 0;
    while (!((net.arms[i] >> k0) & 1)) ++k0;
    float turn = 0;
    if (deg == 2 && !net.straight(i)) {
      int k1 = k0 + 1;
      while (!((net.arms[i] >> k1) & 1)) ++k1;
      const int sep = k1 - k0 > 3 ? 6 - (k1 - k0) : k1 - k0;  // 1: a sharp bend, 2: a gentle one
      turn = 180.0f - 60.0f * static_cast<float>(sep);
    }
    const bool towered = net.tower[i] || deg >= 3 || (deg == 2 && turn > 0 && turn >= k.tower_turn_deg);
    if (deg == 2 && turn == 0 && !towered) continue;  // straight through: the runs meet
    const size_t v0 = out.positions.size(), c0 = out.collision.size(), b0 = out.bones.size();
    if (towered) tower(b, w, 0, base_seed ^ static_cast<uint32_t>(i * 104729));
    else if (deg == 1) pier(b, w, 0, true);
    else pier(b, w, turn, false);
    to_world(out, v0, c0, b0, net.pos(i), k0);
    ++joins;
  }
  // ---- 3. gates: the openings, merged where they touch ----
  struct Opening {
    vec3 a, c;
    int k;
  };
  std::vector<Opening> open;
  for (const Unit &u : net.units) {
    if (!mine_u(u)) continue;
    const vec3 A = net.pos(u.a), Bp = net.pos(u.b);
    if (u.cut0 > 0) open.push_back({A, A + (Bp - A) * u.cut0, u.k});
    if (u.cut1 > 0) open.push_back({Bp + (A - Bp) * u.cut1, Bp, u.k});
  }
  for (bool merged = true; merged;) {
    merged = false;
    for (size_t i = 0; i < open.size() && !merged; ++i)
      for (size_t j = i + 1; j < open.size() && !merged; ++j) {
        if (open[i].k % 3 != open[j].k % 3) continue;
        const vec3 ends[2][2] = {{open[i].a, open[i].c}, {open[j].a, open[j].c}};
        for (int ei = 0; ei < 2 && !merged; ++ei)
          for (int ej = 0; ej < 2 && !merged; ++ej) {
            const vec3 d = ends[0][ei] - ends[1][ej];
            if (d.x * d.x + d.z * d.z > 1e-4f) continue;
            open[i] = {ends[0][1 - ei], ends[1][1 - ej], open[i].k};
            open.erase(open.begin() + static_cast<long>(j));
            merged = true;
          }
      }
  }
  for (const Opening &o : open) {
    out.gate_positions.push_back((o.a + o.c) * 0.5f);
    out.gate_yaws.push_back(dir_yaw(o.k % 3) * 180.0f / kPi);
    out.gate_widths.push_back(foundry::det::hypot(o.c.x - o.a.x, o.c.z - o.a.z));
  }
  // A ruin: broken down to a ragged height over its line's ground, its banners gone, overgrown,
  // rubble beside it (condition.h).
  if (in.condition == wall_v1_spec::kConditionRuin) {
    auto ground = [&](float x, float z) {
      float best = 1e30f, y = 0;
      for (const Unit &u : net.units) {
        const vec3 A = net.pos(u.a), Bp = net.pos(u.b), d = Bp - A;
        const float l2 = d.x * d.x + d.z * d.z;
        const float t = l2 > 0 ? clampf(((x - A.x) * d.x + (z - A.z) * d.z) / l2, 0, 1) : 0.0f;
        const float dx = A.x + d.x * t - x, dz = A.z + d.z * t - z, dd = dx * dx + dz * dz;
        if (dd < best) {
          best = dd;
          y = A.y + d.y * t;
        }
      }
      return y;
    };
    condition::ruin_over(out, base_seed ^ 0x5eedu, 0.5f, ground, w.H + maxf(k.tower_height, 0.0f) * 0.5f, false);
  }
  // ---- 4. guides ----
  auto line = [&](vec3 a, vec3 c, vec3 col) {
    out.guide_lines.push_back(a);
    out.guide_lines.push_back(c);
    out.guide_colors.push_back(pack(col));
  };
  for (size_t s = 0; s < net.q.size(); ++s) {
    if (!mine_n(static_cast<int>(s))) continue;
    const vec3 c = net.pos(static_cast<int>(s)) + v3(0, 0.05f, 0);
    for (int e = 0; e < 6; ++e) line(c + v3(kDirX[e], 0, kDirZ[e]), c + v3(kDirX[(e + 1) % 6], 0, kDirZ[(e + 1) % 6]), v3(0.5f, 0.55f, 0.6f));
  }
  for (const Unit &u : net.units)
    if (mine_u(u)) line(net.pos(u.a) + v3(0, 0.06f, 0), net.pos(u.b) + v3(0, 0.06f, 0), v3(1, 1, 1));
  for (const Opening &o : open) line(o.a + v3(0, 0.1f, 0), o.c + v3(0, 0.1f, 0), v3(1.0f, 0.8f, 0.3f));
  // ---- size ----
  float top = -1e30f, reach = 0;
  for (const vec3 &p : out.positions) {
    top = maxf(top, p.y);
    reach = maxf(reach, foundry::det::hypot(p.x, p.z));
  }
  for (const auto &body : out.collision)
    for (const auto &sh : body.shapes) {
      const float r = sh.type == channels::physics_v1_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
      top = maxf(top, sh.position.y + (sh.type == channels::physics_v1_shape::kTypeBox ? sh.half_extents.y : sh.half_height));
    }
  out.height = out.positions.empty() ? 0 : top;
  out.radius = reach;
  out.links = static_cast<uint32_t>(net.units.size());
  out.joins = joins;
  return true;
}

}  // namespace wall_build_v1

FOUNDRY_SYSTEM(wall_build_v1, channels::wall_v1_spec, channels::wall_v1_model)
