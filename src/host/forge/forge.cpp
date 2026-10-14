#include "forge/forge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"
#include "json/registry.h"

namespace fs = std::filesystem;

namespace forge {

std::string wasm_path(const Paths &p, const std::string &system) { return p.build + "/wasm/" + system + ".wasm"; }
std::string native_path(const Paths &p, const std::string &system) { return p.build + "/native/run_" + system; }

void dump_mismatch(const std::string &system, const std::vector<uint8_t> &input, const std::vector<uint8_t> &wasm,
                   const std::vector<uint8_t> &native) {
  const char *dir = std::getenv("FOUNDRY_DUMP_MISMATCH");
  if (!dir) return;
  auto put = [&](const std::string &name, const std::vector<uint8_t> &b) {
    if (FILE *f = std::fopen((std::string(dir) + "/" + system + "." + name).c_str(), "wb")) {
      std::fwrite(b.data(), 1, b.size(), f);
      std::fclose(f);
    }
  };
  put("input", input);
  put("wasm", wasm);
  put("native", native);
}

namespace {

std::string system_dir(const Paths &p, const std::string &system) { return p.root + "/src/systems/" + system; }

nlohmann::json manifest(const Paths &p, const std::string &system) {
  return core::read_json(system_dir(p, system) + "/system.json");
}

std::string stub_source(const std::string &id, const std::string &input, const std::string &output) {
  return "// " + id + ": TODO describe what this system does.\n"
         "#include \"boundary.h\"\n"
         "#include \"channels/" + input + ".h\"\n"
         "#include \"channels/" + output + ".h\"\n"
         "#include \"packer/" + input + ".h\"\n"
         "#include \"packer/" + output + ".h\"\n\n"
         "namespace " + id + " {\n\n"
         "bool run(const channels::" + input + " &in, channels::" + output + " &out, std::string &error) {\n"
         "  (void)in;\n  (void)out;\n"
         "  error = \"not implemented\";\n"
         "  return false;\n"
         "}\n\n"
         "}  // namespace " + id + "\n\n"
         "FOUNDRY_SYSTEM(" + id + ", channels::" + input + ", channels::" + output + ")\n";
}

}  // namespace

bool create_system(const Paths &p, const std::string &name, int version, const std::string &input,
                   const std::string &output, const std::string &doc, const std::string &source,
                   std::string &error) {
  if (!core::valid_name(name) || version < 1) {
    error = "name must be lowercase letters, digits, and underscores; version must be >= 1";
    return false;
  }
  if (!foundry::json::find_channel(input) || !foundry::json::find_channel(output)) {
    error = "unknown channel: input '" + input + "' or output '" + output + "' (see channel_list)";
    return false;
  }
  const std::string id = name + "_v" + std::to_string(version);
  const std::string dir = system_dir(p, id);
  if (fs::exists(dir)) {
    error = id + " already exists; pick a new version to fork";
    return false;
  }
  fs::create_directories(dir);
  nlohmann::json m = {{"name", name},       {"version", version}, {"kind", "system"}, {"doc", doc},
                      {"input", input},     {"output", output},   {"state", "draft"},
                      {"notes", nlohmann::json::array()},         {"created_at", core::now_iso()}};
  if (!core::write_json(dir + "/system.json", m) ||
      !core::write_text(dir + "/system.cpp", source.empty() ? stub_source(id, input, output) : source)) {
    error = "can't write files in " + dir;
    return false;
  }
  core::log().add("forge", "created system " + id);
  return true;
}

bool write_source(const Paths &p, const std::string &system, const std::string &source, std::string &error) {
  nlohmann::json m = manifest(p, system);
  if (!m.is_object()) {
    error = "no system " + system;
    return false;
  }
  if (m.value("state", "draft") == "final") {
    error = system + " is final and can't be edited; create a new version (fork) instead";
    return false;
  }
  if (!core::write_text(system_dir(p, system) + "/system.cpp", source)) {
    error = "can't write source";
    return false;
  }
  core::log().add("forge", "wrote source for " + system);
  return true;
}

BuildResult build(const Paths &p, const std::string &system) {
  BuildResult r;
  if (!manifest(p, system).is_object()) {
    r.log = "no system " + system;
    return r;
  }
  core::log().add("forge", "building " + system);
  core::Job job("Build: " + system);
  std::string out;
  int status = core::run_process({"cmake", "--build", p.build, "--target", "system_" + system}, out);
  r.ok = status == 0;
  if (out.size() > 6000) out = "...\n" + out.substr(out.size() - 6000);
  r.log = out;
  core::log().add("forge", system + (r.ok ? " built" : " build FAILED"));
  return r;
}

RunResult run_bytes(const Paths &p, wasm::Runner &runner, const std::string &system,
                    const std::vector<uint8_t> &input, bool verify_native) {
  RunResult r;
  r.input_bytes = input;
  nlohmann::json m = manifest(p, system);
  if (!m.is_object()) {
    r.error = "no system " + system;
    return r;
  }
  r.output_channel = m.value("output", "");
  wasm::RunResult w = runner.run(wasm_path(p, system), input);
  r.status = w.status;
  r.error = w.error;
  r.fuel = w.fuel_used;
  r.milliseconds = w.milliseconds;
  r.output_bytes = std::move(w.output);
  if (r.status == 0) {
    const auto *out = foundry::json::find_channel(r.output_channel);
    if (!out || !out->to_json(r.output_bytes.data(), r.output_bytes.size(), r.output)) {
      r.status = -1;
      r.error = "output does not unpack as " + r.output_channel;
    }
  }
  {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s in WebAssembly: status %d, %.1f ms, %llu fuel, %zu bytes in, %zu out",
                  system.c_str(), r.status, r.milliseconds, static_cast<unsigned long long>(r.fuel), input.size(),
                  r.output_bytes.size());
    core::log().add("wasm", buf);
  }
  if (verify_native) {
    r.native_checked = true;
    std::string err;
    std::vector<uint8_t> native_out;
    int status = core::run_process({native_path(p, system)}, err, &input, &native_out);
    if (status < 0) {
      r.native_error = err.empty() ? "native runner failed" : err;
    } else if (status != r.status) {
      r.native_error = "native status " + std::to_string(status) + " differs from WebAssembly status " +
                       std::to_string(r.status);
    } else {
      r.native_match = status != 0 || native_out == r.output_bytes;
      if (!r.native_match) {
        r.native_error = "native output bytes differ from WebAssembly output";
        dump_mismatch(system, input, r.output_bytes, native_out);
      }
    }
    core::log().add("native", system + (r.native_match ? ": native output matches WebAssembly" : ": " + r.native_error));
  }
  return r;
}

