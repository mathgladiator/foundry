// MCP tools for the system lifecycle: create, write, build, run.
#include "forge/tools.h"

#include "json/registry.h"

namespace forge {

void register_tools(mcp::Registry &registry, const Paths &paths, wasm::Runner &runner, workspace::Workspace &ws) {
  using nlohmann::json;
  registry.add({"channel_list", "List channels (the typed structures systems take in and put out) with their schemas.",
                mcp::schema(json::object()),
                [](const json &) {
                  json out = json::array();
                  for (const auto &c : foundry::json::channels()) out.push_back(json::parse(c.schema));
                  return mcp::Result::json(out);
                }});
  registry.add({"system_create",
                "Create a new system: src/systems/<name>/ with system.json and system.cpp. "
                "A system is one pure function from its input channel to its output channel. Omit source for a stub. "
                "An existing system is never overwritten; change it with system_write_source.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"version", {{"type", "integer"}}},
                             {"input", {{"type", "string"}, {"description", "input channel, e.g. mesh"}}},
                             {"output", {{"type", "string"}, {"description", "output channel, e.g. progressive_mesh"}}},
                             {"doc", {{"type", "string"}}},
                             {"source", {{"type", "string"}, {"description", "full system.cpp (optional)"}}}},
                            {"name", "input", "output", "doc"}),
                [paths, &ws](const json &args) {
                  std::string error;
                  if (!create_system(paths, args.value("name", ""), args.value("version", 0), args.value("input", ""),
                                     args.value("output", ""), args.value("doc", ""), args.value("source", ""), error))
                    return mcp::Result::error(error);
                  ws.scan();
                  std::string id = args.value("name", "");
                  return mcp::Result::text("created src/systems/" + id + "; build it with system_build");
                }});
  registry.add({"system_write_source", "Replace a system's system.cpp.",
                mcp::schema({{"system", {{"type", "string"}}}, {"source", {{"type", "string"}}}}, {"system", "source"}),
                [paths](const json &args) {
                  std::string error;
                  if (!write_source(paths, args.value("system", ""), args.value("source", ""), error))
                    return mcp::Result::error(error);
                  return mcp::Result::text("written");
                }});
  registry.add({"system_build",
                "Build a system both ways: the WebAssembly module the host runs and the native static library "
                "(plus its native runner). Returns the build log.",
                mcp::schema({{"system", {{"type", "string"}, {"description", "e.g. mesh_progressive"}}}}, {"system"}),
                [paths](const json &args) {
                  BuildResult r = build(paths, args.value("system", ""));
                  return r.ok ? mcp::Result::text("build ok\n" + r.log) : mcp::Result::error("build failed\n" + r.log);
                }});
  registry.add({"system_run",
                "Run a system in WebAssembly with a JSON input (its input channel). Returns the output (long arrays "
                "shortened), fuel, and time. verify_native also runs the native build and compares output bytes.",
                mcp::schema({{"system", {{"type", "string"}}},
                             {"input", {{"type", "object"}}},
                             {"verify_native", {{"type", "boolean"}, {"default", true}}}},
                            {"system", "input"}),
                [paths, &runner](const json &args) {
                  std::string system = args.value("system", "");
                  RunResult r = run(paths, runner, system, args.value("input", json::object()),
                                    args.value("verify_native", true));
                  json out = {{"status", r.status}, {"fuel", r.fuel}, {"milliseconds", r.milliseconds}};
                  if (r.native_checked) {
                    out["native_matches_wasm"] = r.native_match;
                    if (!r.native_error.empty()) out["native_error"] = r.native_error;
                  }
                  if (r.status != 0) {
                    out["error"] = r.error;
                    return mcp::Result{{{"text", json(out).dump(2), "", ""}}, true};
                  }
                  out["output_channel"] = r.output_channel;
                  out["output"] = summarize(r.output);
                  return mcp::Result::json(out);
                }});
}

}  // namespace forge
