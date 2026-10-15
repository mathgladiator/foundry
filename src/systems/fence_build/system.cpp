// fence_build: a fence network (design/planner.md, "Walls, fences, and gates"). A fence runs on
// the hex grid's diagonal lattice like a wall (line_network): from node to node along lines
// through opposite corners, straight however long it runs, meeting at 60 and 120 degrees. This
// builds a whole network (or one chunk of it) as one model, at its nodes' heights.
//
// 1. Runs: each unit link (3 m) from its node to the next, rising with the ground, less what a
//    gate opens: pickets between two rails, rails, split rails zigzagging, a woven wattle hurdle,
//    a hedge (leafy by season, swaying a little), a low dry-stone wall with coping stones, or iron
//    railings with finials; posts along it every module_m.
// 2. Nodes: a post at every node a fence reaches (so posts never double where links meet; a
//    hedge's rounded clump or a wall's pier where it ends, bends, or branches).
// 3. Gates: each opening (a gate node's 3 m, a gate link's 3 m, merged where they touch) gets a
//    gate that swings open on a hinge (house_bone, by door_time).
// 4. Chunks, as walls'; guides for the viewer: the lattice's links, the cells they run through,
//    the openings.
#include <vector>

#include "boundary.h"
#include "build.h"
#include "condition.h"
#include "network.h"
#include "channels/fence_model.h"
#include "channels/fence_spec.h"
#include "packer/fence_model.h"
#include "packer/fence_spec.h"

