#include "core/config.h"

#include <cstdlib>
#include <fstream>

#include <nlohmann/json.hpp>

namespace core {

std::string expand_home(const std::string &path) {
  if (path.rfind("~/", 0) == 0) {
    const char *home = std::getenv("HOME");
    if (home) return std::string(home) + path.substr(1);
  }
  return path;
}

bool load_config(const std::string &root, const std::string &build, const std::string &workspace, Config &config,
                 std::string &error) {
  config.root = root;
  config.build = build;
  config.workspace = workspace;
  std::ifstream f(workspace + "/foundry.json");
  if (!f) return true;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    if (j.contains("mcp")) {
      config.mcp_host = j["mcp"].value("host", config.mcp_host);
      config.mcp_port = j["mcp"].value("port", config.mcp_port);
    }
    if (j.contains("meshy")) {
      config.meshy_keys_file = j["meshy"].value("keys_file", config.meshy_keys_file);
      config.meshy_budget_credits = j["meshy"].value("budget_credits", config.meshy_budget_credits);
    }
  } catch (const std::exception &e) {
    error = std::string("foundry.json: ") + e.what();
    return false;
  }
  return true;
}

std::string read_env_key(const std::string &path, const std::string &key) {
  std::ifstream f(expand_home(path));
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    auto eq = line.find('=');
    if (eq == std::string::npos || line.substr(0, eq) != key) continue;
    std::string value = line.substr(eq + 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
      value = value.substr(1, value.size() - 2);
    return value;
  }
  return {};
}

}  // namespace core
