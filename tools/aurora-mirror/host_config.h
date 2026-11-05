// colosseum-standalone's settings: a small key = value file in the per-user
// folder Aurora already keeps its caches in (SDL_GetPrefPath(nullptr,
// "Pokemon Colosseum (Aurora)"), on Windows %APPDATA%\Pokemon Colosseum
// (Aurora)\settings.ini). STANDALONE_PREF_DIR names another folder (tests).
#pragma once

#include <string>

struct HostSettings {
  std::string iso;      // disc image (ISO/GCM)
  std::string module;   // gGC6E01_recomp module; empty: next to the exe or in settings folder
  std::string memcard;  // slot A image; empty: MemoryCardA.USA.raw in the settings folder
  bool fullscreen = false;
  int window_width = 1280, window_height = 960;
  bool vsync = false;
  bool audio = true;
  int volume = 100;  // percent
  float deadzone = 0.15f;
  bool pause_on_focus_loss = false;
  bool verbose = false;
  // "4:3", "16:9" or "auto": widescreen (Hor+) at the window's shape; see
  // presentation.h. ModernGekko's launcher defaults to its auto too.
  std::string aspect = "auto";
  std::string frame_rate = "native";  // native, 60, 120, display (up to 240 Hz)
  std::string texture_pack;  // optional replacement-texture directory
  bool hd_text = true;  // Colosseum's text from a host font (native_text.h)
};

// The settings folder, with a trailing separator (created if missing).
const std::string& host_pref_dir();
std::string host_settings_path();
// Reads the settings file; missing keys keep their defaults. False when the
// file does not exist.
bool host_settings_load(HostSettings* settings);
bool host_settings_save(const HostSettings& settings);
