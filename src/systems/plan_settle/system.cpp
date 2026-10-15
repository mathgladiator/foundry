// plan_settle: a plan system (design/planner.md): lays out a village, town, or keep as a plan of
// plots: buildings' lots, road segments, waterfronts' lots, and water. Everything else is open land
// that passes to a plotter, which fills it with nature.
//
// 1. The site: a working hex of cells sized from the plot count, natural heights by the config's
//    terrain (flat, gentle, hill, valley, bank), a river where it says. A walled town's site is a
//    circle inside its wall.
// 2. The skeleton, by scenario: a node (a square, a crossroads, a green, a pond, or a lake with a
//    ring lane round it), entrances on the edge, and least-cost roads from each entrance to the node
//    (costing grade over the road's limit, turns, water to bridge, and rewarding roads already
//    laid, so roads merge). A hill keep's road climbs to its gate in switchbacks; a river town's
//    quay runs a waterfront's depth back from the water.
// 3. Back lanes on contours of the distance from the main roads, alleys down to them (a piece's
//    first way in is a lane, so carts reach it), a ring lane inside a wall, and the wall itself.
// 4. Plots: waterfronts first, where a waterfront lot's wet cells lie on water, its dry cells on
//    land, and its back on a road; then civic plots by the node; then household plots along the
//    roads (the node's edge and high streets first), each lot turned by sixths so its front lies
//    along a road, by class shares and shape weights.
// 5. Pruning: dead-end lanes that serve no plot go; an unwalled village's roads end where its plots
//    do, at an entrance tile.
// 6. Zoning, wealth, and tiers by distance along the roads, so each tier is whole and reachable.
// 7. Levelling: roads graded to their class's limit, the square flat, every plot one pad within a
//    step of its road, the rest eased between.
// 8. Navigation: a polygon per road cell, linked where a walker steps, flagged for carts; every
//    plot reached from every entrance, every trade by cart; a plot not reached is given up (a
//    repair).
// 9. The plan: the village's shape (what it built, the open land within its margin, its open body,
//    the water beside it), road segments with each cell's neighbours (road's mask) and role,
//    water bodies, and the nature ground for a plotter.
//
// Every walk over cells is in index order, every sort has a total order, and randomness comes
// from per-step streams, so WebAssembly and native agree to the byte.
#include <map>
#include <vector>

#include "boundary.h"
#include "channels/plan_plan.h"
#include "channels/plan_request.h"
#include "math.h"
#include "packer/plan_plan.h"
#include "packer/plan_request.h"

namespace plan_settle {

using namespace plan_math;
using channels::plan_cell;
using channels::plan_config;
using channels::plan_plan;
using channels::plan_plot;
using channels::plan_request;
using channels::plot_region;

namespace {

using foundry::vec3;
inline vec3 v3(float x, float y, float z) { return vec3{x, y, z}; }

constexpr int kDq[6] = {1, 1, 0, -1, -1, 0}, kDr[6] = {0, -1, -1, 0, 1, 1};  // 0, 300, 240, 180, 120, 60 degrees
constexpr int kEdge = 4;           // the working site's ring past its radius, cells
constexpr int kApron = 6;          // the nature ground's apron past the village's shape, cells
constexpr float kStep = 0.45f;     // the walker's highest step, metres
constexpr float kRetain = 0.3f;    // neighbours further apart than this beside something built
constexpr float kCellM = kSqrt3;   // metres between neighbouring cells' centres
constexpr float kSin6[6] = {0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f};
constexpr float kCos6[6] = {1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f};
constexpr uint32_t kCivicClass = 3, kCastleClass = 4, kWaterfrontClass = 5, kCentreClass = 6, kFieldClass = 7;
constexpr uint32_t kNoHalf = 6;  // road's half: none

enum Use : uint8_t { kFree = 0, kStreet, kPlaza, kPlot, kGreen, kWater, kWall, kKeepWall, kWaterfront };
enum Street : uint8_t { kNone = 0, kHigh, kLane, kAlley, kBridge, kGate, kQuay };
// road's mask: what a neighbour is.
enum EdgeKind : uint32_t { kERoad = 0, kEFront = 1, kEOpen = 2, kEWater = 3, kEWall = 4, kEGate = 5, kEWaterfront = 6, kESide = 7 };

int absi(int v) { return v < 0 ? -v : v; }
int maxi_(int a, int b) { return a > b ? a : b; }
int mini_(int a, int b) { return a < b ? a : b; }
float absf(float v) { return v < 0 ? -v : v; }
float snap(float h) { return static_cast<float>(round_to_int(h / 0.05f)) * 0.05f; }
// Plots the zoning leaves alone and that open with the settlement: civic buildings, the keep, a
// castle's parts, a square's centre.
bool special(uint32_t use) {
  return use == plan_plot::kUseCivic || use == plan_plot::kUseKeep || use == plan_plot::kUseCastle || use == plan_plot::kUseCentre;
}

// A cell offset turned by sixths, as a building turned by yaw = k * 60 degrees: (q, r) to (q + r, -q).
void turn_cell(int q, int r, int k, int &oq, int &orr) {
  for (int i = 0; i < k; ++i) {
    const int nq = q + r, nr = -q;
    q = nq;
    r = nr;
  }
  oq = q;
  orr = r;
}

struct Lot {
  uint32_t cls = 0, shape = 0;
  std::vector<int> cells, wet, doors;  // q, r pairs; doors: the cells just outside its doors
};

// A lot turned by sixths: its cells (and whether each is wet), and the neighbouring offsets in
// front of (past its front row), behind, and beside it.
struct Turned {
  std::vector<int> cells, front, back, side, doors;
  std::vector<uint8_t> wet;
};

struct Plot {
  int lot = 0, turn = 0, aq = 0, ar = 0;
  uint32_t cls = 0, shape = 0, use = 0, tier = 0;
  bool waterfront = false;
  std::vector<int> cells;     // grid indices
  std::vector<uint8_t> wet;   // per cell: over water
  std::vector<int> frontage;  // road cells in front (a waterfront's: behind), grid indices
  std::vector<int> doors;     // the cells just outside its doors, grid indices
  float pad = 0, wealth = 0, walk = 0, drop = 0;
  bool reached = true, cart = true, alive = true;
};

struct Site {
  int R = 0, N = 0, side = 0;
  std::vector<uint8_t> valid, site, use, street, tier, arterial;
  std::vector<float> nat, h;
  std::vector<int> plot;
  bool in(int q, int r) const { return q >= -N && q <= N && r >= -N && r <= N && hex_distance(q, r) <= N; }
  int idx(int q, int r) const { return (q + N) * side + (r + N); }
  int q_of(int i) const { return i / side - N; }
  int r_of(int i) const { return i % side - N; }
  bool inside(int i) const { return site[i] != 0; }
  float x(int i) const { return cell_x(q_of(i), r_of(i)); }
  float z(int i) const { return cell_z(r_of(i)); }
  int neighbour(int i, int d) const {
    const int q = q_of(i) + kDq[d], r = r_of(i) + kDr[d];
    return in(q, r) ? idx(q, r) : -1;
  }
  bool road(int i) const { return use[i] == kStreet || use[i] == kPlaza; }
};

// ---- a min-heap with a total order (cost, then id) ----

struct Heap {
  struct E {
    float c;
    int id;
  };
  std::vector<E> v;
  static bool less(const E &a, const E &b) { return a.c < b.c || (a.c == b.c && a.id < b.id); }
  void push(float c, int id) {
    v.push_back({c, id});
    size_t i = v.size() - 1;
    while (i > 0) {
      const size_t p = (i - 1) / 2;
      if (!less(v[i], v[p])) break;
      const E t = v[i];
      v[i] = v[p];
      v[p] = t;
      i = p;
    }
  }
  E pop() {
    const E top = v[0];
    v[0] = v.back();
    v.pop_back();
    size_t i = 0;
    for (;;) {
      const size_t l = 2 * i + 1, r = l + 1;
      size_t m = i;
      if (l < v.size() && less(v[l], v[m])) m = l;
      if (r < v.size() && less(v[r], v[m])) m = r;
      if (m == i) break;
      const E t = v[i];
      v[i] = v[m];
      v[m] = t;
      i = m;
    }
    return top;
  }
  bool empty() const { return v.empty(); }
};

// Insertion sort by key, then index: the same everywhere.
template <typename K>
void sort_by(std::vector<int> &items, std::vector<K> ks) {
  for (size_t i = 1; i < items.size(); ++i) {
    const int it = items[i];
    const K k = ks[i];
    size_t j = i;
    while (j > 0 && (ks[j - 1] > k || (ks[j - 1] == k && items[j - 1] > it))) {
      items[j] = items[j - 1];
      ks[j] = ks[j - 1];
      --j;
    }
    items[j] = it;
    ks[j] = k;
  }
}

// An angle-like key around a point, in [0, 4), from + - * / only.
float pseudo_angle(float dx, float dz) {
  const float s = absf(dx) + absf(dz);
  if (s < 1e-6f) return 0;
  const float p = dx / s;
  return dz >= 0 ? 1 - p : 3 + p;
}

float fbm(float x, float z, float scale, uint32_t seed) {
  float sum = 0, amp = 1, norm = 0, f = 1.0f / scale;
  for (int o = 0; o < 4; ++o) {
    sum += amp * noise2(x * f, z * f, seed + 977u * static_cast<uint32_t>(o));
    norm += amp;
    amp *= 0.5f;
    f *= 2.0f;
  }
  return sum / norm;
}

struct PathCost {
  float grade = 0.1f, grade_weight = 4.0f, turn = 0.3f, water = 8.0f, reuse = 0.35f;
};

// The least-cost path over the site's cells from `from` to any goal cell.
std::vector<int> find_path(const Site &s, int from, const std::vector<uint8_t> &goal, const PathCost &pc) {
  const int n = static_cast<int>(s.valid.size());
  std::vector<float> best(static_cast<size_t>(n) * 7, 1e30f);
  std::vector<int> prev(static_cast<size_t>(n) * 7, -1);
  Heap heap;
  best[static_cast<size_t>(from) * 7 + 6] = 0;
  heap.push(0, from * 7 + 6);
  int end = -1;
  while (!heap.empty()) {
    const Heap::E e = heap.pop();
    if (e.c > best[e.id]) continue;
    const int cell = e.id / 7, dir = e.id % 7;
    if (goal[cell] && cell != from) {
      end = e.id;
      break;
    }
    for (int d = 0; d < 6; ++d) {
      if (dir < 6 && ((d + 3) % 6) == dir) continue;
      const int nb = s.neighbour(cell, d);
      if (nb < 0 || !s.valid[nb] || !s.inside(nb)) continue;
      const uint8_t u = s.use[nb];
      if (u == kPlot || u == kKeepWall || u == kWaterfront || u == kGreen || (u == kWall && !goal[nb])) continue;
      float c = 1.0f;
      const float grade = absf(s.nat[nb] - s.nat[cell]) / kCellM;
      if (grade > pc.grade) c += pc.grade_weight * (grade - pc.grade) * 10.0f;
      if (u == kWater) c += pc.water;
      if (u == kStreet || u == kPlaza) c *= pc.reuse;
      if (dir < 6) {
        const int t = absi(d - dir);
        c += pc.turn * static_cast<float>(t > 3 ? 6 - t : t);
      }
      const float nc = e.c + c;
      const int id = nb * 7 + d;
      if (nc < best[id]) {
        best[id] = nc;
        prev[id] = e.id;
        heap.push(nc, id);
      }
    }
  }
  std::vector<int> out;
  for (int id = end; id >= 0; id = prev[id]) out.push_back(id / 7);
  for (size_t i = 0, j = out.size(); i + 1 < j; ++i, --j) {
    const int t = out[i];
    out[i] = out[j - 1];
    out[j - 1] = t;
  }
  return out;
}

int dir_between(const Site &s, int a, int b) {
  for (int d = 0; d < 6; ++d)
    if (s.neighbour(a, d) == b) return d;
  return 0;
}

// Lays a road along a path, `width` cells wide, keeping the better class where roads meet; water
// becomes a bridge, a wall a gate.
void lay(Site &s, const std::vector<int> &path, uint8_t cls, int width, bool arterial, std::vector<std::vector<int>> *paths) {
  auto mark = [&](int c) {
    if (c < 0 || !s.valid[c] || !s.inside(c)) return;
    const uint8_t u = s.use[c];
    if (u == kPlot || u == kKeepWall || u == kPlaza || u == kGreen || u == kWaterfront) return;
    if (u == kWall) {
      s.use[c] = kStreet;
      s.street[c] = kGate;
    } else if (u == kWater) {
      s.use[c] = kStreet;
      s.street[c] = kBridge;
    } else if (u == kStreet) {
      if (s.street[c] != kBridge && s.street[c] != kGate && (s.street[c] == kNone || cls < s.street[c])) s.street[c] = cls;
    } else {
      s.use[c] = kStreet;
      s.street[c] = cls;
    }
    if (arterial) s.arterial[c] = 1;
  };
  for (size_t i = 0; i < path.size(); ++i) {
    const int c = path[i];
    const int d = i + 1 < path.size() ? dir_between(s, c, path[i + 1]) : (i > 0 ? dir_between(s, path[i - 1], c) : 0);
    mark(c);
    if (width >= 2) {
      const int l = s.neighbour(c, (d + 1) % 6);
      if (l >= 0 && s.use[l] != kWater) mark(l);
    }
    if (width >= 3) {
      const int r = s.neighbour(c, (d + 5) % 6);
      if (r >= 0 && s.use[r] != kWater) mark(r);
    }
  }
  if (paths) paths->push_back(path);
}

// Steps from the source cells (within the site when `site_only`; across water when `over_water`).
std::vector<int> distance_from(const Site &s, const std::vector<int> &sources, bool site_only, bool over_water) {
  std::vector<int> d(s.valid.size(), 1 << 29);
  std::vector<int> queue;
  for (int c : sources) {
    d[c] = 0;
    queue.push_back(c);
  }
  for (size_t head = 0; head < queue.size(); ++head) {
    const int c = queue[head];
    for (int k = 0; k < 6; ++k) {
      const int nb = s.neighbour(c, k);
      if (nb < 0 || !s.valid[nb] || d[nb] <= d[c] + 1) continue;
      if (site_only && !s.inside(nb)) continue;
      if (!over_water && (s.use[nb] == kWater || s.use[nb] == kWall || s.use[nb] == kKeepWall)) continue;
      d[nb] = d[c] + 1;
      queue.push_back(nb);
    }
  }
  return d;
}

}  // namespace

bool run(const plan_request &in, plan_plan &out, std::string &error) {
  const plan_config &cfg = in.config;
  out = plan_plan();
  if (cfg.scenario > plan_config::kScenarioCastle || cfg.terrain > plan_config::kTerrainBank || in.tier > 2 ||
      cfg.open_body > plan_config::kOpenBodyLake) {
    error = "scenario, terrain, open body, or tier out of range";
    return false;
  }
  // ---- the lots ----
  std::vector<Lot> lots;
  for (size_t i = 0; i < cfg.lot_class.size(); ++i) {
    Lot l;
    l.cls = cfg.lot_class[i];
    l.shape = i < cfg.lot_shape.size() ? cfg.lot_shape[i] : 0;
    lots.push_back(l);
  }
  for (size_t pass = 0; pass < 3; ++pass) {
    const std::vector<int32_t> &v = pass == 0 ? cfg.lot_cells : (pass == 1 ? cfg.lot_wet : cfg.lot_doors);
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
      const int g = v[i];
      if (g < 0 || static_cast<size_t>(g) >= lots.size()) {
        error = "lot_cells, lot_wet, or lot_doors names a lot that has no class";
        return false;
      }
      std::vector<int> &into = pass == 0 ? lots[g].cells : (pass == 1 ? lots[g].wet : lots[g].doors);
      into.push_back(v[i + 1]);
      into.push_back(v[i + 2]);
    }
  }
  for (const Lot &l : lots)
    if (l.cells.empty() || l.cls > kFieldClass || l.shape > 7) {
      error = "every lot needs cells, a class (0 to 6), and a shape (0 to 7)";
      return false;
    }
  if (lots.empty()) {
    error = "the config has no lots";
    return false;
  }
  std::vector<std::vector<Turned>> turned(lots.size(), std::vector<Turned>(6));
  for (size_t li = 0; li < lots.size(); ++li)
    for (int k = 0; k < 6; ++k) {
      Turned &t = turned[li][k];
      const float fx = kSin6[k], fz = kCos6[k];
      auto has = [&](int q, int r) {
        for (size_t j = 0; j + 1 < t.cells.size(); j += 2)
          if (t.cells[j] == q && t.cells[j + 1] == r) return true;
        return false;
      };
      for (size_t j = 0; j + 1 < lots[li].cells.size(); j += 2) {
        int q, r;
        turn_cell(lots[li].cells[j], lots[li].cells[j + 1], k, q, r);
        t.cells.push_back(q);
        t.cells.push_back(r);
        bool wet = false;
        for (size_t w = 0; w + 1 < lots[li].wet.size(); w += 2)
          wet = wet || (lots[li].wet[w] == lots[li].cells[j] && lots[li].wet[w + 1] == lots[li].cells[j + 1]);
        t.wet.push_back(wet ? 1 : 0);
      }
      for (size_t j = 0; j + 1 < lots[li].doors.size(); j += 2) {
        int q, r;
        turn_cell(lots[li].doors[j], lots[li].doors[j + 1], k, q, r);
        t.doors.push_back(q);
        t.doors.push_back(r);
      }
      // In front means past the lot's front row (and behind, past its back row).
      float lo_p = 1e30f, hi_p = -1e30f;
      for (size_t j = 0; j + 1 < t.cells.size(); j += 2) {
        const float pr = cell_x(t.cells[j], t.cells[j + 1]) * fx + cell_z(t.cells[j + 1]) * fz;
        lo_p = minf(lo_p, pr);
        hi_p = maxf(hi_p, pr);
      }
      for (size_t j = 0; j + 1 < t.cells.size(); j += 2)
        for (int d = 0; d < 6; ++d) {
          const int q = t.cells[j] + kDq[d], r = t.cells[j + 1] + kDr[d];
          if (has(q, r)) continue;
          const float pr = cell_x(q, r) * fx + cell_z(r) * fz;
          std::vector<int> &into = pr > hi_p + 0.1f ? t.front : (pr < lo_p - 0.1f ? t.back : t.side);
          bool seen = false;
          for (size_t m = 0; m + 1 < into.size(); m += 2)
            if (into[m] == q && into[m + 1] == r) seen = true;
          if (!seen) {
            into.push_back(q);
            into.push_back(r);
          }
        }
    }

