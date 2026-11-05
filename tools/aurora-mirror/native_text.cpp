// colosseum-standalone's HD text (see native_text.h). A port of
// ModernGekko's VideoCommon/ColosseumNativeText.cpp (the Aurora half) and the
// constants it takes from ColosseumTextUpscale.h, in its Dolphin fork.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gxruntime/utf8_file.h"
#include "native_text.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ImGui's copy of stb_truetype; imgui_draw.cpp compiles its own static one.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

namespace {

// Per game font: the em the host font is drawn at (the game's cell height) and
// the baseline below the line top.
constexpr float FONT_EM[5] = {22, 18, 13, 18, 13};
constexpr float FONT_BASELINE[5] = {22, 18, 14, 19, 13};

// ColosseumTextUpscale.h: the smooth field the game's own glyphs are rebuilt from.
constexpr float SMOOTH_SIGMA = 0.6f;  // native texels
constexpr float EDGE_LEVEL = 0.6f;
float smooth_edge(float field, float half_width) {
  const float t = (field - (EDGE_LEVEL - half_width)) / (2.0f * half_width);
  const float c = std::clamp(t, 0.0f, 1.0f);
  return c * c * (3.0f - 2.0f * c);
}

struct Glyph {
  uint16_t x = 0, y = 0;  // slot in the 512x512 scratch texture, game pixels
  uint8_t w = 0, h = 0;   // slot size
  int8_t y_offset = 0;    // slot top relative to the line top
  uint8_t font = 0;       // game font id (0: 22px, 1: 18px, 2: 13px, 3/4: condensed)
  uint16_t code = 0;      // UTF-16 code unit
  uint32_t bitmap = 0;    // physical address of the game's own 4bpp glyph bitmap
};

struct Rect {
  int x0, y0, x1, y1;
};

std::atomic<bool> g_enabled{false};
const uint8_t* g_ram = nullptr;
uint32_t g_ram_size = 0;

struct Font {
  std::once_flag tried;  // the CPU thread (slot query) and Aurora's render thread both ask
  bool loaded = false;
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
  // MS PGothic is narrower than the game font: 22px looks letter-spaced and
  // 26px clips tall lines in the message boxes, so 24px is the default.
  float size_scale = 24.0f / 22.0f;
} g_font;

std::string env(const char* standalone, const char* moderngekko) {
  const std::string value = dol_utf8::environment(standalone);
  return value.empty() ? dol_utf8::environment(moderngekko) : value;
}

std::string default_font_path() {
#ifdef _WIN32
  const std::string windir = dol_utf8::environment("WINDIR");
  return (windir.empty() ? std::string("C:\\Windows") : windir) + "\\Fonts\\msgothic.ttc";
#else
  return {};
#endif
}

void load_font() {
  const std::string path_env = env("STANDALONE_TEXT_FONT", "MODERNGEKKO_NATIVE_TEXT_FONT");
  const std::string path = path_env.empty() ? default_font_path() : path_env;
  FILE* f = path.empty() ? nullptr : dol_utf8_fopen(path.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "standalone: HD text: no font at '%s'\n", path.c_str());
    return;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  g_font.data.resize(size > 0 ? size_t(size) : 0);
  const bool read = size > 0 && std::fread(g_font.data.data(), 1, g_font.data.size(), f) == g_font.data.size();
  std::fclose(f);
  if (!read) {
    g_font.data.clear();
    return;
  }
  const std::string face_env = env("STANDALONE_TEXT_FACE", "MODERNGEKKO_NATIVE_TEXT_FACE");
  const char* face = face_env.empty() ? "MS PGothic" : face_env.c_str();
  int offset = stbtt_FindMatchingFont(g_font.data.data(), face, STBTT_MACSTYLE_DONTCARE);
  if (offset < 0) {
    // msgothic.ttc is MS Gothic, MS UI Gothic, MS PGothic.
    const int count = stbtt_GetNumberOfFonts(g_font.data.data());
    offset = stbtt_GetFontOffsetForIndex(g_font.data.data(), count > 2 ? 2 : 0);
  }
  if (offset < 0 || !stbtt_InitFont(&g_font.info, g_font.data.data(), offset)) {
    std::fprintf(stderr, "standalone: HD text: '%s' has no usable face '%s'\n", path.c_str(), face);
    g_font.data.clear();
    return;
  }
  if (const std::string size_env = env("STANDALONE_TEXT_SIZE", "MODERNGEKKO_NATIVE_TEXT_SIZE"); !size_env.empty()) {
    const float px = float(std::atof(size_env.c_str()));
    if (px > 4 && px < 128)
      g_font.size_scale = px / FONT_EM[0];
  }
  g_font.loaded = true;
}

bool font_ready() {
  std::call_once(g_font.tried, load_font);
  return g_font.loaded;
}

// Codes the game reuses for its own symbols, which the host font would draw as
// the Unicode character: the Pokedollar, font 0's yen-for-backslash, the "e+"/"e"
// stand-ins for alpha and beta, the e glyphs at U+337C-E and the speech bubble.
bool is_game_symbol(const Glyph& glyph) {
  switch (glyph.code) {
  case 0x24:
  case 0x2031:
  case 0x337C:
  case 0x337D:
  case 0x337E:
    return true;
  case 0x5C:
    return glyph.font == 0;
  case 0x3B1:
  case 0x3B2:
    return glyph.font <= 2;
  default:
    return false;
  }
}

void put(std::vector<uint8_t>* rgba, uint32_t width, int x, int y, float coverage) {
  const uint8_t a = uint8_t(std::clamp(coverage, 0.0f, 1.0f) * 255.0f + 0.5f);
  if (a == 0)
    return;
  uint8_t* p = rgba->data() + (size_t(y) * width + x) * 4;
  p[0] = p[1] = p[2] = p[3] = std::max(p[3], a);
}

// The host font's glyph, centered in the slot's advance (squeezed to it when
// wider) and sat on the game font's baseline.
bool draw_font_glyph(const Glyph& glyph, uint32_t scale, std::vector<uint8_t>* rgba, uint32_t width, int bx0,
                     int by0, int bw, int bh) {
  const int index = stbtt_FindGlyphIndex(&g_font.info, glyph.code);
  if (index == 0)
    return false;
  const int font = std::min<int>(glyph.font, 4);
  const float em = FONT_EM[font] * g_font.size_scale * scale;
  const float sy = stbtt_ScaleForMappingEmToPixels(&g_font.info, em);
  int advance = 0, lsb = 0;
  stbtt_GetGlyphHMetrics(&g_font.info, index, &advance, &lsb);
  const float advance_px = advance * sy;
  const float squeeze = advance_px > bw ? bw / advance_px : 1.0f;
  const float sx = sy * squeeze;
  const float pen_x = bx0 + (bw - advance_px * squeeze) * 0.5f;
  const float base_y = by0 + (FONT_BASELINE[font] - glyph.y_offset) * scale;
  const int ipx = int(std::floor(pen_x));
  const int ipy = int(std::floor(base_y));
  const float fx = pen_x - ipx, fy = base_y - ipy;
  int ix0, iy0, ix1, iy1;
  stbtt_GetGlyphBitmapBoxSubpixel(&g_font.info, index, sx, sy, fx, fy, &ix0, &iy0, &ix1, &iy1);
  const int gw = ix1 - ix0, gh = iy1 - iy0;
  if (gw <= 0 || gh <= 0)
    return true;  // blank (space and the like)
  std::vector<unsigned char> coverage(size_t(gw) * gh);
  stbtt_MakeGlyphBitmapSubpixel(&g_font.info, coverage.data(), gw, gh, gw, sx, sy, fx, fy, index);
  for (int y = 0; y < gh; ++y) {
    const int ty = ipy + iy0 + y;
    if (ty < by0 || ty >= by0 + bh)
      continue;
    for (int x = 0; x < gw; ++x) {
      const int tx = ipx + ix0 + x;
      if (tx < bx0 || tx >= bx0 + bw)
        continue;
      put(rgba, width, tx, ty, coverage[size_t(y) * gw + x] / 255.0f);
    }
  }
  return true;
}

// The game's own bitmap, rebuilt from a smooth field at output resolution.
void draw_game_glyph(const Glyph& glyph, uint32_t scale, std::vector<uint8_t>* rgba, uint32_t width, int bx0,
                     int by0, int bw, int bh) {
  const int stride = (glyph.w + 1) / 2;
  const uint32_t physical = glyph.bitmap & 0x01FFFFFFu;
  const size_t bytes = size_t(stride) * glyph.h;
  if (!g_ram || physical >= g_ram_size || bytes > g_ram_size - physical)
    return;
  const uint8_t* bits = g_ram + physical;
  auto texel = [&](int x, int y) -> float {
    if (x < 0 || y < 0 || x >= glyph.w || y >= glyph.h)
      return 0.0f;
    const uint8_t b = bits[y * stride + x / 2];
    return ((x & 1) ? (b & 0xF) : (b >> 4)) / 15.0f;
  };
  constexpr float k = 1.0f / (2.0f * SMOOTH_SIGMA * SMOOTH_SIGMA);
  const float half_width = 0.48f / scale;
  for (int py = 0; py < bh; ++py) {
    const float v = (py + 0.5f) / scale;
    for (int px = 0; px < bw; ++px) {
      const float u = (px + 0.5f) / scale;
      const int cx = int(std::floor(u - 0.5f));
      const int cy = int(std::floor(v - 0.5f));
      float field = 0, weights = 0;
      for (int ty = cy - 1; ty <= cy + 2; ++ty)
        for (int tx = cx - 1; tx <= cx + 2; ++tx) {
          const float dx = tx + 0.5f - u, dy = ty + 0.5f - v;
          const float w = std::exp(-(dx * dx + dy * dy) * k);
          field += w * texel(tx, ty);
          weights += w;
        }
      put(rgba, width, bx0 + px, by0 + py, smooth_edge(field / weights, half_width));
    }
  }
}

Glyph decode(const u32 words[5]) {
  Glyph glyph;
  glyph.x = words[0] & 0x1FF;
  glyph.y = (words[0] >> 9) & 0x1FF;
  glyph.font = (words[0] >> 18) & 7;
  glyph.w = words[1] & 0x3F;
  glyph.h = (words[1] >> 6) & 0x3F;
  glyph.y_offset = int8_t((words[1] >> 12) & 0xFF);
  glyph.code = words[3] & 0xFFFF;
  glyph.bitmap = ((words[3] >> 16) & 0xFF) | ((words[4] & 0xFFFFFF) << 8);
  return glyph;
}

uint32_t decode_atlas(const u32 words[5]) { return (words[2] & 0xFFFFFF) << 5; }

// The slot rect a glyph's copy clears: two columns left and right of the
// bitmap and one row above and below.
Rect cleared_rect(const Glyph& glyph) {
  const int x0 = glyph.x - 2;
  const int y0 = glyph.y - 1;
  return {x0, y0, x0 + 2 * ((glyph.w + 5) >> 1), glyph.y + glyph.h + 1};
}

uint64_t glyph_key(const Glyph& glyph) {
  // The bitmap address is one entry of one font, which fixes the size; the
  // code and font are there for the symbols the host font does not draw.
  return (uint64_t(glyph.bitmap) << 32) | (uint64_t(glyph.code) << 16) | (uint64_t(glyph.font) << 8) |
         uint8_t(glyph.y_offset);
}

void rasterize(const Glyph& glyph, uint32_t scale, std::vector<uint8_t>* rgba, uint32_t* width, uint32_t* height) {
  const Rect r = cleared_rect(glyph);
  *width = uint32_t(r.x1 - r.x0) * scale;
  *height = uint32_t(r.y1 - r.y0) * scale;
  rgba->assign(size_t(*width) * *height * 4, 0);
  const int bx0 = (glyph.x - r.x0) * int(scale);
  const int by0 = (glyph.y - r.y0) * int(scale);
  const int bw = glyph.w * int(scale);
  const int bh = glyph.h * int(scale);
  if (is_game_symbol(glyph) || !draw_font_glyph(glyph, scale, rgba, *width, bx0, by0, bw, bh))
    draw_game_glyph(glyph, scale, rgba, *width, bx0, by0, bw, bh);
}

}  // namespace

