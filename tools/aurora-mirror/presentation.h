// colosseum-standalone's widescreen presentation: ModernGekko's Hor+ mode
// (lib/ModernGekko/src/runtime/dolphin_runtime.cpp, the vi_end_field and
// before_frame hooks), driven from the same guest words.
//
// The module carries ModernGekko's widescreen guest hook (0x800A3930 ->
// 0x80005300), which multiplies every projection's aspect by the float at
// 0x8047E724. Hor+ sets that multiplier to target aspect / (4:3) while live
// gameplay, the title fly-through and the front-end are on screen, so the
// game lays its 3D and its own 2D out for the wider frame, and presents the
// frame at the target aspect. Complete 4:3 canvases (field submenus, the PDA,
// character naming) and FMVs keep the authored 4:3 projection and are
// pillarboxed. The front-end additionally tells the renderer its multiplier
// (BP 0x6F) for the few adjustments the game does not make itself (the menu
// backdrop and popup clipping, gxcore_draw.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

enum class AspectSetting { Standard, Wide, Auto };  // 4:3, 16:9, the window's

// Once per field, before the guest runs on: reads the guest words, settles the
// presentation and writes the projection multiplier (and the culling bounds
// that go with it) into guest RAM. `ram` is MEM1; `window_aspect` the window's
// width / height.
void presentation_field(uint8_t* ram, uint32_t ram_size, AspectSetting setting, float window_aspect);

// Keeps the settled multiplier in place (call before the guest draws a frame).
void presentation_enforce(uint8_t* ram, uint32_t ram_size);

// The aspect to present the current frame at: 4:3 (pillarboxed) or the Hor+
// target aspect.
float presentation_present_aspect();
// Exclude movies and complete authored menus from transform interpolation.
bool presentation_interpolation_allowed();

// The front-end's Hor+ multiplier as a BP 0x6F command (5 bytes) when it
// changed since the last call; false otherwise. `resync`: send it regardless.
bool presentation_take_bp(uint8_t out[5], bool resync);

// Aurora preserves the full 640:480 authored canvas (no Dolphin VI aperture
// correction). These margins never cover that canvas. Used by both the ImGui
// compositor and composed renderer-readback captures.
struct NamingMargins { uint32_t left, right; };
inline NamingMargins presentation_naming_margins(uint32_t width, uint32_t height,
                                                uint32_t center_width = 0) {
  const uint64_t authored = center_width ? center_width : (uint64_t(height) * 4 + 2) / 3;
  if (authored >= width || !height) return {0, width};
  const uint32_t left = uint32_t((width - authored) / 2);
  return {left, left + uint32_t(authored)};
}
inline uint32_t presentation_naming_color(uint32_t y, uint32_t height) {
  if (!height || y >= height) return 0x005194;
  const uint32_t authored_y = uint32_t(uint64_t(y) * 480 / height);
  const uint32_t phased_y = authored_y >= 367 ? authored_y + 1 : authored_y;
  return (phased_y / 4) & 1 ? 0x006DC6 : 0x005194;
}
inline void presentation_paint_naming_margins(uint8_t* rgb, uint32_t width, uint32_t height,
                                             uint32_t center_width = 0) {
  const auto margins = presentation_naming_margins(width, height, center_width);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t color = presentation_naming_color(y, height);
    for (uint32_t x = 0; x < width; ++x) {
      if (x >= margins.left && x < margins.right) continue;
      uint8_t* pixel = rgb + (uint64_t(y) * width + x) * 3;
      pixel[0] = uint8_t(color >> 16);
      pixel[1] = uint8_t(color >> 8);
      pixel[2] = uint8_t(color);
    }
  }
}

// Aurora readback may contain either the authored EFB alone or a window-sized
// texture with bars already present. Preserve the entire original center in
// both cases; only cropped input needs a wider output allocation.
inline bool presentation_compose_naming_readback(std::vector<uint8_t>& rgb, uint32_t& width,
                                                 uint32_t height, float window_aspect) {
  if (!height || !(window_aspect > 4.0f / 3.0f)) return false;
  const uint32_t authored_width = uint32_t((uint64_t(height) * 4 + 2) / 3);
  if (width <= authored_width) {
    const uint32_t output_width = uint32_t(float(height) * window_aspect + 0.5f);
    if (output_width <= width) return false;
    std::vector<uint8_t> composed(size_t(output_width) * height * 3);
    presentation_paint_naming_margins(composed.data(), output_width, height, width);
    const auto margins = presentation_naming_margins(output_width, height, width);
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(composed.data() + (size_t(y) * output_width + margins.left) * 3,
                  rgb.data() + size_t(y) * width * 3, size_t(width) * 3);
    rgb = std::move(composed);
    width = output_width;
    return true;
  }
  const auto margins = presentation_naming_margins(width, height);
  if (!margins.left && margins.right == width) return false;
  presentation_paint_naming_margins(rgb.data(), width, height);
  return true;
}
