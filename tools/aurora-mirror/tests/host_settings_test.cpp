// Standalone host regression checks; no disc image or game module required.
// Supply a disposable STANDALONE_PREF_DIR before running this executable.
#include "host_config.h"
#include "frame_rate.h"
#include "presentation.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <stdexcept>

namespace {
bool naming_fill = false;
void check(bool value) { if (!value) throw std::runtime_error("naming margin regression"); }
void test_naming_margins() {
  check(presentation_naming_margins(640, 480).left == 0);
  const auto wide = presentation_naming_margins(1120, 480);
  check(wide.left == 240 && wide.right == 880);
  check(presentation_naming_color(0, 480) == 0x005194);
  check(presentation_naming_color(4, 480) == 0x006DC6);
  check(presentation_naming_color(366, 480) == 0x006DC6);
  check(presentation_naming_color(367, 480) == 0x005194);
  constexpr uint32_t width = 1120, height = 480;
  std::vector<uint8_t> actual(width * height * 3), expected(actual.size());
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t at = (size_t(y) * width + x) * 3;
      if (x >= 240 && x < 880) {
        actual[at] = expected[at] = uint8_t(140 + ((x - 240) / 32 % 2) * 70);
        actual[at + 1] = expected[at + 1] = uint8_t(50 + (y / 32 % 2) * 100);
        actual[at + 2] = expected[at + 2] = 90;
      } else {
        // Independent static template of ModernGekko's authored palette and
        // band cadence. The center is a synthetic stand-in for EFB readback.
        const bool light = ((y + (y >= 367 ? 1 : 0)) % 8) >= 4;
        expected[at] = 0;
        expected[at + 1] = uint8_t(light ? 109 : 81);
        expected[at + 2] = uint8_t(light ? 198 : 148);
      }
    }
  }
  // Existing window-sized readback must repaint bars without reallocating or
  // altering any central EFB bytes.
  uint32_t composed_width = width;
  check(presentation_compose_naming_readback(actual, composed_width, height, 21.0f / 9.0f));
  check(composed_width == width);
  std::vector<uint8_t> cropped(640 * height * 3);
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(cropped.data() + size_t(y) * 640 * 3,
                expected.data() + (size_t(y) * width + 240) * 3, 640 * 3);
  const auto cropped_before = cropped;
  uint32_t cropped_width = 640;
  check(!presentation_compose_naming_readback(cropped, cropped_width, height, 4.0f / 3.0f));
  check(cropped == cropped_before && cropped_width == 640);
  check(presentation_compose_naming_readback(cropped, cropped_width, height, 21.0f / 9.0f));
  check(cropped_width == width && cropped == expected);
  check(actual == expected);  // includes byte-for-byte untouched center
  if (const char* folder = std::getenv("STANDALONE_NAMING_ARTIFACT")) {
    for (bool reference : {false, true}) {
      const std::string path = std::string(folder) + (reference ? "/naming-fill-reference.ppm" : "/naming-fill-actual.ppm");
      FILE* output = std::fopen(path.c_str(), "wb");
      check(output);
      std::fprintf(output, "P6\n%u %u\n255\n", width, height);
      const auto& pixels = reference ? expected : actual;
      check(std::fwrite(pixels.data(), 1, pixels.size(), output) == pixels.size());
      check(std::fclose(output) == 0);
    }
  }
}
} // namespace
extern "C" void aurora_native_set_naming_fill(bool enabled) { naming_fill = enabled; }
extern "C" void aurora_native_set_pda_fill(bool) {}

