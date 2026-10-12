// The live pipeline instance the workspace shows: its parameter values, a
// clock for its time parameters, and the latest posed frame. The human (UI)
// and the agent (MCP) drive the same instance, so each sees what the other set.
//
// Changing a build parameter rebuilds in the background while the previous
// model keeps animating; changing a parameter only update stages read (a clock,
// the wind) only reruns them.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "pipeline/pipeline.h"

namespace pipeline {

class Live {
 public:
  explicit Live(Runner &runner) : runner_(runner) {}
  ~Live();

  // Shows a pipeline (loading it if it isn't the current one).
  bool show(const std::string &name, std::string &error);
  // Shows the first pipeline that picks from this config table, with this entry selected.
  bool show_config(const std::string &table, const std::string &entry, std::string &error);
  // The same, for a config entry's file (config/<table>/<entry>.json).
  bool show_config_file(const std::string &path, std::string &error);
  std::string name() const;
  Definition definition() const;
  // Reloads the definition (after its file or its config changes).
  void reload();

  nlohmann::json values() const;
  // Merges values in; a time parameter set this way stops running.
  void set(const nlohmann::json &patch);

  // Time parameters run from the clock while started: value = now - origin.
  void start(const std::string &param, double now, double from = 0);
  void stop(const std::string &param);
  bool running(const std::string &param) const;
  bool any_running() const;  // any clock or event is running
  Runner &runner() { return runner_; }

  // Builds the current values now, in the calling thread, and returns the stages that ran.
  Runner::Result build_now(bool verify_native);

  // The frame for this moment: advances running time parameters, starts a
  // background rebuild when build parameters changed, and poses the latest build.
  Frame frame(double now);
  // The last build's stage stats and error.
  std::vector<StageRun> build_stages() const;
  std::string error() const;
  bool building() const;

 private:
  nlohmann::json build_values(const nlohmann::json &all) const;
  Runner &runner_;
  mutable std::mutex mutex_;
  Definition def_;
  bool loaded_ = false;
  nlohmann::json values_ = nlohmann::json::object();
  nlohmann::json built_;  // the build parameters of the last finished build (null: none yet)
  nlohmann::json last_failed_;  // the build parameters of the last failed build, so it isn't retried every frame
  std::vector<StageRun> build_stages_;
  std::string error_;
  bool building_ = false;
  std::map<std::string, double> origins_;  // running time parameters
  std::thread worker_;
  int generation_ = 0;
};

}  // namespace pipeline
