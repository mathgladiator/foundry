#include "core/log.h"

#include <cstdio>
#include <ctime>

namespace core {

void Log::add(const std::string &source, const std::string &text) {
  std::time_t t = std::time(nullptr);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back({buf, source, text});
  if (entries_.size() > 500) entries_.erase(entries_.begin(), entries_.begin() + 100);
  std::fprintf(stderr, "[%s] %s: %s\n", buf, source.c_str(), text.c_str());
}

std::vector<LogEntry> Log::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

Log &log() {
  static Log instance;
  return instance;
}

}  // namespace core
