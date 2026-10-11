// Background jobs: long-running work (LOD searches, benchmarks, Meshy tasks,
// builds) registers here so the UI's status bar can show progress.
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace core {

struct JobInfo {
  int id = 0;
  std::string title;   // e.g. "LOD search: garden_gnome"
  std::string status;  // e.g. "LOD1: 812 triangles, 4.7% error"
  float progress = 0;  // 0 to 1; negative means indeterminate
  double started = 0;  // seconds since the jobs registry was created
};

class Jobs {
 public:
  int start(const std::string &title);
  void update(int id, float progress, const std::string &status);
  void finish(int id);
  std::vector<JobInfo> snapshot() const;
  double now() const;

 private:
  mutable std::mutex mutex_;
  std::vector<JobInfo> jobs_;
  int next_ = 1;
};

Jobs &jobs();

// Registers a job for the lifetime of the scope.
class Job {
 public:
  explicit Job(const std::string &title) : id_(jobs().start(title)) {}
  ~Job() { jobs().finish(id_); }
  void update(float progress, const std::string &status) { jobs().update(id_, progress, status); }
  Job(const Job &) = delete;
  Job &operator=(const Job &) = delete;

 private:
  int id_;
};

}  // namespace core
