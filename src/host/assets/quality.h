// Quality-driven levels of detail (design/transforms.md): off-screen renders
// from 14 directions decide how far a mesh can be reduced.
//
// For each view, the full mesh and the candidate are rendered orthographically
// with identical framing, as flat vertex colors with depth testing. A pixel
// that switches between object and background counts 1 (the silhouette
// term); a pixel that stays object but changes color counts
// min(1, color distance / 0.25), with distance the RGB distance scaled to
// [0, 1] (the color term). The error is the sum over all views divided by the
// full mesh's object pixels: 0.01 means 1%.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/meshio.h"

namespace assets {

struct ImageError {
  double silhouette = 0, color = 0;
  double total() const { return silhouette + color; }
};

class QualityJudge {
 public:
  explicit QualityJudge(const runtime::Mesh &reference, int resolution = 256);
  ImageError error(const runtime::Mesh &candidate) const;
  int views() const { return static_cast<int>(dirs_.size()); }
  int resolution() const { return res_; }
  static constexpr double kColorScale = 0.25;  // a color distance of a quarter counts fully

  struct Frame {
    std::vector<uint8_t> mask;
    std::vector<std::array<float, 3>> color;
  };
  // One render per view, framed like the reference.
  std::vector<Frame> render_views(const runtime::Mesh &m) const;

 private:
  Frame render(const runtime::Mesh &m, size_t view) const;
  int res_;
  std::vector<std::array<float, 9>> dirs_;  // right, up, forward per view
  float cx_ = 0, cy_ = 0, cz_ = 0, scale_ = 1;
  std::vector<Frame> ref_;
  double ref_pixels_ = 0;
};

// The error of a level of detail as it moves away from the camera. Each view
// is rendered once at 512 pixels and box-filtered down (like antialiasing and
// mipmapping), so each sample sees the object at the pixel size it would
// cover at that distance with the game camera (style.md section 3: a 0.9 rad
// vertical field of view on a 1080-line screen), from very close down to 4
// pixels. Pixels compare by coverage (0 to 1) and by their average color, so
// differences smaller than a pixel average out as they do on screen.
struct DistanceSample {
  double distance_m = 0;    // camera distance
  double pixels = 0;        // the object's height on screen at that distance
  double error = 0;         // total error (silhouette + color) as a share of the object's pixels
  double wrong_pixels = 0;  // the same error in screen pixels, averaged over the views
  double disagree = 0;      // share of the object's pixels that visibly disagree (see DistanceProfile)
};

// The error of a level of detail as it moves away from the camera, with the
// game camera (style.md section 3: a 0.9 rad vertical field of view on a
// 1080-line screen). Pixels are antialiased (rendered larger and
// box-filtered), so differences smaller than a pixel average out as they do on
// screen. A pixel *disagrees* when its coverage and color differ by more than
// kDisagree (a quarter of the way between the two images).
class DistanceProfile {
 public:
  explicit DistanceProfile(const runtime::Mesh &full, double fov_y = 0.9, int screen_height = 1080);
  // Samples from close up (512 pixels) down to 4 pixels, an octave apart.
  std::vector<DistanceSample> curve(const runtime::Mesh &lod) const;
  // The switch distance: the first distance, going outward, at which fewer
  // than `share` of the object's pixels disagree. Bracketed by the
  // curve, then refined by bisection with direct supersampled renders.
  // Returns 0 if it's already under the share up close; never_reached is set
  // if it isn't even at the farthest sample.
  double switch_distance(const runtime::Mesh &lod, const std::vector<DistanceSample> &curve, double share,
                         double &pixels, bool &never_reached) const;
  static constexpr double kDisagree = 0.25;      // a pixel visibly disagrees above this difference
  static constexpr double kDisagreeShare = 0.10; // default: switch once under 10% of the pixels disagree

 private:
  struct Level {
    int res = 0;
    std::vector<float> coverage;                // per pixel
    std::vector<std::array<float, 3>> color;   // average color of the covered part
  };
  std::vector<std::vector<Level>> chain(const runtime::Mesh &m) const;  // per view, per mip level
  // The share of pixels that disagree, at an arbitrary object size (rendered at about 512 and box-filtered).
  double share_at(const runtime::Mesh &lod, int res) const;
  static void compare(const Level &a, const Level &b, double &wrong, double &disagree, double &object);
  runtime::Mesh full_;
  std::unique_ptr<QualityJudge> judge_;
  std::vector<std::vector<Level>> ref_;
  std::vector<double> distances_, pixels_;
  double frame_m_ = 0, px_per_m_at_1m_ = 0, height_m_ = 0;
};

struct QualityLod {
  double max_error = 0;  // the threshold, e.g. 0.01
  size_t level = 0;      // the progressive level chosen
  uint32_t triangles = 0;
  ImageError error;      // its measured error
};

// For each threshold (e.g. 1%, 5%, 20%), finds the coarsest progressive level
// whose total error stays under it.
std::vector<QualityLod> quality_lods(const ProgressiveMesh &pm, const std::vector<double> &thresholds,
                                     const std::function<void(float, const std::string &)> &progress);

}  // namespace assets
