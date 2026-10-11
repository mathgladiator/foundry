// The MCP server: JSON-RPC 2.0 over Streamable HTTP, on localhost.
// It answers each POST with a single JSON response and offers no SSE stream.
#pragma once

#include <memory>
#include <string>

#include "mcp/handler.h"

namespace mcp {

class Server {
 public:
  explicit Server(const Registry &registry);
  ~Server();
  // Starts listening on a background thread. Returns false if the port is taken.
  bool start(const std::string &host, int port);
  void stop();
  std::string url() const;
  int requests() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mcp
