// Threads for the runtime: a pool that splits one batch across workers (the
// update broadcast, design/submit.md), and a queue of background jobs
// (generation).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace runtime {

// Runs fn(begin, end) over [0, count) in chunks, on the workers and the
// calling thread, and returns when every chunk is done.
class Pool {
 public:
  explicit Pool(unsigned workers);
  ~Pool();
  void parallel_for(size_t count, size_t chunk, const std::function<void(size_t, size_t)> &fn);
  unsigned threads() const { return static_cast<unsigned>(workers_.size()) + 1; }

 private:
  void work();
  bool take(size_t &begin, size_t &end);
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  const std::function<void(size_t, size_t)> *fn_ = nullptr;
  size_t next_ = 0, count_ = 0, chunk_ = 1, busy_ = 0;
  unsigned batch_ = 0;
  bool stop_ = false;
};

// Runs jobs in the background, first in first out.
class Queue {
 public:
  explicit Queue(unsigned workers);
  ~Queue();
  void push(std::function<void()> job);
  size_t pending() const;

 private:
  std::vector<std::thread> workers_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  size_t running_ = 0;
  bool stop_ = false;
};

}  // namespace runtime
