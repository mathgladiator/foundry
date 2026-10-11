#include "mcp/server.h"

#include <atomic>
#include <thread>

#include <httplib.h>

#include "core/log.h"

namespace mcp {

void Registry::add(Tool tool) { tools_.push_back(std::move(tool)); }

const Tool *Registry::find(const std::string &name) const {
  for (const auto &t : tools_)
    if (t.name == name) return &t;
  return nullptr;
}

nlohmann::json schema(nlohmann::json properties, std::vector<std::string> required) {
  nlohmann::json s = {{"type", "object"}, {"properties", std::move(properties)}};
  if (!required.empty()) s["required"] = required;
  return s;
}

namespace {

const char *kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

nlohmann::json rpc_result(const nlohmann::json &id, nlohmann::json result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json rpc_error(const nlohmann::json &id, int code, const std::string &message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::json content_json(const Content &c) {
  if (c.type == "image") return {{"type", "image"}, {"data", c.data}, {"mimeType", c.mime}};
  return {{"type", "text"}, {"text", c.text}};
}

}  // namespace

struct Server::Impl {
  const Registry &registry;
  httplib::Server http;
  std::thread thread;
  std::string host;
  int port = 0;
  std::atomic<int> requests{0};

  explicit Impl(const Registry &r) : registry(r) {}

  // Handles one JSON-RPC message. Returns null for notifications.
  nlohmann::json handle(const nlohmann::json &msg) {
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method"))
      return rpc_error(msg.is_object() ? msg.value("id", nlohmann::json()) : nullptr, -32600, "invalid request");
    const std::string method = msg["method"];
    const bool notification = !msg.contains("id");
    const nlohmann::json id = notification ? nlohmann::json() : msg["id"];
    const nlohmann::json params = msg.value("params", nlohmann::json::object());
    if (notification) return nullptr;

    if (method == "initialize") {
      std::string requested = params.value("protocolVersion", "");
      std::string version = kProtocolVersions[0];
      for (const char *v : kProtocolVersions)
        if (requested == v) version = v;
      return rpc_result(id, {{"protocolVersion", version},
                             {"capabilities", {{"tools", {{"listChanged", false}}}}},
                             {"serverInfo", {{"name", "foundry"}, {"version", "0.1.0"}}},
                             {"instructions",
                              "The foundry workspace. Use workspace_list to see pipelines, systems, "
                              "channels, fixed assets, and concept art. Only the human approves; "
                              "agents build, run, view, and leave notes."}});
    }
    if (method == "ping") return rpc_result(id, nlohmann::json::object());
    if (method == "tools/list") {
      nlohmann::json tools = nlohmann::json::array();
      for (const auto &t : registry.tools())
        tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
      return rpc_result(id, {{"tools", tools}});
    }
    if (method == "tools/call") {
      const std::string name = params.value("name", "");
      const Tool *tool = registry.find(name);
      if (!tool) return rpc_error(id, -32602, "unknown tool: " + name);
      nlohmann::json args = params.value("arguments", nlohmann::json::object());
      core::log().add("mcp", "call " + name + " " + args.dump().substr(0, 160));
      Result result;
      try {
        result = tool->call(args);
      } catch (const std::exception &e) {
        result = Result::error(std::string("tool failed: ") + e.what());
      }
      if (result.is_error) core::log().add("mcp", name + " failed: " + (result.content.empty() ? "" : result.content[0].text.substr(0, 200)));
      nlohmann::json content = nlohmann::json::array();
      for (const auto &c : result.content) content.push_back(content_json(c));
      return rpc_result(id, {{"content", content}, {"isError", result.is_error}});
    }
    return rpc_error(id, -32601, "method not found: " + method);
  }
};

Server::Server(const Registry &registry) : impl_(std::make_unique<Impl>(registry)) {}

Server::~Server() { stop(); }

bool Server::start(const std::string &host, int port) {
  impl_->host = host;
  impl_->port = port;
  auto &http = impl_->http;
  http.Post("/mcp", [this](const httplib::Request &req, httplib::Response &res) {
    ++impl_->requests;
    // Only accept browser-free local clients: reject cross-origin requests.
    if (req.has_header("Origin")) {
      std::string origin = req.get_header_value("Origin");
      if (origin.find("://localhost") == std::string::npos && origin.find("://127.0.0.1") == std::string::npos) {
        res.status = 403;
        return;
      }
    }
    nlohmann::json msg;
    try {
      msg = nlohmann::json::parse(req.body);
    } catch (const std::exception &) {
      res.set_content(rpc_error(nullptr, -32700, "parse error").dump(), "application/json");
      return;
    }
    nlohmann::json reply;
    if (msg.is_array()) {
      reply = nlohmann::json::array();
      for (const auto &m : msg) {
        auto r = impl_->handle(m);
        if (!r.is_null()) reply.push_back(r);
      }
      if (reply.empty()) reply = nullptr;
    } else {
      reply = impl_->handle(msg);
    }
    if (reply.is_null()) {
      res.status = 202;  // notifications and responses get no body
      return;
    }
    res.set_content(reply.dump(), "application/json");
  });
  http.Get("/mcp", [](const httplib::Request &, httplib::Response &res) { res.status = 405; });
  http.Delete("/mcp", [](const httplib::Request &, httplib::Response &res) { res.status = 405; });
  if (!http.bind_to_port(host, port)) return false;
  impl_->thread = std::thread([this] { impl_->http.listen_after_bind(); });
  core::log().add("mcp", "listening on " + url());
  return true;
}

void Server::stop() {
  if (impl_ && impl_->thread.joinable()) {
    impl_->http.stop();
    impl_->thread.join();
  }
}

std::string Server::url() const {
  return "http://" + impl_->host + ":" + std::to_string(impl_->port) + "/mcp";
}

int Server::requests() const { return impl_->requests; }

}  // namespace mcp
