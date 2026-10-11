#include "viewport/image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include "core/util.h"

namespace viewport {

std::vector<uint8_t> encode_png(const Image &image) {
  std::vector<uint8_t> out;
  stbi_write_png_to_func(
      [](void *ctx, void *data, int size) {
        auto *v = static_cast<std::vector<uint8_t> *>(ctx);
        v->insert(v->end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
      },
      &out, image.width, image.height, 4, image.rgba.data(), image.width * 4);
  return out;
}

bool decode_image(const uint8_t *data, size_t size, Image &out) {
  int w, h, n;
  uint8_t *pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
  if (!pixels) return false;
  out.width = w;
  out.height = h;
  out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
  stbi_image_free(pixels);
  return true;
}

bool load_image(const std::string &path, Image &out) {
  std::vector<uint8_t> bytes;
  return core::read_file(path, bytes) && decode_image(bytes.data(), bytes.size(), out);
}

Image downsample(const Image &image, int factor) {
  if (factor <= 1) return image;
  Image out;
  out.width = image.width / factor;
  out.height = image.height / factor;
  out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x)
      for (int c = 0; c < 4; ++c) {
        int sum = 0;
        for (int dy = 0; dy < factor; ++dy)
          for (int dx = 0; dx < factor; ++dx)
            sum += image.rgba[((static_cast<size_t>(y) * factor + dy) * image.width + x * factor + dx) * 4 + c];
        out.rgba[(static_cast<size_t>(y) * out.width + x) * 4 + c] = static_cast<uint8_t>(sum / (factor * factor));
      }
  return out;
}

}  // namespace viewport
