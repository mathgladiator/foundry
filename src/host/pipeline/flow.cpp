#include "pipeline/flow.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace pipeline {

namespace {

using nlohmann::json;

void bindings(const json &b, const std::string &path, std::set<std::string> &params,
              std::vector<std::pair<std::string, std::string>> &binds, std::vector<FlowEdge> &edges) {
  if (b.is_object()) {
    for (auto it = b.begin(); it != b.end(); ++it)
      bindings(it.value(), path.empty() ? it.key() : path + "." + it.key(), params, binds, edges);
    return;
  }
  if (b.is_array()) {
    for (const auto &v : b) bindings(v, path, params, binds, edges);
    return;
  }
  if (!b.is_string()) return;
  const std::string s = b.get<std::string>();
  if (s.size() > 1 && s[0] == '$') {
    params.insert(s.substr(1));
    binds.push_back({s.substr(1), path});
  }
  if (s.size() > 1 && s[0] == '@') {
    const size_t dot = s.find('.');
    FlowEdge e{s.substr(1, dot == std::string::npos ? std::string::npos : dot - 1),
               dot == std::string::npos ? "" : s.substr(dot + 1)};
    for (const auto &x : edges)
      if (x.from == e.from && x.field == e.field) return;
    edges.push_back(e);
  }
}

std::string count_text(double n) {
  if (n <= 0) return "unbounded";
  char buf[64];
  if (n < 1e6) std::snprintf(buf, sizeof buf, "%.0f", n);
  else std::snprintf(buf, sizeof buf, "%.2g", n);
  return buf;
}

}  // namespace

Flow analyze(const Definition &def) {
  Flow f;
  std::map<std::string, double> card;
  for (const auto &p : def.params) {
    FlowParam fp;
    fp.name = p.name;
    fp.type = p.type;
    if (p.type == "config") {
      fp.table = p.table;
      fp.entries = p.values.size();
    }
    if (p.type == "enum" || p.type == "config") fp.cardinality = static_cast<double>(p.values.size());
    else if (p.type == "u32") fp.cardinality = p.max - p.min + 1;
    fp.cardinality_text = p.type == "time" || p.type == "event" ? "every frame"
                          : p.type == "f32"                       ? "continuous"
                                                                  : count_text(fp.cardinality);
    card[p.name] = fp.cardinality;
    f.params.push_back(fp);
  }
  std::map<std::string, size_t> index;
  for (const auto &s : def.stages) {
    FlowStage fs;
    fs.id = s.id;
    fs.system = s.system;
    fs.phase = s.phase.empty() ? "build" : s.phase;
    fs.in = s.input_channel;
    fs.out = s.output_channel;
    std::set<std::string> direct;
    bindings(s.input, "", direct, fs.binds, fs.upstream);
    fs.direct.assign(direct.begin(), direct.end());
    std::set<std::string> all = direct;
    for (const auto &e : fs.upstream) {
      auto it = index.find(e.from);
      if (it == index.end()) continue;
      const FlowStage &up = f.stages[it->second];
      all.insert(up.all.begin(), up.all.end());
      fs.depth = std::max(fs.depth, up.depth + 1);
    }
    fs.all.assign(all.begin(), all.end());
    double n = 1;
    for (const auto &p : fs.all) n = card[p] > 0 && n > 0 ? n * card[p] : 0;
    fs.distinct = n;
    bool clock = false;
    for (const auto &p : fs.all)
      for (const auto &dp : def.params)
        if (dp.name == p && (dp.type == "time" || dp.type == "event")) clock = true;
    fs.distinct_text = clock ? "every frame, per instance" : n > 0 ? count_text(n) : "unbounded (continuous inputs)";
    index[fs.id] = f.stages.size();
    f.stages.push_back(fs);
  }
  for (auto &p : f.params)
    for (const auto &s : f.stages)
      if (std::find(s.all.begin(), s.all.end(), p.name) != s.all.end()) p.reruns.push_back(s.id);
  return f;
}

json Flow::to_json() const {
  json ps = json::array(), ss = json::array();
  for (const auto &p : params) {
    json j = {{"name", p.name}, {"type", p.type}, {"cardinality", p.cardinality_text}, {"reruns", p.reruns}};
    if (!p.table.empty()) j["config"] = {{"table", p.table}, {"entries", p.entries}};
    ps.push_back(j);
  }
  for (const auto &s : stages) {
    json up = json::array();
    for (const auto &e : s.upstream) up.push_back(e.field.empty() ? "@" + e.from : "@" + e.from + "." + e.field);
    json binds = json::object();
    for (const auto &[param, field] : s.binds) binds[field] = "$" + param;
    ss.push_back({{"id", s.id}, {"system", s.system}, {"phase", s.phase}, {"in", s.in}, {"out", s.out},
                  {"reads", up}, {"params", s.direct}, {"binds", binds}, {"cache_key", s.all},
                  {"distinct_outputs", s.distinct_text}});
  }
  return {{"params", ps}, {"stages", ss}};
}

json probe(Runner &runner, const Definition &def, const json &base_in, int samples) {
  const json base = runner.complete(def, base_in);
  Runner::Result ref = runner.run(def, base, false, false, true);
  json out = {{"base", base}, {"params", json::array()}};
  if (!ref.ok) {
    out["error"] = ref.error;
    return out;
  }
  for (const auto &p : def.params) {
    std::vector<json> alts;
    if (p.type == "enum" || p.type == "config") {
      for (const auto &v : p.values)
        if (json(v) != base[p.name] && static_cast<int>(alts.size()) < samples) alts.push_back(v);
    } else if (p.type == "u32" || p.type == "f32") {
      for (int k = 0; k < samples; ++k) {
        double x = p.min + (p.max - p.min) * (k + 0.5) / samples;
        json v = p.type == "u32" ? json(static_cast<uint32_t>(x)) : json(x);
        if (v != base[p.name]) alts.push_back(v);
      }
    } else {  // a clock: a few moments
      for (double t : {0.7, 2.3, 4.1}) alts.push_back(t);
    }
    json changed = json::object();
    for (const auto &s : def.stages) changed[s.id] = false;
    for (const auto &v : alts) {
      json values = base;
      values[p.name] = v;
      Runner::Result r = runner.run(def, values, false, false, true);
      if (!r.ok) continue;
      for (const auto &s : def.stages)
        if (r.output_hashes[s.id] != ref.output_hashes[s.id]) changed[s.id] = true;
    }
    out["params"].push_back({{"name", p.name}, {"tried", alts}, {"changes", changed}});
  }
  return out;
}

}  // namespace pipeline
