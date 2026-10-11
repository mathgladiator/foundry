#include "core/jobs.h"

#include <chrono>

#include "core/log.h"

namespace core {

namespace {
const auto g_epoch = std::chrono::steady_clock::now();
}

double Jobs::now() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count();
}

int Jobs::start(const std::string &title) {
  std::lock_guard<std::mutex> lock(mutex_);
  JobInfo j;
  j.id = next_++;
  j.title = title;
  j.progress = -1;
  j.started = now();
  jobs_.push_back(j);
  return j.id;
}

void Jobs::update(int id, float progress, const std::string &status) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &j : jobs_)
    if (j.id == id) {
      j.progress = progress;
      j.status = status;
    }
}

void Jobs::finish(int id) {
  std::string title;
  double took = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
      if (it->id == id) {
        title = it->title;
        took = now() - it->started;
        jobs_.erase(it);
        break;
      }
  }
  if (took > 1.0) log().add("job", title + " finished in " + std::to_string(static_cast<int>(took + 0.5)) + " s");
}

std::vector<JobInfo> Jobs::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_;
}

Jobs &jobs() {
  static Jobs j;
  return j;
}

}  // namespace core
