// Text and simple shapes drawn into RGBA frames, for the QA video's titles
// and parameter callouts. Roboto (from Dear ImGui's fonts), rasterized with
// stb_truetype.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stress {

struct Rgba {
  uint8_t r, g, b, a;
};

struct Canvas {
  int width = 0, height = 0;
  std::vector<uint8_t> px;  // RGBA, rows top to bottom
  void fill_rect(int x, int y, int w, int h, Rgba c);  // alpha-blended
};

// Draws UTF-8 text with its top-left at (x, y), `size` pixels tall; returns the
// width drawn. A dark shadow keeps it readable over any background.
int draw_text(Canvas &c, int x, int y, float size, const std::string &text, Rgba color, bool shadow = true);
int text_width(float size, const std::string &text);

}  // namespace stress
