#include "plan/planner.h"

#include <algorithm>
#include <filesystem>
#include <map>

#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace plan {

const std::vector<std::string> &classes() {
  static const std::vector<std::string> v = {"solo", "family", "extended", "civic", "castle", "waterfront", "centre", "field"};
  return v;
}
const std::vector<std::string> &shapes() {
  static const std::vector<std::string> v = {"wide", "deep", "round", "long", "broad"};
  return v;
}
const std::vector<std::string> &uses() {
  static const std::vector<std::string> v = {"home", "store", "workshop", "inn", "farm", "civic", "keep", "harbour", "way", "water", "castle", "centre", "field"};
  return v;
}
const std::vector<std::string> &tiers() {
  static const std::vector<std::string> v = {"hamlet", "village", "town"};
  return v;
}

std::vector<std::string> names(const forge::Paths &paths) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(paths.workspace + "/planners", ec))
    if (e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

int index_of(const std::vector<std::string> &v, const std::string &s) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == s) return static_cast<int>(i);
  return -1;
}

// The lots measured from a building pipeline (the file's "lots.from"): for each class value and
// shape, the config entries whose shape fields name that shape, built once to read the cells they
// stand on (the pipeline's draw.footprint.lot).
bool measure_lots(const forge::Paths &paths, pipeline::Runner &runner, const nlohmann::json &spec, std::vector<Lot> &out,
                  std::string &error) {
  const std::string pipe = spec.value("pipeline", "");
  pipeline::Definition def;
  if (!pipeline::load(paths, pipe, def, error)) return false;
  if (def.footprint_lot.empty()) {
    error = pipe + " has no draw.footprint.lot to measure lots from";
    return false;
  }
  const std::string class_param = spec.value("class_param", ""), type_param = spec.value("type_param", "");
  const std::string shape_param = spec.value("shape_param", ""), table = spec.value("table", "");
  const nlohmann::json shape_fields = spec.value("shape_fields", nlohmann::json::object());
  const std::vector<std::string> class_values = spec.value("classes", std::vector<std::string>());
  // Every entry of the table, by name, with its shape fields.
  std::map<std::string, nlohmann::json> entries;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(paths.workspace + "/config/" + table, ec)) {
    if (e.path().extension() != ".json" || e.path().stem() == "table") continue;
    entries[e.path().stem().string()] = core::read_json(e.path().string());
  }
  if (entries.empty()) {
    error = "no entries in config/" + table;
    return false;
  }
  for (size_t c = 0; c < class_values.size() && c < 3; ++c)
    for (size_t sh = 0; sh < shapes().size(); ++sh) {
      Lot lot;
      lot.cls = static_cast<uint32_t>(c);
      lot.shape = static_cast<uint32_t>(sh);
      for (const auto &[name, entry] : entries) {
        const nlohmann::json v = entry.value("value", entry);
        for (auto it = shape_fields.begin(); it != shape_fields.end(); ++it)
          if (v.value(it.key(), std::string()) == shapes()[sh])
            lot.builds.push_back({{type_param, name}, {class_param, class_values[c]}, {shape_param, it.value()}});
      }
      if (lot.builds.empty()) continue;  // no entry has this shape: no such lot
      nlohmann::json values = lot.builds[0];
      values["stage"] = spec.value("measure_stage", std::string("basic"));
      auto r = runner.run(def, values, false, false);
      if (!r.ok || r.frame.lot_cells.empty()) {
        error = pipe + " didn't build for " + values.dump() + ": " + r.error;
        return false;
      }
      lot.cells = r.frame.lot_cells;
      lot.doors = r.frame.door_cells;
      const std::string fp = r.frame.facts.value("footprint", std::string());
      lot.name = fp.empty() ? std::to_string(lot.cells.size() / 2) + " cells" : fp;
      out.push_back(lot);
    }
  return true;
}