namespace fence_build {

using channels::fence_kind;
using channels::fence_model;
using channels::fence_spec;
using channels::house_bone;

namespace {

using B = Builder<fence_model>;

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

vec3 leaf(const fence_kind &k, uint32_t season) {
  switch (season) {
    case fence_spec::kSeasonSpring: return lerp(k.color, v3(0.5f, 0.75f, 0.35f), 0.35f);
    case fence_spec::kSeasonFall: return lerp(k.color, v3(0.7f, 0.45f, 0.2f), 0.5f);
    case fence_spec::kSeasonWinter: return lerp(k.color, v3(0.4f, 0.36f, 0.3f), 0.55f);
    default: return k.color;
  }
}

// A post at x (its foot `below` under the line), `h` tall.
void post(B &b, const fence_kind &k, float x, float y, float h, float below) {
  const float r = k.style == fence_kind::kStyleIron ? 0.025f : 0.055f;
  b.box(v3(x, y + (h - below) * 0.5f, 0), v3(r, (h + below) * 0.5f, r), k.post_color);
  if (k.style == fence_kind::kStyleIron) b.frustum(v3(x, y + h, 0), v3(x, y + h + 0.12f, 0), 0.035f, 0, 4, k.post_color);
  else b.box(v3(x, y + h + 0.02f, 0), v3(r + 0.015f, 0.02f, r + 0.015f), k.post_color * 1.1f);
}

void run_piece(B &b, const fence_kind &k, float len, float rise, uint32_t season, uint32_t seed, int detail) {
  const float module = clampf(k.module_m, 0.8f, 6.0f);
  auto pick = [&](uint32_t salt) { return unit(hash_u32(seed ^ (salt * 0x9e3779b9u))); };
  auto yat = [&](float x) { return len > 0 ? rise * x / len : 0.0f; };
  const float H = k.height, T = k.thickness;
  const float below = 0.3f + (rise < 0 ? -rise : rise);
  // A rail from x0 to x1 at height y along the line.
  auto rail = [&](float x0, float x1, float y, float half_y, float half_z, float z, vec3 col) {
    const vec3 a = v3(x0, y + yat(x0), z), c = v3(x1, y + yat(x1), z);
    const vec3 ax = normalize(c - a);
    b.box((a + c) * 0.5f, ax, normalize(cross(v3(0, 0, 1), ax)), v3(0, 0, 1), v3(length(c - a) * 0.5f, half_y, half_z), col);
  };
  switch (k.style) {
    case fence_kind::kStylePicket: {
      for (float x = module; x < len - 0.3f; x += module) post(b, k, x, yat(x), H, below);
      rail(0.05f, len, H * 0.25f, 0.035f, 0.02f, 0.06f, k.post_color);
      rail(0.05f, len, H * 0.75f, 0.035f, 0.02f, 0.06f, k.post_color);
      const int n = detail == 2 ? 0 : maxi_(1, round_to_int(len / (detail == 0 ? 0.13f : 0.26f)) - 1);
      for (int i = 1; i <= n; ++i) {
        const float x = len * static_cast<float>(i) / static_cast<float>(n + 1);
        const float y0 = yat(x) + 0.04f, y1 = yat(x) + H * (0.9f + 0.04f * pick(10 + static_cast<uint32_t>(i)));
        b.box(v3(x, (y0 + y1) * 0.5f, 0.1f), v3(0.035f, (y1 - y0) * 0.5f, 0.012f), k.color);
        if (detail == 0) b.triangle(v3(x - 0.035f, y1, 0.1f), v3(x + 0.035f, y1, 0.1f), v3(x, y1 + 0.06f, 0.1f), k.color, 0, false, v3(0, 0, 1));
      }
      if (detail == 2) rail(0.05f, len, H * 0.5f, H * 0.4f, 0.012f, 0.1f, k.color);
      break;
    }
    case fence_kind::kStyleRail: {
      for (float x = module; x < len - 0.3f; x += module) post(b, k, x, yat(x), H, below);
      for (int r = 0; r < 3; ++r) rail(0.0f, len, H * (0.3f + 0.3f * static_cast<float>(r)), 0.045f, 0.03f, 0.07f, k.color);
      break;
    }
    case fence_kind::kStyleSplitRail: {
      // Rails zigzag: each crosses the line at the piece's ends, out to either side at its middle.
      const float z = 0.22f;
      for (int r = 0; r < 3; ++r) {
        const float y = H * (0.25f + 0.32f * static_cast<float>(r));
        const vec3 a = v3(0, y, -z * (r % 2 ? -1.0f : 1.0f)), c = v3(len, y + rise, z * (r % 2 ? -1.0f : 1.0f));
        b.beam(a + v3(0, yat(0), 0), c, 0.05f, k.color * (0.9f + 0.15f * pick(20 + static_cast<uint32_t>(r))));
      }
      break;
    }
    case fence_kind::kStyleWattle: {
      for (float x = module; x < len - 0.3f; x += module) post(b, k, x, yat(x), H, below);
      // A woven panel: courses bulging alternately either side.
      const int n = detail == 0 ? 8 : (detail == 1 ? 4 : 1);
      for (int i = 0; i < n; ++i) {
        const float y0 = 0.05f + (H - 0.1f) * static_cast<float>(i) / static_cast<float>(n);
        const float y1 = 0.05f + (H - 0.1f) * static_cast<float>(i + 1) / static_cast<float>(n);
        const float z = (i % 2 ? 1.0f : -1.0f) * 0.02f;
        rail(0.06f, len, (y0 + y1) * 0.5f, (y1 - y0) * 0.5f, 0.03f, z, k.color * (i % 2 ? 0.9f : 1.0f));
      }
      break;
    }
    case fence_kind::kStyleHedge: {
      const vec3 col = leaf(k, season);
      const uint32_t sway = b.bone(0, v3(len * 0.5f, 0, 0), v3(1, 0, 0), house_bone::kMotionSway, 0.6f, 0.03f, 0.3f, pick(1));
      const int n = detail == 0 ? 4 : (detail == 1 ? 2 : 1);
      for (int i = 0; i < n; ++i) {
        const float x0 = len * static_cast<float>(i) / static_cast<float>(n), x1 = len * static_cast<float>(i + 1) / static_cast<float>(n);
        const float h = H * (0.92f + 0.12f * pick(30 + static_cast<uint32_t>(i)));
        const float y = yat((x0 + x1) * 0.5f);
        b.box(v3((x0 + x1) * 0.5f, y + (h - 0.1f) * 0.5f, 0), v3((x1 - x0) * 0.5f, (h + 0.1f) * 0.5f, T * 0.5f), col * (0.92f + 0.12f * pick(50 + static_cast<uint32_t>(i))), sway, true);
        if (detail == 0) b.box(v3((x0 + x1) * 0.5f, y + h, 0), v3((x1 - x0) * 0.45f, 0.08f, T * 0.42f), col * 1.06f, sway, true);
      }
      break;
    }
    case fence_kind::kStyleLowWall: {
      // Courses of stones, each its own size, and coping stones on edge along the top.
      const int courses = detail == 2 ? 1 : 3;
      for (int c = 0; c < courses; ++c) {
        const float y0 = H * 0.85f * static_cast<float>(c) / static_cast<float>(courses) - (c == 0 ? 0.3f : 0.0f);
        const float y1 = H * 0.85f * static_cast<float>(c + 1) / static_cast<float>(courses);
        float x = c % 2 ? 0.0f : 0.0f;
        int s = 0;
        while (x < len - 0.01f) {
          const float w = minf(len - x, 0.35f + 0.3f * pick(70 + static_cast<uint32_t>(c * 31 + s)));
          const float xm = x + w * 0.5f;
          const float shade = 0.86f + 0.2f * pick(90 + static_cast<uint32_t>(c * 31 + s));
          b.box(v3(xm, (y0 + y1) * 0.5f + yat(xm), 0), v3(w * 0.5f - 0.01f, (y1 - y0) * 0.5f, T * 0.5f - 0.02f * static_cast<float>(c)), k.color * shade, 0, c == courses - 1);
          x += w;
          ++s;
          if (detail == 2) break;
        }
      }
      if (detail < 2) {
        const int n = maxi_(1, round_to_int(len / 0.22f));
        for (int i = 0; i < n; ++i) {
          const float xm = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
          b.box(v3(xm, H * 0.85f + 0.09f + yat(xm), 0), v3(len / static_cast<float>(n) * 0.5f - 0.02f, 0.1f, T * 0.4f), k.post_color, 0, true);
        }
      }
      break;
    }
    case fence_kind::kStyleLivingWillow: {
      // Willow wands planted at a slant both ways, crossing in diamonds, in leaf.
      const vec3 col = leaf(k, season);
      const float step = detail == 0 ? 0.3f : (detail == 1 ? 0.6f : 1.2f);
      for (int dir = -1; dir <= 1; dir += 2)
        for (float x = step * 0.5f; x < len; x += step) {
          const float xb = x + static_cast<float>(dir) * H * 0.6f;
          if (xb < 0 || xb > len) continue;
          b.beam(v3(x, yat(x) - 0.1f, 0.02f * static_cast<float>(dir)), v3(xb, yat(xb) + H, 0.02f * static_cast<float>(dir)), 0.02f, k.post_color, 0, false);
        }
      const int tufts = detail == 2 ? 1 : static_cast<int>(len / 0.5f);
      for (int i = 0; i < tufts; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(tufts);
        b.frustum(v3(x, yat(x) + H * 0.8f, 0), v3(x, yat(x) + H + 0.25f, 0), 0.22f, 0.05f, 5, col * (0.9f + 0.2f * pick(30 + static_cast<uint32_t>(i))), 0, true, true);
      }
      break;
    }
    case fence_kind::kStyleStoneSlab: {
      // Upright slabs of stone set edge to edge, each its own height and lean.
      const int n = maxi_(1, round_to_int(len / 0.75f));
      for (int i = 0; i < n; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
        const float h = H * (0.8f + 0.3f * pick(40 + static_cast<uint32_t>(i)));
        const float lean = (pick(60 + static_cast<uint32_t>(i)) - 0.5f) * 0.12f;
        const vec3 ay = normalize(v3(lean, 1, (pick(80 + static_cast<uint32_t>(i)) - 0.5f) * 0.1f));
        const vec3 ax = normalize(cross(ay, v3(0, 0, 1)) * -1.0f);
        b.box(v3(x, yat(x) + (h - 0.3f) * 0.5f, 0), ax, ay, cross(ax, ay), v3(len / static_cast<float>(n) * 0.5f - 0.02f, (h + 0.3f) * 0.5f, T * 0.5f),
              k.color * (0.82f + 0.25f * pick(100 + static_cast<uint32_t>(i))), 0, true);
      }
      break;
    }
    case fence_kind::kStyleReed: {
      // Bundles of reeds lashed to rails between stakes, their heads ragged.
      for (float x = module; x < len - 0.3f; x += module) post(b, k, x, yat(x), H + 0.1f, below);
      for (float y : {H * 0.25f, H * 0.7f}) rail(0.0f, len, y, 0.03f, 0.03f, 0.08f, k.post_color);
      const int n = detail == 0 ? static_cast<int>(len / 0.12f) : (detail == 1 ? static_cast<int>(len / 0.3f) : 0);
      for (int i = 0; i < n; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
        const float h = H * (0.9f + 0.2f * pick(120 + static_cast<uint32_t>(i)));
        b.frustum(v3(x, yat(x) - 0.05f, 0), v3(x, yat(x) + h, 0), 0.06f, 0.025f, 4, k.color * (0.88f + 0.2f * pick(140 + static_cast<uint32_t>(i))), 0, false, true);
      }
      if (detail == 2) rail(0.0f, len, H * 0.5f, H * 0.5f, 0.05f, 0, k.color);
      break;
    }
    default: {  // iron railings
      for (float x = module; x < len - 0.3f; x += module) post(b, k, x, yat(x), H, below);
      for (float y : {0.12f, H * 0.85f}) rail(0.03f, len, y, 0.02f, 0.015f, 0, k.post_color);
      const int n = detail == 2 ? 0 : maxi_(1, round_to_int(len / 0.14f) - 1);
      for (int i = 1; i <= n; ++i) {
        const float x = len * static_cast<float>(i) / static_cast<float>(n + 1);
        b.box(v3(x, yat(x) + H * 0.5f, 0), v3(0.012f, H * 0.48f, 0.012f), k.color);
        if (detail == 0) b.frustum(v3(x, yat(x) + H * 0.98f, 0), v3(x, yat(x) + H * 0.98f + 0.08f, 0), 0.025f, 0, 4, k.color);
      }
      if (detail == 2) rail(0.03f, len, H * 0.5f, H * 0.38f, 0.008f, 0, k.color);
      break;
    }
  }
  const float hz = maxf(T * 0.5f, 0.12f);
  b.solid(v3(len * 0.5f, (H + rise) * 0.5f, 0), v3(len * 0.5f, (H + (rise < 0 ? -rise : rise)) * 0.5f, hz));
}

void joint(B &b, const fence_kind &k, uint32_t season) {
  const float H = k.height, T = k.thickness;
  if (k.style == fence_kind::kStyleHedge) {
    b.frustum(v3(0, -0.1f, 0), v3(0, H, 0), T * 0.62f, T * 0.5f, 8, leaf(k, season), 0, true, true);
  } else if (k.style == fence_kind::kStyleLowWall) {
    b.box(v3(0, H * 0.5f - 0.15f, 0), v3(T * 0.55f, H * 0.5f + 0.15f, T * 0.55f), k.color * 0.95f);
    b.box(v3(0, H + 0.04f, 0), v3(T * 0.6f, 0.06f, T * 0.6f), k.post_color);
  } else if (k.style == fence_kind::kStyleLivingWillow) {
    b.frustum(v3(0, -0.1f, 0), v3(0, H + 0.3f, 0), 0.06f, 0.03f, 5, k.post_color, 0, false, false);
    b.frustum(v3(0, H * 0.7f, 0), v3(0, H + 0.4f, 0), 0.3f, 0.06f, 6, leaf(k, season), 0, true, true);
  } else if (k.style == fence_kind::kStyleStoneSlab) {
    b.box(v3(0, H * 0.55f - 0.15f, 0), v3(T * 0.9f, H * 0.55f + 0.15f, T * 0.9f), k.color * 0.92f);
  } else if (k.style == fence_kind::kStyleSplitRail) {
    b.beam(v3(0, -0.1f, -0.08f), v3(0, H + 0.1f, 0.08f), 0.045f, k.post_color);
    b.beam(v3(0, -0.1f, 0.08f), v3(0, H + 0.1f, -0.08f), 0.045f, k.post_color);
  } else {
    post(b, k, 0, 0, H + 0.08f, 0.3f);
  }
  b.solid_cylinder(v3(0, 0, 0), maxf(T * 0.55f, 0.08f), H);
}

void gate(B &b, const fence_kind &k, float len) {
  const float H = minf(k.height, 1.6f) * 0.95f, x0 = -len * 0.5f;
  post(b, k, x0, 0, maxf(k.height, H) + 0.1f, 0.4f);
  const uint32_t hinge = b.bone(0, v3(x0 + 0.06f, 0, 0), v3(0, 1, 0), house_bone::kMotionHinge, 0, 1.6f, 0, 0);
  const float w = len - 0.14f, xa = x0 + 0.08f, xb = xa + w;
  const vec3 col = k.gate_color;
  for (float y : {0.15f, H * 0.5f, H - 0.06f}) b.box(v3((xa + xb) * 0.5f, y, 0.04f), v3(w * 0.5f, 0.04f, 0.025f), col, hinge);
  for (float x : {xa + 0.04f, xb - 0.04f}) b.box(v3(x, H * 0.5f + 0.05f, 0.04f), v3(0.04f, H * 0.5f - 0.05f, 0.025f), col * 0.9f, hinge);
  b.beam(v3(xa + 0.06f, 0.18f, 0.07f), v3(xb - 0.06f, H - 0.1f, 0.07f), 0.03f, col * 0.85f, hinge);
  b.solid(v3((xa + xb) * 0.5f, H * 0.5f, 0.04f), v3(w * 0.5f, H * 0.5f, 0.05f));
}

}  // namespace

bool run(const fence_spec &in, fence_model &out, std::string &error) {
  const fence_kind &k = in.kind;
  if (in.season > 3 || in.detail > 2 || k.style > fence_kind::kStyleReed || in.condition > fence_spec::kConditionRuin) {
    error = "season, detail, or the kind's style out of range";
    return false;
  }
  out = fence_model();
  Network net;
  if (!expand(in.network, in.chunks, net, error)) return false;
  B b{out};
  b.winter = in.season == fence_spec::kSeasonWinter;
  b.detail = static_cast<int>(in.detail);
  b.sides = in.detail == 0 ? 8 : (in.detail == 1 ? 6 : 4);
  b.bone(-1, v3(0, 0, 0), v3(0, 1, 0), house_bone::kMotionFixed, 0, 0, 0, 0);
  const uint32_t base_seed = hash_u32(in.seed * 2654435761u ^ hash_str(k.name.c_str()));
  // A chunk past the last wraps round, so every chunk asked for has something in it.
  const bool all = net.parts <= 1;
  const uint32_t chunk = in.chunk % net.parts;
  auto mine_u = [&](const Unit &u) { return all || u.chunk == chunk; };
  auto mine_n = [&](int i) { return all || net.node_chunk[i] == chunk; };
  const float L = 3.0f;
  // ---- 1. runs ----
  for (size_t ui = 0; ui < net.units.size(); ++ui) {
    const Unit &u = net.units[ui];
    if (!mine_u(u)) continue;
    const float x0 = u.cut0 * L, x1 = L - u.cut1 * L;
    if (x1 - x0 < 0.05f) continue;
    const vec3 A = net.pos(u.a), Bp = net.pos(u.b);
    const float rise = Bp.y - A.y;
    const size_t v0 = out.positions.size(), c0 = out.collision.size(), b0 = out.bones.size();
    run_piece(b, k, x1 - x0, rise * (x1 - x0) / L, in.season, base_seed ^ static_cast<uint32_t>(ui * 7919u), b.detail);
    to_world(out, v0, c0, b0, A + dir(u.k) * x0 + v3(0, rise * x0 / L, 0), u.k);
  }
  // ---- 2. a post (or a clump, or a pier) at every node it reaches ----
  const bool continuous = k.style == fence_kind::kStyleHedge || k.style == fence_kind::kStyleLowWall ||
                          k.style == fence_kind::kStyleStoneSlab || k.style == fence_kind::kStyleLivingWillow;
  uint32_t joins = 0;
  for (size_t s = 0; s < net.q.size(); ++s) {
    const int i = static_cast<int>(s);
    if (!mine_n(i) || net.arms[i] == 0 || net.gate[i]) continue;
    if (continuous && net.straight(i)) continue;  // a hedge or a wall runs straight on through
    int k0 = 0;
    while (!((net.arms[i] >> k0) & 1)) ++k0;
    const size_t v0 = out.positions.size(), c0 = out.collision.size(), b0 = out.bones.size();
    joint(b, k, in.season);
    to_world(out, v0, c0, b0, net.pos(i), k0);
    ++joins;
  }
  // Posts at the openings' ends (where a run stops at a gate), and the gates.
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
    // Along the opening: from o.a toward o.c, in the frame of whichever of k, k + 3 points that way.
    const vec3 d = o.c - o.a;
    const int kk = d.x * kDirX[o.k] + d.z * kDirZ[o.k] >= 0 ? o.k : (o.k + 3) % 6;
    // An end on a node that has its own post: the gate stops short of it, so posts never double.
    auto has_post = [&](vec3 p) {
      for (size_t s = 0; s < net.q.size(); ++s) {
        const vec3 q = net.pos(static_cast<int>(s));
        if ((q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z) < 1e-4f) return net.arms[s] != 0 && !net.gate[s] && !(continuous && net.straight(static_cast<int>(s)));
      }
      return false;
    };
    const vec3 u = d * (1.0f / maxf(foundry::det::hypot(d.x, d.z), 1e-6f));
    const bool post_a = has_post(o.a), post_c = has_post(o.c);
    const vec3 ga = o.a + u * (post_a ? 0.12f : 0.0f), gc = o.c - u * (post_c ? 0.12f : 0.0f);
    const float len = foundry::det::hypot(gc.x - ga.x, gc.z - ga.z);
    size_t v0 = out.positions.size(), c0 = out.collision.size(), b0 = out.bones.size();
    gate(b, k, len);  // centred on its origin, its hinge post at -len / 2
    to_world(out, v0, c0, b0, (ga + gc) * 0.5f, kk);
    if (post_c) continue;
    v0 = out.positions.size();
    c0 = out.collision.size();
    b0 = out.bones.size();
    post(b, k, 0, 0, maxf(k.height, 1.0f) + 0.1f, 0.4f);  // the latch post
    to_world(out, v0, c0, b0, gc, kk);
  }
  // A ruin: broken and leaning to a ragged height over its line's ground, its gates gone (condition.h).
  if (in.condition == fence_spec::kConditionRuin) {
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
    condition::ruin_over(out, hash_u32(in.seed * 2654435761u ^ 0x5eedu), 0.6f, ground, k.height * 1.25f, false);
  }
  // ---- guides ----
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
      const float r = sh.type == channels::physics_shape::kTypeBox ? foundry::det::hypot(sh.half_extents.x, sh.half_extents.z) : sh.radius;
      reach = maxf(reach, foundry::det::hypot(sh.position.x, sh.position.z) + r);
      top = maxf(top, sh.position.y + (sh.type == channels::physics_shape::kTypeBox ? sh.half_extents.y : sh.half_height));
    }
  out.height = out.positions.empty() ? 0 : top;
  out.radius = reach;
  out.links = static_cast<uint32_t>(net.units.size());
  out.joins = joins;
  return true;
}

}  // namespace fence_build

FOUNDRY_SYSTEM(fence_build, channels::fence_spec, channels::fence_model)