int main() {
  test_naming_margins();
  assert(std::getenv("STANDALONE_PREF_DIR"));
  // Both 60-field cutscenes and 30-frame gameplay must meet the target.
  static_assert(frame_interpolation_layers(0, true, 2) == 1);
  static_assert(frame_interpolation_layers(60, true, 1) == 1);
  static_assert(frame_interpolation_layers(60, true, 2) == 2);
  static_assert(frame_interpolation_layers(120, true, 1) == 2);
  static_assert(frame_interpolation_layers(120, true, 2) == 4);
  static_assert(frame_interpolation_layers(240, true, 2) == 4);
  static_assert(frame_interpolation_layers(120, false, 2) == 1);
  static_assert(!frame_interpolation_catchup(false, 0.020));
  static_assert(frame_interpolation_catchup(false, 0.040));
  static_assert(frame_interpolation_catchup(true, 0.020));
  static_assert(!frame_interpolation_catchup(true, 0.003));
  static_assert(!frame_interpolation_catchup(true, -0.010));
  HostSettings settings;
  assert(settings.frame_rate == "native");
  for (const char* rate : {"native", "60", "120", "display"}) {
    settings.frame_rate = rate;
    settings.texture_pack = "texture packs/replacements";
    settings.iso = "disc images/game.iso";
    assert(host_settings_save(settings));
    HostSettings loaded;
    assert(host_settings_load(&loaded));
    assert(loaded.frame_rate == rate);
    assert(loaded.texture_pack == settings.texture_pack);
    assert(loaded.iso == settings.iso);
  }
  FILE* file = std::fopen(host_settings_path().c_str(), "w");
  assert(file);
  std::fputs("frame_rate = 999\naspect = invalid\n", file);
  std::fclose(file);
  assert(host_settings_load(&settings));
  assert(settings.frame_rate == "native");
  assert(settings.aspect == "auto");

  // Boot and movie scenes must remain excluded even when widescreen is on.
  std::vector<uint8_t> ram(0x1800000);
  for (AspectSetting aspect : {AspectSetting::Standard, AspectSetting::Wide, AspectSetting::Auto}) {
    presentation_field(ram.data(), unsigned(ram.size()), aspect, 16.0f / 9.0f);
    assert(!presentation_interpolation_allowed());
    ram[0x47B440] = ram[0x47B441] = 1;
    presentation_field(ram.data(), unsigned(ram.size()), aspect, 16.0f / 9.0f);
    assert(!presentation_interpolation_allowed());
    ram[0x47B440] = ram[0x47B441] = 0;
  }
  ram[0x40837F] = 1;  // live field, valid player position mirror
  presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Standard, 4.0f / 3.0f);
  assert(presentation_interpolation_allowed());
  // The real title confirmation reuses 0x0069 with its menu bit clear.
  ram[0x40837F] = 0xA1;
  ram[0x402436] = 0;
  ram[0x402437] = 0x69;
  ram[0x402504] = 0x18;
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);
  // Real presets/keyboard keep a valid navigation mirror and overlay0x18.
  // Confirmed naming must own its complete 4:3 canvas anyway.
  ram[0x40836F] = 1;
  ram[0x402436] = 0x2e;
  ram[0x402437] = 0xfd;
  for (int i = 0; i < 3; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(naming_fill && presentation_present_aspect() == 4.0f / 3.0f);
  ram[0x402436] = 0x2e;
  ram[0x402437] = 0xf5;  // character naming
  presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Standard, 4.0f / 3.0f);
  assert(!presentation_interpolation_allowed());
  ram[0x40836F] = 1;  // naming owns the complete authored menu canvas
  for (int i = 0; i < 3; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(naming_fill);
  ram[0x402437] = 0xed;  // observed post-entry "Your name?" phase
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(naming_fill && presentation_present_aspect() == 4.0f / 3.0f);
  ram[0x402436] = 0;
  ram[0x402437] = 0x69;  // name confirmation, distinct from menu-clear title prompt
  ram[0x402504] = 0x16;
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(naming_fill);
  ram[0x40837F] = 1;  // field/shop Yes/No prompt must not become naming
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);
  ram[0x40837F] = 0xA1;
  ram[0x40836F] = 0;
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);
  ram[0x402436] = 0x2e;
  ram[0x402437] = 0xed;
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);  // 0x2EED also requires a live menu
  ram[0x40836F] = 1;
  ram[0x40837F] = 1;  // same ID outside frontend cannot keep naming active
  for (int i = 0; i < 30; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);
  ram[0x40837F] = 0xA1;
  ram[0x402437] = 0xfd;
  for (int i = 0; i < 3; ++i)
    presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Standard, 4.0f / 3.0f);
  check(!naming_fill);
  ram[0x47B440] = ram[0x47B441] = 1;
  presentation_field(ram.data(), unsigned(ram.size()), AspectSetting::Wide, 21.0f / 9.0f);
  check(!naming_fill);
  std::puts("host settings and boot/movie interpolation exclusions passed");
}
