// A camera's path through a map (design/map.md, "Videos"): what an automated video of the live map
// flies. A tour is a function of time, so a recording steps it by whole frames whatever the frame
// rate, and waits for the ground round each frame to be built before keeping it.
//   sites: an orbit over each site (all of them, or those named) and a flight between them, turning
//          toward the next as it goes;
//   road:  along one link's route at a rider's eye, looking ahead;
//   keys:  through the given camera keys, eased between them.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace map {

struct Pose {
  float eye[3] = {0, 0, 0};
  float heading = 0, pitch = 0;  // radians: heading from north (-z) toward east (+x), pitch up
};

struct TourOptions {
  float orbit_s = 8.0f;       // seconds over each site
  float speed = 90.0f;        // metres a second between sites (sites), or along the road (road)
  float sweep_deg = 100.0f;   // how far each orbit goes round
};

class Tour {
 public:
  using Ground = std::function<float(float, float)>;  // the ground's height under x, z
  // sites: names to visit in that order (empty: every site, in the plan's order).
  bool sites(const nlohmann::json &plan, const std::vector<std::string> &names, const TourOptions &o, const Ground &ground,
             std::string &error);
  // road: the link at index link (the plan's road_of), from its first segment to its last.
  bool road(const nlohmann::json &plan, int link, const TourOptions &o, const Ground &ground, std::string &error);
  // keys: [{x, z, y (metres above the ground), heading, pitch (degrees), seconds (from the key before)}].
  bool keys(const nlohmann::json &keys, const Ground &ground, std::string &error);
  double seconds() const { return seconds_; }
  Pose at(double t) const;

 private:
  struct Leg {
    double t0 = 0, t1 = 0;
    std::function<void(float s, float eye[3], float target[3])> f;  // s from 0 to 1 over the leg
  };
  std::vector<Leg> legs_;
  double seconds_ = 0;
};

}  // namespace map
