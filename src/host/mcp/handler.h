// The MCP handler interface (ORG.md, "src/host"). The server speaks the
// protocol and dispatches to tools; tools never see HTTP or JSON-RPC.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp {

// One piece of a tool result: text, or an image.
struct Content {
  std::string type;  // "text" or "image"
  std::string text;  // for text
  std::string data;  // base64, for images
  std::string mime;  // for images, e.g. "image/png"
};

struct Result {
  std::vector<Content> content;
  bool is_error = false;

  static Result text(const std::string &t) { return {{{"text", t, "", ""}}, false}; }
  static Result json(const nlohmann::json &j) { return text(j.dump(2)); }
  static Result error(const std::string &t) { return {{{"text", t, "", ""}}, true}; }
};

struct Tool {
  std::string name;
  std::string description;
  nlohmann::json input_schema;  // a JSON Schema object
  std::function<Result(const nlohmann::json &args)> call;
};

class Registry {
 public:
  void add(Tool tool);
  const std::vector<Tool> &tools() const { return tools_; }
  const Tool *find(const std::string &name) const;

 private:
  std::vector<Tool> tools_;
};

// Helpers for building input schemas.
nlohmann::json schema(nlohmann::json properties, std::vector<std::string> required = {});

}  // namespace mcp
