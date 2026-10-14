#include "wasm/runner.h"

#include <sys/stat.h>

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <wasmtime.h>

#include "core/util.h"

namespace wasm {

namespace {

constexpr uint64_t kFuel = 60'000'000'000ull;           // instructions, roughly (a boundary at a 12 km view distance needs over 20 billion)
constexpr int64_t kMemoryLimit = 1024ll * 1024 * 1024;  // bytes per instance

std::string error_text(wasmtime_error_t *error, wasm_trap_t *trap) {
  wasm_byte_vec_t msg;
  if (error) {
    wasmtime_error_message(error, &msg);
    wasmtime_error_delete(error);
  } else {
    wasm_trap_message(trap, &msg);
    wasm_trap_delete(trap);
  }
  std::string s(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  return s;
}

}  // namespace

struct Runner::Impl {
  wasm_engine_t *engine = nullptr;
  std::mutex mutex;
  struct Cached {
    wasmtime_module_t *module = nullptr;
    int64_t mtime = 0;
  };
  std::map<std::string, Cached> modules;

  Impl() {
    wasm_config_t *config = wasm_config_new();
    wasmtime_config_consume_fuel_set(config, true);
    engine = wasm_engine_new_with_config(config);
  }
  ~Impl() {
    for (auto &[path, c] : modules) wasmtime_module_delete(c.module);
    wasm_engine_delete(engine);
  }

  // Loads (or reuses) a compiled module; recompiles when the file changes.
  wasmtime_module_t *load(const std::string &path, std::string &err) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
      err = "no module at " + path + " (has the system been built?)";
      return nullptr;
    }
    int64_t mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = modules.find(path);
    if (it != modules.end() && it->second.mtime == mtime) return it->second.module;
    std::vector<uint8_t> bytes;
    if (!core::read_file(path, bytes)) {
      err = "can't read " + path;
      return nullptr;
    }
    wasmtime_module_t *module = nullptr;
    if (wasmtime_error_t *e = wasmtime_module_new(engine, bytes.data(), bytes.size(), &module)) {
      err = "compile failed: " + error_text(e, nullptr);
      return nullptr;
    }
    if (it != modules.end()) wasmtime_module_delete(it->second.module);
    modules[path] = {module, mtime};
    return module;
  }
};

Runner::Runner() : impl_(std::make_unique<Impl>()) {}
Runner::~Runner() = default;

RunResult Runner::run(const std::string &path, const std::vector<uint8_t> &input) {
  RunResult r;
  auto start = std::chrono::steady_clock::now();
  wasmtime_module_t *module = impl_->load(path, r.error);
  if (!module) return r;

  wasmtime_store_t *store = wasmtime_store_new(impl_->engine, nullptr, nullptr);
  wasmtime_store_limiter(store, kMemoryLimit, -1, -1, -1, -1);
  wasmtime_context_t *ctx = wasmtime_store_context(store);
  wasmtime_linker_t *linker = wasmtime_linker_new(impl_->engine);
  auto done = [&](const std::string &err) {
    if (!err.empty()) {
      r.status = -1;
      r.error = err;
    }
    uint64_t left = 0;
    if (!wasmtime_context_get_fuel(ctx, &left)) r.fuel_used = kFuel - left;
    wasmtime_linker_delete(linker);
    wasmtime_store_delete(store);
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
  };

  wasmtime_context_set_fuel(ctx, kFuel);
  if (wasmtime_error_t *e = wasmtime_linker_define_wasi(linker)) return done("wasi: " + error_text(e, nullptr));
  // No preopens, no environment, no arguments, no inherited stdio.
  if (wasmtime_error_t *e = wasmtime_context_set_wasi(ctx, wasi_config_new())) return done("wasi: " + error_text(e, nullptr));

  wasmtime_instance_t instance;
  wasm_trap_t *trap = nullptr;
  if (wasmtime_error_t *e = wasmtime_linker_instantiate(linker, ctx, module, &instance, &trap))
    return done("instantiate: " + error_text(e, nullptr));
  if (trap) return done("instantiate trapped: " + error_text(nullptr, trap));

  auto get = [&](const char *name, wasmtime_extern_t &out) {
    return wasmtime_instance_export_get(ctx, &instance, name, std::strlen(name), &out);
  };
  auto call = [&](const char *name, std::vector<int32_t> args, int32_t *result, std::string &err) {
    wasmtime_extern_t fn;
    if (!get(name, fn) || fn.kind != WASMTIME_EXTERN_FUNC) {
      err = std::string("module has no export ") + name;
      return false;
    }
    std::vector<wasmtime_val_t> in(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      in[i].kind = WASMTIME_I32;
      in[i].of.i32 = args[i];
    }
    wasmtime_val_t out;
    wasm_trap_t *t = nullptr;
    if (wasmtime_error_t *e = wasmtime_func_call(ctx, &fn.of.func, in.data(), in.size(), &out, result ? 1 : 0, &t)) {
      err = std::string(name) + ": " + error_text(e, nullptr);
      return false;
    }
    if (t) {
      err = std::string(name) + " trapped: " + error_text(nullptr, t);
      return false;
    }
    if (result) *result = out.of.i32;
    return true;
  };

  std::string err;
  wasmtime_extern_t init;
  if (get("_initialize", init) && !call("_initialize", {}, nullptr, err)) return done(err);
  wasmtime_extern_t mem;
  if (!get("memory", mem) || mem.kind != WASMTIME_EXTERN_MEMORY) return done("module exports no memory");

  int32_t ptr = 0, status = 0, out_ptr = 0, out_len = 0;
  if (!call("foundry_alloc", {static_cast<int32_t>(input.size())}, &ptr, err)) return done(err);
  {
    uint8_t *data = wasmtime_memory_data(ctx, &mem.of.memory);
    size_t size = wasmtime_memory_data_size(ctx, &mem.of.memory);
    if (static_cast<size_t>(static_cast<uint32_t>(ptr)) + input.size() > size) return done("foundry_alloc returned a bad pointer");
    std::memcpy(data + static_cast<uint32_t>(ptr), input.data(), input.size());
  }
  if (!call("foundry_run", {ptr, static_cast<int32_t>(input.size())}, &status, err)) return done(err);
  if (!call("foundry_output_ptr", {}, &out_ptr, err)) return done(err);
  if (!call("foundry_output_len", {}, &out_len, err)) return done(err);
  {
    // Memory may have grown during the run, so look it up again.
    uint8_t *data = wasmtime_memory_data(ctx, &mem.of.memory);
    size_t size = wasmtime_memory_data_size(ctx, &mem.of.memory);
    uint32_t p = static_cast<uint32_t>(out_ptr), n = static_cast<uint32_t>(out_len);
    if (static_cast<size_t>(p) + n > size) return done("output is outside the module's memory");
    if (status == 0) {
      r.output.assign(data + p, data + p + n);
    } else {
      r.error.assign(reinterpret_cast<const char *>(data + p), n);
    }
  }
  r.status = status;
  return done("");
}

}  // namespace wasm
