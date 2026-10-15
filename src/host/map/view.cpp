#include "map/view.h"

#include "core/log.h"

#include <cmath>

namespace map {

View::~View() {
  if (worker_.joinable()) worker_.join();
}

bool View::show(const std::string &name, std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name == name_ && file_.is_object()) return true;
  }
  nlohmann::json f;
  if (!load(paths_, name, f, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  name_ = name;
  file_ = f;
  ++version_;
  return true;
}

bool View::create(const std::string &name, uint32_t radius, std::string &error) {
  if (!core_valid(name)) {
    error = "a map's name is lowercase letters, digits, and underscores";
    return false;
  }
  const nlohmann::json f = blank(name, radius);
  if (!save(paths_, name, f, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  name_ = name;
  file_ = f;
  ++version_;
  return true;
}

bool View::core_valid(const std::string &name) {
  if (name.empty()) return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

std::string View::name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return name_;
}

nlohmann::json View::file() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_;
}

bool View::edit(const nlohmann::json &ops, std::string &error) {
  nlohmann::json f;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    f = file_;
    name = name_;
  }
  if (name.empty()) {
    error = "no map shown";
    return false;
  }
  if (!apply(f, ops, error)) return false;
  if (!save(paths_, name, f, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  file_ = f;
  ++version_;
  return true;
}

std::map<std::string, std::shared_ptr<const plan::Settlement>> View::settlements() {
  nlohmann::json f = file();
  std::map<std::string, std::shared_ptr<const plan::Settlement>> out;
  for (const auto &st : f.value("sites", nlohmann::json::array())) {
    const std::string planner = st.value("planner", "");
    if (planner.empty()) continue;
    const std::string key = planner + "|" + std::to_string(st.value("seed", 1u));
    std::shared_ptr<const plan::Settlement> s;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = settled_.find(key);
      if (it != settled_.end()) s = it->second;
    }
    if (!s) {
      auto made = std::make_shared<plan::Settlement>();
      std::string error;
      // On a map a town plans on flat ground: the map levels its site's flat and carries the rivers.
      const nlohmann::json overrides = {{"terrain", "flat"}, {"relief_m", 0}, {"river", false}};
      if (!plan::settle(paths_, pipelines_, runner_, planner, st.value("seed", 1u), overrides, nlohmann::json::object(), *made, error))
        made->error = error;
      // The map's ground draws the streets' surfaces (map_ground), so its road pieces keep only boards.
      for (plan::Fill &fl : made->fills)
        if (fl.what == "road_cells") fl.values["paving"] = "none";
      core::log().add("map", name() + ": settled " + st.value("name", "") + " (" + planner + "): " + std::to_string(made->fills.size()) +
                                 " pieces" + (made->error.empty() ? "" : ", " + made->error));
      s = made;
      std::lock_guard<std::mutex> lock(mutex_);
      settled_[key] = s;
    }
    out[st.value("name", "")] = s;
  }
  return out;
}

// Each settlement's flat carries the cells it occupies and its streets' materials, in the map's cells,
// for map_ground: nothing grows on what is built, and the ground draws the streets.
static void dress_flats(const nlohmann::json &file, nlohmann::json &plan, const std::map<std::string, std::shared_ptr<const plan::Settlement>> &towns) {
  if (!plan.contains("flats")) return;
  for (const auto &st : file.value("sites", nlohmann::json::array())) {
    auto it = towns.find(st.value("name", ""));
    if (it == towns.end() || !it->second || !it->second->error.empty()) continue;
    const plan::Settlement &s = *it->second;
    const int sq = st.value("q", 0), sr = st.value("r", 0);
    const float cx = 1.7320508f * (static_cast<float>(sq) + static_cast<float>(sr) * 0.5f), cz = 1.5f * static_cast<float>(sr);
    for (auto &f : plan["flats"]) {
      if (std::abs(f["position"][0].get<float>() - cx) > 0.01f || std::abs(f["position"][2].get<float>() - cz) > 0.01f) continue;
      nlohmann::json built = nlohmann::json::array(), streets = nlohmann::json::array(), mats = nlohmann::json::array();
      for (size_t k = 0; k + 1 < s.built.size(); k += 2) {
        built.push_back(sq + s.built[k]);
        built.push_back(sr + s.built[k + 1]);
      }
      for (size_t k = 0; k + 1 < s.streets.size(); k += 2) {
        const std::string &sf = s.street_surfaces[k / 2];
        if (sf == "planks" || sf == "boardwalk") continue;  // boards stay the road piece's own
        streets.push_back(sq + s.streets[k]);
        streets.push_back(sr + s.streets[k + 1]);
        mats.push_back(sf == "cobble" || sf == "flagstone" ? 4 : (sf == "sand" ? 2 : 1));
      }
      f["built"] = built;
      f["streets"] = streets;
      f["street_materials"] = mats;
    }
  }
}

PlanRun View::plan(bool verify_native) {
  nlohmann::json f;
  const auto towns = settlements();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    f = file_;
    // Each settlement's flat holds its plan.
    for (auto &st : f["sites"]) {
      auto it = towns.find(st.value("name", ""));
      if (it != towns.end() && it->second->radius_m > 0) st["radius"] = static_cast<uint32_t>(it->second->radius_m / 1.65f) + 3;
    }
    const std::string key = spec_of(f).dump();
    if (key == plan_key_ && plan_.ok && (!verify_native || plan_.native_checked)) return plan_;
  }
  PlanRun r = map::plan(paths_, runner_, f, verify_native);
  if (r.ok) dress_flats(f, r.plan, towns);
  std::lock_guard<std::mutex> lock(mutex_);
  if (r.ok) {
    plan_key_ = spec_of(f).dump();
    plan_ = r;
  }
  return r;
}

Picture View::picture() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_object() && picture_.version != version_ && composing_ == 0) {
    if (worker_.joinable()) worker_.join();
    composing_ = version_;
    picture_.building = true;
    const int v = version_;
    worker_ = std::thread([this, v] { compose(v); });
  }
  return picture_;
}

void View::compose(int version) {
  PlanRun r = plan(false);
  nlohmann::json f = file();
  Picture p;
  p.version = version;
  if (r.ok) {
    AtlasOptions o;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      o = atlas_options;
    }
    p.atlas = std::make_shared<viewport::Image>(atlas(f, r.plan, o));
    p.report = report(r.plan);
    p.report["milliseconds"] = r.milliseconds;
    p.report["fuel"] = r.fuel;
    core::log().add("map", name() + ": planned in " + std::to_string(static_cast<int>(r.milliseconds)) + " ms (" +
                               std::to_string(p.report.value("chunks", 0)) + " chunks)");
  } else {
    p.error = r.error;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  picture_ = p;
  picture_.version = version;
  composing_ = 0;
}

}  // namespace map
