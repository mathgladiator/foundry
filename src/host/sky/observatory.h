// Observatories (design/observatory.md): the workspace's skies, one per planet,
// in observatories/<name>.json. Not pipelines: an observatory is data (a sun,
// moons, air, stars, constellations, clouds, an aurora) and a saved state, with
// no versions. This slice loads them, keeps each one's state as the human or the
// agent changes it (and plays its day forward), remembers which observatory each
// viewer is under, and builds the sundial an observatory shows.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/mesh.h"
#include "sky/sky.h"
#include "viewport/raster.h"

namespace sky {

using Observatory = runtime::sky::Observatory;
using State = runtime::sky::State;

// Parses an observatory file. `from` loads another observatory's constellations (constellations_from).
bool parse(const nlohmann::json &j, Observatory &out, std::string &error,
           const std::function<bool(const std::string &, nlohmann::json &)> &from = nullptr);
// A state from JSON over a base (only the keys given change); constellations by name.
State state_of(const nlohmann::json &j, const Observatory &o, State base);
nlohmann::json state_json(const State &s, const Observatory &o);
// What the sky shows now: the clock and date, the sun and moons (where, phase), the light.
nlohmann::json describe(const Observatory &o, const State &s);
std::string clock_text(float time_of_day);
std::string date_text(const Observatory &o, float year);

// A sundial at a latitude: a paved round, a pedestal, a bronze dial with hour lines from 6 to 18,
// and a gnomon whose edge points at the celestial pole, so its shadow tells the time (and a ring of
// posts that throw long shadows).
runtime::Mesh sundial(float latitude_deg);

class Observatories {
 public:
  void set_root(const std::string &workspace);
  std::vector<std::string> names() const;
  // An observatory and its state now (loaded again when its file changes; its saved start until
  // set here, played forward when it plays).
  bool get(const std::string &name, std::shared_ptr<const Observatory> &out, State &state, std::string &error);
  bool set_state(const std::string &name, const State &state, std::string &error);
  // How fast its day plays, in hours of its day per second (0: still).
  void set_play(const std::string &name, float hours_per_second);
  float play(const std::string &name) const;
  bool save(const std::string &name, std::string &error);  // the state into its file, as its start
  bool reset(const std::string &name, std::string &error);  // back to its file's start
  // The seeds its sky is generated from, into its file (stars.seed: the star chart and the Milky Way;
  // clouds.seed: the cloud map). A seed always gives the same sky; a negative one is left as it is.
  bool set_seeds(const std::string &name, long long star_seed, long long cloud_seed, std::string &error);
  // Which observatory an item's view is under ("" none), e.g. "pipeline:tree_v1".
  std::string view_of(const std::string &item) const;
  void set_view(const std::string &item, const std::string &name);
  // The sky an item's view is under now, or false.
  bool scene_for(const std::string &item, viewport::SkyScene &out);
  bool scene(const std::string &name, viewport::SkyScene &out);
  // The sundial for an observatory's latitude now (cached).
  std::shared_ptr<const runtime::Mesh> sundial_for(float latitude_deg);
  double now() const;  // seconds since the foundry started: the sky's clock (clouds, twinkling, aurora)
  int version() const;

 private:
  struct Entry {
    std::shared_ptr<const Observatory> observatory;
    nlohmann::json file;
    long long mtime = 0;
    State state;
    double state_at = 0;  // when the state was set (its time of day plays from then)
    float hours_per_second = 0;
  };
  bool load_locked(const std::string &name, Entry &e, std::string &error);
  State advanced(const Entry &e) const;
  std::string root_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::string> views_;
  std::map<int, std::shared_ptr<const runtime::Mesh>> sundials_;
  int version_ = 0;
};

Observatories &observatories();

}  // namespace sky
