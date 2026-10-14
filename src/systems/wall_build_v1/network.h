// A wall's or fence's network on the hex grid's diagonal lattice (line_v1_network): its nodes, its
// links split into unit links (one diagonal step: 3 m, vertex to vertex through a cell and along the
// edge between the next two), each node's arms, the openings its gates leave, its chunks, and a
// frame per unit link to build in. Each system carries its own copy (spine.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "channels/line_v1_network.h"
#include "math.h"

namespace wall_build_v1 {

using namespace wall_math;

constexpr float kSqrt3 = 1.7320508f;
// The diagonal directions, toward corner k (30 + 60 k degrees from +x toward +z), as axial steps.
constexpr int kDiagQ[6] = {1, -1, -2, -1, 1, 2}, kDiagR[6] = {1, 2, 1, -1, -2, -1};
constexpr float kDirX[6] = {0.8660254f, 0.0f, -0.8660254f, -0.8660254f, 0.0f, 0.8660254f};
constexpr float kDirZ[6] = {0.5f, 1.0f, 0.5f, -0.5f, -1.0f, -0.5f};
// A collision box's yaw (physics_v1_shape: +x turns toward -z as it grows) that lays +x along k.
inline float dir_yaw(int k) { return -(30.0f + 60.0f * static_cast<float>(k)) * kPi / 180.0f; }
inline float cell_x(int q, int r) { return kSqrt3 * (static_cast<float>(q) + 0.5f * static_cast<float>(r)); }
inline float cell_z(int r) { return 1.5f * static_cast<float>(r); }
inline vec3 dir(int k) { return v3(kDirX[k], 0, kDirZ[k]); }

struct Unit {
  int a, b, k;           // from node a to node b, direction k
  float cut0 = 0, cut1 = 0;  // how much of it a gate opens: [0, cut0] at a's end, [1 - cut1, 1] at b's (a whole gated link: both 0.5)
  uint32_t chunk = 0;
};

struct Network {
  std::vector<int> q, r;
  std::vector<float> h;
  std::vector<uint8_t> arms;   // per node: bit k if a unit link leaves along k
  std::vector<uint8_t> gate, tower;
  std::vector<Unit> units;
  std::vector<uint32_t> node_chunk;
  uint32_t parts = 1;  // chunks, at most one per unit link
  vec3 centre = v3(0, 0, 0);
  std::string note;

