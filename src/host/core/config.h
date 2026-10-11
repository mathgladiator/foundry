// Host configuration, read from foundry.json at the workspace root.
#pragma once

#include <string>

namespace core {

struct Config {
  std::string root;       // the source root (src/, the build)
  std::string workspace;  // the workspace root: the project's data, and foundry.json
  std::string build;  // the CMake build directory
  std::string mcp_host = "127.0.0.1";
  int mcp_port = 7878;
  std::string meshy_keys_file = "~/.config/hexbound/keys.env";
  int meshy_budget_credits = 250;
};

// Loads <workspace>/foundry.json over the defaults. Returns false (with a
// message) only if the file exists and can't be parsed.
bool load_config(const std::string &root, const std::string &build, const std::string &workspace, Config &config,
                 std::string &error);

// Reads KEY=VALUE lines from an env-style file (path may start with ~).
// Returns an empty string if the file or key is missing.
std::string read_env_key(const std::string &path, const std::string &key);

std::string expand_home(const std::string &path);

}  // namespace core
