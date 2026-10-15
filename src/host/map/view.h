// The map the workspace shows (design/map.md): its file, its macro plan (run when the file changes,
// cached by its spec), and its atlas, made in the background for the viewport.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <thread>

#include <nlohmann/json.hpp>

#include "map/map.h"
#include "map/tour.h"
#include "pipeline/pipeline.h"
#include "plan/fill.h"

namespace map {

struct Picture {
  int version = 0;  // bumps as each atlas finishes
  std::shared_ptr<const viewport::Image> atlas;
  nlohmann::json report;
  std::string error;
  bool building = false;
};

class View {
 public:
  View(forge::Paths paths, pipeline::Runner &pipelines, wasm::Runner &runner)
      : paths_(std::move(paths)), pipelines_(pipelines), runner_(runner) {}
  ~View();
  bool show(const std::string &name, std::string &error);
  // Creates a map (a hexagon of `radius` cells) and shows it.
  bool create(const std::string &name, uint32_t radius, std::string &error);
  std::string name() const;
  const forge::Paths &paths() const { return paths_; }
  nlohmann::json file() const;
  // Applies edits (map::apply) and saves the file.
  bool edit(const nlohmann::json &ops, std::string &error);
  // The current file's plan, run now if it is stale (blocking).
  PlanRun plan(bool verify_native = false);
  // Each settlement site's plan and fill (its planner on flat ground, cached by planner and seed),
  // by site name; plan() settles them first, to size their flats.
  std::map<std::string, std::shared_ptr<const plan::Settlement>> settlements();
  pipeline::Runner &pipelines() { return pipelines_; }
  wasm::Runner &runner() { return runner_; }
  // The latest atlas; starts making one in the background when the file changed.
  Picture picture();
  AtlasOptions atlas_options;  // what the viewport's atlas shows
  // A camera the agent asks the live view for (map_look): flying at eye, heading and pitch in radians.
  struct Look {
    bool pending = false, live = true, play = false, grass = true, probe = false;
    float eye[3] = {0, 0, 0};
    float heading = 0, pitch = 0;
  };
  void request_look(const Look &l) {
    std::lock_guard<std::mutex> lock(mutex_);
    look_ = l;
    look_.pending = true;
  }
  std::string probe;  // the last walker probe (map_look with probe), filled by the live view

  // A video of the live map (map_video, or the viewport's Record): asked for here, recorded by the
  // live view over as many frames as it takes, its progress and result reported back.
  struct Video {
    std::string mode = "sites";  // sites, road, keys
    std::vector<std::string> sites;
    int link = 0;
    nlohmann::json keys;
    TourOptions tour;
    int width = 1280, height = 720, fps = 30;
    bool grass = true;
    double max_wait_s = 4;  // the longest a frame waits for the ground round it to be built
    float reach = 1.8f;     // times the live view's streaming radii while it records
  };
  struct VideoStatus {
    bool running = false, done = false;
    std::string path, error;
    int frames = 0, total = 0, waited = 0;  // waited: frames kept before everything near was built
    double seconds = 0, wall_s = 0;
    uint64_t serial = 0;
  };
  uint64_t request_video(const Video &v) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_ = v;
    video_pending_ = true;
    status_ = VideoStatus();
    status_.serial = ++video_serial_;
    return status_.serial;
  }
  bool take_video(Video &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!video_pending_) return false;
    out = video_;
    video_pending_ = false;
    return true;
  }
  bool video_wanted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_pending_;
  }
  void cancel_video() {
    std::lock_guard<std::mutex> lock(mutex_);
    video_cancel_ = true;
    video_pending_ = false;
  }
  bool take_cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool c = video_cancel_;
    video_cancel_ = false;
    return c;
  }
  void set_video_status(const VideoStatus &s) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t serial = status_.serial;
    status_ = s;
    status_.serial = serial;
  }
  VideoStatus video_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }
  bool take_look(Look &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!look_.pending) return false;
    out = look_;
    look_.pending = false;
    return true;
  }

 private:
  void compose(int version);
  static bool core_valid(const std::string &name);
  forge::Paths paths_;
  Video video_;
  VideoStatus status_;
  bool video_pending_ = false, video_cancel_ = false;
  uint64_t video_serial_ = 0;
  pipeline::Runner &pipelines_;
  wasm::Runner &runner_;
  std::map<std::string, std::shared_ptr<const plan::Settlement>> settled_;  // by planner|seed
  mutable std::mutex mutex_;
  std::string name_;
  nlohmann::json file_;
  int version_ = 0;
  std::string plan_key_;  // the spec the cached plan is for
  PlanRun plan_;
  Picture picture_;
  int composing_ = 0;
  Look look_;
  std::thread worker_;
};

}  // namespace map
