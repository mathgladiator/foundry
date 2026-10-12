#include "pipeline/live.h"

#include <algorithm>
#include <filesystem>

#include "core/jobs.h"
#include "core/util.h"
#include "pipeline/config.h"

namespace fs = std::filesystem;

namespace pipeline {

Live::~Live() {
  if (worker_.joinable()) worker_.join();
}

bool Live::show(const std::string &name, std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_ && def_.name == name) return true;
  }
  Definition def;
  if (!load(runner_.paths(), name, def, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  def_ = def;
  loaded_ = true;
  values_ = runner_.complete(def_, nlohmann::json::object());
  built_ = nullptr;
  last_failed_ = nullptr;
  build_stages_.clear();
  error_.clear();
  origins_.clear();
  for (const auto &p : def_.params)
    if (p.type == "time") origins_[p.name] = 0;  // the clock runs from the start
  ++generation_;
  return true;
}

bool Live::show_config(const std::string &table, const std::string &entry, std::string &error) {
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto &e : fs::directory_iterator(runner_.paths().workspace + "/pipelines", ec))
    if (e.path().extension() == ".json") names.push_back(e.path().stem().string());
  std::sort(names.begin(), names.end());
  for (const auto &n : names) {
    Definition def;
    std::string ignored;
    if (!load(runner_.paths(), n, def, ignored)) continue;
    for (const auto &p : def.params) {
      if (p.type != "config" || p.table != table) continue;
      if (!show(n, error)) return false;
      set({{p.name, entry}});
      return true;
    }
  }
  error = "no pipeline picks from config/" + table;
  return false;
}

bool Live::show_config_file(const std::string &path, std::string &error) {
  std::string table, entry;
  if (!config::split(path, table, entry)) {
    error = path + " is not a config entry";
    return false;
  }
  return show_config(table, entry, error);
}

std::string Live::name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? def_.name : "";
}

Definition Live::definition() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return def_;
}

void Live::reload() {
  std::string name = this->name(), error;
  if (name.empty()) return;
  Definition def;
  if (!load(runner_.paths(), name, def, error)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  def_ = def;
  values_ = runner_.complete(def_, values_);
  built_ = nullptr;  // rebuild: its config or a system may have changed
  last_failed_ = nullptr;
}

nlohmann::json Live::values() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return values_;
}

void Live::set(const nlohmann::json &patch) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    values_[it.key()] = it.value();
    origins_.erase(it.key());
  }
  values_ = runner_.complete(def_, values_);
}

void Live::start(const std::string &param, double now, double from) {
  std::lock_guard<std::mutex> lock(mutex_);
  origins_[param] = now - from;
}

void Live::stop(const std::string &param) {
  std::lock_guard<std::mutex> lock(mutex_);
  origins_.erase(param);
}

bool Live::running(const std::string &param) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return origins_.count(param) > 0;
}

nlohmann::json Live::build_values(const nlohmann::json &all) const {
  nlohmann::json b = nlohmann::json::object();
  for (const auto &p : def_.params)
    if (!def_.is_update_param(p.name)) b[p.name] = all[p.name];
  return b;
}

Runner::Result Live::build_now(bool verify_native) {
  Definition def;
  nlohmann::json values;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    def = def_;
    values = values_;
  }
  Runner::Result r = runner_.run(def, values, verify_native, true);
  std::lock_guard<std::mutex> lock(mutex_);
  if (def_.name == def.name) {
    build_stages_ = r.stages;
    error_ = r.error;
    if (r.ok) {
      built_ = build_values(values);
      last_failed_ = nullptr;
    } else {
      last_failed_ = build_values(values);
    }
  }
  return r;
}

Frame Live::frame(double now) {
  Definition def;
  nlohmann::json values, built;
  bool rebuild = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return {};
    for (const auto &[name, origin] : origins_) values_[name] = now - origin;
    def = def_;
    values = values_;
    built = built_;
    if (!building_ && (built.is_null() || build_values(values_) != built)) {
      // An error stays until the values change, rather than retrying every frame.
      if (last_failed_ != build_values(values_)) {
        rebuild = true;
        building_ = true;
      }
    }
  }
  if (rebuild) {
    if (worker_.joinable()) worker_.join();
    int generation = generation_;
    worker_ = std::thread([this, def, values, generation] {
      core::Job job("Build: " + def.name);
      Runner::Result r = runner_.run(def, values, false, true);
      std::lock_guard<std::mutex> lock(mutex_);
      building_ = false;
      if (generation != generation_) return;
      build_stages_ = r.stages;
      error_ = r.error;
      if (r.ok) {
        built_ = build_values(values);
        last_failed_ = nullptr;
      } else {
        last_failed_ = build_values(values);
      }
    });
  }
  if (built.is_null()) return {};
  // Pose the last finished build with the current update parameters.
  nlohmann::json v = values;
  for (auto it = built.begin(); it != built.end(); ++it) v[it.key()] = it.value();
  Runner::Result r = runner_.run(def, v, false, false);
  if (!r.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = r.error;
  }
  return r.ok ? r.frame : Frame{};
}

bool Live::any_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !origins_.empty();
}

std::vector<StageRun> Live::build_stages() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return build_stages_;
}

std::string Live::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

bool Live::building() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return building_;
}

}  // namespace pipeline
