// What the runtime knows about a pipeline (design/spine.md): a type that can
// build what instances share from their build parameters, and pose one
// instance each frame from its update parameters and clocks. The generated
// code in src/library/<pipeline>.cpp implements it on top of the systems,
// passing channels as plain structures (no packers on this path).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "boundary.h"

namespace runtime {

// The draw stage's output, in the form the renderer takes.
struct DrawMesh {
  std::vector<foundry::vec3> positions, normals;
  std::vector<uint32_t> colors;  // packed RGBA, red in the lowest byte
  std::vector<uint32_t> indices;
  std::vector<uint32_t> bone_a, bone_b;  // empty for a rigid mesh
  std::vector<float> bone_w;
  std::vector<float> sway;  // a field tile's: how far each vertex moves in a 10 m/s wind (empty: none)
  void clear() { *this = DrawMesh{}; }
};

// One collision shape (the physics_v1_shape channel's convention, design/spine.md Layer 1), in the
// body's space: metres, +Y up.
struct CollisionShape {
  enum Type : uint32_t { kSphere = 0, kBox = 1, kCapsule = 2, kTaperedCapsule = 3, kCylinder = 4, kConvexHull = 5 };
  uint32_t type = kSphere;
  float position[3] = {0, 0, 0};
  float rotation[3] = {0, 0, 0};  // axis-angle
  float radius = 0, radius_top = 0, half_height = 0;
  float half_extents[3] = {0, 0, 0};
  std::vector<float> points;  // a convex hull's points, xyz
};

// One collision body a build outputs: a purpose and a compound of shapes.
struct CollisionBody {
  enum Purpose : uint32_t { kSolid = 0, kSoft = 1, kQuery = 2, kSensor = 3 };
  uint32_t purpose = kSolid;
  std::string name;  // a sensor's purpose
  std::vector<CollisionShape> shapes;
};

// A fire point a build outputs (design/submit.md, "Fire"): where (the build's space), which fire
// config, and when it burns: always (a hearth, a torch), only while the instance burns, or its
// smoke alone (a chimney).
struct FirePoint {
  enum Lit : uint32_t { kBurning = 0, kAlways = 1, kSmoke = 2 };
  float pos[3] = {0, 0, 0};
  uint32_t config = 0;
  uint32_t lit = kAlways;
};

// A terrain (design/boundary.md, "Levels"; draw.terrain): a quadtree of nodes, each a grid of
// heights with skirts and its own image, that the runtime draws as a cut by distance, each vertex
// morphing toward its parent's surface as the node nears its parent's distance.
struct TerrainNode {
  float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};  // its box, the build's space
  uint32_t child[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};  // -x-z, +x-z, -x+z, +x+z
  float spacing = 1;            // its grid's spacing, metres
  uint32_t first = 0, count = 0;  // its triangles in indices
};
struct Terrain {
  // The runtime refines a node nearer than this many of its spacings, by quality (low, medium,
  // high); a build refines its tree for the highest (boundary_terrain_v1's kRefine).
  static constexpr float kRefine[3] = {20.0f, 32.0f, 48.0f};
  std::vector<TerrainNode> nodes;   // node 0 is the root
  std::vector<float> vertices;      // per vertex: position xyz, normal xyz, u, v, morph (9 floats)
  std::vector<uint32_t> indices;
  uint32_t image_size = 0;          // each node's image is image_size x image_size texels
  std::vector<uint32_t> images;     // node after node, packed RGBA
  uint64_t triangles = 0;           // in every node
  // What the GPU keeps: released once uploaded (the nodes stay, to pick the cut).
  void release_upload() {
    vertices = {};
    indices = {};
    images = {};
  }
};

// What a pipeline's build stages leave behind, shared by every instance with
// the same build parameters. Generated code subclasses it to keep the stage
// outputs that update stages read (the tree keeps its rig).
// A figure a model leaves for the game to fill from its own systems (draw.figures; design/boundary.md,
// "Figures"): a tree or bush with its species, age, and seed, in the model's space.
struct Figure {
  float position[3] = {0, 0, 0};
  float yaw = 0;  // degrees
  uint32_t kind = 0;  // 0 tree, 1 bush
  std::string species;
  uint32_t age = 0;
  float height = 0;
  uint32_t seed = 0;
};