void native_text_set_enabled(bool enabled) { g_enabled = enabled; }

bool native_text_available() { return font_ready(); }

void native_text_set_guest_ram(const uint8_t* ram, uint32_t size) {
  g_ram = ram;
  g_ram_size = size;
}

int native_text_place_slot(uint32_t code, uint32_t font, int32_t height, int32_t* y_offset) {
  if (!g_enabled || !font_ready())
    return 0;
  Glyph glyph;
  glyph.code = uint16_t(code);
  glyph.font = uint8_t(font);
  if (is_game_symbol(glyph))
    return 0;
  const int index = stbtt_FindGlyphIndex(&g_font.info, glyph.code);
  int ix0, iy0, ix1, iy1;
  if (index == 0 || !stbtt_GetGlyphBox(&g_font.info, index, &ix0, &iy0, &ix1, &iy1))
    return 0;
  const int f = std::min<int>(int(font), 4);
  const float sy = stbtt_ScaleForMappingEmToPixels(&g_font.info, FONT_EM[f] * g_font.size_scale);
  // The host ink's rows relative to the line top, as draw_font_glyph places it.
  const float top = FONT_BASELINE[f] - iy1 * sy;
  const float bottom = FONT_BASELINE[f] - iy0 * sy;
  int offset = *y_offset;
  if (bottom - top > height)
    offset = int(std::floor((top + bottom - height) * 0.5f));
  else if (top < offset)
    offset = int(std::floor(top));
  else if (bottom > offset + height)
    offset = int(std::ceil(bottom)) - height;
  if (offset == *y_offset || offset < -128 || offset > 127)
    return 0;
  *y_offset = offset;
  return 1;
}

