// The planner the workspace shows (design/planner.md, "In the foundry"): its plan for a seed, a plot
// count, and a tier, and a composed picture of it: the village's shape (its levelled cells coloured
// by use, or height, tier, or wealth), water, and its wall and gate tiles, flat and outlined; every
// plot filled from the preview fill named in the planner's file (houses, civic buildings, castle
// parts, road cells, waterfronts), which a plot without a fill simply outlines; its lines (walls,
// the keep's wall, fences) laid by the fill's line system and filled with walls, fences, and gates,
// as a game would; and, with a plotter named, nature grown on the plan's open land and apron. Plot
// outlines, entrances, and the navigation mesh are overlays. The human (UI) and the agent (MCP)
// drive the same view.
//
// Planning runs the planner's plan system in WebAssembly (and natively too when asked, comparing
// the bytes); composing runs the fill's pipeline for each plot. Both happen in the background, so
// the UI stays responsive.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "plan/planner.h"
#include "viewport/raster.h"
#include "wasm/runner.h"

namespace plan {

// What the view shows, and the plan it asks for.
struct Settings {
  uint32_t seed = 1, plots = 0, tier = 2;  // plots 0: the middle of the config's range
  bool fill = true, outlines = true, nav = false, collision = false;
  bool lines = true;                       // walls, fences, and gates laid on the plan's lines
  std::string nature;                      // the plotter that grows the plan's open land ("" none)
  std::string colour = "use";              // use, height, tier, wealth
  nlohmann::json shared = nlohmann::json::object();     // values every filled building shares (season, wind)
  nlohmann::json overrides = nlohmann::json::object();  // config fields changed in the view, not yet saved
};

struct Picture {
  std::shared_ptr<const runtime::Mesh> mesh;
  std::vector<viewport::Line> lines, collision;
  std::vector<runtime::FirePoint> fires;  // every placed build's fire points, in the plan's space
  nlohmann::json report = nlohmann::json::object();
  int version = 0;
  bool building = false;
  std::string error;
};

class View {
 public:
  View(forge::Paths paths, pipeline::Runner &runner, wasm::Runner &wasm) : paths_(std::move(paths)), runner_(runner), wasm_(wasm) {}
  ~View();

  bool show(const std::string &name, std::string &error);  // loads it if it isn't the current one
  std::string name() const;
  Planner planner() const;
  Settings settings() const;
  // Changes settings: seed, plots, tier (name or index), fill, nature (a plotter, or ""), outlines,
  // nav, collision, colour, shared (merged), config (merged into the overrides; null removes one).
  bool set(const nlohmann::json &patch, std::string &error);
  // The config the plan system gets: the file's, with the overrides and the lots.
  nlohmann::json config() const;
  // Writes the overrides into the planner's file (and clears them), and the view's seed, plots, and tier.
  bool save(std::string &error);

  // Plans now (on this thread) with the current settings; verify_native also runs the plan system
  // natively and compares the bytes. Returns the report.
  bool plan(bool verify_native, nlohmann::json &report, std::string &error);
  nlohmann::json report() const;  // the last plan's report
  nlohmann::json plan_json() const;

  // The ground and what stands on it as triangles (xyz per vertex, three per triangle): what Play walks.
  std::vector<float> walk_triangles();
  // The latest picture; plans and composes in the background when the settings changed.
  Picture picture();

 private:
  bool run_plan(const Settings &st, const nlohmann::json &config, bool verify_native, nlohmann::json &plan,
                nlohmann::json &report, std::string &error);
  void compose(int version);

  forge::Paths paths_;
  pipeline::Runner &runner_;
  wasm::Runner &wasm_;
  mutable std::mutex mutex_;
  bool loaded_ = false;
  Planner planner_;
  std::map<std::string, pipeline::Definition> fill_defs_;  // every pipeline the fill names, by name
  Settings settings_;
  nlohmann::json plan_, report_ = nlohmann::json::object();
  std::string plan_key_;  // the settings and config the plan was made for
  int version_ = 1;
  Picture picture_;
  int composing_ = 0;
  std::thread worker_;
};

}  // namespace plan
