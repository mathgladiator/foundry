#include "viewport/tools.h"

#include <algorithm>

#include "viewport/raster.h"

namespace viewport {

using json = nlohmann::json;

namespace {

const char *kVisions[] = {"typical", "protanopia", "deuteranopia", "tritanopia"};
const char *kEffects[] = {"none", "greyscale", "tint"};

json to_json(const runtime::Look &l) {
  return {{"vision", kVisions[std::clamp(l.vision, 0, 3)]}, {"contrast", l.contrast},  {"saturation", l.saturation},
          {"edges", l.edges},                              {"effect", kEffects[std::clamp(l.effect, 0, 2)]},
          {"tint", {l.tint[0], l.tint[1], l.tint[2]}},     {"strength", l.strength}};
}

int index_of(const char *const *names, int n, const std::string &s, int fallback) {
  for (int i = 0; i < n; ++i)
    if (s == names[i]) return i;
  return fallback;
}

}  // namespace

void register_tools(mcp::Registry &registry) {
  registry.add({"look_get", "The game's look every view in the foundry uses (design/submit.md, \"The look\"): colour vision, contrast, "
                            "saturation, edges (lines between colours), and the effect (none, greyscale, tint) with its tint and strength.",
                mcp::schema(json::object()), [](const json &) { return mcp::Result::json(to_json(look())); }});
  registry.add({"look_set",
                "Set the game's look for every view (the game does the same with foundry_set_look): vision (typical, protanopia, "
                "deuteranopia, tritanopia), contrast 0..1, saturation (1 as made), edges 0..1, effect (none, greyscale, tint), tint "
                "[r, g, b], strength 0..1; reset: true first. Screenshots and renders use it from now on.",
                mcp::schema({{"vision", {{"type", "string"}}},
                             {"contrast", {{"type", "number"}}},
                             {"saturation", {{"type", "number"}}},
                             {"edges", {{"type", "number"}}},
                             {"effect", {{"type", "string"}}},
                             {"tint", {{"type", "array"}}},
                             {"strength", {{"type", "number"}}},
                             {"reset", {{"type", "boolean"}}}}),
                [](const json &a) {
                  runtime::Look l = a.value("reset", false) ? runtime::Look() : look();
                  if (a.contains("vision")) l.vision = index_of(kVisions, 4, a["vision"].get<std::string>(), l.vision);
                  if (a.contains("effect")) l.effect = index_of(kEffects, 3, a["effect"].get<std::string>(), l.effect);
                  l.contrast = std::clamp(a.value("contrast", l.contrast), 0.0f, 1.0f);
                  l.saturation = std::clamp(a.value("saturation", l.saturation), 0.0f, 3.0f);
                  l.edges = std::clamp(a.value("edges", l.edges), 0.0f, 1.0f);
                  l.strength = std::clamp(a.value("strength", l.strength), 0.0f, 1.0f);
                  if (a.contains("tint"))
                    for (int i = 0; i < 3; ++i) l.tint[i] = std::clamp(a["tint"][i].get<float>(), 0.0f, 1.0f);
                  set_look(l);
                  return mcp::Result::json(to_json(l));
                }});
}

}  // namespace viewport
