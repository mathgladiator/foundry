// RGBA8 images, with PNG encoding and image decoding (via stb).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace viewport {

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
};

std::vector<uint8_t> encode_png(const Image &image);
// Decodes PNG or JPEG bytes to RGBA8. Returns false on failure.
bool decode_image(const uint8_t *data, size_t size, Image &out);
bool load_image(const std::string &path, Image &out);
// Box-filters down by an integer factor (for supersampling).
Image downsample(const Image &image, int factor);

}  // namespace viewport
