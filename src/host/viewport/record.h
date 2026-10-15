// A video of a view (design/map.md, "Videos"): frames read back from a GPU texture and piped to
// ffmpeg as H.264 in MP4 (yuv420p, faststart), the same encoding as the QA videos. ffmpeg runs as its
// own process, so its encoders never link into the foundry.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdio>
#include <string>
#include <vector>

namespace viewport {

class Recorder {
 public:
  ~Recorder();
  // Starts a video at path, width x height (each made even), at fps frames a second.
  bool open(SDL_GPUDevice *device, const std::string &path, int width, int height, int fps, std::string &error);
  // Adds the texture (RGBA8, the size opened with) as the next frame: copied off the GPU and waited for.
  bool add(SDL_GPUTexture *texture);
  // Finishes the file; the number of frames written.
  int close();
  bool recording() const { return pipe_ != nullptr; }
  int frames() const { return frames_; }
  int width() const { return w_; }
  int height() const { return h_; }
  int fps() const { return fps_; }
  const std::string &path() const { return path_; }

 private:
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUTransferBuffer *buffer_ = nullptr;
  FILE *pipe_ = nullptr;
  int w_ = 0, h_ = 0, fps_ = 30, frames_ = 0;
  std::string path_;
  std::vector<uint8_t> row_;
};

}  // namespace viewport
