#include "text.h"

#include <algorithm>
#include <map>

#define STB_TRUETYPE_IMPLEMENTATION
#include "roboto_medium.h"
#include "stb_truetype.h"

namespace stress {

namespace {

struct Glyph {
  std::vector<uint8_t> alpha;
  int w = 0, h = 0, x0 = 0, y0 = 0, advance = 0;
};

struct Font {
  stbtt_fontinfo info;
  int ascent = 0;
  std::map<std::pair<int, uint32_t>, Glyph> glyphs;  // (size, codepoint)
  Font() {
    stbtt_InitFont(&info, roboto_medium, stbtt_GetFontOffsetForIndex(roboto_medium, 0));
    int descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  }
  const Glyph &glyph(float size, uint32_t cp) {
    auto key = std::make_pair(static_cast<int>(size * 4), cp);
    auto it = glyphs.find(key);
    if (it != glyphs.end()) return it->second;
    Glyph g;
    const float scale = stbtt_ScaleForPixelHeight(&info, size);
    int adv, lsb, x1, y1;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &adv, &lsb);
    stbtt_GetCodepointBitmapBox(&info, static_cast<int>(cp), scale, scale, &g.x0, &g.y0, &x1, &y1);
    g.w = x1 - g.x0;
    g.h = y1 - g.y0;
    g.advance = static_cast<int>(adv * scale + 0.5f);
    if (g.w > 0 && g.h > 0) {
      g.alpha.assign(static_cast<size_t>(g.w) * g.h, 0);
      stbtt_MakeCodepointBitmap(&info, g.alpha.data(), g.w, g.h, g.w, scale, scale, static_cast<int>(cp));
    }
    return glyphs.emplace(key, std::move(g)).first->second;
  }
};

Font &font() {
  static Font f;
  return f;
}

std::vector<uint32_t> decode(const std::string &s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int n = 1;
    if (c >= 0xf0) { cp = c & 0x07; n = 4; }
    else if (c >= 0xe0) { cp = c & 0x0f; n = 3; }
    else if (c >= 0xc0) { cp = c & 0x1f; n = 2; }
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    out.push_back(cp);
    i += n;
  }
  return out;
}

void blend(Canvas &c, int x, int y, Rgba col, int alpha) {
  if (x < 0 || y < 0 || x >= c.width || y >= c.height || alpha <= 0) return;
  uint8_t *p = &c.px[(static_cast<size_t>(y) * c.width + x) * 4];
  const int a = alpha * col.a / 255;
  p[0] = static_cast<uint8_t>((col.r * a + p[0] * (255 - a)) / 255);
  p[1] = static_cast<uint8_t>((col.g * a + p[1] * (255 - a)) / 255);
  p[2] = static_cast<uint8_t>((col.b * a + p[2] * (255 - a)) / 255);
  p[3] = 255;
}

int draw_pass(Canvas *c, int x, int y, float size, const std::string &text, Rgba color) {
  Font &f = font();
  const float scale = stbtt_ScaleForPixelHeight(&f.info, size);
  const int baseline = y + static_cast<int>(f.ascent * scale + 0.5f);
  int pen = x;
  const auto cps = decode(text);
  for (size_t i = 0; i < cps.size(); ++i) {
    const Glyph &g = f.glyph(size, cps[i]);
    if (c)
      for (int gy = 0; gy < g.h; ++gy)
        for (int gx = 0; gx < g.w; ++gx) blend(*c, pen + g.x0 + gx, baseline + g.y0 + gy, color, g.alpha[gy * g.w + gx]);
    pen += g.advance;
    if (i + 1 < cps.size())
      pen += static_cast<int>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(cps[i]), static_cast<int>(cps[i + 1])) * scale);
  }
  return pen - x;
}

}  // namespace

void Canvas::fill_rect(int x, int y, int w, int h, Rgba c) {
  for (int yy = std::max(0, y); yy < std::min(height, y + h); ++yy)
    for (int xx = std::max(0, x); xx < std::min(width, x + w); ++xx) blend(*this, xx, yy, c, 255);
}

int text_width(float size, const std::string &text) { return draw_pass(nullptr, 0, 0, size, text, {0, 0, 0, 0}); }

int draw_text(Canvas &c, int x, int y, float size, const std::string &text, Rgba color, bool shadow) {
  if (shadow) {
    const int o = std::max(1, static_cast<int>(size / 18));
    draw_pass(&c, x + o, y + o, size, text, {0, 0, 0, 170});
  }
  return draw_pass(&c, x, y, size, text, color);
}

}  // namespace stress
