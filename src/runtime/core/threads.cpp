#include "core/threads.h"

namespace runtime {

Pool::Pool(unsigned workers) {
  for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
}

Pool::~Pool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto &t : workers_) t.join();
}

bool Pool::take(size_t &begin, size_t &end) {
  if (!fn_ || next_ >= count_) return false;
  begin = next_;
  end = begin + chunk_ < count_ ? begin + chunk_ : count_;
  next_ = end;
  ++busy_;
  return true;
}

void Pool::work() {
  unsigned seen = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stop_ || (batch_ != seen && fn_ && next_ < count_); });
    if (stop_) return;
    seen = batch_;
    size_t b, e;
    while (take(b, e)) {
      const auto *fn = fn_;
      lock.unlock();
      (*fn)(b, e);
      lock.lock();
      if (--busy_ == 0 && next_ >= count_) done_.notify_all();
    }
  }
}

void Pool::parallel_for(size_t count, size_t chunk, const std::function<void(size_t, size_t)> &fn) {
  if (count == 0) return;
  if (workers_.empty() || count <= chunk) {
    fn(0, count);
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  fn_ = &fn;
  next_ = 0;
  count_ = count;
  chunk_ = chunk ? chunk : 1;
  ++batch_;
  wake_.notify_all();
  size_t b, e;
  while (take(b, e)) {
    lock.unlock();
    fn(b, e);
    lock.lock();
    --busy_;
  }
  done_.wait(lock, [&] { return busy_ == 0 && next_ >= count_; });
  fn_ = nullptr;
}

Queue::Queue(unsigned workers) {
  for (unsigned i = 0; i < workers; ++i)
    workers_.emplace_back([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
        if (stop_ && jobs_.empty()) return;
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        ++running_;
        lock.unlock();
        job();
        lock.lock();
        --running_;
      }
    });
}

Queue::~Queue() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto &t : workers_) t.join();
}

void Queue::push(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  wake_.notify_one();
}

size_t Queue::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_.size() + running_;
}

}  // namespace runtime
