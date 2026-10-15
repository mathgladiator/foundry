#include "viewport/record.h"

#include <algorithm>
#include <filesystem>

namespace viewport {

namespace {

std::string quote(const std::string &s) {
  std::string o = "'";
  for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return o + "'";
}

std::string encoder() {
  std::string out;
  FILE *p = popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
  if (!p) return "";
  char buf[512];
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  if (out.empty()) return "";
  return out.find("libx264") != std::string::npos ? "-c:v libx264 -preset medium -crf 20" : "-c:v mpeg4 -q:v 3";
}

}  // namespace

Recorder::~Recorder() { close(); }

bool Recorder::open(SDL_GPUDevice *device, const std::string &path, int width, int height, int fps, std::string &error) {
  close();
  const std::string enc = encoder();
  if (enc.empty()) {
    error = "ffmpeg isn't installed (it encodes the video)";
    return false;
  }
  device_ = device;
  w_ = std::max(16, width & ~1);
  h_ = std::max(16, height & ~1);
  fps_ = std::max(1, fps);
  frames_ = 0;
  path_ = path;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  SDL_GPUTransferBufferCreateInfo info = {};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  info.size = static_cast<Uint32>(w_ * h_ * 4);
  buffer_ = SDL_CreateGPUTransferBuffer(device_, &info);
  if (!buffer_) {
    error = std::string("no transfer buffer: ") + SDL_GetError();
    return false;
  }
  const std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(w_) + "x" + std::to_string(h_) +
                          " -r " + std::to_string(fps_) + " -i - -an " + enc + " -pix_fmt yuv420p -movflags +faststart " + quote(path);
  pipe_ = popen(cmd.c_str(), "w");
  if (!pipe_) {
    error = "couldn't start ffmpeg";
    SDL_ReleaseGPUTransferBuffer(device_, buffer_);
    buffer_ = nullptr;
    return false;
  }
  return true;
}

bool Recorder::add(SDL_GPUTexture *texture) {
  if (!pipe_ || !texture) return false;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion region = {};
  region.texture = texture;
  region.w = static_cast<Uint32>(w_);
  region.h = static_cast<Uint32>(h_);
  region.d = 1;
  SDL_GPUTextureTransferInfo dst = {};
  dst.transfer_buffer = buffer_;
  SDL_DownloadFromGPUTexture(copy, &region, &dst);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!fence) return false;
  SDL_WaitForGPUFences(device_, true, &fence, 1);
  SDL_ReleaseGPUFence(device_, fence);
  const uint8_t *px = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device_, buffer_, false));
  if (!px) return false;
  row_.assign(px, px + static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4);
  SDL_UnmapGPUTransferBuffer(device_, buffer_);
  for (size_t i = 3; i < row_.size(); i += 4) row_[i] = 255;
  const bool ok = fwrite(row_.data(), 1, row_.size(), pipe_) == row_.size();
  if (ok) ++frames_;
  return ok;
}

int Recorder::close() {
  if (pipe_) {
    pclose(pipe_);
    pipe_ = nullptr;
  }
  if (buffer_) {
    SDL_ReleaseGPUTransferBuffer(device_, buffer_);
    buffer_ = nullptr;
  }
  return frames_;
}

}  // namespace viewport