// A terrain's ground (draw.ground; design/terrain.md): hexbound columns of spans in 1/8 m units, its
// water, roads, bridges, navigation mesh, and the hull for a boundary, in the model's space.
struct Ground {
  std::vector<int32_t> cells;  // q, r pairs
  std::vector<uint32_t> span_first;  // per cell, plus one past the end
  std::vector<int32_t> span_lo, span_hi;
  std::vector<uint32_t> span_material;
  std::vector<int32_t> water;  // per cell, units; -32768 dry
  std::vector<uint32_t> flags;
  std::vector<int32_t> road_cells;
  std::vector<uint32_t> road_counts;
  struct Bridge {
    float a[3], b[3], width;
  };
  std::vector<Bridge> bridges;
  std::vector<float> nav_vertices;  // xyz
  std::vector<uint32_t> nav_polys, nav_cells, nav_links;
  std::vector<int32_t> hull_cells;  // q, r pairs
  std::vector<uint32_t> hull_edges;
  std::vector<float> hull_heights, hull_rises;
  std::vector<int32_t> opening_cells;  // the hull's cells openings leave through, q, r pairs
  std::vector<uint32_t> opening_kinds, opening_widths;
  // A grid over the cells for lookups: index (r - r0) * w + (q - q0), -1 where none.
  int32_t q0 = 0, r0 = 0, w = 0, h = 0;
  std::vector<int32_t> index;
  void build_index() {
    if (cells.size() < 2) return;
    int32_t q1 = cells[0], r1 = cells[1];
    q0 = q1;
    r0 = r1;
    for (size_t i = 0; i + 1 < cells.size(); i += 2) {
      q0 = std::min(q0, cells[i]);
      q1 = std::max(q1, cells[i]);
      r0 = std::min(r0, cells[i + 1]);
      r1 = std::max(r1, cells[i + 1]);
    }
    w = q1 - q0 + 1;
    h = r1 - r0 + 1;
    index.assign(size_t(w) * size_t(h), -1);
    for (size_t i = 0; i + 1 < cells.size(); i += 2) index[size_t(cells[i + 1] - r0) * size_t(w) + size_t(cells[i] - q0)] = int32_t(i / 2);
  }
  int32_t cell(int32_t q, int32_t r) const {
    if (q < q0 || r < r0 || q >= q0 + w || r >= r0 + h) return -1;
    return index[size_t(r - r0) * size_t(w) + size_t(q - q0)];
  }
};

struct Build {
  virtual ~Build() = default;
  DrawMesh mesh;          // released once uploaded, or right away without a GPU
  uint32_t bones = 1;     // pose matrices per instance
  uint32_t triangles = 0;
  float height = 0, radius = 0;  // bounding sphere, centred at half height
  size_t bytes = 0;              // CPU bytes kept after the mesh is released
  uint64_t model_hash = 0;       // the packed draw output (diagnostics)
  std::vector<double> stage_ms;  // per build stage
  std::string problem;           // a failed geometry check, if any
  std::vector<CollisionBody> collision;  // static for the build (empty: collides with nothing)
  float footprint_radius = 0;            // the ground contact at the largest age, metres (0: none)
  uint32_t footprint_cells = 0;          // its footprint on the hex grid (0: none)
  std::vector<FirePoint> fires;          // its fire points (draw.fires)
  std::shared_ptr<Terrain> terrain;      // its terrain (draw.terrain); null for everything else
  std::vector<Figure> figures;           // what it leaves for the game to fill (draw.figures)
  std::shared_ptr<const Ground> ground;  // its ground (draw.ground); null for everything else
};

// A terrain's reach: the build's height and radius grow to hold every node's box (so it is never
// culled while any of its land could show).
inline void terrain_bounds(Build &b) {
  if (!b.terrain) return;
  float top = b.height, r2 = b.radius * b.radius;
  for (const TerrainNode &n : b.terrain->nodes) top = std::max(top, n.max[1]);
  for (const TerrainNode &n : b.terrain->nodes)
    for (int c = 0; c < 8; ++c) {
      const float x = (c & 1) ? n.max[0] : n.min[0], y = ((c & 2) ? n.max[1] : n.min[1]) - top * 0.5f,
                  z = (c & 4) ? n.max[2] : n.min[2];
      r2 = std::max(r2, x * x + y * y + z * z);
    }
  b.height = top;
  b.radius = std::sqrt(r2);
}

// One instance's update state: the update stages' persistent inputs (filled
// once from the build) and the latest pose.
struct PoseState {
  virtual ~PoseState() = default;
  std::vector<float> matrices;  // column-major 4x4 per bone
  bool settled = false;
  std::vector<double> stage_ms;  // the last update, per update stage
};