RunResult run(const Paths &p, wasm::Runner &runner, const std::string &system, const nlohmann::json &input,
              bool verify_native) {
  nlohmann::json m = manifest(p, system);
  RunResult r;
  if (!m.is_object()) {
    r.error = "no system " + system;
    return r;
  }
  const auto *in = foundry::json::find_channel(m.value("input", ""));
  if (!in) {
    r.error = "system's input channel '" + m.value("input", "") + "' is unknown";
    return r;
  }
  std::vector<uint8_t> bytes;
  try {
    in->from_json(input, bytes);
  } catch (const std::exception &e) {
    r.error = std::string("input doesn't match ") + in->name + ": " + e.what();
    return r;
  }
  return run_bytes(p, runner, system, bytes, verify_native);
}

nlohmann::json summarize(const nlohmann::json &j, size_t max_items) {
  if (j.is_array()) {
    if (j.size() <= max_items) {
      nlohmann::json out = nlohmann::json::array();
      for (const auto &v : j) out.push_back(summarize(v, max_items));
      return out;
    }
    nlohmann::json first = nlohmann::json::array();
    for (size_t i = 0; i < 3; ++i) first.push_back(j[i]);
    return {{"count", j.size()}, {"first", first}};
  }
  if (j.is_object()) {
    nlohmann::json out = nlohmann::json::object();
    for (auto it = j.begin(); it != j.end(); ++it) out[it.key()] = summarize(it.value(), max_items);
    return out;
  }
  return j;
}

}  // namespace forge
