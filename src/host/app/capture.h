// Window capture for the ui_screenshot tool: an MCP thread asks, the UI
// thread renders the next frame into an offscreen texture and reads it back.
#pragma once

#include <condition_variable>
#include <mutex>

#include "viewport/image.h"

namespace app {

class Capture {
 public:
  // Called from a tool thread. Waits up to timeout_ms; false if no UI answered.
  bool request(viewport::Image &out, int timeout_ms);
  // Called by the UI thread each frame.
  bool wanted();
  void fulfil(viewport::Image image);
  // Marks the UI as running (or not), so requests fail fast without a window.
  void set_ui_running(bool running);

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool ui_running_ = false;
  int requested_ = 0, served_ = 0;
  viewport::Image image_;
};

Capture &capture();

}  // namespace app