// A field (design/plotter.md, "Fields"): a pipeline whose one instance covers many hex cells,
// each showing one of a few shared tiles. A cell is what the field's per-cell stage reads...
struct FieldCell {
  int32_t q = 0, r = 0;
  uint32_t ground = 0;
  float moisture = 0.5f;
  float height = 0;  // the cell's ground height, metres (where its tile stands)
};
// ...and its choice is what the cell shows (the cell stage's output, by the field convention).
struct FieldChoice {
  bool present = false;
  uint32_t variant = 0, turn = 0;  // turn: sixths of a turn about +Y
  float scale = 1;
  float tint[3] = {1, 1, 1};
};

// A parameter, by name (diagnostics: the foundry sets shared values such as wind by name).
struct ParamInfo {
  enum Kind : uint32_t { kEnum = 0, kU32 = 1, kF32 = 2, kBool = 3 };
  const char *name;
  uint32_t kind;
  size_t offset;
  bool build;                  // changing it builds something new
  const char *const *values;   // kEnum: the values' names
  uint32_t count;
  double min, max;             // kU32, kF32
};

struct StageInfo {
  std::string label;  // pipeline.stage, e.g. tree_v1.rig
  bool update = false;
};

class PipelineType {
 public:
  virtual ~PipelineType() = default;
  virtual const char *name() const = 0;
  virtual size_t params_size() const = 0;
  // Clocks: [0] is the instance's time, then one per event (seconds since it
  // fired, negative before).
  virtual size_t event_count() const = 0;
  virtual bool has_update() const = 0;
  // The build parameter the runtime sets to pick a level of detail (an enum, finest first; the
  // game's value is the finest it allows). 0 levels: the pipeline has none.
  virtual uint32_t detail_levels() const { return 0; }
  virtual size_t detail_offset() const { return 0; }
  // Per quality (0 low, 1 medium, 2 high): the projected size in pixels below which level L gives
  // way to L + 1, measured against the finest level (the pipeline's draw.detail_pixels). Null: the
  // runtime's defaults.
  virtual const float *detail_pixels(int quality) const {
    (void)quality;
    return nullptr;
  }
  virtual const std::vector<StageInfo> &stages() const = 0;
  // A key for the build parameters, including the pipeline's identity.
  virtual uint64_t build_key(const void *params) const = 0;
  // Runs the build stages. Thread-safe; runs on the generation threads.
  virtual std::shared_ptr<Build> build(const void *params, std::string &error) const = 0;
  // The same with the game's own data for a parameter (a boundary's hull as cells, draw.terrain's
  // hull): payload replaces that parameter's config entry. Pipelines without one ignore it.
  virtual std::shared_ptr<Build> build_with(const void *params, const std::vector<uint8_t> &payload, std::string &error) const {
    (void)payload;
    return build(params, error);
  }
  // Fills an instance's update state from a finished build.
  virtual std::unique_ptr<PoseState> bind(const Build &build) const = 0;
  // Runs the update stages for one instance.
  virtual bool update(const Build &build, PoseState &state, const void *params, const float *clocks,
                      std::string &error) const = 0;
  // Fields (the pipeline's draw.field): whether it is one; its cell stage run over cells with the
  // field's parameters; the parameters of the tile a variant names (the cell's own parameters
  // set to fixed values, so every cell choosing it shares one build); and its wind.
  // The f32 parameter named wind_heading, in degrees toward which the wind blows from +Z toward
  // +X: the game gives it in the world; the runtime hands update stages the instance's own frame.
  // SIZE_MAX: none.
  virtual size_t wind_heading_offset() const { return SIZE_MAX; }
  // The parameters, by name.
  virtual const ParamInfo *param_table(size_t &count) const {
    count = 0;
    return nullptr;
  }
  virtual bool is_field() const { return false; }
  virtual bool field_cells(const void *params, const FieldCell *cells, size_t count, FieldChoice *out,
                           std::string &error) const {
    (void)params, (void)cells, (void)count, (void)out;
    error = "not a field";
    return false;
  }
  virtual void field_tile_params(const void *params, uint32_t variant, void *out) const {
    (void)params, (void)variant, (void)out;
  }
  virtual void field_wind(const void *params, float &speed, float &heading) const {
    (void)params;
    speed = heading = 0;
  }
  // The packed pose output, hashed (diagnostics).
  virtual uint64_t pose_hash(const PoseState &state) const = 0;
};

// FNV-1a, 64 bits: the same hash the foundry uses for stage outputs.
inline uint64_t fnv64(const void *data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace runtime
