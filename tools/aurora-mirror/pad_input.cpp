// Port 1 from SDL3, which Aurora's window already runs: Aurora opens gamepads
// as they connect (its event pump, on the render thread), and this takes its
// own reference to the first connected one each field, so hot-plugging just
// works. The keyboard counts while no gamepad is connected and the game window
// has focus.

#include "gxruntime/utf8_file.h"
#include "pad_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kZ = 0x0010, kR = 0x0020,
                   kL = 0x0040, kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000;

constexpr int kMainRadius = 100;  // GC main stick reach from center
constexpr int kCRadius = 90;
float g_deadzone = 0.15f;
constexpr int kScriptHoldFields = 6;

struct ScriptPress {
  uint64_t field;
  uint16_t buttons;
  uint64_t hold = kScriptHoldFields;
  int stick_dx = 0, stick_dy = 0;  // main stick, -1..1 per axis (y up)
  bool menu = false, quit = false, close = false;  // host actions (pad_input_script_action)
};
std::vector<ScriptPress> g_script;
bool g_scripted = false;

SDL_Gamepad* g_pad = nullptr;
SDL_JoystickID g_pad_id = 0;

uint8_t clamp_axis(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// A stick pair from SDL's -32768..32767 (y down) to GC around 0x80 (y up), with
// a radial deadzone rescaled so motion starts at zero just past it.
void stick(Sint16 sx, Sint16 sy, int radius, uint8_t* out_x, uint8_t* out_y) {
  float x = std::max(-1.0f, sx / 32767.0f), y = std::max(-1.0f, sy / 32767.0f);
  const float m = std::sqrt(x * x + y * y);
  if (m <= g_deadzone) {
    *out_x = *out_y = 0x80;
    return;
  }
  const float scale = std::min(1.0f, (m - g_deadzone) / (1.0f - g_deadzone)) / m;
  x *= scale;
  y *= scale;
  *out_x = clamp_axis(0x80 + int(std::lround(x * radius)));
  *out_y = clamp_axis(0x80 - int(std::lround(y * radius)));
}

// Analog trigger 0..255; the digital L/R bit only when fully pressed, as on
// the real controller (and Dolphin's default).
uint8_t trigger(Sint16 v, uint16_t bit, uint16_t* buttons) {
  const int t = std::clamp(int(v), 0, 32767);
  if (t >= 32000)
    *buttons |= bit;
  return uint8_t(t * 255 / 32767);
}

void update_gamepad() {
  if (g_pad && !SDL_GamepadConnected(g_pad)) {
    std::fprintf(stderr, "input: gamepad disconnected\n");
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
  }
  if (g_pad)
    return;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (!ids)
    return;
  for (int i = 0; i < count && !g_pad; ++i) {
    g_pad = SDL_OpenGamepad(ids[i]);
    if (g_pad) {
      g_pad_id = ids[i];
      std::fprintf(stderr, "input: gamepad %u connected: %s\n", unsigned(g_pad_id), SDL_GetGamepadName(g_pad));
    }
  }
  SDL_free(ids);
}

PadInput read_gamepad() {
  PadInput p;
  static constexpr struct {
    SDL_GamepadButton button;
    uint16_t bit;
  } kButtons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, kA},          {SDL_GAMEPAD_BUTTON_EAST, kB},
      {SDL_GAMEPAD_BUTTON_WEST, kX},           {SDL_GAMEPAD_BUTTON_NORTH, kY},
      {SDL_GAMEPAD_BUTTON_START, kStart},      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kZ},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
  };
  for (const auto& b : kButtons)
    if (SDL_GetGamepadButton(g_pad, b.button))
      p.buttons |= b.bit;
  stick(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY),
        kMainRadius, &p.stick_x, &p.stick_y);
  stick(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY),
        kCRadius, &p.c_x, &p.c_y);
  p.trigger_l = trigger(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), kL, &p.buttons);
  p.trigger_r = trigger(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), kR, &p.buttons);
  return p;
}

