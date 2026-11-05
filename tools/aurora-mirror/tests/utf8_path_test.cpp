// Exercise the same UTF-8 paths used by SDL dialogs, settings and private cards.
#include "gxruntime/utf8_file.h"
#include "host_config.h"
#include "memcard.h"
#include "pad_input.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
  wchar_t* wide_name = dol_utf8_to_wide(name);
  wchar_t* wide_value = dol_utf8_to_wide(value.c_str());
  assert(wide_name && wide_value);
  assert(_wputenv_s(wide_name, wide_value) == 0);
  std::free(wide_name);
  std::free(wide_value);
#else
  assert(setenv(name, value.c_str(), 1) == 0);
#endif
}

int main() {
  const std::string base = dol_utf8::environment("STANDALONE_PATH_TEST_ROOT");
  assert(!base.empty());
  const auto* suffix = u8"/\u8def\u5f84-\u00e9-\U0001f30d";
  const std::string dir = base + reinterpret_cast<const char*>(suffix);
  assert(SDL_CreateDirectory(dir.c_str()));
  set_environment("STANDALONE_PREF_DIR", dir);
  assert(dol_utf8::environment("STANDALONE_PREF_DIR") == dir);
  HostSettings settings;
  settings.iso = dir + "/disc.iso";
  settings.module = dir + "/module.dll";
  assert(host_settings_save(settings));
  HostSettings loaded;
  assert(host_settings_load(&loaded));
  assert(loaded.iso == settings.iso && loaded.module == settings.module);
  settings.volume = 37;  // also exercise replacing an existing settings file
  assert(host_settings_save(settings));
  assert(host_settings_load(&loaded) && loaded.volume == 37);

  const std::string script = dir + "/input.txt";
  FILE* file = dol_utf8_fopen(script.c_str(), "wb");
  assert(file);
  assert(std::fputs("10 A\n20 QUIT\n", file) >= 0);
  assert(std::fclose(file) == 0);
  set_environment("STANDALONE_INPUT_SCRIPT", script);
  pad_input_init();
  assert(pad_input_read(10).buttons == 0x0100);
  assert(pad_input_script_action(20).quit);

  const std::string card = dir + "/card.raw";
  const uint8_t flash_id[12]{};
  assert(memcard_open(card, flash_id, 0));
  const uint8_t first_byte = memcard_header()[0];
  memcard_flush(true);
  assert(memcard_open(card, flash_id, 0));
  assert(memcard_header()[0] == first_byte);
  SDL_PathInfo info{};
  assert(SDL_GetPathInfo(card.c_str(), &info) && info.size == 16 * 1024 * 1024);
#ifdef _WIN32
  wchar_t* wide = dol_utf8_to_wide(dir.c_str());
  assert(wide);
  char* roundtrip = dol_utf8_from_wide(wide);
  assert(roundtrip && dir == roundtrip);
  std::free(roundtrip);
  std::free(wide);
  assert(!dol_utf8_to_wide("\xC0\xAF"));  // reject invalid UTF-8 safely
#endif
  std::puts("UTF-8 settings, replacement writes, input script and card I/O passed");
}
