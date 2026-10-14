#include "pipeline/config.h"

#include <algorithm>
#include <filesystem>

#include "core/util.h"
#include "json/registry.h"

namespace fs = std::filesystem;

namespace pipeline::config {

using nlohmann::json;

std::string dir(const forge::Paths &paths, const std::string &table) { return paths.workspace + "/config/" + table; }

std::string entry_path(const forge::Paths &paths, const std::string &table, const std::string &entry) {
  return dir(paths, table) + "/" + entry + ".json";
}

json table(const forge::Paths &paths, const std::string &table) {
  if (!core::valid_name(table)) return nullptr;
  json t = core::read_json(dir(paths, table) + "/table.json");
  return t.is_object() ? t : json();
}

std::vector<std::string> entries(const forge::Paths &paths, const std::string &table) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir(paths, table), ec)) {
    const std::string stem = e.path().stem().string();
    if (e.path().extension() == ".json" && stem != "table") out.push_back(stem);
  }
  std::sort(out.begin(), out.end());
  return out;
}

json entry(const forge::Paths &paths, const std::string &table, const std::string &entry) {
  if (!core::valid_name(table) || !core::valid_name(entry) || entry == "table") return nullptr;
  json e = core::read_json(entry_path(paths, table, entry));
  return e.is_object() && e.contains("value") ? e : json();
}

bool split(const std::string &path, std::string &table, std::string &entry) {
  const fs::path p(path);
  if (p.extension() != ".json" || p.parent_path().parent_path().filename() != "config") return false;
  table = p.parent_path().filename().string();
  entry = p.stem().string();
  return entry != "table";
}

json fields(const json &table) { return channel_fields(table.value("channel", "")); }

json blank(const std::string &channel) {
  json out = json::object();
  for (const auto &f : channel_fields(channel)) {
    const std::string type = f["type"], name = f["name"];
    if (f.contains("enum")) out[name] = f["enum"][0];
    else if (type.size() > 2 && type.substr(type.size() - 2) == "[]") out[name] = json::array();
    else if (type == "f32" || type == "u32" || type == "i32") out[name] = f.value("min", 0.0);
    else if (type == "bool") out[name] = false;
    else if (type == "string") out[name] = "";
    else if (type == "vec3") out[name] = {0, 0, 0};
    else out[name] = blank(type);
  }
  return out;
}

json channel_fields(const std::string &channel) {
  const auto *info = foundry::json::find_channel(channel);
  if (!info) return json::array();
  const json schema = json::parse(info->schema);
  json out = json::array();
  for (const auto &f : schema["fields"]) {
    json j = {{"name", f["name"]}, {"type", f["type"]}};
    for (const char *k : {"min", "max", "enum", "doc"})
      if (f.contains(k)) j[k] = f[k];
    out.push_back(j);
  }
  return out;
}

bool apply(const json &table, json &value, const json &values, std::string &error) {
  const auto *info = foundry::json::find_channel(table.value("channel", ""));
  if (!info) {
    error = "unknown channel " + table.value("channel", "");
    return false;
  }
  const json schema = json::parse(info->schema);
  json next = value;
  for (auto it = values.begin(); it != values.end(); ++it) {
    const json *f = nullptr;
    for (const auto &x : schema["fields"])
      if (x["name"] == it.key()) f = &x;
    if (!f) {
      error = "no field " + it.key() + " in " + info->name;
      return false;
    }
    const json &v = it.value();
    if (f->contains("enum")) {
      if (!v.is_string() || std::find((*f)["enum"].begin(), (*f)["enum"].end(), v) == (*f)["enum"].end()) {
        error = it.key() + " is one of " + (*f)["enum"].dump();
        return false;
      }
    } else if (v.is_number() && (f->contains("min") || f->contains("max"))) {
      const double x = v.get<double>();
      if ((f->contains("min") && x < (*f)["min"].get<double>()) || (f->contains("max") && x > (*f)["max"].get<double>())) {
        error = it.key() + " must be in [" + f->value("min", json()).dump() + ", " + f->value("max", json()).dump() + "]";
        return false;
      }
    }
    next[it.key()] = v;
  }
  try {
    std::vector<uint8_t> bytes;
    info->from_json(next, bytes);
  } catch (const std::exception &e) {
    error = std::string("doesn't fit ") + info->name + ": " + e.what();
    return false;
  }
  value = next;
  return true;
}

}  // namespace pipeline::config
