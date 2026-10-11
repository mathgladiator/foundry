#include "app/capture.h"

#include <chrono>

namespace app {

bool Capture::request(viewport::Image &out, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!ui_running_) return false;
  int ticket = ++requested_;
  bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return served_ >= ticket; });
  if (ok) out = image_;
  return ok;
}

bool Capture::wanted() {
  std::lock_guard<std::mutex> lock(mutex_);
  return requested_ > served_;
}

void Capture::fulfil(viewport::Image image) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    image_ = std::move(image);
    served_ = requested_;
  }
  cv_.notify_all();
}

void Capture::set_ui_running(bool running) {
  std::lock_guard<std::mutex> lock(mutex_);
  ui_running_ = running;
}

Capture &capture() {
  static Capture c;
  return c;
}

}  // namespace app