// Aurora's render thread only.
bool native_text_rasterize(const u32 words[5], u32 scale, AuroraTextGlyph* out) {
  if (!g_enabled || !font_ready())
    return false;
  const Glyph glyph = decode(words);
  const Rect r = cleared_rect(glyph);
  const int x0 = std::max(r.x0, 0), y0 = std::max(r.y0, 0);
  const int x1 = std::min(r.x1, 512), y1 = std::min(r.y1, 512);
  if (x0 >= x1 || y0 >= y1)
    return false;

  // Clipped images by glyph and scale; a few thousand cover any screen.
  struct Image {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
  };
  static std::unordered_map<uint64_t, Image> cache;
  static uint32_t cache_scale = 0;
  if (cache_scale != scale || cache.size() > 4096) {
    cache.clear();
    cache_scale = scale;
  }
  // The clip depends on the slot, not just the image.
  const uint64_t key = glyph_key(glyph) ^ (uint64_t(x0 - r.x0) << 6) ^ (uint64_t(y0 - r.y0) << 4) ^
                       (uint64_t(r.x1 - x1) << 2) ^ uint64_t(r.y1 - y1);
  Image& image = cache[key];
  if (image.rgba.empty()) {
    std::vector<uint8_t> full;
    uint32_t width = 0, height = 0;
    rasterize(glyph, scale, &full, &width, &height);
    image.width = uint32_t(x1 - x0) * scale;
    image.height = uint32_t(y1 - y0) * scale;
    image.rgba.resize(size_t(image.width) * image.height * 4);
    const size_t row = size_t(image.width) * 4;
    for (uint32_t y = 0; y < image.height; ++y) {
      const size_t sx = size_t(x0 - r.x0) * scale;
      const size_t sy = size_t(y0 - r.y0) * scale + y;
      std::memcpy(image.rgba.data() + y * row, full.data() + (sy * width + sx) * 4, row);
    }
  }
  out->atlas = decode_atlas(words);
  out->x = uint32_t(x0) * scale;
  out->y = uint32_t(y0) * scale;
  out->width = image.width;
  out->height = image.height;
  out->rgba = image.rgba.data();
  return true;
}
