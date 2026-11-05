// colosseum-standalone's settings file (see host_config.h).
#include "gxruntime/utf8_file.h"
#include "host_config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::string trim(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const size_t last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

bool parse_bool(const std::string& v) {
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace

const std::string& host_pref_dir() {
  static const std::string dir = [] {
    std::string path;
    if (const std::string env = dol_utf8::environment("STANDALONE_PREF_DIR"); !env.empty()) {
      path = env;
      if (path.back() != '/' && path.back() != '\\')
        path += '/';
      SDL_CreateDirectory(path.c_str());
    } else if (char* pref = SDL_GetPrefPath(nullptr, "Pokemon Colosseum (Aurora)")) {
      path = pref;
      SDL_free(pref);
    }
    return path;
  }();
  return dir;
}

std::string host_settings_path() { return host_pref_dir() + "settings.ini"; }

bool host_settings_load(HostSettings* s) {
  FILE* f = dol_utf8_fopen(host_settings_path().c_str(), "r");
  if (!f)
    return false;
  char line[2048];
  while (std::fgets(line, sizeof(line), f)) {
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
      continue;
    const size_t eq = text.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = trim(text.substr(0, eq));
    std::string value = trim(text.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (key == "iso") s->iso = value;
    else if (key == "module") s->module = value;
    else if (key == "memcard") s->memcard = value;
    else if (key == "fullscreen") s->fullscreen = parse_bool(value);
    else if (key == "window_width") s->window_width = std::atoi(value.c_str());
    else if (key == "window_height") s->window_height = std::atoi(value.c_str());
    else if (key == "vsync") s->vsync = parse_bool(value);
    else if (key == "audio") s->audio = parse_bool(value);
    else if (key == "volume") s->volume = std::atoi(value.c_str());
    else if (key == "deadzone") s->deadzone = float(std::atof(value.c_str()));
    else if (key == "pause_on_focus_loss") s->pause_on_focus_loss = parse_bool(value);
    else if (key == "verbose") s->verbose = parse_bool(value);
    else if (key == "aspect") s->aspect = value;
    else if (key == "frame_rate") s->frame_rate = value;
    else if (key == "texture_pack") s->texture_pack = value;
    else if (key == "hd_text") s->hd_text = parse_bool(value);
    else std::fprintf(stderr, "settings: unknown key %s\n", key.c_str());
  }
  std::fclose(f);
  if (s->window_width < 320 || s->window_height < 240) {
    s->window_width = 1280;
    s->window_height = 960;
  }
  if (s->aspect != "4:3" && s->aspect != "16:9" && s->aspect != "auto")
    s->aspect = "auto";
  if (s->frame_rate != "native" && s->frame_rate != "60" && s->frame_rate != "120" && s->frame_rate != "display")
    s->frame_rate = "native";
  if (s->volume < 0) s->volume = 0;
  if (s->volume > 100) s->volume = 100;
  return true;
}

bool host_settings_save(const HostSettings& s) {
  const std::string path = host_settings_path();
  const std::string temp = path + ".tmp";
  FILE* f = dol_utf8_fopen(temp.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "settings: cannot write %s\n", temp.c_str());
    return false;
  }
  std::fprintf(f,
               "# colosseum-standalone settings (written by the game; edit while it is closed)\n"
               "# disc image (ISO or GCM)\n"
               "iso = %s\n"
               "# recompiled module; empty: next to the exe, then the settings folder\n"
               "module = %s\n"
               "# memory card image for slot A; empty: MemoryCardA.USA.raw in this folder\n"
               "memcard = %s\n"
               "fullscreen = %d\n"
               "window_width = %d\n"
               "window_height = %d\n"
               "vsync = %d\n"
               "audio = %d\n"
               "# 0-100\n"
               "volume = %d\n"
               "# stick deadzone, fraction of full deflection\n"
               "deadzone = %.2f\n"
               "pause_on_focus_loss = %d\n"
               "# status lines in the log\n"
               "verbose = %d\n"
               "# 4:3, 16:9, or auto: widescreen at the window's shape (21:9 on an ultrawide)\n"
               "aspect = %s\n"
               "# text drawn from a host font at the output resolution\n"
               "hd_text = %d\n"
               "# native, 60, 120, or display (up to 240 Hz); guest timing stays 59.94 Hz\n"
               "frame_rate = %s\n"
               "# optional replacement-texture directory; restart after changing\n"
               "texture_pack = %s\n",
               s.iso.c_str(), s.module.c_str(), s.memcard.c_str(), s.fullscreen, s.window_width, s.window_height,
               s.vsync, s.audio, s.volume, s.deadzone, s.pause_on_focus_loss, s.verbose, s.aspect.c_str(),
               s.hd_text, s.frame_rate.c_str(), s.texture_pack.c_str());
  const bool ok = std::fclose(f) == 0;
  if (!ok || !SDL_RenamePath(temp.c_str(), path.c_str())) {
    std::fprintf(stderr, "settings: cannot write %s\n", path.c_str());
    return false;
  }
  return true;
}