PadInput read_keyboard() {
  PadInput p;
  if (!SDL_GetKeyboardFocus())  // none of this process's windows has focus
    return p;
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  if (!keys)
    return p;
  const auto down = [&](SDL_Scancode s) { return int(s) < count && keys[s]; };
  static constexpr struct {
    SDL_Scancode key;
    uint16_t bit;
  } kKeys[] = {
      {SDL_SCANCODE_RETURN, kStart}, {SDL_SCANCODE_X, kA}, {SDL_SCANCODE_Z, kB}, {SDL_SCANCODE_C, kX},
      {SDL_SCANCODE_S, kY},          {SDL_SCANCODE_D, kZ}, {SDL_SCANCODE_T, kUp}, {SDL_SCANCODE_G, kDown},
      {SDL_SCANCODE_F, kLeft},       {SDL_SCANCODE_H, kRight},
  };
  const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;  // Alt+Enter is the fullscreen hotkey
  for (const auto& k : kKeys)
    if (down(k.key) && !(alt && k.key == SDL_SCANCODE_RETURN))
      p.buttons |= k.bit;
  if (down(SDL_SCANCODE_Q)) {
    p.buttons |= kL;
    p.trigger_l = 0xFF;
  }
  if (down(SDL_SCANCODE_W)) {
    p.buttons |= kR;
    p.trigger_r = 0xFF;
  }
  const auto axis = [&](SDL_Scancode neg, SDL_Scancode pos, int radius) {
    return uint8_t(0x80 + (down(pos) ? radius : 0) - (down(neg) ? radius : 0));
  };
  p.stick_x = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, kMainRadius);
  p.stick_y = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, kMainRadius);
  p.c_x = axis(SDL_SCANCODE_J, SDL_SCANCODE_L, kCRadius);
  p.c_y = axis(SDL_SCANCODE_K, SDL_SCANCODE_I, kCRadius);
  return p;
}

uint16_t button_named(const std::string& name) {
  static constexpr struct {
    const char* name;
    uint16_t bit;
  } kNames[] = {{"A", kA},         {"B", kB},       {"X", kX},       {"Y", kY},
                {"Z", kZ},         {"L", kL},       {"R", kR},       {"START", kStart},
                {"UP", kUp},       {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight}};
  for (const auto& n : kNames)
    if (name == n.name)
      return n.bit;
  return 0;
}

}  // namespace

void pad_input_init() {
  const std::string script_path = dol_utf8::environment("STANDALONE_INPUT_SCRIPT");
  const char* path = script_path.c_str();
  if (script_path.empty())
    return;
  g_scripted = true;
  FILE* f = dol_utf8_fopen(path, "r");
  if (!f) {
    std::fprintf(stderr, "input: cannot read script %s\n", path);
    return;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    if (char* hash = std::strchr(line, '#'))
      *hash = 0;
    char* rest = nullptr;
    const unsigned long long field = std::strtoull(line, &rest, 0);
    if (rest == line)
      continue;
    ScriptPress press{field, 0};
    if (*rest == '+')  // "<field>+<fields held> ..."
      press.hold = std::strtoull(rest + 1, &rest, 0);
    for (char* token = std::strtok(rest, " \t\r\n,"); token; token = std::strtok(nullptr, " \t\r\n,")) {
      std::string name = token;
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      if (name == "STICK_UP") press.stick_dy = 1;
      else if (name == "STICK_DOWN") press.stick_dy = -1;
      else if (name == "STICK_LEFT") press.stick_dx = -1;
      else if (name == "STICK_RIGHT") press.stick_dx = 1;
      else if (name == "MENU") press.menu = true;
      else if (name == "QUIT") press.quit = true;
      else if (name == "CLOSE") press.close = true;
      else if (const uint16_t bit = button_named(name)) press.buttons |= bit;
      else std::fprintf(stderr, "input: unknown button %s in script\n", token);
    }
    g_script.push_back(press);
  }
  std::fclose(f);
  std::printf("input: scripted, %zu presses from %s\n", g_script.size(), path);
}

PadInput pad_input_read(uint64_t field) {
  if (g_scripted) {
    PadInput p;
    for (const ScriptPress& s : g_script)
      if (!s.menu && !s.quit && !s.close && field >= s.field && field < s.field + s.hold) {
        p.buttons |= s.buttons;
        if (s.stick_dx)
          p.stick_x = clamp_axis(0x80 + s.stick_dx * kMainRadius);
        if (s.stick_dy)
          p.stick_y = clamp_axis(0x80 + s.stick_dy * kMainRadius);
      }
    static uint32_t last = 0;
    const uint32_t state = uint32_t(p.buttons) << 16 | uint32_t(p.stick_x) << 8 | p.stick_y;
    if (state != last) {
      std::printf("input: field %llu buttons 0x%04X stick %u,%u\n", (unsigned long long)field, p.buttons,
                  p.stick_x, p.stick_y);
      last = state;
    }
    return p;
  }
  update_gamepad();
  return g_pad ? read_gamepad() : read_keyboard();
}

void pad_input_set_deadzone(float deadzone) { g_deadzone = std::clamp(deadzone, 0.0f, 0.9f); }

PadScriptAction pad_input_script_action(uint64_t field) {
  PadScriptAction action;
  for (const ScriptPress& s : g_script) {
    if (s.field != field)
      continue;
    if (s.menu)
      action.menu_repaints = s.hold;
    action.quit |= s.quit;
    action.close |= s.close;
  }
  return action;
}
