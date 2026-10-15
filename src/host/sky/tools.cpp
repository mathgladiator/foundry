#include "sky/tools.h"

#include <algorithm>
#include <cmath>

#include "assets/view.h"
#include "core/util.h"
#include "sky/observatory.h"
#include "viewport/image.h"

namespace sky {

using json = nlohmann::json;

namespace {

json summary(const std::string &name) {
  std::shared_ptr<const Observatory> o;
  State s;
  std::string error;
  if (!observatories().get(name, o, s, error)) return {{"name", name}, {"error", error}};
  json constellations = json::array(), moons = json::array();
  for (const auto &c : o->constellations) constellations.push_back(c.name);
  for (const auto &m : o->moons) moons.push_back({{"name", m.name}, {"distance_km", m.distance_km}});
  return {{"name", name},
          {"constellations", constellations},
          {"moons", moons},
          {"seeds", {{"stars", o->star_seed}, {"clouds", o->cloud_seed}}},
          {"stars", {{"count_at_density_1", o->stars}, {"max_density", runtime::sky::kMaxStarDensity}}},
          {"state", state_json(s, *o)},
          {"play_hours_per_second", observatories().play(name)},
          {"now", describe(*o, s)}};
}

}  // namespace

void register_tools(mcp::Registry &registry) {
  registry.add({"observatory_get",
                "Describe an observatory (design/observatory.md): a planet's sky in observatories/<name>.json, with its "
                "constellations, moons, its state now (time_of_day, year, latitude_deg, clouds, cloud_wind, aurora, "
                "shadows, and which constellations are lit or connected), and what that shows: the clock and date, the "
                "sun's and moons' altitude and azimuth (from north toward east), the moons' phases, the light, the "
                "stars, and the aurora. Without a name, lists the observatories.",
                mcp::schema({{"observatory", {{"type", "string"}, {"description", "e.g. earth"}}}}),
                [](const json &args) {
                  const std::string name = args.value("observatory", "");
                  if (name.empty()) {
                    json out = json::array();
                    for (const auto &n : observatories().names()) out.push_back(n);
                    return mcp::Result::json({{"observatories", out}});
                  }
                  json out = summary(name);
                  if (out.contains("error")) return mcp::Result::error(out["error"].get<std::string>());
                  return mcp::Result::json(out);
                }});

  registry.add({"observatory_set",
                "Change an observatory's state, as its inspector does: state is a patch of {time_of_day 0..1 (0.5 noon), "
                "year 0..1, latitude_deg, clouds 0..1, cloud_wind [x, z] m/s, aurora 0..1, star_density 0..4 (background stars, brightest first; 1 is the observatory's count), "
                "moon_distance_km {moon name: km, 0 its usual distance; nearer is bigger and lights the ground more}, "
                "shadows, constellations: {name: dark|lit|connected} or one of those for all}. star_seed and cloud_seed "
                "set the seeds its sky is generated from (written into its file; a seed always gives the same stars, "
                "Milky Way, and clouds). play_hours_per_second plays its day forward (0 "
                "stops). view puts an item's viewer under this sky (e.g. pipeline:tree, plotter:happy_glen, "
                "config:tree_genomes_oak); with off, takes it away. save writes the state into its file as its "
                "start.",
                mcp::schema({{"observatory", {{"type", "string"}}},
                             {"state", {{"type", "object"}}},
                             {"play_hours_per_second", {{"type", "number"}}},
                             {"view", {{"type", "string"}}},
                             {"off", {{"type", "boolean"}}},
                             {"save", {{"type", "boolean"}}},
                             {"star_seed", {{"type", "integer"}}},
                             {"cloud_seed", {{"type", "integer"}}}},
                            {"observatory"}),
                [](const json &args) {
                  const std::string name = args.value("observatory", "");
                  std::shared_ptr<const Observatory> o;
                  State s;
                  std::string error;
                  if (!observatories().get(name, o, s, error)) return mcp::Result::error(error);
                  if ((args.contains("star_seed") || args.contains("cloud_seed")) &&
                      !observatories().set_seeds(name, args.value("star_seed", -1LL), args.value("cloud_seed", -1LL), error))
                    return mcp::Result::error(error);
                  if (args.contains("state")) {
                    s = state_of(args["state"], *o, s);
                    if (!observatories().set_state(name, s, error)) return mcp::Result::error(error);
                  }
                  if (args.contains("play_hours_per_second"))
                    observatories().set_play(name, args["play_hours_per_second"].get<float>());
                  if (args.contains("view")) {
                    const std::string item = args["view"].get<std::string>();
                    observatories().set_view(item, args.value("off", false) ? "" : name);
                  }
                  if (args.value("save", false) && !observatories().save(name, error)) return mcp::Result::error(error);
                  return mcp::Result::json(summary(name));
                }});

  registry.add({"observatory_view",
                "Render an observatory: its sundial (whose gnomon points at the celestial pole, so its shadow tells "
                "the time) under its sky, with the sun, moons, stars, lit constellations, clouds and their shadows, "
                "the aurora, and the shadow map. Orbit the sundial (yaw, pitch, zoom), or stand beside it and look "
                "(look: {azimuth_deg from north toward east, elevation_deg, fov_deg}). state overrides the state for "
                "this picture only.",
                mcp::schema({{"observatory", {{"type", "string"}}},
                             {"state", {{"type", "object"}}},
                             {"yaw", {{"type", "number"}}},
                             {"pitch", {{"type", "number"}}},
                             {"zoom", {{"type", "number"}}},
                             {"look", {{"type", "object"}}},
                             {"size", {{"type", "integer"}, {"description", "pixels (default 768)"}}}},
                            {"observatory"}),
                [](const json &args) {
                  const std::string name = args.value("observatory", "");
                  viewport::SkyScene scene;
                  std::string error;
                  if (!observatories().get(name, scene.observatory, scene.state, error)) return mcp::Result::error(error);
                  if (args.contains("state")) scene.state = state_of(args["state"], *scene.observatory, scene.state);
                  scene.time = observatories().now();
                  assets::ViewContent content;
                  content.label = "observatory:" + name;
                  content.mesh = observatories().sundial_for(scene.state.latitude_deg);
                  viewport::Camera cam;
                  cam.yaw = args.value("yaw", 0.6f);
                  cam.pitch = args.value("pitch", 0.12f);
                  cam.zoom = args.value("zoom", 0.75f);
                  if (args.contains("look")) {
                    const json &l = args["look"];
                    const float rad = 3.14159265f / 180;
                    cam.free = true;
                    cam.azimuth = l.value("azimuth_deg", 0.0f) * rad;
                    cam.elevation = l.value("elevation_deg", 30.0f) * rad;
                    cam.fov = l.value("fov_deg", 70.0f) * rad;
                  }
                  viewport::Overlays ov;
                  ov.conformance = ov.collision = ov.hero = ov.hex = ov.grid = false;
                  const int size = std::clamp(args.value("size", 768), 128, 2048);
                  viewport::Image img = assets::render_view(content, cam, ov, false, size, size, 0, &scene);
                  auto png = viewport::encode_png(img);
                  mcp::Result r;
                  json text = describe(*scene.observatory, scene.state);
                  text["renderer"] = viewport::last_renderer();
                  r.content.push_back({"text", text.dump(), "", ""});
                  r.content.push_back({"image", "", core::base64(png.data(), png.size()), "image/png"});
                  return r;
                }});
}

}  // namespace sky
