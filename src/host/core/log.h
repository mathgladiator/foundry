// The activity log: what the agent and the host are doing, shown in the UI.
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace core {

struct LogEntry {
  std::string time;  // HH:MM:SS
  std::string source;  // e.g. "mcp", "build", "meshy"
  std::string text;
};

class Log {
 public:
  void add(const std::string &source, const std::string &text);
  std::vector<LogEntry> snapshot() const;

 private:
  mutable std::mutex mutex_;
  std::vector<LogEntry> entries_;
};

Log &log();

}  // namespace core