// Lots measured by size from a pipeline whose lot is set by a size parameter (the file's
// "lots.waterfront", "lots.civic", "lots.castle"): each size built once to read its cells (the
// pipeline's draw.footprint.lot) and which are wet (draw.footprint.wet). Its builds are every
// config entry, or with "size_field" the entries whose field names that size. Each size's class
// is "class" (or "classes"[size]); its shape is its order among its class's sizes.
bool measure_sized(const forge::Paths &paths, pipeline::Runner &runner, const nlohmann::json &spec, const std::string &cls,
                   std::vector<Lot> &out, std::string &error) {
  const std::string pipe = spec.value("pipeline", "");
  pipeline::Definition def;
  if (!pipeline::load(paths, pipe, def, error)) return false;
  if (def.footprint_lot.empty()) {
    error = pipe + " has no draw.footprint.lot to measure lots from";
    return false;
  }
  const std::string size_param = spec.value("size_param", "size"), type_param = spec.value("type_param", "type"), table = spec.value("table", "");
  const std::string size_field = spec.value("size_field", "");
  const nlohmann::json classes_by_size = spec.value("classes", nlohmann::json::object());
  std::map<std::string, nlohmann::json> entries;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(paths.workspace + "/config/" + table, ec))
    if (e.path().extension() == ".json" && e.path().stem() != "table") entries[e.path().stem().string()] = core::read_json(e.path().string());
  if (entries.empty()) {
    error = "no entries in config/" + table;
    return false;
  }
  std::map<int, uint32_t> next_shape;
  for (const auto &size : spec.value("sizes", std::vector<std::string>())) {
    Lot lot;
    const int c = index_of(classes(), classes_by_size.value(size, spec.value("class", cls)));
    if (c < 0) {
      error = pipe + ": size " + size + " names no plan class";
      return false;
    }
    lot.cls = static_cast<uint32_t>(c);
    lot.shape = next_shape[c]++;
    for (const auto &[name, entry] : entries) {
      const nlohmann::json v = entry.value("value", entry);
      if (!size_field.empty() && v.value(size_field, std::string()) != size) continue;
      lot.builds.push_back({{type_param, name}, {size_param, size}});
    }
    if (lot.builds.empty()) {
      // No entry is this size: measure with the first entry, so the lot still exists for any type.
      lot.builds.push_back({{type_param, entries.begin()->first}, {size_param, size}});
    }
    auto r = runner.run(def, lot.builds[0], false, false);
    if (!r.ok || r.frame.lot_cells.empty()) {
      error = pipe + " didn't build for " + lot.builds[0].dump() + ": " + r.error;
      return false;
    }
    lot.cells = r.frame.lot_cells;
    lot.wet = r.frame.wet_cells;
    lot.doors = r.frame.door_cells;
    lot.name = r.frame.facts.value("footprint", size);
    out.push_back(lot);
  }
  return true;
}

}  // namespace

bool load(const forge::Paths &paths, pipeline::Runner &runner, const std::string &name, Planner &out, std::string &error) {
  const std::string path = paths.workspace + "/planners/" + name + ".json";
  nlohmann::json j = core::read_json(path);
  if (!j.is_object()) {
    error = "no planner " + name + " (planners/" + name + ".json)";
    return false;
  }
  out = Planner();
  out.name = name;
  out.file = j;
  out.system = j.value("system", "");
  out.doc = j.value("doc", "");
  out.state = j.value("state", "draft");
  out.fill = j.value("fill", nlohmann::json::object());
  if (out.system.empty()) {
    error = "planner " + name + " names no plan system";
    return false;
  }
  const nlohmann::json lots = j.value("lots", nlohmann::json::object());
  if (lots.contains("from") && !measure_lots(paths, runner, lots["from"], out.lots, error)) return false;
  for (const char *part : {"waterfront", "civic", "castle", "field"})
    if (lots.contains(part) && lots[part].is_object() && !measure_sized(paths, runner, lots[part], part, out.lots, error)) return false;
  if (out.lots.empty()) {
    error = "planner " + name + " has no lots (lots.from, lots.waterfront, lots.civic, lots.castle)";
    return false;
  }
  out.config = j.value("config", nlohmann::json::object());
  nlohmann::json lc = nlohmann::json::array(), ls = nlohmann::json::array(), cells = nlohmann::json::array(), wet = nlohmann::json::array(),
                 doors = nlohmann::json::array();
  for (size_t i = 0; i < out.lots.size(); ++i) {
    lc.push_back(out.lots[i].cls);
    ls.push_back(out.lots[i].shape);
    for (size_t k = 0; k + 1 < out.lots[i].cells.size(); k += 2) {
      cells.push_back(static_cast<int>(i));
      cells.push_back(out.lots[i].cells[k]);
      cells.push_back(out.lots[i].cells[k + 1]);
    }
    for (size_t k = 0; k + 1 < out.lots[i].doors.size(); k += 2) {
      doors.push_back(static_cast<int>(i));
      doors.push_back(out.lots[i].doors[k]);
      doors.push_back(out.lots[i].doors[k + 1]);
    }
    for (size_t k = 0; k + 1 < out.lots[i].wet.size(); k += 2) {
      wet.push_back(static_cast<int>(i));
      wet.push_back(out.lots[i].wet[k]);
      wet.push_back(out.lots[i].wet[k + 1]);
    }
  }
  out.config["lot_class"] = lc;
  out.config["lot_shape"] = ls;
  out.config["lot_cells"] = cells;
  out.config["lot_wet"] = wet;
  out.config["lot_doors"] = doors;
  return true;
}

}  // namespace plan
