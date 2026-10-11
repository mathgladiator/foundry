// Runs systems compiled to WebAssembly, using Wasmtime.
//
// Every run gets a fresh store and instance, so runs can't affect each other.
// WASI is linked with no preopened directories, no environment, and no
// arguments: a system can compute, but it can't reach the host.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wasm {

struct RunResult {
  // 0 = ok; 1 = the system reported an error; 2 = the input didn't unpack;
  // -1 = the host failed (load, instantiate, trap, out of fuel).
  int status = -1;
  std::vector<uint8_t> output;  // the packed output channel when status == 0
  std::string error;            // the message when status != 0
  uint64_t fuel_used = 0;
  double milliseconds = 0;
};

class Runner {
 public:
  Runner();
  ~Runner();
  // Runs foundry_run in the module at `path` with `input` (a packed channel).
  RunResult run(const std::string &path, const std::vector<uint8_t> &input);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wasm
