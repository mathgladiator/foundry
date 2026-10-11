// The forge: the system lifecycle. Scaffolds systems, writes their source,
// builds them (WebAssembly and native), and runs them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wasm/runner.h"

namespace forge {

struct Paths {
  std::string root;       // source root (src/systems, src/channels)
  std::string workspace;  // workspace root (the project's data)
  std::string build;  // CMake build directory
};

std::string wasm_path(const Paths &p, const std::string &system);
std::string native_path(const Paths &p, const std::string &system);

// Creates src/systems/<name>_v<version>/ with a manifest and a source file
// (a stub that reports "not implemented" when `source` is empty).
bool create_system(const Paths &p, const std::string &name, int version, const std::string &input,
                   const std::string &output, const std::string &doc, const std::string &source,
                   std::string &error);

// Replaces a system's system.cpp. Refuses final systems: fork them instead.
bool write_source(const Paths &p, const std::string &system, const std::string &source, std::string &error);

struct BuildResult {
  bool ok = false;
  std::string log;  // the build output (tail)
};
BuildResult build(const Paths &p, const std::string &system);

struct RunResult {
  int status = -1;            // as wasm::RunResult
  std::string error;
  std::vector<uint8_t> input_bytes;
  std::vector<uint8_t> output_bytes;
  nlohmann::json output;      // the output channel as JSON
  std::string output_channel;
  uint64_t fuel = 0;
  double milliseconds = 0;
  bool native_checked = false;
  bool native_match = false;
  std::string native_error;
};

// Runs a system in WebAssembly with a JSON input (its input channel). With
// verify_native, also runs the native runner and compares output bytes.
RunResult run(const Paths &p, wasm::Runner &runner, const std::string &system, const nlohmann::json &input,
              bool verify_native);

// Same, with already-packed input bytes.
RunResult run_bytes(const Paths &p, wasm::Runner &runner, const std::string &system,
                    const std::vector<uint8_t> &input, bool verify_native);

// Shortens long arrays in a JSON value for display ({"count": N, "first": [...]}).
nlohmann::json summarize(const nlohmann::json &j, size_t max_items = 12);

}  // namespace forge