  int find(int qq, int rr) const {
    for (size_t i = 0; i < q.size(); ++i)
      if (q[i] == qq && r[i] == rr) return static_cast<int>(i);
    return -1;
  }
  int add(int qq, int rr, float hh) {
    const int f = find(qq, rr);
    if (f >= 0) return f;
    q.push_back(qq);
    r.push_back(rr);
    h.push_back(hh);
    return static_cast<int>(q.size() - 1);
  }
  vec3 pos(int i) const { return v3(cell_x(q[i], r[i]), h[i], cell_z(r[i])); }
  int degree(int i) const {
    int n = 0;
    for (int k = 0; k < 6; ++k) n += (arms[i] >> k) & 1;
    return n;
  }
  // Straight through: exactly two opposite arms.
  bool straight(int i) const { return degree(i) == 2 && ((arms[i] & 7) == ((arms[i] >> 3) & 7)); }
};

// Builds the network from the channel: long links split into unit links through the nodes between
// (their heights interpolated), gates marked, chunks by angle round the middle.
inline bool expand(const channels::line_v1_network &in, uint32_t chunks, Network &n, std::string &error) {
  if (in.nodes.size() % 2 != 0 || in.links.size() % 2 != 0) {
    error = "nodes and links come in pairs";
    return false;
  }
  const size_t count = in.nodes.size() / 2;
  for (size_t i = 0; i < count; ++i) n.add(in.nodes[2 * i], in.nodes[2 * i + 1], i < in.heights.size() ? in.heights[i] : 0.0f);
  std::vector<int> first_unit, last_unit;  // per input link: its unit links
  for (size_t l = 0; l + 1 < in.links.size(); l += 2) {
    const uint32_t a = in.links[l], b = in.links[l + 1];
    first_unit.push_back(-1);
    last_unit.push_back(-1);
    if (a >= count || b >= count || a == b) continue;
    const int dq = in.nodes[2 * b] - in.nodes[2 * a], dr = in.nodes[2 * b + 1] - in.nodes[2 * a + 1];
    int dk = -1, steps = 0;
    for (int k = 0; k < 6 && dk < 0; ++k) {
      // dq, dr = s * step k for a whole s > 0
      const int s = kDiagQ[k] != 0 ? dq / kDiagQ[k] : dr / kDiagR[k];
      if (s > 0 && s * kDiagQ[k] == dq && s * kDiagR[k] == dr) {
        dk = k;
        steps = s;
      }
    }
    if (dk < 0) {
      if (n.note.empty()) n.note = "a link that isn't along a diagonal line was left out";
      continue;
    }
    const float ha = in.heights.size() > a ? in.heights[a] : 0.0f, hb = in.heights.size() > b ? in.heights[b] : 0.0f;
    int prev = static_cast<int>(a);
    first_unit.back() = static_cast<int>(n.units.size());
    for (int s = 1; s <= steps; ++s) {
      const int node = s == steps ? static_cast<int>(b)
                                  : n.add(in.nodes[2 * a] + kDiagQ[dk] * s, in.nodes[2 * a + 1] + kDiagR[dk] * s,
                                          ha + (hb - ha) * static_cast<float>(s) / static_cast<float>(steps));
      bool dup = false;
      for (const Unit &u : n.units) dup = dup || (u.a == prev && u.b == node) || (u.a == node && u.b == prev);
      if (!dup) n.units.push_back({prev, node, dk});
      prev = node;
    }
    last_unit.back() = static_cast<int>(n.units.size()) - 1;
  }
  const size_t N = n.q.size();
  n.arms.assign(N, 0);
  n.gate.assign(N, 0);
  n.tower.assign(N, 0);
  for (const Unit &u : n.units) {
    n.arms[u.a] |= static_cast<uint8_t>(1u << u.k);
    n.arms[u.b] |= static_cast<uint8_t>(1u << ((u.k + 3) % 6));
  }
  for (uint32_t g : in.towers)
    if (g < N) n.tower[g] = 1;
  // Gates: a node's only where the line runs straight through it; a link's at its middle unit.
  for (uint32_t g : in.gate_nodes) {
    if (g >= N || !n.straight(static_cast<int>(g))) continue;
    n.gate[g] = 1;
    for (Unit &u : n.units) {
      if (u.a == static_cast<int>(g)) u.cut0 = 0.5f;
      if (u.b == static_cast<int>(g)) u.cut1 = 0.5f;
    }
  }
  for (uint32_t gl : in.gate_links) {
    if (gl >= first_unit.size() || first_unit[gl] < 0) continue;
    Unit &u = n.units[static_cast<size_t>((first_unit[gl] + last_unit[gl]) / 2)];
    u.cut0 = 0.5f;
    u.cut1 = 0.5f;
  }
  // The middle, and chunks: unit links by angle round it, in equal runs.
  vec3 c = v3(0, 0, 0);
  for (size_t i = 0; i < N; ++i) c = c + n.pos(static_cast<int>(i));
  n.centre = N ? c * (1.0f / static_cast<float>(N)) : c;
  // Only units with something to build count toward chunks (a link a gate opens whole builds
  // nothing), so every chunk asked for has something in it.
  auto solid = [&](const Unit &u) { return u.cut0 + u.cut1 < 0.99f; };
  size_t solids = 0;
  for (const Unit &u : n.units) solids += solid(u) ? 1 : 0;
  uint32_t parts = chunks < 1 ? 1 : chunks;
  if (parts > solids) parts = solids == 0 ? 1 : static_cast<uint32_t>(solids);
  n.parts = parts;
  std::vector<int> order;
  std::vector<float> key;
  for (size_t i = 0; i < n.units.size(); ++i) {
    order.push_back(static_cast<int>(i));
    const vec3 m = (n.pos(n.units[i].a) + n.pos(n.units[i].b)) * 0.5f;
    const float dx = m.x - n.centre.x, dz = m.z - n.centre.z, s = absf(dx) + absf(dz);
    key.push_back(s < 1e-6f ? 0.0f : (dz >= 0 ? 1 - dx / s : 3 + dx / s));
  }
  for (size_t i = 1; i < order.size(); ++i)  // insertion sort by (key, index)
    for (size_t j = i; j > 0 && (key[j - 1] > key[j] || (key[j - 1] == key[j] && order[j - 1] > order[j])); --j) {
      const float tk = key[j];
      key[j] = key[j - 1];
      key[j - 1] = tk;
      const int to = order[j];
      order[j] = order[j - 1];
      order[j - 1] = to;
    }
  // Solid units in equal runs by angle; an opened unit goes with the solid one before it.
  size_t si = 0;
  uint32_t last = 0;
  for (size_t s = 0; s < order.size(); ++s) {
    Unit &u = n.units[static_cast<size_t>(order[s])];
    if (solid(u)) last = static_cast<uint32_t>((si++ * parts) / (solids ? solids : 1));
    u.chunk = last;
  }
  n.node_chunk.assign(N, 0xffffffffu);
  for (size_t s = 0; s < order.size(); ++s) {
    const Unit &u = n.units[static_cast<size_t>(order[s])];
    for (int e : {u.a, u.b})
      if (n.node_chunk[e] == 0xffffffffu) n.node_chunk[e] = u.chunk;
  }
  return true;
}

// A frame to build in: local +x along direction k from `at`, +y up, +z toward a counter-clockwise
// network's inside. Transforms what a builder made since `from`.
template <typename Model>
void to_world(Model &out, size_t from_vertex, size_t from_body, size_t from_bone, vec3 at, int k) {
  // The planners' turn (x' = x cos y + z sin y, z' = -x sin y + z cos y) with +x along k: local +z
  // goes to (-dz, dx), which is a counter-clockwise network's inside.
  const vec3 ax = dir(k), az = v3(-kDirZ[k], 0, kDirX[k]);
  auto P = [&](vec3 p) { return at + ax * p.x + v3(0, p.y, 0) + az * p.z; };
  auto D = [&](vec3 p) { return ax * p.x + v3(0, p.y, 0) + az * p.z; };
  for (size_t i = from_vertex; i < out.positions.size(); ++i) {
    out.positions[i] = P(out.positions[i]);
    out.normals[i] = D(out.normals[i]);
  }
  for (size_t bi = from_bone; bi < out.bones.size(); ++bi) {
    out.bones[bi].pivot = P(out.bones[bi].pivot);
    out.bones[bi].axis = D(out.bones[bi].axis);
  }
  for (size_t bi = from_body; bi < out.collision.size(); ++bi)
    for (auto &sh : out.collision[bi].shapes) {
      sh.position = P(sh.position);
      sh.rotation = v3(sh.rotation.x, sh.rotation.y + dir_yaw(k), sh.rotation.z);
      for (vec3 &p : sh.points) p = P(p);
    }
}

}  // namespace wall_build_v1