  // ---- 1. the site ----
  const uint32_t lo = cfg.plots_min < 1 ? 1 : cfg.plots_min, hi = cfg.plots_max < lo ? lo : cfg.plots_max;
  const uint32_t target = in.plots ? (in.plots < lo ? lo : (in.plots > hi ? hi : in.plots)) : (lo + hi) / 2;
  float share[3] = {maxf(cfg.share_solo, 0), maxf(cfg.share_family, 0), maxf(cfg.share_extended, 0)};
  {
    const float sum = share[0] + share[1] + share[2];
    for (float &v : share) v = sum > 0 ? v / sum : 1.0f / 3.0f;
  }
  float mean_cells = 0;
  {
    float by[3] = {0, 0, 0};
    int n[3] = {0, 0, 0};
    for (const Lot &l : lots)
      if (l.cls < 3) {
        by[l.cls] += static_cast<float>(l.cells.size() / 2);
        ++n[l.cls];
      }
    for (int c = 0; c < 3; ++c) mean_cells += share[c] * (n[c] ? by[c] / static_cast<float>(n[c]) : 36.0f);
  }
  const bool hill = cfg.scenario == plan_config::kScenarioHillKeep;
  const bool castle = cfg.scenario == plan_config::kScenarioCastle;
  const bool keep_ward = (hill || castle) && cfg.keep_ward_cells >= 3;
  const bool walls_planned = cfg.walls != plan_config::kWallsNone;
  const bool river_town = cfg.scenario == plan_config::kScenarioRiverTown;
  const bool walled = cfg.walls != plan_config::kWallsNone;
  const bool village = cfg.scenario <= plan_config::kScenarioRoundVillage || cfg.scenario == plan_config::kScenarioLakeVillage;
  const uint32_t body = cfg.open_body;
  const int sq = static_cast<int>(cfg.square_cells);
  const float ward_cells = keep_ward ? 3.0f * static_cast<float>(cfg.keep_ward_cells * (cfg.keep_ward_cells + 1)) : 0.0f;
  const float need = static_cast<float>(target) * mean_cells * clampf(cfg.density, 1.2f, 8.0f) + static_cast<float>(cfg.civic) * 80.0f +
                     ward_cells * 1.2f + static_cast<float>(cfg.outer_parts) * 90.0f +
                     static_cast<float>(cfg.waterfronts) * 30.0f + static_cast<float>(cfg.crop_fields) * 110.0f +
                     (body == plan_config::kOpenBodyLake ? 3.0f * static_cast<float>((sq + 4) * (sq + 4)) : 0.0f);
  Site s;
  s.R = static_cast<int>(foundry::det::sqrt(need / 3.0f)) + 3;
  if (hill || castle) s.R += 4;
  s.R = s.R < 10 ? 10 : (s.R > 72 ? 72 : s.R);
  s.N = s.R + kEdge + kApron + (walls_planned ? 8 : 0);  // a wall hugging what's built may stand past the site's rim
  s.side = 2 * s.N + 1;
  const size_t cells = static_cast<size_t>(s.side) * static_cast<size_t>(s.side);
  s.valid.assign(cells, 0);
  s.site.assign(cells, 0);
  s.use.assign(cells, kFree);
  s.street.assign(cells, kNone);
  s.tier.assign(cells, 0);
  s.arterial.assign(cells, 0);
  s.nat.assign(cells, 0);
  s.h.assign(cells, 0);
  s.plot.assign(cells, -1);
  // A walled town's site is a circle inside its wall; any other's, the hex of radius R.
  const float wall_m = (static_cast<float>(s.R) - 0.5f) * kCellM;
  for (int q = -s.N; q <= s.N; ++q)
    for (int r = -s.N; r <= s.N; ++r) {
      if (!s.in(q, r)) continue;
      const int i = s.idx(q, r);
      s.valid[i] = 1;
      if (walled) {
        const float x = cell_x(q, r), z = cell_z(r);
        s.site[i] = x * x + z * z <= wall_m * wall_m ? 1 : 0;
      } else {
        s.site[i] = hex_distance(q, r) <= s.R ? 1 : 0;
      }
    }
  const float Rm = static_cast<float>(s.R) * 1.5f;
  const uint32_t seed = hash_u32(in.seed * 2654435761u + 0x5e771e5u);
  const float relief = maxf(cfg.relief_m, 0), feat = maxf(cfg.feature_cells, 2) * kCellM;
  const int summit_q = 0, summit_r = -(s.R * 9) / 20;
  const float river_half = 0.75f * static_cast<float>(cfg.river_cells < 1 ? 1 : cfg.river_cells) + 0.1f;
  const float river_z = cfg.terrain == plan_config::kTerrainBank ? -0.62f * Rm : 0.0f;
  for (size_t i = 0; i < cells; ++i) {
    if (!s.valid[i]) continue;
    const float x = s.x(static_cast<int>(i)), z = s.z(static_cast<int>(i));
    const float n = fbm(x + 1000.0f, z + 1000.0f, feat, seed);
    float h = 0;
    switch (cfg.terrain) {
      case plan_config::kTerrainFlat: h = relief * 0.15f * n; break;
      case plan_config::kTerrainGentle: h = relief * n; break;
      case plan_config::kTerrainHill: {
        const float dx = x - cell_x(summit_q, summit_r), dz = z - cell_z(summit_r);
        const float d = foundry::det::sqrt(dx * dx + dz * dz) / (Rm * 1.15f);
        const float t = clampf(1.0f - d, 0, 1);
        h = relief * smooth(t) + relief * 0.12f * n;
        break;
      }
      case plan_config::kTerrainValley: {
        const float t = clampf(absf(z - river_z) / (Rm * 1.3f), 0, 1);
        h = relief * t * t * (1.6f - 0.6f * t) + relief * 0.15f * n;
        break;
      }
      case plan_config::kTerrainBank: {
        const float t = clampf((z - river_z) / (Rm * 1.8f), -0.2f, 1);
        h = relief * t + relief * 0.12f * n;
        break;
      }
      default: break;
    }
    s.nat[i] = h;
  }
  // Water: flat a little below its banks, which stay above it.
  auto flood = [&](const std::vector<int> &wet) {
    float level = 1e30f;
    for (int c : wet) level = minf(level, s.nat[c]);
    for (int c : wet) {
      s.use[c] = kWater;
      s.nat[c] = level - 0.7f;
    }
    for (int c : wet)
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(c, d);
        if (nb >= 0 && s.use[nb] != kWater && s.nat[nb] < level + 0.2f) s.nat[nb] = level + 0.2f;
      }
  };
  if (cfg.river) {
    std::vector<int> wet;
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && absf(s.z(static_cast<int>(i)) - river_z) < river_half) wet.push_back(static_cast<int>(i));
    flood(wet);
  }

  // ---- 2. the skeleton ----
  Rng rng(seed, 2u);
  std::vector<std::vector<int>> paths;
  int node = s.idx(0, 0);
  if (cfg.river) {
    const int r = static_cast<int>((river_z + river_half + 4.5f) / 1.5f) + 1;
    node = s.idx(-r / 2, r);
  }
  // The keep's ward: a circle round the summit (a castle's: the north of its site), ringed by its
  // own wall, a bailey (a square) in its middle, a gate facing the town, and a road from the gate to
  // the bailey. The keep and the castle's parts are placed round the bailey with the other plots.
  int keep_gate = -1;
  std::vector<Plot> plots;
  std::vector<uint8_t> in_ward(cells, 0), wall_ring(cells, 0);  // ring: 1 the wall, 2 the keep's wall
  std::vector<int> bailey;
  int ward_centre = -1;
  if (hill || castle) node = s.idx(-(s.R * 3 / 10) / 2, s.R * 3 / 10);
  if (keep_ward) {
    ward_centre = s.idx(summit_q, summit_r);
    const int rk = mini_(static_cast<int>(cfg.keep_ward_cells), s.R / 2);
    const float wr = (static_cast<float>(rk) - 0.5f) * kCellM;
    const float cxw = s.x(ward_centre), czw = s.z(ward_centre);
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || !s.inside(static_cast<int>(i))) continue;
      const float dx = s.x(static_cast<int>(i)) - cxw, dz = s.z(static_cast<int>(i)) - czw;
      if (dx * dx + dz * dz <= wr * wr) in_ward[i] = 1;
    }
    for (size_t i = 0; i < cells; ++i) {
      if (!in_ward[i]) continue;
      bool rim = false;
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        rim = rim || nb < 0 || !in_ward[nb];
      }
      if (rim) {
        s.use[i] = kKeepWall;
        wall_ring[i] = 2;
      }
    }
    // The gate: the ring cell nearest the town's node.
    float best = 1e30f;
    for (size_t i = 0; i < cells; ++i) {
      if (wall_ring[i] != 2) continue;
      const float dx = s.x(static_cast<int>(i)) - s.x(node), dz = s.z(static_cast<int>(i)) - s.z(node);
      if (dx * dx + dz * dz < best) {
        best = dx * dx + dz * dz;
        keep_gate = static_cast<int>(i);
      }
    }
    // The keep at the ward's middle (the largest castle lot), its front toward the gate; the
    // bailey a ring of square round it, so the keep stands like a square's centre; the castle's
    // parts are placed later, backing onto the wall and fronting the bailey.
    int keep_lot = -1;
    for (size_t li = 0; li < lots.size(); ++li)
      if (lots[li].cls == kCastleClass && (keep_lot < 0 || lots[li].cells.size() > lots[static_cast<size_t>(keep_lot)].cells.size()))
        keep_lot = static_cast<int>(li);
    int reach = 1;
    if (keep_lot >= 0) {
      const float gx = s.x(keep_gate) - cxw, gz = s.z(keep_gate) - czw;
      int turn = 0;
      float best_dot = -1e30f;
      for (int k = 0; k < 6; ++k) {
        const float dt = kSin6[k] * gx + kCos6[k] * gz;
        if (dt > best_dot) {
          best_dot = dt;
          turn = k;
        }
      }
      const Turned &t = turned[static_cast<size_t>(keep_lot)][turn];
      bool fits = true;
      for (size_t j = 0; j + 1 < t.cells.size() && fits; j += 2) {
        const int q = summit_q + t.cells[j], r = summit_r + t.cells[j + 1];
        fits = s.in(q, r) && in_ward[s.idx(q, r)] && wall_ring[s.idx(q, r)] == 0;
      }
      if (fits) {
        Plot k;
        k.lot = keep_lot;
        k.turn = turn;
        k.aq = summit_q;
        k.ar = summit_r;
        k.cls = kCastleClass;
        k.shape = lots[static_cast<size_t>(keep_lot)].shape;
        k.use = plan_plot::kUseKeep;
        const int id = static_cast<int>(plots.size());
        for (size_t j = 0; j + 1 < t.cells.size(); j += 2) {
          const int c = s.idx(summit_q + t.cells[j], summit_r + t.cells[j + 1]);
          k.cells.push_back(c);
          k.wet.push_back(0);
          s.use[c] = kPlot;
          s.plot[c] = id;
          reach = maxi_(reach, hex_distance(t.cells[j], t.cells[j + 1]));
        }
        for (size_t j = 0; j + 1 < t.doors.size(); j += 2)
          if (s.in(summit_q + t.doors[j], summit_r + t.doors[j + 1])) k.doors.push_back(s.idx(summit_q + t.doors[j], summit_r + t.doors[j + 1]));
        plots.push_back(k);
      }
    }
    for (size_t i = 0; i < cells; ++i)
      if (in_ward[i] && wall_ring[i] == 0 && s.use[i] == kFree &&
          hex_distance(s.q_of(static_cast<int>(i)) - summit_q, s.r_of(static_cast<int>(i)) - summit_r) <= reach + 2) {
        s.use[i] = kPlaza;
        bailey.push_back(static_cast<int>(i));
      }
    for (Plot &k : plots)
      if (k.use == plan_plot::kUseKeep)
        for (int c : k.cells)
          for (int d = 0; d < 6; ++d) {
            const int nb = s.neighbour(c, d);
            if (nb < 0 || s.use[nb] != kPlaza) continue;
            bool seen = false;
            for (int f : k.frontage) seen = seen || f == nb;
            if (!seen) k.frontage.push_back(nb);
          }
    s.use[keep_gate] = kStreet;
    s.street[keep_gate] = kGate;
    s.arterial[keep_gate] = 1;
  }
  // The wall: the ring of site cells beside the outside, laid before the roads, so roads come through gates.
  if (walled)
    for (size_t i = 0; i < cells; ++i) {
      if (!s.site[i] || s.use[i] == kWater || s.use[i] == kKeepWall || s.use[i] == kPlaza) continue;
      bool rim = false;
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        rim = rim || nb < 0 || !s.site[nb];
      }
      if (rim) {
        s.use[i] = kWall;
        wall_ring[i] = 1;
      }
    }
  // Entrances: by scenario, on the site's rim; never in water.
  std::vector<int> entrances;
  {
    std::vector<int> dirs;
    const int count = static_cast<int>(cfg.entrances < 1 ? 1 : (cfg.entrances > 6 ? 6 : cfg.entrances));
    if (cfg.scenario == plan_config::kScenarioStreetVillage || river_town) {
      dirs = {0, 3};
      if (count >= 3) dirs.push_back(river_town ? 1 : 4);
      if (count >= 4) dirs.push_back(river_town ? 5 : 1);
    } else if (hill || castle) {
      dirs = {4};
      if (count >= 2) dirs.push_back(5);
    } else if (count == 1) {
      dirs = {static_cast<int>(rng.next() % 6u)};
    } else {
      const int start = static_cast<int>(rng.next() % 6u);
      for (int k = 0; k < count; ++k) dirs.push_back((start + (k * 6) / count) % 6);
    }
    for (int d : dirs) {
      int q = 0, r = 0;
      while (s.in(q + kDq[d], r + kDr[d]) && s.inside(s.idx(q + kDq[d], r + kDr[d]))) {
        q += kDq[d];
        r += kDr[d];
      }
      for (int tries = 0; tries < s.R && s.use[s.idx(q, r)] == kWater; ++tries) {
        q += kDq[(d + 2) % 6];
        r += kDr[(d + 2) % 6];
        while (s.in(q, r) && !s.inside(s.idx(q, r))) {
          q -= kDq[d];
          r -= kDr[d];
        }
      }
      if (!s.in(q, r)) continue;
      const int e = s.idx(q, r);
      bool dup = false;
      for (int x : entrances) dup = dup || x == e;
      if (!dup) entrances.push_back(e);
    }
  }
  // The node: a square, a green, a pond in a green, or a lake (with a ring lane a waterfront's
  // depth back).
  std::vector<uint8_t> is_node(cells, 0);
  const bool lake = body == plan_config::kOpenBodyLake;
  const bool green = body == plan_config::kOpenBodyGreen || body == plan_config::kOpenBodyPond;
  {
    const int nq = s.q_of(node), nr = s.r_of(node);
    const int stretch = cfg.scenario == plan_config::kScenarioGreenVillage ? s.R / 3 : 0;
    std::vector<int> wet;
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || !s.inside(static_cast<int>(i)) || s.use[i] != kFree) continue;
      const int q = s.q_of(static_cast<int>(i)) - nq, r = s.r_of(static_cast<int>(i)) - nr;
      int d = hex_distance(q, r);
      for (int t = -stretch; t <= stretch; ++t) {
        const int dd = hex_distance(q - t, r);
        d = dd < d ? dd : d;
      }
      if (d > sq) continue;
      is_node[i] = 1;
      if (lake) wet.push_back(static_cast<int>(i));
      else s.use[i] = green ? kGreen : kPlaza;
    }
    if (lake) flood(wet);
    if (green || lake) {
      const int gap = lake ? 3 : 1;
      std::vector<int> body_cells;
      for (size_t i = 0; i < cells; ++i)
        if (is_node[i]) body_cells.push_back(static_cast<int>(i));
      const std::vector<int> D = distance_from(s, body_cells, true, true);
      for (size_t i = 0; i < cells; ++i)
        if (s.valid[i] && s.inside(static_cast<int>(i)) && s.use[i] == kFree && D[i] == gap) {
          s.use[i] = kStreet;
          s.street[i] = kLane;
          s.arterial[i] = 1;
        }
      if (body == plan_config::kOpenBodyPond) {
        int low = -1;
        for (size_t i = 0; i < cells; ++i)
          if (s.use[i] == kGreen && hex_distance(s.q_of(static_cast<int>(i)) - nq, s.r_of(static_cast<int>(i)) - nr) >= 1 &&
              (low < 0 || s.nat[i] < s.nat[low]))
            low = static_cast<int>(i);
        if (low >= 0) {
          std::vector<int> pond = {low};
          for (int d = 0; d < 6; d += 2) {
            const int nb = s.neighbour(low, d);
            if (nb >= 0 && s.use[nb] == kGreen) pond.push_back(nb);
          }
          flood(pond);
        }
      }
    }
  }
  // A centre at the square's middle (a well, a market cross, a fountain): a building on a centre
  // lot, so the square's road rings it. The largest centre lot that keeps a ring of square round it.
  if (cfg.centre && !green && !lake && sq >= 2) {
    int best = -1;
    for (size_t li = 0; li < lots.size(); ++li) {
      if (lots[li].cls != kCentreClass) continue;
      bool fits = true;
      const Turned &t = turned[li][0];
      for (size_t j = 0; j + 1 < t.cells.size() && fits; j += 2) {
        const int q = s.q_of(node) + t.cells[j], r = s.r_of(node) + t.cells[j + 1];
        fits = s.in(q, r) && s.use[s.idx(q, r)] == kPlaza && hex_distance(t.cells[j], t.cells[j + 1]) <= sq - 1;
      }
      if (fits && (best < 0 || lots[li].cells.size() > lots[static_cast<size_t>(best)].cells.size())) best = static_cast<int>(li);
    }
    if (best >= 0) {
      Plot p;
      p.lot = best;
      p.aq = s.q_of(node);
      p.ar = s.r_of(node);
      p.cls = kCentreClass;
      p.shape = lots[static_cast<size_t>(best)].shape;
      p.use = plan_plot::kUseCentre;
      const Turned &t = turned[static_cast<size_t>(best)][0];
      const int id = static_cast<int>(plots.size());
      for (size_t j = 0; j + 1 < t.cells.size(); j += 2) {
        const int c = s.idx(p.aq + t.cells[j], p.ar + t.cells[j + 1]);
        p.cells.push_back(c);
        p.wet.push_back(0);
        s.use[c] = kPlot;
        s.plot[c] = id;
      }
      for (int c : p.cells)
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(c, d);
          if (nb < 0 || s.use[nb] != kPlaza) continue;
          bool seen = false;
          for (int f : p.frontage) seen = seen || f == nb;
          if (!seen) p.frontage.push_back(nb);
        }
      plots.push_back(p);
    }
  }
  // Roads from each entrance to the node (or its ring), and on a hill up to the keep's gate.
  const bool planned = cfg.scenario == plan_config::kScenarioWalledTown;
  const uint8_t main_cls = cfg.scenario == plan_config::kScenarioHamlet ? kLane : kHigh;
  const int main_w = static_cast<int>(main_cls == kHigh ? cfg.high_width : cfg.lane_width);
  std::vector<std::vector<int>> entrance_paths;
  {
    std::vector<uint8_t> goal(cells, 0);
    for (size_t i = 0; i < cells; ++i)
      if ((is_node[i] && s.use[i] == kPlaza) || ((green || lake) && s.use[i] == kStreet)) goal[i] = 1;
    if (!green && !lake) goal[node] = 1;
    PathCost pc;
    pc.grade = main_cls == kHigh ? cfg.high_grade : cfg.lane_grade;
    pc.grade_weight = 6.0f;
    pc.turn = planned ? 3.0f : 0.25f;
    for (size_t e = 0; e < entrances.size(); ++e) {
      const std::vector<int> p = find_path(s, entrances[e], goal, pc);
      lay(s, p, main_cls, main_w, true, &paths);
      entrance_paths.push_back(p);
      for (int c : p) goal[c] = 1;
    }
    if (keep_gate >= 0) {
      // From the keep's gate down to the nearest road the entrances laid, at a cart's grade.
      std::vector<uint8_t> g2(cells, 0);
      for (size_t i = 0; i < cells; ++i)
        if (s.road(static_cast<int>(i)) && !in_ward[i] && static_cast<int>(i) != keep_gate) g2[i] = 1;
      PathCost up = pc;
      up.grade_weight = 60.0f;
      up.turn = 0.6f;
      const std::vector<int> p = find_path(s, keep_gate, g2, up);
      lay(s, p, kHigh, cfg.high_width >= 2 ? 2 : 1, true, &paths);
      // Inside: from the gate to the bailey.
      if (!bailey.empty()) {
        std::vector<uint8_t> g3(cells, 0);
        for (int c : bailey) g3[c] = 1;
        const std::vector<int> q = find_path(s, keep_gate, g3, up);
        std::vector<int> inside;
        for (int c : q)
          if (in_ward[c]) inside.push_back(c);
        lay(s, inside, kHigh, 1, true, &paths);
      }
    }
    // A river town's quay: along the water on the town's side, a waterfront's depth back.
    if (river_town) {
      std::vector<int> wet;
      for (size_t i = 0; i < cells; ++i)
        if (s.use[i] == kWater) wet.push_back(static_cast<int>(i));
      const std::vector<int> D = distance_from(s, wet, true, true);
      for (size_t i = 0; i < cells; ++i)
        if (s.valid[i] && s.inside(static_cast<int>(i)) && s.use[i] == kFree && D[i] == 3 && s.z(static_cast<int>(i)) > river_z) {
          s.use[i] = kStreet;
          s.street[i] = kQuay;
          s.arterial[i] = 1;
        }
    }
  }
  // A ring lane inside a palisade or stone wall.
  if (walled && cfg.walls != plan_config::kWallsHedge && !village)
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || s.use[i] != kFree || !s.inside(static_cast<int>(i))) continue;
      bool by_wall = false;
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        by_wall = by_wall || (nb >= 0 && s.use[nb] == kWall);
      }
      if (by_wall) {
        s.use[i] = kStreet;
        s.street[i] = kLane;
      }
    }

  // ---- 3. back lanes on contours, and alleys ----
  if (cfg.block_cells > 0 && static_cast<int>(cfg.block_cells) < s.R - 2) {
    std::vector<int> primary;
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && (s.use[i] == kStreet || s.use[i] == kPlaza || s.use[i] == kGreen)) primary.push_back(static_cast<int>(i));
    const std::vector<int> D = distance_from(s, primary, true, false);
    const int B = static_cast<int>(cfg.block_cells);
    const int nq = s.q_of(node), nr = s.r_of(node);
    for (int level = B; level < s.R - 2; level += B + 1) {
      std::vector<int> ring;
      std::vector<float> key;
      for (size_t i = 0; i < cells; ++i)
        if (s.valid[i] && s.use[i] == kFree && D[i] == level && s.inside(static_cast<int>(i)) && !in_ward[i]) {
          ring.push_back(static_cast<int>(i));
          key.push_back(pseudo_angle(s.x(static_cast<int>(i)) - cell_x(nq, nr), s.z(static_cast<int>(i)) - cell_z(nr)));
        }
      sort_by(ring, key);
      for (int c : ring) {
        s.use[c] = kStreet;
        s.street[c] = kLane;
      }
      if (cfg.lane_width >= 2)
        for (int c : ring)
          for (int d = 0; d < 6; ++d) {
            const int nb = s.neighbour(c, d);
            if (nb >= 0 && s.use[nb] == kFree && D[nb] == level + 1 && (d % 2) == 0) {
              s.use[nb] = kStreet;
              s.street[nb] = kLane;
            }
          }
      const int every = cfg.alley_every ? static_cast<int>(cfg.alley_every) : 1 << 20;
      std::vector<uint8_t> piece_done(cells, 0);
      for (size_t k = 0; k < ring.size(); ++k) {
        const int c = ring[k];
        bool connect = static_cast<int>(k) % every == every / 2;
        uint8_t cls = kAlley;
        if (!piece_done[c]) {
          std::vector<int> stack = {c};
          piece_done[c] = 1;
          while (!stack.empty()) {
            const int x = stack.back();
            stack.pop_back();
            for (int d = 0; d < 6; ++d) {
              const int nb = s.neighbour(x, d);
              if (nb >= 0 && !piece_done[nb] && s.use[nb] == kStreet && D[nb] == level) {
                piece_done[nb] = 1;
                stack.push_back(nb);
              }
            }
          }
          connect = true;
          cls = kLane;
        }
        if (!connect) continue;
        int at = c, prev_d = -1;
        for (int steps = 0; steps < 4 * B + 8; ++steps) {
          int next = -1, nd = -1;
          for (int t = 0; t < 6; ++t) {
            const int d = prev_d >= 0 ? (prev_d + (t % 2 ? (t + 1) / 2 : 6 - t / 2)) % 6 : t;
            const int nb = s.neighbour(at, d);
            if (nb >= 0 && D[nb] == D[at] - 1) {
              next = nb;
              nd = d;
              break;
            }
          }
          if (next < 0) break;
          at = next;
          prev_d = nd;
          if (s.use[at] == kStreet || s.use[at] == kPlaza || s.use[at] == kGreen) break;
          s.use[at] = kStreet;
          s.street[at] = cls;
        }
      }
    }
  }

  // ---- 4. plots ----
  std::vector<int> along;  // steps from the node along roads
  auto measure_along = [&]() {
    std::vector<int> src;
    for (size_t i = 0; i < cells; ++i)
      if ((is_node[i] && s.use[i] == kPlaza) || (static_cast<int>(i) == node && s.road(node)) ||
          ((green || lake) && s.use[i] == kStreet && s.arterial[i] && s.street[i] == kLane))
        src.push_back(static_cast<int>(i));
    along.assign(cells, 1 << 29);
    std::vector<int> queue = src;
    for (int c : src) along[c] = 0;
    for (size_t head = 0; head < queue.size(); ++head) {
      const int c = queue[head];
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(c, d);
        if (nb < 0 || along[nb] <= along[c] + 1 || !s.road(nb)) continue;
        along[nb] = along[c] + 1;
        queue.push_back(nb);
      }
    }
  };
  measure_along();
  std::vector<int> fronts;
  {
    std::vector<int> key;
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && s.inside(static_cast<int>(i)) && s.road(static_cast<int>(i)) && s.street[i] != kBridge && s.street[i] != kGate &&
          along[i] < (1 << 29) && !in_ward[i]) {
        fronts.push_back(static_cast<int>(i));
        const int cls_rank = s.use[i] == kPlaza || s.street[i] == kHigh || s.street[i] == kQuay ? 0 : (s.street[i] == kLane ? 1 : 2);
        bool by_node = false;
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(static_cast<int>(i), d);
          by_node = by_node || (nb >= 0 && is_node[nb]);
        }
        key.push_back((by_node ? 0 : cls_rank * 1000 + 1000) + along[i]);
      }
    sort_by(fronts, key);
  }
  // Free for a plot: open site cells, inside the keep's ward only while placing its parts.
  bool ward_mode = false;
  auto free_cell = [&](int q, int r) {
    if (!s.in(q, r)) return false;
    const int c = s.idx(q, r);
    return s.inside(c) && s.use[c] == kFree && (in_ward[c] != 0) == ward_mode;
  };
  uint32_t castle_cap = 1u << 20;
  // Every lot of a class (its shape first), turn, and anchor that puts its front on road cell f.
  auto try_place = [&](int f, uint32_t cls, uint32_t shape, Plot &best) {
    float best_score = -1e30f;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass)
      for (size_t li = 0; li < lots.size(); ++li) {
        if (lots[li].cls != cls || ((lots[li].shape == shape) != (pass == 0))) continue;
        if (cls == kCastleClass && lots[li].shape > castle_cap) continue;  // the keep's lot is the keep's
        for (int k = 0; k < 6; ++k) {
          const Turned &t = turned[li][k];
          for (size_t fo = 0; fo + 1 < t.front.size(); fo += 2) {
            const int aq = s.q_of(f) - t.front[fo], ar = s.r_of(f) - t.front[fo + 1];
            bool ok = true;
            float lo_h = 1e30f, hi_h = -1e30f;
            for (size_t j = 0; j + 1 < t.cells.size() && ok; j += 2) {
              const int q = aq + t.cells[j], r = ar + t.cells[j + 1];
              ok = free_cell(q, r);
              if (ok) {
                const float h = s.nat[s.idx(q, r)];
                lo_h = minf(lo_h, h);
                hi_h = maxf(hi_h, h);
              }
            }
            if (!ok || hi_h - lo_h > (ward_mode ? 6.0f : 2.5f)) continue;  // a ward's parts terrace a hilltop
            int front_street = 0, front_all = 0, back_street = 0, side_street = 0, snug = 0;
            for (size_t j = 0; j + 1 < t.front.size(); j += 2) {
              const int q = aq + t.front[j], r = ar + t.front[j + 1];
              ++front_all;
              if (s.in(q, r) && s.road(s.idx(q, r)) && s.street[s.idx(q, r)] != kBridge) ++front_street;
            }
            if (front_street < 2 || front_street * 2 < front_all) continue;
            // A lot with doors stands only where a door opens onto a road.
            if (!t.doors.empty()) {
              bool door_on_road = false;
              for (size_t j = 0; j + 1 < t.doors.size(); j += 2) {
                const int q = aq + t.doors[j], r = ar + t.doors[j + 1];
                door_on_road = door_on_road || (s.in(q, r) && s.road(s.idx(q, r)) && s.street[s.idx(q, r)] != kBridge);
              }
              if (!door_on_road) continue;
            }
            for (size_t j = 0; j + 1 < t.back.size(); j += 2) {
              const int q = aq + t.back[j], r = ar + t.back[j + 1];
              if (!s.in(q, r)) continue;
              const uint8_t u = s.use[s.idx(q, r)];
              if (u == kStreet || u == kPlaza) ++back_street;
              if (u == kPlot) ++snug;
            }
            for (size_t j = 0; j + 1 < t.side.size(); j += 2) {
              const int q = aq + t.side[j], r = ar + t.side[j + 1];
              if (!s.in(q, r)) continue;
              const uint8_t u = s.use[s.idx(q, r)];
              if (u == kStreet || u == kPlaza) ++side_street;
              if (u == kPlot) ++snug;
            }
            const float score = 12.0f * static_cast<float>(front_street) / static_cast<float>(front_all) + 0.6f * static_cast<float>(snug) -
                                1.5f * static_cast<float>(back_street) - 0.4f * static_cast<float>(side_street) - 0.15f * (hi_h - lo_h) -
                                0.002f * static_cast<float>(hash_combine(seed, static_cast<uint32_t>(aq * 31 + ar * 7 + k * 3 + static_cast<int>(li))) % 100u);
            if (score > best_score) {
              best_score = score;
              best = Plot();
              best.lot = static_cast<int>(li);
              best.turn = k;
              best.aq = aq;
              best.ar = ar;
              best.cls = cls;
              best.shape = lots[li].shape;
              found = true;
            }
          }
        }
      }
    return found;
  };
  auto commit = [&](Plot &p) {
    const Turned &t = turned[p.lot][p.turn];
    p.cells.clear();
    p.wet.clear();
    for (size_t j = 0; j + 1 < t.cells.size(); j += 2) {
      p.cells.push_back(s.idx(p.aq + t.cells[j], p.ar + t.cells[j + 1]));
      p.wet.push_back(t.wet[j / 2]);
    }
    for (size_t j = 0; j + 1 < t.doors.size(); j += 2)
      if (s.in(p.aq + t.doors[j], p.ar + t.doors[j + 1])) p.doors.push_back(s.idx(p.aq + t.doors[j], p.ar + t.doors[j + 1]));
    // Its frontage: a building's road cells in front, a waterfront's behind.
    const std::vector<int> &face = p.waterfront ? t.back : t.front;
    for (size_t j = 0; j + 1 < face.size(); j += 2) {
      const int q = p.aq + face[j], r = p.ar + face[j + 1];
      if (s.in(q, r) && s.road(s.idx(q, r))) p.frontage.push_back(s.idx(q, r));
    }
    const int id = static_cast<int>(plots.size());
    for (int c : p.cells) {
      s.use[c] = p.waterfront ? kWaterfront : kPlot;
      s.plot[c] = id;
    }
    plots.push_back(p);
  };
  // Waterfronts: where a waterfront lot's wet cells lie on water, its dry cells on land in the site,
  // and its back on a road; nearest the node first.
  if (cfg.waterfronts > 0) {
    std::vector<int> wet;
    for (size_t i = 0; i < cells; ++i)
      if (s.use[i] == kWater) wet.push_back(static_cast<int>(i));
    const std::vector<int> D = distance_from(s, wet, false, true);
    std::vector<int> anchors;
    std::vector<int> key;
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && s.inside(static_cast<int>(i)) && s.use[i] == kFree && D[i] == 1) {
        anchors.push_back(static_cast<int>(i));
        key.push_back(hex_distance(s.q_of(static_cast<int>(i)) - s.q_of(node), s.r_of(static_cast<int>(i)) - s.r_of(node)));
      }
    sort_by(anchors, key);
    Rng wr(seed, 7u);
    uint32_t placed = 0;
    for (size_t ai = 0; ai < anchors.size() && placed < cfg.waterfronts; ++ai) {
      const int a = anchors[ai];
      if (s.use[a] != kFree) continue;
      float best_score = -1e30f;
      Plot best;
      bool found = false;
      const uint32_t salt = wr.next();
      for (size_t li = 0; li < lots.size(); ++li) {
        if (lots[li].cls != kWaterfrontClass) continue;
        for (int k = 0; k < 6; ++k) {
          const Turned &t = turned[li][k];
          bool ok = true;
          for (size_t j = 0; j + 1 < t.cells.size() && ok; j += 2) {
            const int q = s.q_of(a) + t.cells[j], r = s.r_of(a) + t.cells[j + 1];
            if (!s.in(q, r)) {
              ok = false;
              break;
            }
            const int c = s.idx(q, r);
            ok = t.wet[j / 2] ? s.use[c] == kWater : (s.inside(c) && s.use[c] == kFree);
          }
          if (!ok) continue;
          int back_road = 0;
          for (size_t j = 0; j + 1 < t.back.size(); j += 2) {
            const int q = s.q_of(a) + t.back[j], r = s.r_of(a) + t.back[j + 1];
            // Deliveries: a road a cart can use.
            if (s.in(q, r) && s.use[s.idx(q, r)] == kStreet &&
                (s.street[s.idx(q, r)] == kHigh || s.street[s.idx(q, r)] == kLane || s.street[s.idx(q, r)] == kQuay))
              ++back_road;
          }
          if (back_road < 1) continue;
          const float score = static_cast<float>(back_road) + 0.05f * static_cast<float>(lots[li].cells.size() / 2) +
                              0.01f * static_cast<float>(hash_combine(salt, static_cast<uint32_t>(li * 7 + static_cast<size_t>(k))) % 100u);
          if (score > best_score) {
            best_score = score;
            best = Plot();
            best.lot = static_cast<int>(li);
            best.turn = k;
            best.aq = s.q_of(a);
            best.ar = s.r_of(a);
            best.cls = kWaterfrontClass;
            best.waterfront = true;
            best.use = plan_plot::kUseHarbour;
            found = true;
          }
        }
      }
      if (found) {
        commit(best);
        ++placed;
      }
    }
  }
  // The castle's parts round the keep's bailey (the keep stands at its middle): nearest the
  // ward's middle first, each preferring the next size down, never the keep's.
  uint32_t castle_shapes = 0;
  for (const Lot &l : lots)
    if (l.cls == kCastleClass) castle_shapes = l.shape + 1 > castle_shapes ? l.shape + 1 : castle_shapes;
  castle_cap = castle_shapes >= 2 ? castle_shapes - 2 : 0;
  if (keep_ward && castle_shapes > 0) {
    ward_mode = true;
    std::vector<int> ward_fronts;
    std::vector<float> near_key;
    for (size_t i = 0; i < cells; ++i)
      if (in_ward[i] && s.road(static_cast<int>(i)) && s.street[i] != kGate) ward_fronts.push_back(static_cast<int>(i));
    std::vector<int> by_near = ward_fronts;
    for (int c : by_near) near_key.push_back(static_cast<float>(hex_distance(s.q_of(c) - s.q_of(ward_centre), s.r_of(c) - s.r_of(ward_centre))));
    sort_by(by_near, near_key);
    uint32_t placed = 0;
    for (size_t fi = 0; fi < by_near.size() && placed < cfg.castle_parts; ++fi) {
      Plot p;
      const uint32_t want = castle_shapes > 1 ? (castle_shapes - 2) - placed % (castle_shapes - 1) : 0;
      if (try_place(by_near[fi], kCastleClass, want, p)) {
        p.use = plan_plot::kUseCastle;
        commit(p);
        ++placed;
      }
    }
    ward_mode = false;
  }
  // A castle's parts in its outer ward (barracks, stables, storehouses), along its roads.
  if (cfg.outer_parts > 0 && castle_shapes > 0) {
    uint32_t placed = 0;
    for (size_t fi = 0; fi < fronts.size() && placed < cfg.outer_parts; ++fi) {
      Plot p;
      const uint32_t want = castle_shapes > 2 ? 1u + placed % 2u : 0u;
      if (try_place(fronts[fi], kCastleClass, want, p)) {
        p.use = plan_plot::kUseCastle;
        commit(p);
        ++placed;
      }
    }
  }
  // Civic plots, as near the node as they fit: the first the largest civic lot, then smaller.
  {
    uint32_t civic_shapes = 0;
    for (const Lot &l : lots)
      if (l.cls == kCivicClass) civic_shapes = l.shape + 1 > civic_shapes ? l.shape + 1 : civic_shapes;
    uint32_t placed = 0;
    for (size_t fi = 0; fi < fronts.size() && placed < cfg.civic && civic_shapes > 0; ++fi) {
      Plot p;
      const uint32_t want = (civic_shapes - 1) - placed % civic_shapes;
      if (try_place(fronts[fi], kCivicClass, want, p)) {
        p.use = plan_plot::kUseCivic;
        commit(p);
        ++placed;
      }
    }
  }
  // Households, by class deficit and the shape weights.
  {
    Rng prng(seed, 5u);
    const float shape_w[5] = {maxf(cfg.shape_wide, 0), maxf(cfg.shape_deep, 0), maxf(cfg.shape_round, 0), maxf(cfg.shape_long, 0),
                              maxf(cfg.shape_broad, 0)};
    float shape_sum = 0;
    for (float w : shape_w) shape_sum += w;
    uint32_t count[3] = {0, 0, 0}, homes = 0;
    for (int pass = 0; pass < 3 && homes < target; ++pass)
      for (size_t fi = 0; fi < fronts.size() && homes < target; ++fi) {
        const int f = fronts[fi];
        if (!s.road(f)) continue;
        uint32_t order[3] = {0, 1, 2};
        float lag[3];
        for (int c = 0; c < 3; ++c) lag[c] = share[c] * static_cast<float>(homes + 1) - static_cast<float>(count[c]);
        for (int a = 0; a < 3; ++a)
          for (int b = a + 1; b < 3; ++b)
            if (lag[order[b]] > lag[order[a]] || (lag[order[b]] == lag[order[a]] && order[b] < order[a])) {
              const uint32_t t = order[a];
              order[a] = order[b];
              order[b] = t;
            }
        float pick = prng.uniform() * (shape_sum > 0 ? shape_sum : 1);
        uint32_t shape = 0;
        for (uint32_t k = 0; k < 5; ++k) {
          if (pick < shape_w[k]) {
            shape = k;
            break;
          }
          pick -= shape_w[k];
          shape = k;
        }
        for (int o = 0; o < 3; ++o) {
          if (share[order[o]] <= 0) continue;
          Plot p;
          if (try_place(f, order[o], shape, p)) {
            commit(p);
            ++count[order[o]];
            ++homes;
            break;
          }
        }
      }
  }

  // Crop fields at the settlement's edge: the farthest frontages first (a farm's fields lie beyond
  // its houses), each its own size by turn.
  if (cfg.crop_fields > 0) {
    uint32_t field_shapes = 0;
    for (const Lot &l : lots)
      if (l.cls == kFieldClass) field_shapes = l.shape + 1 > field_shapes ? l.shape + 1 : field_shapes;
    uint32_t placed = 0;
    for (size_t fi = fronts.size(); fi-- > 0 && placed < cfg.crop_fields && field_shapes > 0;) {
      if (!s.road(fronts[fi])) continue;
      Plot p;
      const uint32_t want = (field_shapes - 1) - placed % field_shapes;
      if (try_place(fronts[fi], kFieldClass, want, p)) {
        p.use = plan_plot::kUseField;
        commit(p);
        ++placed;
      }
    }
  }

  // ---- 5. pruning ----
  // Roads stay where something needs them: each plot's cheapest way to the node (cart roads cost
  // least, so a cart's way is kept where there is one), each entrance's, and the width beside them.
  // Loops and back lanes that serve no plot go back to open land.
  {
    std::vector<uint8_t> needed(cells, 0), plot_needed(cells, 0);
    auto is_cart_road = [&](int c) {
      const uint8_t k = s.street[c];
      return s.use[c] == kPlaza || (s.use[c] == kStreet && (k == kHigh || k == kLane || k == kBridge || k == kGate || k == kQuay));
    };
    // The cheapest ways to the node: by any road, and by cart roads only.
    auto ways = [&](bool cart, std::vector<float> &cost, std::vector<int> &parent) {
      cost.assign(cells, 1e30f);
      parent.assign(cells, -1);
      Heap heap;
      for (size_t i = 0; i < cells; ++i)
        if (s.road(static_cast<int>(i)) && (is_node[i] || static_cast<int>(i) == node ||
                                             ((green || lake) && s.arterial[i] && s.street[i] == kLane))) {
          cost[i] = 0;
          heap.push(0, static_cast<int>(i));
        }
      while (!heap.empty()) {
        const Heap::E e = heap.pop();
        if (e.c > cost[e.id]) continue;
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(e.id, d);
          if (nb < 0 || !s.road(nb) || (cart && !is_cart_road(nb))) continue;
          if (e.c + 1.0f < cost[nb]) {
            cost[nb] = e.c + 1.0f;
            parent[nb] = e.id;
            heap.push(cost[nb], nb);
          }
        }
      }
    };
    std::vector<float> cost, cart_cost;
    std::vector<int> parent, cart_parent;
    ways(false, cost, parent);
    ways(true, cart_cost, cart_parent);
    auto mark = [&](int from, const std::vector<int> &par, std::vector<uint8_t> &into) {
      for (int c = from; c >= 0 && !into[c]; c = par[c]) into[c] = 1;
    };
    for (const Plot &p : plots) {
      int best = -1, best_cart = -1;
      for (int f : p.frontage) {
        if (cost[f] < 1e29f && (best < 0 || cost[f] < cost[best])) best = f;
        if (cart_cost[f] < 1e29f && (best_cart < 0 || cart_cost[f] < cart_cost[best_cart])) best_cart = f;
      }
      if (best_cart >= 0) {
        mark(best_cart, cart_parent, plot_needed);
        mark(best_cart, cart_parent, needed);
      } else if (best >= 0) {
        mark(best, parent, plot_needed);
        mark(best, parent, needed);
      }
      for (int f : p.frontage) {
        plot_needed[f] = 1;
        needed[f] = 1;
      }
    }
    // Ring lanes, the quay, and the roads in from the entrances stay whole (the entrance roads' far
    // ends are cut back below).
    for (size_t i = 0; i < cells; ++i)
      if (s.use[i] == kStreet && s.arterial[i]) needed[i] = 1;
    for (int e : entrances)
      if (cost[e] < 1e29f) mark(e, parent, needed);
    for (size_t i = 0; i < cells; ++i)
      if (s.use[i] == kPlaza || (keep_gate >= 0 && static_cast<int>(i) == keep_gate)) needed[i] = 1;
    if (keep_gate >= 0) mark(keep_gate, parent, needed);
    for (size_t i = 0; i < cells; ++i) {
      if (s.use[i] != kStreet || needed[i]) continue;
      // The width beside a needed road of its class.
      bool beside = false;
      for (int d = 0; d < 6 && !beside; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        beside = nb >= 0 && needed[nb] && s.street[nb] == s.street[i] &&
                 (s.street[i] == kHigh || s.street[i] == kQuay || (s.street[i] == kLane && cfg.lane_width >= 2));
      }
      if (beside) continue;
      if (s.street[i] == kBridge) {
        s.use[i] = kWater;
      } else {
        s.use[i] = kFree;
      }
      s.street[i] = kNone;
      s.arterial[i] = 0;
    }
    // An unwalled village's roads end where its plots do: each entrance road is cut back to the
    // first cell within three of a plot, which becomes the entrance.
    if (!walled) {
      std::vector<int> built;
      for (size_t i = 0; i < cells; ++i)
        if (s.use[i] == kPlot || s.use[i] == kWaterfront) built.push_back(static_cast<int>(i));
      const std::vector<int> D = distance_from(s, built, false, true);
      for (size_t e = 0; e < entrance_paths.size() && e < entrances.size(); ++e) {
        const std::vector<int> &p = entrance_paths[e];
        size_t keep = 0;
        while (keep < p.size() && D[p[keep]] > 3 && !plot_needed[p[keep]]) ++keep;
        if (keep == 0 || keep >= p.size()) continue;
        auto cut = [&](int x) {
          if (x >= 0 && s.use[x] == kStreet && D[x] > 3 && !plot_needed[x] && s.street[x] != kBridge) {
            s.use[x] = kFree;
            s.street[x] = kNone;
            s.arterial[x] = 0;
          }
        };
        for (size_t i = 0; i < keep; ++i) {
          const int c = p[i];
          if (s.street[c] == kBridge) continue;
          cut(c);
          for (int d = 0; d < 6; ++d) cut(s.neighbour(c, d));
        }
        entrances[e] = p[keep];
      }
    }
    for (int e : entrances)
      if (s.use[e] == kStreet) s.street[e] = kGate;
    // A plot whose frontage went with a road keeps only what's left.
    for (Plot &p : plots) {
      std::vector<int> f2;
      for (int f : p.frontage)
        if (s.road(f)) f2.push_back(f);
      p.frontage = f2;
    }
    measure_along();
  }

  // ---- 6. zoning, wealth, and tiers ----
  {
    int max_along = 1;
    for (Plot &p : plots) {
      int a = 1 << 29;
      for (int f : p.frontage) a = along[f] < a ? along[f] : a;
      if (p.frontage.empty() && p.use == plan_plot::kUseKeep) a = 0;
      p.walk = static_cast<float>(a < (1 << 29) ? a : 0);
      if (a < (1 << 29) && a > max_along) max_along = a;
    }
    float hmin = 1e30f, hmax = -1e30f;
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && s.inside(static_cast<int>(i))) {
        hmin = minf(hmin, s.nat[i]);
        hmax = maxf(hmax, s.nat[i]);
      }
    Rng zr(seed, 6u);
    int inns = 0, stores = 0;
    const int want_inns = village ? 1 : 2;
    std::vector<int> order;
    std::vector<float> key;
    for (size_t i = 0; i < plots.size(); ++i) {
      order.push_back(static_cast<int>(i));
      key.push_back(plots[i].walk);
    }
    sort_by(order, key);
    for (int pi : order) {
      Plot &p = plots[pi];
      const float t = clampf(p.walk / static_cast<float>(max_along), 0, 1);
      float w = 1.0f - t;
      if (hill) {
        float hh = 0;
        for (int c : p.cells) hh += s.nat[c];
        hh /= static_cast<float>(p.cells.size());
        w = 0.5f * w + 0.5f * clampf((hh - hmin) / maxf(hmax - hmin, 0.1f), 0, 1);
      }
      p.wealth = clampf(w, 0, 1);
      if (special(p.use) || p.waterfront || p.use == plan_plot::kUseField) continue;
      bool on_square = false, on_high = false, cart_front = false;
      for (int f : p.frontage) {
        on_high = on_high || s.street[f] == kHigh || s.street[f] == kQuay;
        cart_front = cart_front || s.street[f] == kHigh || s.street[f] == kLane || s.street[f] == kQuay;
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(f, d);
          on_square = on_square || (nb >= 0 && is_node[nb]);
        }
      }
      int to_entrance = 1 << 29;
      for (int e : entrances)
        for (int c : p.frontage) {
          const int d = hex_distance(s.q_of(c) - s.q_of(e), s.r_of(c) - s.r_of(e));
          to_entrance = d < to_entrance ? d : to_entrance;
        }
      const float roll = zr.uniform();
      const int far = hex_distance(s.q_of(p.cells[0]) - s.q_of(node), s.r_of(p.cells[0]) - s.r_of(node));
      if (cfg.scenario == plan_config::kScenarioHamlet) {
        p.use = roll < 0.7f ? plan_plot::kUseFarm : plan_plot::kUseHome;
        if (inns == 0 && on_square && cart_front) {
          p.use = plan_plot::kUseInn;
          ++inns;
        }
      } else if (inns < want_inns && p.cls >= 1 && (on_square || on_high) && cart_front) {
        p.use = plan_plot::kUseInn;
        ++inns;
      } else if ((on_square || (on_high && t < 0.5f)) && cart_front && stores < static_cast<int>(plots.size() / 4 + 1) && roll < 0.75f) {
        p.use = plan_plot::kUseStore;
        ++stores;
      } else if (to_entrance <= 8 && cart_front && roll < 0.6f) {
        p.use = plan_plot::kUseWorkshop;
      } else if (village && far > (s.R * 7) / 10 && p.cls >= 1 && roll < 0.6f) {
        p.use = plan_plot::kUseFarm;
      } else {
        p.use = roll < 0.12f && cart_front ? plan_plot::kUseWorkshop : plan_plot::kUseHome;
      }
    }
    // Tiers: the nearest 30% (at least three, and every civic plot and the keep) at the hamlet tier,
    // to 65% at the village tier, the rest at the town tier; roads open with the plots they serve.
    const size_t n = order.size();
    std::vector<int> reach_t(3, 0);
    for (size_t k = 0; k < n; ++k) {
      Plot &p = plots[order[k]];
      const float frac = static_cast<float>(k + 1) / static_cast<float>(n);
      p.tier = (k < 3 || frac <= 0.3f || special(p.use)) ? 0u : (frac <= 0.65f ? 1u : 2u);
      for (uint32_t tt = p.tier; tt < 3; ++tt) reach_t[tt] = maxi_(reach_t[tt], static_cast<int>(p.walk));
    }
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i]) continue;
      if ((s.use[i] == kPlot || s.use[i] == kWaterfront) && s.plot[i] >= 0) s.tier[i] = static_cast<uint8_t>(plots[s.plot[i]].tier);
      else if (s.use[i] == kStreet && !s.arterial[i] && s.street[i] != kGate)
        s.tier[i] = along[i] <= reach_t[0] ? 0 : (along[i] <= reach_t[1] ? 1 : 2);
    }
  }

  // ---- 7. levelling ----
  for (size_t i = 0; i < cells; ++i) s.h[i] = s.nat[i];
  {
    float sum = 0;
    int n = 0;
    for (size_t i = 0; i < cells; ++i)
      if (is_node[i] && s.use[i] == kPlaza) {
        sum += s.nat[i];
        ++n;
      }
    const float node_h = n ? snap(sum / static_cast<float>(n)) : 0;
    for (size_t i = 0; i < cells; ++i)
      if (is_node[i] && s.use[i] == kPlaza) s.h[i] = node_h;
    auto grade_of = [&](int c) {
      const uint8_t k = s.street[c];
      return k == kHigh || k == kBridge || k == kGate || k == kQuay ? cfg.high_grade : (k == kAlley ? maxf(cfg.lane_grade, 0.25f) : cfg.lane_grade);
    };
    for (int iter = 0; iter < 600; ++iter) {
      bool changed = false;
      for (size_t i = 0; i < cells; ++i) {
        if (!s.valid[i] || s.use[i] != kStreet) continue;
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(static_cast<int>(i), d);
          if (nb < 0 || !s.road(nb)) continue;
          const float lim = minf(grade_of(static_cast<int>(i)), s.use[nb] == kPlaza ? cfg.high_grade : grade_of(nb)) * kCellM;
          const float diff = s.h[i] - s.h[nb];
          if (absf(diff) <= lim + 1e-4f) continue;
          const float fix = absf(diff) - lim;
          if (s.use[nb] == kPlaza) s.h[i] -= diff > 0 ? fix : -fix;
          else {
            s.h[i] -= (diff > 0 ? fix : -fix) * 0.5f;
            s.h[nb] += (diff > 0 ? fix : -fix) * 0.5f;
          }
          changed = true;
        }
      }
      if (!changed) break;
    }
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && s.use[i] == kStreet) s.h[i] = snap(s.h[i]);
    // Pads: within a quarter metre of their road, as near their own ground as that allows; a
    // waterfront's dry cells only (its wet cells stay water).
    for (Plot &p : plots) {
      float mean = 0;
      int dry = 0;
      for (size_t j = 0; j < p.cells.size(); ++j)
        if (!p.wet[j]) {
          mean += s.nat[p.cells[j]];
          ++dry;
        }
      mean /= static_cast<float>(dry ? dry : 1);
      float fs = mean;
      if (!p.frontage.empty()) {
        fs = 0;
        for (int f : p.frontage) fs += s.h[f];
        fs /= static_cast<float>(p.frontage.size());
      }
      p.pad = snap(clampf(mean, fs - 0.25f, fs + 0.25f));
      float water_h = 1e30f;
      for (size_t j = 0; j < p.cells.size(); ++j) {
        if (p.wet[j]) water_h = minf(water_h, s.nat[p.cells[j]]);
        else s.h[p.cells[j]] = p.pad;
      }
      p.drop = water_h < 1e29f ? maxf(p.pad - water_h, 0) : 0;
    }
    // The rest eases between what was set and its own ground; the site's outer cells keep theirs.
    for (int iter = 0; iter < 40; ++iter) {
      std::vector<float> next = s.h;
      for (size_t i = 0; i < cells; ++i) {
        if (!s.valid[i]) continue;
        const uint8_t u = s.use[i];
        if (u == kStreet || u == kPlot || u == kPlaza || u == kWater || u == kWaterfront) continue;
        if (hex_distance(s.q_of(static_cast<int>(i)), s.r_of(static_cast<int>(i))) >= s.N) continue;
        float sum2 = 0;
        int n2 = 0;
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(static_cast<int>(i), d);
          if (nb < 0 || s.use[nb] == kWater) continue;
          sum2 += s.h[nb];
          ++n2;
        }
        if (n2) next[i] = 0.3f * s.nat[i] + 0.7f * sum2 / static_cast<float>(n2);
      }
      s.h.swap(next);
    }
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i]) s.h[i] = snap(s.h[i]);
  }

  // ---- 7b. a road's edges and the lines: doors, walls, fences, halves, and flow ----
  const uint32_t tier = in.tier;
  auto walkable = [&](int c, uint32_t t) { return s.road(c) && s.tier[c] <= t; };
  auto cart_ok = [&](int c) {
    const uint8_t k = s.street[c];
    return s.use[c] == kPlaza || (s.use[c] == kStreet && (k == kHigh || k == kLane || k == kBridge || k == kGate || k == kQuay));
  };
  const float cart_step = maxf(cfg.high_grade, cfg.lane_grade) * kCellM + 0.051f;
  // flow: the edges a walker crosses (both cells' bits must be set); half: road's half; fence:
  // road edges a fence runs along (none now: fences run on the lattice, off the road).
  std::vector<uint8_t> flow(cells, 0), half(cells, static_cast<uint8_t>(kNoHalf)), fence(cells, 0);
  for (size_t i = 0; i < cells; ++i) {
    if (!s.valid[i] || !walkable(static_cast<int>(i), tier)) continue;
    for (int d = 0; d < 6; ++d) {
      const int nb = s.neighbour(static_cast<int>(i), d);
      if (nb >= 0 && walkable(nb, tier)) flow[i] |= static_cast<uint8_t>(1u << d);
    }
  }
  auto crosses = [&](int c, int d, int nb) { return ((flow[c] >> d) & 1u) && ((flow[nb] >> ((d + 3) % 6)) & 1u); };
  // Doors: a road cell in front of a plot's door is a strict way: its edges toward the plot are
  // kept clear, and it takes no half.
  std::vector<uint8_t> door_edges(cells, 0);
  for (const Plot &p : plots) {
    if (!p.alive || p.tier > tier) continue;
    for (int d : p.doors) {
      if (!walkable(d, tier)) continue;
      for (int e = 0; e < 6; ++e) {
        const int nb = s.neighbour(d, e);
        if (nb >= 0 && (s.use[nb] == kPlot || s.use[nb] == kWaterfront) && s.plot[nb] >= 0 && &plots[s.plot[nb]] == &p)
          door_edges[d] |= static_cast<uint8_t>(1u << e);
      }
    }
  }
  // ---- the diagonal lattice: walls and fences run on it ----
  // Nodes are cells with (q - r) a multiple of 3; a link joins nodes a diagonal step apart (toward a
  // corner), running vertex to vertex through each cell and along the edge between the two cells
  // either side of the step (its gap cells).
  static const int kDiagQ[6] = {1, -1, -2, -1, 1, 2}, kDiagR[6] = {1, 2, 1, -1, -2, -1};
  static const int kGapA[6] = {0, 4, 3, 3, 2, 0}, kGapB[6] = {5, 5, 4, 2, 1, 1};  // the neighbour directions either side of a step
  auto lattice_node = [&](int c) { return ((s.q_of(c) - s.r_of(c)) % 3 + 3) % 3 == 0; };
  auto step = [&](int c, int k) {
    const int q = s.q_of(c) + kDiagQ[k], r = s.r_of(c) + kDiagR[k];
    return s.in(q, r) ? s.idx(q, r) : -1;
  };
  struct Link {
    int a, b, k;
  };
  // The boundary of regions on the lattice: owner[c] labels each node (-1 none); a triangle of three
  // nodes with one owner is that owner's; an edge in exactly one of an owner's triangles is on its
  // boundary, oriented as its triangle runs (counter-clockwise: the region on its left). An edge two
  // owners share appears once.
  auto boundary = [&](const std::vector<int> &owner) {
    std::vector<Link> out_links;
    std::vector<int64_t> keys;
    std::vector<int> count, first;
    std::map<int64_t, size_t> index;  // ordered: the same everywhere
    auto add = [&](int a, int b2, int k, int o) {
      const int lo = a < b2 ? a : b2, hi = a < b2 ? b2 : a;
      const int64_t key = (static_cast<int64_t>(o) << 42) | (static_cast<int64_t>(lo) << 21) | static_cast<int64_t>(hi);
      const auto it = index.find(key);
      if (it != index.end()) {
        ++count[it->second];
        return;
      }
      index[key] = keys.size();
      keys.push_back(key);
      count.push_back(1);
      first.push_back(static_cast<int>(out_links.size()));
      out_links.push_back({a, b2, k});
    };
    for (size_t i = 0; i < cells; ++i) {
      const int n = static_cast<int>(i);
      if (!s.valid[i] || !lattice_node(n)) continue;
      const int n0 = step(n, 0), n1 = step(n, 1);
      if (n0 < 0 || n1 < 0) continue;
      const int o = owner[i];
      if (o >= 0 && owner[n0] == o && owner[n1] == o) {  // (n, n + D0, n + D1)
        add(n, n0, 0, o);
        add(n0, n1, 2, o);
        add(n1, n, 4, o);
      }
      const int n01 = step(n0, 1), o1 = owner[n0];  // the rhombus's other triangle needs n's owner not at all
      if (n01 >= 0 && o1 >= 0 && owner[n01] == o1 && owner[n1] == o1) {  // (n + D0, n + D0 + D1, n + D1)
        add(n0, n01, 1, o1);
        add(n01, n1, 3, o1);
        add(n1, n0, 5, o1);
      }
    }
    std::vector<Link> result;
    std::map<int64_t, int> seen;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (count[i] != 1) continue;
      const Link l = out_links[static_cast<size_t>(first[i])];
      const int lo = l.a < l.b ? l.a : l.b, hi = l.a < l.b ? l.b : l.a;
      const int64_t pair = (static_cast<int64_t>(lo) << 21) | static_cast<int64_t>(hi);
      if (seen.count(pair)) continue;
      seen[pair] = 1;
      result.push_back(l);
    }
    return result;
  };
  // A network from links: its nodes (in order of first use), heights, links, and gates where a road
  // crosses (a road node, or a link whose gap cells are both road).
  auto network = [&](const std::vector<Link> &links, bool gates) {
    channels::line_network net;
    std::vector<int> node_of(cells, -1);
    auto node = [&](int c) {
      if (node_of[c] < 0) {
        node_of[c] = static_cast<int>(net.nodes.size() / 2);
        net.nodes.push_back(s.q_of(c));
        net.nodes.push_back(s.r_of(c));
        net.heights.push_back(s.h[c]);
        if (gates && walkable(c, tier)) net.gate_nodes.push_back(static_cast<uint32_t>(node_of[c]));
      }
      return static_cast<uint32_t>(node_of[c]);
    };
    for (const Link &l : links) {
      const uint32_t a = node(l.a), b2 = node(l.b);
      net.links.push_back(a);
      net.links.push_back(b2);
      const int g1 = s.neighbour(l.a, kGapA[l.k]), g2 = s.neighbour(l.a, kGapB[l.k]);
      if (gates && g1 >= 0 && g2 >= 0 && walkable(g1, tier) && walkable(g2, tier))
        net.gate_links.push_back(static_cast<uint32_t>(net.links.size() / 2 - 1));
    }
    return net;
  };
  // Walls hug what's built: a metaball of the plots, squares, and roads near plots (each adds
  // (1 - (d / R)^2)^2 within R, the wall's reach), at least three cells round every plot, so a wall
  // never comes nearer a building than a cell. The old ring (laid round the site to keep roads to
  // gates while planning) is cleared; the wall's nodes on open land become wall tiles, and its gate
  // cells gate tiles.
  std::vector<uint8_t> wall_inside(cells, 0);
  auto hug = [&](const std::vector<uint8_t> &limit, uint8_t kind) {
    std::vector<int> sources;
    std::vector<uint8_t> near_plot(cells, 0);
    {
      std::vector<int> plot_cells;
      for (const Plot &p : plots)
        if (p.alive)
          for (size_t j = 0; j < p.cells.size(); ++j)
            if (limit[p.cells[j]] && !p.wet[j]) plot_cells.push_back(p.cells[j]);
      const std::vector<int> Dp = distance_from(s, plot_cells, false, true);
      for (size_t i = 0; i < cells; ++i) {
        if (!s.valid[i] || !limit[i]) continue;
        const bool plotted = (s.use[i] == kPlot || s.use[i] == kWaterfront) && s.plot[i] >= 0 && plots[s.plot[i]].alive;
        const bool square = s.use[i] == kPlaza;
        const bool lane = s.use[i] == kStreet && Dp[i] <= 3;
        if (plotted || square || lane) sources.push_back(static_cast<int>(i));
      }
    }
    if (sources.empty()) return std::vector<Link>();
    const std::vector<int> D = distance_from(s, sources, false, true);
    const float reach = clampf(cfg.wall_reach_cells > 0 ? cfg.wall_reach_cells : 6.0f, 2.0f, 20.0f);
    const float Rm = reach * kCellM;
    const int span = static_cast<int>(reach) + 1;
    std::vector<float> field(cells, 0);
    for (int c : sources)
      for (int dq = -span; dq <= span; ++dq)
        for (int dr = -span; dr <= span; ++dr) {
          if (hex_distance(dq, dr) > span) continue;
          const int q = s.q_of(c) + dq, r = s.r_of(c) + dr;
          if (!s.in(q, r)) continue;
          const int t = s.idx(q, r);
          const float dx = s.x(t) - s.x(c), dz = s.z(t) - s.z(c);
          const float u = 1.0f - (dx * dx + dz * dz) / (Rm * Rm);
          if (u > 0) field[t] += u * u;
        }
    // The metaball's nodes.
    std::vector<uint8_t> in_ball(cells, 0);
    for (size_t i = 0; i < cells; ++i)
      if (s.valid[i] && limit[i] && s.use[i] != kWater && (field[i] >= 1.0f || D[i] <= 3) && lattice_node(static_cast<int>(i))) in_ball[i] = 1;
    // Straight sides: a lattice boundary can only run along the three vertex directions, so a side
    // at another angle would zigzag. The metaball is split into lobes (what survives eroding it two
    // lattice steps, in connected pieces), and each lobe becomes its lattice hexagon (the least
    // a, b, a + b ranges round it, grown back by the erosion); the wall runs round their union.
    // Nodes as lattice coordinates: (q, r) = a (1, 1) + b (-1, 2).
    auto lat_a = [&](int c) { return s.q_of(c) + (s.r_of(c) - s.q_of(c)) / 3; };
    auto lat_b = [&](int c) { return (s.r_of(c) - s.q_of(c)) / 3; };
    const int erode = 2;
    std::vector<uint8_t> core(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
      if (!in_ball[i]) continue;
      bool keep = true;
      for (int k = 0; k < 6 && keep; ++k) {
        int c = static_cast<int>(i);
        for (int st = 0; st < erode && keep; ++st) {
          c = step(c, k);
          keep = c >= 0 && in_ball[c];
        }
      }
      core[i] = keep ? 1 : 0;
    }
    std::vector<int> owner(cells, -1), lobe(cells, -1);
    struct Range {
      int a0, a1, b0, b1, s0, s1;
    };
    std::vector<Range> hulls;
    for (size_t i = 0; i < cells; ++i) {
      if (!core[i] || lobe[i] >= 0) continue;
      Range h{1 << 20, -(1 << 20), 1 << 20, -(1 << 20), 1 << 20, -(1 << 20)};
      std::vector<int> stack = {static_cast<int>(i)};
      lobe[i] = static_cast<int>(hulls.size());
      while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        const int a = lat_a(c), bb = lat_b(c);
        h = {mini_(h.a0, a), maxi_(h.a1, a), mini_(h.b0, bb), maxi_(h.b1, bb), mini_(h.s0, a + bb), maxi_(h.s1, a + bb)};
        for (int k = 0; k < 6; ++k) {
          const int nb = step(c, k);
          if (nb >= 0 && core[nb] && lobe[nb] < 0) {
            lobe[nb] = lobe[i];
            stack.push_back(nb);
          }
        }
      }
      hulls.push_back({h.a0 - erode, h.a1 + erode, h.b0 - erode, h.b1 + erode, h.s0 - erode, h.s1 + erode});
    }
    // Anything the metaball held that no lobe covers (a thin arm) gets a small hexagon of its own.
    auto covered = [&](int c) {
      const int a = lat_a(c), bb = lat_b(c);
      for (const Range &h : hulls)
        if (a >= h.a0 && a <= h.a1 && bb >= h.b0 && bb <= h.b1 && a + bb >= h.s0 && a + bb <= h.s1) return true;
      return false;
    };
    for (size_t i = 0; i < cells; ++i)
      if (in_ball[i] && D[i] <= 1 && !covered(static_cast<int>(i))) {
        const int a = lat_a(static_cast<int>(i)), bb = lat_b(static_cast<int>(i));
        hulls.push_back({a - 1, a + 1, bb - 1, bb + 1, a + bb - 1, a + bb + 1});
      }
    // Hexagons that overlap or touch merge into one when the merged hexagon is little bigger than
    // the two together, so the wall has few notches; others stay apart (a wall bends in round them).
    auto area = [](const Range &h) {
      int n = 0;
      for (int a = h.a0; a <= h.a1; ++a)
        for (int bb = h.b0; bb <= h.b1; ++bb)
          if (a + bb >= h.s0 && a + bb <= h.s1) ++n;
      return n;
    };
    for (bool merged = true; merged;) {
      merged = false;
      for (size_t i = 0; i < hulls.size() && !merged; ++i)
        for (size_t j = i + 1; j < hulls.size() && !merged; ++j) {
          const Range &x = hulls[i], &y = hulls[j];
          const Range both{maxi_(x.a0, y.a0) - 1, mini_(x.a1, y.a1) + 1, maxi_(x.b0, y.b0) - 1, mini_(x.b1, y.b1) + 1, maxi_(x.s0, y.s0) - 1,
                           mini_(x.s1, y.s1) + 1};
          const int touch = area(both);
          if (touch == 0) continue;
          const Range u{mini_(x.a0, y.a0), maxi_(x.a1, y.a1), mini_(x.b0, y.b0), maxi_(x.b1, y.b1), mini_(x.s0, y.s0), maxi_(x.s1, y.s1)};
          const Range inter{maxi_(x.a0, y.a0), mini_(x.a1, y.a1), maxi_(x.b0, y.b0), mini_(x.b1, y.b1), maxi_(x.s0, y.s0), mini_(x.s1, y.s1)};
          const int together = area(x) + area(y) - area(inter);
          if (static_cast<float>(area(u)) > 1.35f * static_cast<float>(together)) continue;
          hulls[i] = u;
          hulls.erase(hulls.begin() + static_cast<long>(j));
          merged = true;
        }
    }
    // No 60 degree corners: a merged range's acute corners are cut (no node of either part lay there).
    for (Range &h : hulls) {
      h.s0 = maxi_(h.s0, h.a0 + h.b0 + 1);
      h.s1 = mini_(h.s1, h.a1 + h.b1 - 1);
    }
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || !lattice_node(static_cast<int>(i)) || !covered(static_cast<int>(i))) continue;
      owner[i] = 0;
    }
    // What's inside (for the shape): every cell within the union's nodes' reach.
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || s.use[i] == kWater) continue;
      bool in = false;
      for (int k = 0; k < 6 && !in; ++k) in = owner[i] == 0 || (s.neighbour(static_cast<int>(i), k) >= 0 && owner[s.neighbour(static_cast<int>(i), k)] == 0);
      if (in) wall_inside[i] = kind;
    }
    return boundary(owner);
  };
  if (walled || keep_ward) {
    for (size_t i = 0; i < cells; ++i)
      if ((s.use[i] == kWall || s.use[i] == kKeepWall) && wall_ring[i]) s.use[i] = kFree;
    std::vector<std::pair<uint32_t, std::vector<Link>>> walls_laid;
    if (walled) {
      std::vector<uint8_t> limit(cells, 0);
      for (size_t i = 0; i < cells; ++i) limit[i] = s.valid[i] ? 1 : 0;
      walls_laid.push_back({channels::plan_line::kKindWall, hug(limit, 1)});
    }
    if (keep_ward) walls_laid.push_back({channels::plan_line::kKindKeepWall, hug(in_ward, 2)});
    // A wall hugging the plots may stand past the site's rim, where the entrance roads end: each
    // runs on outward, away from the node, until it is two cells past the wall, so it passes
    // through a gate.
    if (walled)
      for (int &e : entrances) {
        if (!wall_inside[e]) continue;
        const float ox = s.x(e) - s.x(node), oz = s.z(e) - s.z(node);
        int d_out = 0;
        float best = -1e30f;
        for (int d = 0; d < 6; ++d) {
          static const float kX[6] = {1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f}, kZ[6] = {0.0f, -0.8660254f, -0.8660254f, 0.0f, 0.8660254f, 0.8660254f};
          const float dt = kX[d] * ox + kZ[d] * oz;
          if (dt > best) {
            best = dt;
            d_out = d;
          }
        }
        int at = e, past = 0;
        const uint8_t cls = s.street[e] == kGate ? static_cast<uint8_t>(kHigh) : s.street[e];
        for (int guard = 0; guard < 40 && past < 2; ++guard) {
          const int nb = s.neighbour(at, d_out);
          if (nb < 0 || !s.valid[nb] || s.use[nb] == kPlot || s.use[nb] == kWaterfront || s.use[nb] == kWater) break;
          if (s.use[nb] != kStreet && s.use[nb] != kPlaza) {
            s.use[nb] = kStreet;
            s.street[nb] = cls;
            s.arterial[nb] = 1;
            s.tier[nb] = 0;
          }
          if (s.street[e] == kGate) s.street[e] = cls;
          at = nb;
          if (!wall_inside[at]) ++past;
        }
        s.street[at] = kGate;
        e = at;
      }
    for (const auto &[kind, links] : walls_laid) {
      if (links.empty()) continue;
      for (const Link &l : links)
        for (int c : {l.a, l.b}) {
          if (s.use[c] == kFree || s.use[c] == kGreen) s.use[c] = kind == channels::plan_line::kKindWall ? kWall : kKeepWall;
          else if (walkable(c, tier)) s.street[c] = kGate;
        }
      channels::plan_line line;
      line.kind = kind;
      line.network = network(links, true);
      line.network.name = kind == channels::plan_line::kKindWall ? "wall" : "keep_wall";
      out.lines.push_back(line);
    }
  }
  // The flow again: roads may have run on through a wall.
  for (size_t i = 0; i < cells; ++i) {
    flow[i] = 0;
    if (!s.valid[i] || !walkable(static_cast<int>(i), tier)) continue;
    for (int d = 0; d < 6; ++d) {
      const int nb = s.neighbour(static_cast<int>(i), d);
      if (nb >= 0 && walkable(nb, tier)) flow[i] |= static_cast<uint8_t>(1u << d);
    }
  }
  // Fences: a share of household plots get a yard, the open land within fence_cells of the plot
  // (each cell the nearest plot's), and a fence round plot and yard on the lattice, as the yard's
  // hexagon. Neighbouring
  // yards share the fence between them, so fences meet in T junctions. A fence never runs through a
  // lot or a road, and never across one: where the boundary would, the fence stops (a yard's front
  // stays open to its street).
  if (cfg.fence_share > 0 && cfg.fence_cells > 0) {
    Rng fr(seed, 9u);
    std::vector<int> owner(cells, -1), dist(cells, 1 << 29), queue;
    for (size_t pi = 0; pi < plots.size(); ++pi) {
      const Plot &p = plots[pi];
      const float roll = fr.uniform();
      if (!p.alive || p.tier > tier || p.cls > 2 || roll >= cfg.fence_share) continue;
      for (int c : p.cells) {
        owner[c] = static_cast<int>(pi);
        dist[c] = 0;
        queue.push_back(c);
      }
    }
    for (size_t h = 0; h < queue.size(); ++h) {
      const int c = queue[h];
      if (dist[c] >= static_cast<int>(cfg.fence_cells)) continue;
      for (int d = 0; d < 6; ++d) {
        const int nb = s.neighbour(c, d);
        if (nb < 0 || dist[nb] <= dist[c] + 1 || (s.use[nb] != kFree && s.use[nb] != kGreen) || !s.inside(nb)) continue;
        dist[nb] = dist[c] + 1;
        owner[nb] = owner[c];
        queue.push_back(nb);
      }
    }
    // Each yard as its lattice hexagon (the least a, b, a + b ranges round its nodes, its acute
    // corners cut), so its fence runs straight; where yards overlap, the earlier plot's wins, so a
    // shared fence is one hexagon's side. Nodes on a road or another plot's lot belong to none.
    auto la = [&](int c) { return s.q_of(c) + (s.r_of(c) - s.q_of(c)) / 3; };
    auto lb = [&](int c) { return (s.r_of(c) - s.q_of(c)) / 3; };
    struct Hex {
      int p, a0, a1, b0, b1, s0, s1;
    };
    std::vector<Hex> hexes;
    for (size_t pi = 0; pi < plots.size(); ++pi) {
      Hex h{static_cast<int>(pi), 1 << 20, -(1 << 20), 1 << 20, -(1 << 20), 1 << 20, -(1 << 20)};
      bool any = false;
      for (size_t i = 0; i < cells; ++i) {
        if (owner[i] != static_cast<int>(pi) || !lattice_node(static_cast<int>(i))) continue;
        const int a = la(static_cast<int>(i)), bb = lb(static_cast<int>(i));
        h = {h.p, mini_(h.a0, a), maxi_(h.a1, a), mini_(h.b0, bb), maxi_(h.b1, bb), mini_(h.s0, a + bb), maxi_(h.s1, a + bb)};
        any = true;
      }
      if (!any) continue;
      h.s0 = maxi_(h.s0, h.a0 + h.b0 + 1);
      h.s1 = mini_(h.s1, h.a1 + h.b1 - 1);
      hexes.push_back(h);
    }
    std::vector<int> node_owner(cells, -1);
    for (size_t i = 0; i < cells; ++i) {
      const int c = static_cast<int>(i);
      if (!s.valid[i] || !lattice_node(c) || s.road(c) || s.use[i] == kWater || s.use[i] == kWall || s.use[i] == kKeepWall) continue;
      const int a = la(c), bb = lb(c);
      for (const Hex &h : hexes) {
        if (a < h.a0 || a > h.a1 || bb < h.b0 || bb > h.b1 || a + bb < h.s0 || a + bb > h.s1) continue;
        const bool lot = (s.use[i] == kPlot || s.use[i] == kWaterfront) && s.plot[i] >= 0;
        if (lot && s.plot[i] != h.p) break;  // another plot's lot: no one's
        node_owner[i] = h.p;
        break;
      }
    }
    std::vector<Link> links;
    auto clear_cell = [&](int c) { return c >= 0 && (s.use[c] == kFree || s.use[c] == kGreen); };
    for (const Link &l : boundary(node_owner)) {
      const int g1 = s.neighbour(l.a, kGapA[l.k]), g2 = s.neighbour(l.a, kGapB[l.k]);
      if (!clear_cell(l.a) || !clear_cell(l.b)) continue;
      if (g1 < 0 || g2 < 0 || s.road(g1) || s.road(g2) || s.use[g1] == kWall || s.use[g2] == kWall) continue;
      links.push_back(l);
    }
    // One line per connected piece.
    std::vector<int> comp(links.size(), -1);
    int comps = 0;
    for (size_t i = 0; i < links.size(); ++i) {
      if (comp[i] >= 0) continue;
      std::vector<size_t> stack = {i};
      comp[i] = comps;
      while (!stack.empty()) {
        const size_t x = stack.back();
        stack.pop_back();
        for (size_t j = 0; j < links.size(); ++j)
          if (comp[j] < 0 && (links[j].a == links[x].a || links[j].a == links[x].b || links[j].b == links[x].a || links[j].b == links[x].b)) {
            comp[j] = comps;
            stack.push_back(j);
          }
      }
      ++comps;
    }
    for (int ci = 0; ci < comps; ++ci) {
      std::vector<Link> part;
      for (size_t i = 0; i < links.size(); ++i)
        if (comp[i] == ci) part.push_back(links[i]);
      channels::plan_line line;
      line.kind = channels::plan_line::kKindFence;
      line.network = network(part, false);
      line.network.name = "fence";
      out.lines.push_back(line);
    }
  }
  // Halves: by the class's share, an edge piece takes half a cell, never a fenced edge. Off the
  // square it takes only edges the flow doesn't use; on a square or market it may take ways
  // through too (rows facing one way), if every road cell reached before is still reached, on
  // foot and by cart, from every entrance.
  {
    Rng hr(seed, 8u);
    const int rows = static_cast<int>(hr.next() % 6u);
    auto share_of = [&](int c) {
      if (s.use[c] == kPlaza) return in_ward[c] || village ? cfg.halves_square : cfg.halves_market;
      switch (s.street[c]) {
        case kHigh: return cfg.halves_high;
        case kLane: return cfg.halves_lane;
        case kQuay: return cfg.halves_quay;
        default: return 0.0f;
      }
    };
    std::vector<int> starts;
    for (int e : entrances)
      if (walkable(e, tier)) starts.push_back(e);
    // What each entrance reaches, on foot (bit 2e) and by cart (bit 2e + 1).
    auto reached = [&](std::vector<uint16_t> &seen) {
      seen.assign(cells, 0);
      std::vector<int> q;
      for (size_t e = 0; e < starts.size() && e < 8; ++e)
        for (int cart = 0; cart < 2; ++cart) {
          const uint16_t bit = static_cast<uint16_t>(1u << (2 * e + static_cast<size_t>(cart)));
          if (cart && !cart_ok(starts[e])) continue;
          q.clear();
          q.push_back(starts[e]);
          seen[starts[e]] |= bit;
          for (size_t h = 0; h < q.size(); ++h)
            for (int d = 0; d < 6; ++d) {
              const int nb = s.neighbour(q[h], d);
              if (nb < 0 || (seen[nb] & bit) || !walkable(nb, tier) || !crosses(q[h], d, nb) || (cart && !cart_ok(nb))) continue;
              const float step = absf(s.h[nb] - s.h[q[h]]);
              if (step > kStep + 1e-3f || (cart && step > cart_step)) continue;
              seen[nb] |= bit;
              q.push_back(nb);
            }
        }
    };
    std::vector<uint16_t> before, after;
    reached(before);
    auto still = [&]() {
      reached(after);
      for (size_t i = 0; i < cells; ++i)
        if ((before[i] & after[i]) != before[i]) return false;
      return true;
    };
    for (size_t i = 0; i < cells; ++i) {
      const int c = static_cast<int>(i);
      if (!s.valid[i] || !walkable(c, tier) || s.street[i] == kGate || s.street[i] == kBridge || door_edges[i]) continue;
      const float share = share_of(c);
      if (share <= 0) continue;
      const float roll = hr.uniform();
      if (roll >= share) continue;
      const bool square = s.use[i] == kPlaza;
      // Options: halves off the flow first (scored by the buildings they back onto), then on a
      // square the rows' two ways.
      int best = -1, best_score = -1;
      for (int o = 0; o < 6; ++o) {
        const int e1 = o, e2 = (o + 1) % 6;
        if (((flow[i] >> e1) & 1u) || ((flow[i] >> e2) & 1u) || ((fence[i] >> e1) & 1u) || ((fence[i] >> e2) & 1u)) continue;
        int score = 0;
        for (int e : {e1, e2}) {
          const int nb = s.neighbour(c, e);
          if (nb >= 0 && (s.use[nb] == kPlot || s.use[nb] == kWaterfront || s.use[nb] == kWater)) score += 2;
          else if (nb >= 0 && (s.use[nb] == kWall || s.use[nb] == kKeepWall)) score += 1;
        }
        if (score > best_score) {
          best_score = score;
          best = o;
        }
      }
      if (best >= 0) {
        half[i] = static_cast<uint8_t>(best);
        continue;
      }
      if (!square) continue;
      for (int o : {rows, (rows + 3) % 6}) {
        const int e1 = o, e2 = (o + 1) % 6;
        if (((fence[i] >> e1) & 1u) || ((fence[i] >> e2) & 1u)) continue;
        const uint8_t keep_flow = flow[i];
        uint8_t taken = static_cast<uint8_t>((1u << e1) | (1u << e2));
        if ((flow[i] & ~taken) == 0) continue;  // it must keep a way through
        flow[i] = static_cast<uint8_t>(flow[i] & ~taken);
        if (still()) {
          half[i] = static_cast<uint8_t>(o);
          break;
        }
        flow[i] = keep_flow;
      }
    }
  }

  // ---- 8. navigation, and repairs ----
  auto reach = [&](int from, uint32_t tier, bool cart) {
    std::vector<float> d(cells, 1e30f);
    Heap heap;
    if (!walkable(from, tier)) return d;
    d[from] = 0;
    heap.push(0, from);
    while (!heap.empty()) {
      const Heap::E e = heap.pop();
      if (e.c > d[e.id]) continue;
      for (int k = 0; k < 6; ++k) {
        const int nb = s.neighbour(e.id, k);
        if (nb < 0 || !walkable(nb, tier) || !crosses(e.id, k, nb)) continue;
        const float step = absf(s.h[nb] - s.h[e.id]);
        if (step > kStep + 1e-3f) continue;
        if (cart && (!cart_ok(nb) || !cart_ok(e.id) || step > cart_step)) continue;
        const float nc = e.c + kCellM;
        if (nc < d[nb]) {
          d[nb] = nc;
          heap.push(nc, nb);
        }
      }
    }
    return d;
  };
  {
    std::vector<std::vector<float>> walk;
    std::vector<float> cart(cells, 1e30f);
    for (int e : entrances) {
      walk.push_back(reach(e, tier, false));
      const std::vector<float> c = reach(e, tier, true);
      for (size_t i = 0; i < cells; ++i) cart[i] = minf(cart[i], c[i]);
    }
    for (Plot &p : plots) {
      if (p.tier > tier) continue;
      bool all = !walk.empty();
      float nearest = 1e30f;
      for (const auto &w : walk) {
        float best = 1e30f;
        for (int f : p.frontage) best = minf(best, w[f]);
        all = all && best < 1e29f;
        nearest = minf(nearest, best);
      }
      bool carted = false;
      for (int f : p.frontage) carted = carted || cart[f] < 1e29f;
      p.reached = all;
      p.cart = carted;
      p.walk = nearest < 1e29f ? nearest : 0;
      if (!all && p.use != plan_plot::kUseKeep) {
        p.alive = false;
        for (size_t j = 0; j < p.cells.size(); ++j) {
          const int c = p.cells[j];
          s.use[c] = p.wet[j] ? kWater : kFree;
          s.plot[c] = -1;
          s.tier[c] = 0;
        }
        ++out.repairs;
      }
    }
  }
  {
    std::vector<int> poly_of(cells, -1);
    static const float cx[6] = {0.8660254f, 0.0f, -0.8660254f, -0.8660254f, 0.0f, 0.8660254f};
    static const float cz[6] = {0.5f, 1.0f, 0.5f, -0.5f, -1.0f, -0.5f};
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i] || !walkable(static_cast<int>(i), tier)) continue;
      poly_of[i] = static_cast<int>(out.nav_flags.size());
      const uint32_t base = static_cast<uint32_t>(out.nav_vertices.size());
      const float x = s.x(static_cast<int>(i)), z = s.z(static_cast<int>(i));
      if (half[i] < kNoHalf) {
        // The half its edge piece leaves: from the middle of edge o + 5, round the corners of
        // edges o + 4 and o + 3, to the middle of edge o + 2.
        const int o = half[i];
        auto mid = [&](int e) {
          const int k0 = (6 - e) % 6, k1 = (11 - e) % 6;
          return v3(x + (cx[k0] + cx[k1]) * 0.5f, s.h[i], z + (cz[k0] + cz[k1]) * 0.5f);
        };
        out.nav_vertices.push_back(mid((o + 5) % 6));
        for (int k : {(7 - o) % 6, (8 - o) % 6, (9 - o) % 6}) out.nav_vertices.push_back(v3(x + cx[k], s.h[i], z + cz[k]));
        out.nav_vertices.push_back(mid((o + 2) % 6));
        for (uint32_t k = 0; k < 5; ++k) out.nav_polys.push_back(base + k);
        out.nav_counts.push_back(5);
      } else {
        for (int k = 0; k < 6; ++k) {
          out.nav_vertices.push_back(v3(x + cx[k], s.h[i], z + cz[k]));
          out.nav_polys.push_back(base + static_cast<uint32_t>(k));
        }
        out.nav_counts.push_back(6);
      }
      out.nav_flags.push_back(1u | (cart_ok(static_cast<int>(i)) ? 2u : 0u));
      out.nav_area.push_back(s.use[i] == kPlaza ? 1u : (s.street[i] == kBridge ? 3u : (s.street[i] == kGate ? 4u : 0u)));
    }
    for (size_t i = 0; i < cells; ++i) {
      if (poly_of[i] < 0) continue;
      for (int d = 0; d < 3; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        if (nb < 0 || poly_of[nb] < 0 || !crosses(static_cast<int>(i), d, nb)) continue;
        const float step = absf(s.h[i] - s.h[nb]);
        if (step > kStep + 1e-3f) continue;
        out.nav_links.push_back(static_cast<uint32_t>(poly_of[i]));
        out.nav_links.push_back(static_cast<uint32_t>(poly_of[nb]));
        out.highest_step_m = maxf(out.highest_step_m, step);
      }
      for (int d = 0; d < 3; ++d) {
        const int nb = s.neighbour(static_cast<int>(i), d);
        if (nb >= 0 && s.use[nb] == kStreet && s.use[i] == kStreet) out.steepest_street = maxf(out.steepest_street, absf(s.h[i] - s.h[nb]) / kCellM);
      }
    }
  }

  // ---- 9. the plan ----
  auto open_road = [&](int c) { return c >= 0 && s.road(c) && (s.tier[c] <= tier || s.arterial[c]); };
  // The shape: what it built (at the tier), its open body, open land within its margin, the water
  // beside it, and (walled) everything inside the wall.
  std::vector<uint8_t> shape(cells, 0);
  std::vector<int> built;
  for (size_t i = 0; i < cells; ++i) {
    if (!s.valid[i]) continue;
    const uint8_t u = s.use[i];
    const bool b = ((u == kPlot || u == kWaterfront) && s.plot[i] >= 0 && plots[s.plot[i]].alive && plots[s.plot[i]].tier <= tier) ||
                   open_road(static_cast<int>(i)) || u == kWall || u == kKeepWall;
    if (b) built.push_back(static_cast<int>(i));
  }
  const std::vector<int> Db = distance_from(s, built, false, true);
  const int margin = static_cast<int>(cfg.margin_cells);
  for (size_t i = 0; i < cells; ++i) {
    if (!s.valid[i]) continue;
    const uint8_t u = s.use[i];
    const bool is_built = Db[i] == 0;
    const bool body_cell = is_node[i] && (u == kGreen || u == kWater);
    const bool near = Db[i] <= margin && u != kWater;
    const bool water_by = u == kWater && Db[i] <= 2;
    const bool walled_in = wall_inside[i] && u != kWater;
    shape[i] = is_built || body_cell || near || water_by || walled_in ? 1 : 0;
  }
  // Plot IDs: buildings and waterfronts first, then road segments, then water.
  std::vector<int> out_id(plots.size(), -1);
  struct OutPlot {
    plan_plot p;
    std::vector<int> cells;
  };
  std::vector<OutPlot> outs;
  for (size_t pi = 0; pi < plots.size(); ++pi) {
    const Plot &p = plots[pi];
    if (!p.alive || p.tier > tier) continue;
    OutPlot o;
    o.p.id = static_cast<uint32_t>(outs.size());
    o.p.kind = p.waterfront ? plan_plot::kKindWaterfront : plan_plot::kKindBuilding;
    o.p.tier = p.tier;
    o.p.size_class = p.cls;
    o.p.shape = p.shape;
    o.p.lot = static_cast<uint32_t>(p.lot);
    o.p.anchor_q = p.aq;
    o.p.anchor_r = p.ar;
    o.p.turn = static_cast<uint32_t>(p.turn);
    o.p.pad = p.pad;
    o.p.position = v3(cell_x(p.aq, p.ar), p.pad, cell_z(p.ar));
    o.p.yaw_deg = 60.0f * static_cast<float>(p.turn);
    o.p.use = p.use;
    o.p.road = 0;
    o.p.wealth = p.wealth;
    o.p.frontage = static_cast<uint32_t>(p.frontage.size());
    o.p.walk_m = p.walk;
    o.p.reached = p.reached;
    o.p.cart = p.cart;
    o.p.drop = p.drop;
    o.cells = p.cells;
    out_id[pi] = static_cast<int>(o.p.id);
    out.longest_walk_m = maxf(out.longest_walk_m, p.walk);
    if (!p.reached) ++out.plots_unreached;
    const bool trade = p.use == plan_plot::kUseStore || p.use == plan_plot::kUseWorkshop || p.use == plan_plot::kUseInn ||
                       p.use == plan_plot::kUseCivic || p.use == plan_plot::kUseHarbour || p.use == plan_plot::kUseCastle;
    if (trade && !p.cart) ++out.cart_unreached;
    outs.push_back(o);
  }
  std::vector<int> cell_plot(cells, -1);
  for (size_t pi = 0; pi < plots.size(); ++pi)
    if (out_id[pi] >= 0)
      for (int c : plots[pi].cells) cell_plot[c] = out_id[pi];
  auto road_class = [&](int c) -> uint32_t {
    if (s.use[c] == kPlaza) return village || in_ward[c] ? plan_plot::kRoadSquare : plan_plot::kRoadMarket;
    switch (s.street[c]) {
      case kHigh: return plan_plot::kRoadHigh;
      case kLane: return plan_plot::kRoadLane;
      case kAlley: return plan_plot::kRoadAlley;
      case kBridge: return plan_plot::kRoadBridge;
      case kGate: return plan_plot::kRoadGate;
      case kQuay: return plan_plot::kRoadQuay;
      default: return plan_plot::kRoadLane;
    }
  };
  // Road segments: each square as one; the rest as runs of one class, at most 16 cells each.
  {
    std::vector<uint8_t> taken(cells, 0);
    std::vector<std::vector<int>> segs;
    // Each square (the node's, a keep's bailey) as one segment: its connected plaza cells.
    for (size_t i = 0; i < cells; ++i) {
      if (!open_road(static_cast<int>(i)) || s.use[i] != kPlaza || taken[i]) continue;
      std::vector<int> square = {static_cast<int>(i)};
      taken[i] = 1;
      for (size_t head = 0; head < square.size(); ++head)
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(square[head], d);
          if (nb < 0 || taken[nb] || !open_road(nb) || s.use[nb] != kPlaza) continue;
          taken[nb] = 1;
          square.push_back(nb);
        }
      segs.push_back(square);
    }
    for (size_t i = 0; i < cells; ++i) {
      if (!open_road(static_cast<int>(i)) || taken[i]) continue;
      std::vector<int> seg = {static_cast<int>(i)};
      taken[i] = 1;
      const uint32_t cls = road_class(static_cast<int>(i));
      for (size_t head = 0; head < seg.size() && seg.size() < 16; ++head)
        for (int d = 0; d < 6 && seg.size() < 16; ++d) {
          const int nb = s.neighbour(seg[head], d);
          if (!open_road(nb) || taken[nb] || road_class(nb) != cls) continue;
          taken[nb] = 1;
          seg.push_back(nb);
        }
      segs.push_back(seg);
    }
    for (const auto &sg : segs) {
      OutPlot o;
      o.p.id = static_cast<uint32_t>(outs.size());
      o.p.kind = plan_plot::kKindRoad;
      o.p.use = plan_plot::kUseWay;
      o.p.road = road_class(sg[0]);
      o.p.anchor_q = s.q_of(sg[0]);
      o.p.anchor_r = s.r_of(sg[0]);
      o.p.pad = s.h[sg[0]];
      o.p.position = v3(s.x(sg[0]), s.h[sg[0]], s.z(sg[0]));
      o.p.reached = true;
      o.p.cart = cart_ok(sg[0]);
      int t = 0;
      for (int c : sg) t = maxi_(t, s.tier[c]);
      o.p.tier = static_cast<uint32_t>(t);
      o.cells = sg;
      for (int c : sg) cell_plot[c] = static_cast<int>(o.p.id);
      outs.push_back(o);
    }
  }
  // Water bodies within the shape, one plot each.
  {
    std::vector<uint8_t> seen(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
      if (s.use[i] != kWater || !shape[i] || seen[i]) continue;
      std::vector<int> body_cells = {static_cast<int>(i)};
      seen[i] = 1;
      for (size_t head = 0; head < body_cells.size(); ++head)
        for (int d = 0; d < 6; ++d) {
          const int nb = s.neighbour(body_cells[head], d);
          if (nb >= 0 && s.use[nb] == kWater && shape[nb] && !seen[nb]) {
            seen[nb] = 1;
            body_cells.push_back(nb);
          }
        }
      OutPlot o;
      o.p.id = static_cast<uint32_t>(outs.size());
      o.p.kind = plan_plot::kKindWater;
      o.p.use = plan_plot::kUseWater;
      o.p.anchor_q = s.q_of(body_cells[0]);
      o.p.anchor_r = s.r_of(body_cells[0]);
      o.p.pad = s.h[body_cells[0]];
      o.p.position = v3(s.x(body_cells[0]), s.h[body_cells[0]], s.z(body_cells[0]));
      o.p.reached = true;
      o.cells = body_cells;
      for (int c : body_cells) cell_plot[c] = static_cast<int>(o.p.id);
      outs.push_back(o);
    }
  }
  // Each plot's cells, heights, edges (road's mask), and roles.
  auto edge_kind = [&](int c, int nb) -> uint32_t {
    if (nb < 0 || !shape[nb]) return kEOpen;
    const uint8_t u = s.use[nb];
    if (u == kStreet || u == kPlaza) {
      if (!open_road(nb)) return kEOpen;
      return s.street[nb] == kGate ? kEGate : kERoad;
    }
    if (u == kWater) return kEWater;
    if (u == kWall || u == kKeepWall) return kEWall;
    if (u == kWaterfront) return cell_plot[nb] >= 0 ? kEWaterfront : kEOpen;
    if (u == kPlot) {
      const int pid = s.plot[nb];
      if (pid < 0 || out_id[pid] < 0) return kEOpen;
      for (int f : plots[pid].frontage)
        if (f == c) return kEFront;
      return kESide;
    }
    return kEOpen;
  };
  for (OutPlot &o : outs) {
    const bool road = o.p.kind == plan_plot::kKindRoad;
    for (int c : o.cells) {
      o.p.cells.push_back(s.q_of(c));
      o.p.cells.push_back(s.r_of(c));
      o.p.heights.push_back(s.h[c]);
      uint32_t mask = 0;
      for (int d = 0; d < 6; ++d) mask |= edge_kind(c, s.neighbour(c, d)) << (3 * d);
      o.p.edges.push_back(mask);
      o.p.roles.push_back(road && s.street[c] == kBridge ? 1u : 0u);
      if (road) {
        o.p.halves.push_back(half[c]);
        o.p.flows.push_back(flow[c]);
        o.p.fences.push_back(fence[c]);
        o.p.doors.push_back(door_edges[c]);
      }
    }
    out.plots.push_back(o.p);
  }
  // The shape's cells.
  for (size_t i = 0; i < cells; ++i) {
    if (!shape[i]) continue;
    plan_cell c;
    c.q = s.q_of(static_cast<int>(i));
    c.r = s.r_of(static_cast<int>(i));
    c.height = s.h[i];
    c.tier = s.tier[i];
    c.plot = cell_plot[i];
    c.road = 0;
    const uint8_t u = s.use[i];
    if (open_road(static_cast<int>(i))) {
      c.use = s.street[i] == kGate ? plan_cell::kUseGate : plan_cell::kUseRoad;
      c.road = road_class(static_cast<int>(i));
    } else if (u == kWater) {
      c.use = plan_cell::kUseWater;
    } else if (u == kWall) {
      c.use = plan_cell::kUseWall;
    } else if (u == kKeepWall) {
      c.use = plan_cell::kUseKeepWall;
    } else if (u == kPlot && cell_plot[i] >= 0) {
      c.use = plan_cell::kUseBuilding;
    } else if (u == kWaterfront && cell_plot[i] >= 0) {
      c.use = plan_cell::kUseWaterfront;
    } else {
      c.use = plan_cell::kUseOpen;
      c.plot = -1;
      ++out.open_cells;
    }
    out.cells.push_back(c);
  }
  // Retaining: built cells beside a neighbour more than 0.3 m away.
  for (size_t i = 0; i < cells; ++i) {
    if (!shape[i]) continue;
    for (int d = 0; d < 3; ++d) {
      const int nb = s.neighbour(static_cast<int>(i), d);
      if (nb < 0 || !shape[nb] || s.use[i] == kWater || s.use[nb] == kWater) continue;
      const bool b1 = s.use[i] != kFree && s.use[i] != kGreen, b2 = s.use[nb] != kFree && s.use[nb] != kGreen;
      if ((b1 || b2) && absf(s.h[i] - s.h[nb]) > kRetain) out.retaining_m += 1.0f;
    }
  }
  // The nature ground: the shape and an apron round it. Open land is available (wetter by water),
  // water is water, and what's built is a path no plant takes. Grouped by height and moisture.
  {
    std::vector<int> wet;
    for (size_t i = 0; i < cells; ++i)
      if (s.use[i] == kWater) wet.push_back(static_cast<int>(i));
    const std::vector<int> Dw = distance_from(s, wet, false, true);
    struct Group {
      uint32_t kind;
      int h, m;
      std::vector<int32_t> cells;
    };
    std::vector<Group> groups;
    for (size_t i = 0; i < cells; ++i) {
      if (!s.valid[i]) continue;
      const bool in_shape = shape[i] != 0;
      if (!in_shape && Db[i] > margin + kApron) continue;
      const uint8_t u = s.use[i];
      uint32_t kind = 0;  // 0 open, 1 water, 2 built
      if (u == kWater) kind = 1;
      else if (Db[i] == 0) kind = 2;
      const float moist = kind == 1 ? 1.0f : clampf(0.85f - 0.08f * static_cast<float>(Dw[i] < 20 ? Dw[i] : 20), 0.3f, 0.85f);
      const int hq = round_to_int(s.h[i] / 0.05f), mq = round_to_int(moist / 0.05f);
      Group *g = nullptr;
      for (auto &gg : groups)
        if (gg.kind == kind && gg.h == hq && gg.m == mq) g = &gg;
      if (!g) {
        groups.push_back({kind, hq, mq, {}});
        g = &groups.back();
      }
      g->cells.push_back(s.q_of(static_cast<int>(i)));
      g->cells.push_back(s.r_of(static_cast<int>(i)));
    }
    for (const Group &g : groups) {
      plot_region r;
      r.shape = plot_region::kShapeCells;
      r.cells = g.cells;
      r.priority = 10;
      r.cut = false;
      r.set = 1 | 2 | 8 | 16;  // ground, available, height, moisture
      r.ground = g.kind == 1 ? plot_region::kGroundWater : (g.kind == 2 ? plot_region::kGroundPath : plot_region::kGroundSoil);
      r.available = g.kind == 0;
      r.keep_clear_m = 0;
      r.height = static_cast<float>(g.h) * 0.05f;
      r.moisture = static_cast<float>(g.m) * 0.05f;
      out.nature.regions.push_back(r);
    }
  }
  for (int e : entrances) {
    out.entrances.push_back(s.q_of(e));
    out.entrances.push_back(s.r_of(e));
  }
  out.radius = static_cast<uint32_t>(s.R);
  uint32_t alive = 0;
  for (const Plot &p : plots)
    if (p.alive && !p.waterfront && !special(p.use)) ++alive;
  if (alive < lo) out.note = "only " + std::to_string(alive) + " plots fit (the config asks for at least " + std::to_string(lo) + ")";
  return true;
}

}  // namespace plan_settle

FOUNDRY_SYSTEM(plan_settle, channels::plan_request, channels::plan_plan)
