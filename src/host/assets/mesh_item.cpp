#include "assets/mesh_item.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "assets/glb.h"
#include "assets/lod.h"
#include "assets/progressive.h"
#include "assets/quality.h"
#include "assets/sizing.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"
#include "perf/bench.h"
#include "viewport/raster.h"

namespace fs = std::filesystem;

namespace assets {

namespace {

// Each operation reads item.json, works (for up to a minute on a big mesh),
// and writes it back whole. Operations on the same item take turns, so each
// starts from the result of the one before instead of overwriting it.
std::mutex &item_lock(const std::string &root, const std::string &item) {
  static std::mutex guard;
  static std::map<std::string, std::unique_ptr<std::mutex>> locks;
  std::lock_guard<std::mutex> g(guard);
  auto &m = locks[root + "/" + item];
  if (!m) m = std::make_unique<std::mutex>();
  return *m;
}

// Takes the item's turn, showing the job as waiting until then.
std::unique_lock<std::mutex> take_turn(const std::string &root, const std::string &item, core::Job *job) {
  std::unique_lock<std::mutex> lock(item_lock(root, item), std::try_to_lock);
  if (!lock.owns_lock()) {
    if (job) job->update(0, "waiting for the previous change to " + item);
    lock.lock();
  }
  return lock;
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string base_name(const std::string &item) {
  return ends_with(item, "_mesh") ? item.substr(0, item.size() - 5) : item;
}

nlohmann::json read_item(const std::string &root, const std::string &item) {
  return core::read_json(item_dir(root, item) + "/item.json");
}

// The fit of a mesh against a conformance shape (measured, not changed).
nlohmann::json fit_of(const std::string &root, const runtime::Mesh &mesh, const std::string &conformance) {
  nlohmann::json hull = hull_report(root, mesh);
  Conformance conf;
  std::string ignored;
  if (conformance.empty() || !load_conformance(root, conformance, conf, ignored)) return {{"hull", hull}};
  const double height = hull.value("height_m", 0.0), radius = hull["footprint"].value("radius_m", 0.0);
  const bool grounded = hull.value("grounded", false), centred = hull["footprint"].value("centred", false);
  const bool fits = height <= conf.height + 1e-4 && radius <= conf.radius + 1e-4 && grounded && centred;
  char report[300];
  std::snprintf(report, sizeof report, "height %.3f m of %.3f m; footprint radius %.3f m of %.3f m; %s%s%s", height,
                conf.height, radius, conf.radius, fits ? "fits" : "does NOT fit",
                grounded ? "" : "; NOT grounded at y = 0", centred ? "" : "; footprint NOT centred");
  return {{"conformance", {{"name", conf.name}, {"shape", conf.shape}, {"radius", conf.radius}, {"height", conf.height}}},
          {"height", height}, {"max_radius", radius}, {"grounded", grounded}, {"centred", centred},
          {"fits", fits}, {"report", report}, {"volume_m3", hull.value("volume_m3", 0.0)}};
}

bool load_import(const std::string &root, const std::string &item, runtime::Mesh &mesh, std::string &error) {
  const std::string dir = item_dir(root, item);
  if (load_mesh(dir + "/import.bin", mesh)) return true;
  if (!import_gltf(dir + "/model.glb", mesh, error)) return false;
  save_mesh(dir + "/import.bin", mesh);
  return true;
}

// The switch metadata for a LOD: its distance curve and switch distance.
nlohmann::json switch_json(const DistanceProfile &profile, const runtime::Mesh &lod, bool first, double share) {
  std::vector<DistanceSample> curve = profile.curve(lod);
  double px = 0;
  bool never = false;
  double d = profile.switch_distance(lod, curve, share, px, never);
  nlohmann::json samples = nlohmann::json::array();
  for (const auto &c : curve)
    samples.push_back({{"distance_m", c.distance_m}, {"pixels", c.pixels}, {"error", c.error},
                       {"wrong_pixels", c.wrong_pixels}, {"disagree", c.disagree}});
  nlohmann::json out = {{"distance_m", first ? 0.0 : d}, {"pixels", first ? (curve.empty() ? 0 : curve.front().pixels) : px},
                        {"disagree_threshold", DistanceProfile::kDisagree},
                        {"disagree_share", share},
                        {"never_reached", first ? false : never}, {"curve", samples}};
  // LOD0 is the closest level shipped, so it's used from 0; record where it would switch on its own too.
  if (first) out["matches_full_from_m"] = d;
  return out;
}

// Bakes one LOD from a progressive level: a vertex-cache-optimized list,
// saved as rec_lod<k>.bin, with its stats and image error.
// Also profiles its error by distance for its switch distance (see
// DistanceProfile). LOD0, the closest level shipped, starts at 0.
nlohmann::json prepare_lod(const std::string &dir, const ProgressiveMesh &pm, const QualityJudge &judge,
                           const DistanceProfile &profile, double share, size_t k, size_t level, double max_error,
                           runtime::Mesh &list) {
  list = optimize_list(extract_level(pm, level));
  std::string file = "rec_lod" + std::to_string(k) + ".bin";
  save_mesh(dir + "/" + file, list);
  ImageError e = judge.error(list);
  nlohmann::json l = mesh_stats(list);
  l["level"] = k;
  l["progressive_level"] = level;
  l["file"] = file;
  l["error"] = {{"silhouette", e.silhouette}, {"color", e.color}, {"total", e.total()}};
  if (max_error >= 0) l["max_error"] = max_error;
  l["switch"] = switch_json(profile, list, k == 0, share);
  return l;
}

// The GPU benchmark of one LOD, as a perf run.
nlohmann::json bench_run(const std::string &label, const runtime::Mesh &mesh, float radius, std::string &device,
                         std::string &driver, std::string &error) {
  perf::Options options;
  perf::Result r = perf::benchmark(mesh, radius, options);
  if (!r.ok) {
    error = r.error;
    return nullptr;
  }
  device = r.device;
  driver = r.driver;
  nlohmann::json run = perf::to_json(r, options);
  run["label"] = label;
  run["triangles"] = mesh.triangles();
  run["index_count"] = mesh.indices.size();
  run["budget_triangles"] = static_cast<double>(r.budget) * mesh.triangles();
  return run;
}

float item_radius(const nlohmann::json &j) {
  return j.contains("fit") && j["fit"].contains("max_radius") ? j["fit"]["max_radius"].get<float>() : 0.25f;
}

}  // namespace

const std::vector<double> &lod_thresholds() {
  static const std::vector<double> t = {0.01, 0.05, 0.20};
  return t;
}

double item_switch_share(const nlohmann::json &j) {
  double s = j.is_object() ? j.value("switch_share", DistanceProfile::kDisagreeShare) : DistanceProfile::kDisagreeShare;
  return std::clamp(s, kSwitchShareMin, kSwitchShareMax);
}

std::vector<double> item_thresholds(const nlohmann::json &j) {
  std::vector<double> t = lod_thresholds();
  if (j.is_object() && j.contains("lod_thresholds") && j["lod_thresholds"].is_array() &&
      j["lod_thresholds"].size() == t.size())
    for (size_t i = 0; i < t.size(); ++i) t[i] = j["lod_thresholds"][i].get<double>();
  return t;
}

std::string item_dir(const std::string &root, const std::string &item) { return root + "/meshy/" + item; }

bool load_conformance(const std::string &root, const std::string &name, Conformance &out, std::string &error) {
  nlohmann::json j = core::read_json(root + "/conformance/" + name + ".json");
  if (!j.is_object()) {
    error = "no conformance shape '" + name + "' (see conformance_list)";
    return false;
  }
  out.name = name;
  out.shape = j.value("shape", "");
  out.radius = j.value("radius", 0.0f);
  out.height = j.value("height", 0.0f);
  return true;
}

bool load_full(const std::string &root, const std::string &item, runtime::Mesh &mesh, std::string &error) {
  if (load_mesh(item_dir(root, item) + "/full.bin", mesh)) return true;
  return load_import(root, item, mesh, error);
}

bool load_item_progressive(const std::string &root, const std::string &item, ProgressiveMesh &pm) {
  return load_progressive_file(item_dir(root, item) + "/progressive.bin", pm);
}

bool mesh_transform(const std::string &root, const std::string &item, const nlohmann::json &ops, bool reset,
                    nlohmann::json &j, std::string &error) {
  auto turn = take_turn(root, item, nullptr);
  const std::string dir = item_dir(root, item);
  j = read_item(root, item);
  if (!j.is_object() || j.value("kind", "") != "mesh") {
    error = "no mesh item " + item;
    return false;
  }
  if (j.value("state", "draft") == "final") {
    error = item + " is approved (final); its sizing can't change";
    return false;
  }
  runtime::Mesh mesh;
  if (!load_import(root, item, mesh, error)) return false;
  nlohmann::json all = reset ? nlohmann::json::array() : j.value("ops", nlohmann::json::array());
  for (const auto &op : ops) all.push_back(op);
  nlohmann::json state = {{"conformance", reset ? "" : j.value("conformance", "")}};
  if (!apply_ops(root, mesh, all, state, error)) return false;
  if (!save_mesh(dir + "/full.bin", mesh)) {
    error = "can't write full.bin";
    return false;
  }
  // The recommendation was for the old mesh.
  fs::remove(dir + "/progressive.bin");
  for (const char *f : {"rec_lod0.bin", "rec_lod1.bin", "rec_lod2.bin"}) fs::remove(dir + "/" + f);
  j.erase("recommended");
  j.erase("perf");
  j.erase("progressive");
  j["ops"] = all;
  j["conformance"] = state.value("conformance", "");
  j["fit"] = fit_of(root, mesh, j["conformance"]);
  j["stats"] = mesh_stats(mesh);
  if (!core::write_json(dir + "/item.json", j)) {
    error = "can't write item.json";
    return false;
  }
  core::log().add("mesh", item + ": " + std::to_string(all.size()) + " sizing ops; " + j["fit"].value("report", ""));
  return true;
}

bool mesh_recommend(const std::string &root, const std::string &item, const std::vector<uint32_t> *triangles,
                    nlohmann::json &j, std::string &error) {
  core::Job job("Recommend LODs: " + item);
  auto turn = take_turn(root, item, &job);
  const std::string dir = item_dir(root, item);
  j = read_item(root, item);
  if (!j.is_object() || j.value("kind", "") != "mesh") {
    error = "no mesh item " + item;
    return false;
  }
  if (j.value("state", "draft") == "final") {
    error = item + " is approved (final); its LODs are stamped";
    return false;
  }
  runtime::Mesh full;
  if (!load_full(root, item, full, error)) return false;

  ProgressiveMesh pm;
  if (!load_item_progressive(root, item, pm)) {
    job.update(0, "progressive mesh: collapsing edges");
    auto start = core::jobs().now();
    if (!build_progressive(full, pm, error, [&](float f) { job.update(0.3f * f, "progressive mesh: collapsing edges"); }))
      return false;
    save_progressive(dir + "/progressive.bin", pm);
    j["progressive"] = {{"transform", "volume-preserving edge collapse"},
                        {"report", pm.report},
                        {"levels", pm.level_triangles.size()},
                        {"min_triangles", pm.level_triangles.back()},
                        {"ms", (core::jobs().now() - start) * 1000}};
  }

  // Levels: by error, or at explicit triangle counts.
  struct Pick {
    size_t level;
    double max_error;  // < 0 when chosen by triangle count
  };
  std::vector<Pick> picks;
  if (triangles && !triangles->empty()) {
    std::vector<uint32_t> t = *triangles;
    std::sort(t.begin(), t.end(), std::greater<uint32_t>());
    for (uint32_t n : t) picks.push_back({level_for(pm, n), -1});
  } else {
    const std::vector<double> thresholds = item_thresholds(j);
    j["lod_thresholds"] = thresholds;
    auto q = quality_lods(pm, thresholds, [&](float f, const std::string &s) { job.update(0.3f + 0.4f * f, s); });
    for (const auto &l : q) picks.push_back({l.level, l.max_error});
  }

  // Prepare each level for the GPU: a vertex-cache-optimized list.
  job.update(0.7f, "preparing optimized lists");
  QualityJudge judge(full);
  job.update(0.7f, "profiling error by distance");
  DistanceProfile profile(full);
  const double share = item_switch_share(j);
  nlohmann::json lods = nlohmann::json::array();
  std::vector<std::pair<std::string, runtime::Mesh>> variants;  // label, mesh (for the benchmark)
  float radius = 0.25f;
  if (j.contains("fit") && j["fit"].contains("max_radius")) radius = j["fit"]["max_radius"];
  for (size_t i = 0; i < picks.size(); ++i) {
    runtime::Mesh list;
    lods.push_back(prepare_lod(dir, pm, judge, profile, share, i, picks[i].level, picks[i].max_error, list));
    variants.push_back({"LOD" + std::to_string(i), list});
  }
  j.erase("strip_lod0");
  j["recommended"] = {{"lods", lods},
                      {"method", {{"metric", "off-screen renders from 14 views; a pixel counts 1 if it switches between "
                                             "object and background, and min(1, color distance / 0.25) if it stays "
                                             "object but changes color; summed over the full mesh's object pixels"},
                                  {"views", judge.views()},
                                  {"resolution", judge.resolution()},
                                  {"color_scale", QualityJudge::kColorScale},
                                  {"thresholds", item_thresholds(j)},
                                  {"switch", "each LOD's switch distance is the first distance, going outward (game "
                                             "camera: 0.9 rad vertical field of view, 1080 lines), at which under the switch threshold (default 10%) of "
                                             "the object's antialiased pixels visibly disagree with the full mesh (a "
                                             "pixel disagrees above a quarter difference in coverage and color), over 14 "
                                             "views, refined by bisection; LOD0 starts at 0; the game multiplies switch "
                                             "distances by its quality factor"}}},
                      {"at", core::now_iso()}};

  // The performance phase: each LOD.
  perf::Options options;
  nlohmann::json runs = nlohmann::json::array();
  std::string device, driver;
  for (size_t v = 0; v < variants.size(); ++v) {
    job.update(0.75f + 0.25f * static_cast<float>(v) / variants.size(), "benchmark: " + variants[v].first);
    std::string bench_error;
    nlohmann::json run = bench_run(variants[v].first, variants[v].second, radius, device, driver, bench_error);
    if (run.is_null()) {
      j["perf_error"] = bench_error;
      break;
    }
    runs.push_back(run);
    core::log().add("perf", item + " " + variants[v].first + ": " + std::to_string(run.value("budget_instances", 0)) +
                                (run.value("budget_is_lower_bound", false) ? "+" : "") + " instances at 60 FPS");
  }
  j["perf"] = {{"runs", runs}, {"device", device}, {"driver", driver},
               {"resolution", {options.width, options.height}}, {"fps_floor", options.fps_floor},
               {"method", "instanced cel shader, off screen; instance counts double from 1 until FPS < floor, then bisect"},
               {"measured_at", core::now_iso()}};

  // Keep concurrent edits (notes, state) from the file on disk.
  nlohmann::json latest = read_item(root, item);
  j["notes"] = latest.value("notes", nlohmann::json::array());
  j["state"] = latest.value("state", "draft");
  if (!core::write_json(dir + "/item.json", j)) {
    error = "can't write item.json";
    return false;
  }
  std::string summary;
  for (const auto &l : lods) {
    char b[100];
    std::snprintf(b, sizeof b, "%sLOD%d %d tris (%.2f%%)", summary.empty() ? "" : ", ", l.value("level", 0),
                  l.value("triangles", 0), l["error"].value("total", 0.0) * 100);
    summary += b;
  }
  core::log().add("mesh", item + ": recommended " + summary);
  return true;
}

void cascade_limits(std::vector<double> &t, size_t k, double value) {
  const size_t n = t.size();
  if (k >= n) return;
  // Leave room for the others' gaps on both sides.
  t[k] = std::clamp(value, kLimitMin + kLimitGap * k, kLimitMax - kLimitGap * (n - 1 - k));
  for (size_t i = k + 1; i < n; ++i) t[i] = std::max(t[i], t[i - 1] + kLimitGap);
  for (size_t i = k; i-- > 0;) t[i] = std::min(t[i], t[i + 1] - kLimitGap);
}

bool mesh_regenerate_lod(const std::string &root, const std::string &item, size_t k, double max_error,
                         nlohmann::json &j, std::string &error) {
  core::Job job("Regenerate LOD" + std::to_string(k) + ": " + item);
  auto turn = take_turn(root, item, &job);
  const std::string dir = item_dir(root, item);
  j = read_item(root, item);
  if (!j.is_object() || j.value("kind", "") != "mesh") {
    error = "no mesh item " + item;
    return false;
  }
  if (j.value("state", "draft") == "final") {
    error = item + " is approved (final); its LODs are stamped";
    return false;
  }
  ProgressiveMesh pm;
  runtime::Mesh full;
  if (!j.contains("recommended") || !load_item_progressive(root, item, pm) || !load_full(root, item, full, error)) {
    error = "no recommendation yet; run mesh_recommend first";
    return false;
  }
  auto &lods = j["recommended"]["lods"];
  const std::vector<double> before = item_thresholds(j);
  std::vector<double> t = before;
  if (k >= lods.size() || k >= t.size()) {
    error = "no LOD" + std::to_string(k);
    return false;
  }
  cascade_limits(t, k, max_error);
  j["lod_thresholds"] = t;
  j["recommended"]["method"]["thresholds"] = t;

  // Search all limits together, so the levels stay in order (each no finer
  // than the one before); rebake only the LODs whose limit or level changed.
  auto q = quality_lods(pm, t, [&](float f, const std::string &s) { job.update(0.5f * f, s); });
  std::vector<size_t> changed;
  for (size_t i = 0; i < lods.size() && i < q.size(); ++i)
    if (std::fabs(t[i] - before[i]) > 1e-9 || lods[i].value("progressive_level", size_t(0)) != q[i].level) changed.push_back(i);
  QualityJudge judge(full);
  DistanceProfile profile(full);
  std::string device, driver, bench_error;
  nlohmann::json runs = j.contains("perf") ? j["perf"].value("runs", nlohmann::json::array()) : nlohmann::json::array();
  for (size_t c = 0; c < changed.size(); ++c) {
    const size_t i = changed[c];
    const float f0 = 0.5f + 0.5f * c / changed.size(), f1 = 0.5f + 0.5f * (c + 1) / changed.size();
    job.update(f0, "baking LOD" + std::to_string(i) + " and profiling error by distance");
    runtime::Mesh list;
    lods[i] = prepare_lod(dir, pm, judge, profile, item_switch_share(j), i, q[i].level, t[i], list);
    job.update(0.5f * (f0 + f1), "benchmark: LOD" + std::to_string(i));
    nlohmann::json run = bench_run("LOD" + std::to_string(i), list, item_radius(j), device, driver, bench_error);
    if (run.is_null()) {
      j["perf_error"] = bench_error;
      continue;
    }
    bool replaced = false;
    for (auto &r : runs)
      if (r.value("label", "") == run.value("label", "")) {
        r = run;
        replaced = true;
      }
    if (!replaced) runs.push_back(run);
  }
  std::sort(runs.begin(), runs.end(),
            [](const nlohmann::json &a, const nlohmann::json &b) { return a.value("label", "") < b.value("label", ""); });
  j["perf"]["runs"] = runs;
  if (!device.empty()) {
    j["perf"]["device"] = device;
    j["perf"]["driver"] = driver;
    j["perf"]["measured_at"] = core::now_iso();
  }

  nlohmann::json latest = read_item(root, item);
  j["notes"] = latest.value("notes", nlohmann::json::array());
  j["state"] = latest.value("state", "draft");
  if (!core::write_json(dir + "/item.json", j)) {
    error = "can't write item.json";
    return false;
  }
  std::string summary;
  for (size_t i : changed) {
    char b[120];
    std::snprintf(b, sizeof b, "%sLOD%zu %d triangles at %.2f%% (limit %.1f%%)", summary.empty() ? "" : ", ", i,
                  lods[i].value("triangles", 0), lods[i]["error"].value("total", 0.0) * 100, t[i] * 100);
    summary += b;
  }
  core::log().add("mesh", item + ": " + (summary.empty() ? std::string("no LOD changed") : summary));
  return true;
}

bool mesh_set_switch_share(const std::string &root, const std::string &item, double share, nlohmann::json &j,
                           std::string &error) {
  core::Job job("Switch distances: " + item);
  auto turn = take_turn(root, item, &job);
  const std::string dir = item_dir(root, item);
  j = read_item(root, item);
  if (!j.is_object() || j.value("kind", "") != "mesh") {
    error = "no mesh item " + item;
    return false;
  }
  if (j.value("state", "draft") == "final") {
    error = item + " is approved (final); its LODs are stamped";
    return false;
  }
  runtime::Mesh full;
  if (!j.contains("recommended") || !load_full(root, item, full, error)) {
    error = "no recommendation yet; run mesh_recommend first";
    return false;
  }
  share = std::clamp(share, kSwitchShareMin, kSwitchShareMax);
  j["switch_share"] = share;
  job.update(0.1f, "profiling error by distance");
  DistanceProfile profile(full);
  auto &lods = j["recommended"]["lods"];
  std::string summary;
  for (size_t i = 0; i < lods.size(); ++i) {
    job.update(0.2f + 0.8f * i / lods.size(), "LOD" + std::to_string(i));
    runtime::Mesh lod;
    if (!load_mesh(dir + "/" + lods[i].value("file", ""), lod)) continue;
    lods[i]["switch"] = switch_json(profile, lod, i == 0, share);
    char b[80];
    std::snprintf(b, sizeof b, "%sLOD%zu %.1f m", summary.empty() ? "" : ", ", i, lods[i]["switch"].value("distance_m", 0.0));
    summary += b;
  }
  nlohmann::json latest = read_item(root, item);
  j["notes"] = latest.value("notes", nlohmann::json::array());
  j["state"] = latest.value("state", "draft");
  if (!core::write_json(dir + "/item.json", j)) {
    error = "can't write item.json";
    return false;
  }
  char b[60];
  std::snprintf(b, sizeof b, ": switch threshold %.1f%%: ", share * 100);
  core::log().add("mesh", item + b + summary);
  return true;
}

bool stamp(const std::string &root, const std::string &item, std::string &error) {
  core::Job job("Stamp: " + item);
  auto turn = take_turn(root, item, &job);
  nlohmann::json j = read_item(root, item);
  if (!j.is_object() || !j.contains("recommended")) {
    error = item + " has no recommended LODs to stamp (run mesh_recommend, then approve)";
    return false;
  }
  const std::string name = base_name(item);
  const std::string out = root + "/fixed/" + name;
  nlohmann::json old = core::read_json(out + "/manifest.json");
  if (old.is_object() && old.value("state", "") == "final" && old.value("stamped_from", "") != item) {
    error = "fixed/" + name + " is already a final asset from another item";
    return false;
  }
  fs::create_directories(out);
  for (const auto &e : fs::directory_iterator(out))
    if (e.path().extension() == ".bin") fs::remove(e.path());
  const std::string dir = item_dir(root, item);
  nlohmann::json lods = nlohmann::json::array();
  for (auto l : j["recommended"]["lods"]) {
    std::string target = "lod" + std::to_string(l.value("level", 0)) + ".bin";
    fs::copy_file(dir + "/" + l.value("file", ""), out + "/" + target, fs::copy_options::overwrite_existing);
    l["file"] = target;
    lods.push_back(l);
  }
  runtime::Mesh lod0;
  if (load_mesh(out + "/lod0.bin", lod0)) {
    viewport::Scene scene;
    scene.mesh = &lod0;
    if (j.contains("fit") && j["fit"].contains("conformance")) {
      scene.has_conformance = true;
      scene.conformance_radius = j["fit"]["conformance"].value("radius", 0.0f);
      scene.conformance_height = j["fit"]["conformance"].value("height", 0.0f);
    }
    auto png = viewport::encode_png(viewport::render(scene, 256, 256));
    core::write_file(out + "/thumbnail.png", png.data(), png.size());
  }
  nlohmann::json m = {{"name", name},
                      {"kind", "fixed"},
                      {"state", "final"},
                      {"stamped_from", item},
                      {"approved_at", j.value("approved_at", core::now_iso())},
                      {"stamped_at", core::now_iso()},
                      {"provenance", j.value("provenance", nlohmann::json())},
                      {"ops", j.value("ops", nlohmann::json::array())},
                      {"fit", j.value("fit", nlohmann::json())},
                      {"stats", j.value("stats", nlohmann::json())},
                      {"lods", lods},
                      {"lod_method", j["recommended"].value("method", nlohmann::json())},
                      {"perf", j.value("perf", nlohmann::json())},
                      {"notes", nlohmann::json::array()}};
  if (!core::write_json(out + "/manifest.json", m)) {
    error = "can't write " + out + "/manifest.json";
    return false;
  }
  j["stamped"] = {{"fixed", name}, {"at", core::now_iso()}};
  core::write_json(dir + "/item.json", j);
  core::log().add("mesh", item + " stamped into fixed/" + name + ": " + std::to_string(lods.size()) + " LODs");
  return true;
}

}  // namespace assets
