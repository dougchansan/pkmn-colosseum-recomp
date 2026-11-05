#include "pad_input.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
SDL_Joystick* joystick = nullptr;
SDL_JoystickID attach() {
  SDL_VirtualJoystickDesc desc{};
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  desc.name = "Colosseum synthetic regression pad";
  const auto id = SDL_AttachVirtualJoystick(&desc);
  check(id != 0, SDL_GetError());
  joystick = SDL_OpenJoystick(id);
  check(joystick != nullptr, SDL_GetError());
  check(SDL_IsGamepad(id), "virtual joystick has no gamepad mapping");
  return id;
}
void axis(SDL_GamepadAxis a, Sint16 value) {
  check(SDL_SetJoystickVirtualAxis(joystick, int(a), value), SDL_GetError());
}
PadInput read(bool check_math = false) {
  SDL_UpdateJoysticks();
  SDL_UpdateGamepads();
  // SDL's event/update code can set floating-point flags independently of
  // the host mapping. Measure only the mapping call below.
  if (check_math)
    std::feclearexcept(FE_ALL_EXCEPT);
  return pad_input_read(0);
}
void neutral() {
  for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
    axis(SDL_GamepadAxis(a), a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? -32768 : 0);
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    check(SDL_SetJoystickVirtualButton(joystick, b, false), SDL_GetError());
}
void detach(SDL_JoystickID id) {
  SDL_CloseJoystick(joystick);
  joystick = nullptr;
  check(SDL_DetachVirtualJoystick(id), SDL_GetError());
  SDL_UpdateJoysticks();
  SDL_UpdateGamepads();
}
}

int main() {
  try {
    check(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
      std::printf("Physical gamepad (read-only enumeration): %u %s\n", unsigned(ids[i]),
                  SDL_GetGamepadNameForID(ids[i]));
    SDL_free(ids);
    // Never open or inject input into attached user hardware. Port 1 selects
    // the first gamepad, so this test must run in an otherwise empty session.
    if (count) {
      std::puts("SKIP: physical gamepad present; synthetic test requires no physical gamepads");
      SDL_Quit();
      return 77;
    }
    std::puts("Physical gamepads: 0");
    auto id = attach();
    neutral();
    pad_input_set_deadzone(0.15f);
    auto p = read();
    check(p.buttons == 0 && p.stick_x == 128 && p.stick_y == 128 && p.c_x == 128 && p.c_y == 128 &&
          p.trigger_l == 0 && p.trigger_r == 0, "neutral pad");
    for (float deadzone : {0.0f, -1.0f}) {
      pad_input_set_deadzone(deadzone);
      p = read(true);
      check(p.stick_x == 128 && p.stick_y == 128 && p.c_x == 128 && p.c_y == 128,
            "zero deadzone center");
      check(!std::fetestexcept(FE_INVALID | FE_DIVBYZERO), "zero deadzone center must not divide by zero");
    }
    pad_input_set_deadzone(0.15f);
    const struct { SDL_GamepadButton button; unsigned bit; } mappings[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, 0x100}, {SDL_GAMEPAD_BUTTON_EAST, 0x200},
      {SDL_GAMEPAD_BUTTON_WEST, 0x400}, {SDL_GAMEPAD_BUTTON_NORTH, 0x800},
      {SDL_GAMEPAD_BUTTON_START, 0x1000}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x10},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 8}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 4},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 2},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0}, {SDL_GAMEPAD_BUTTON_BACK, 0}
    };
    for (const auto& m : mappings) {
      check(SDL_SetJoystickVirtualButton(joystick, m.button, true), SDL_GetError());
      check(read().buttons == m.bit, "digital button mapping");
      check(SDL_SetJoystickVirtualButton(joystick, m.button, false), SDL_GetError());
      check(read().buttons == 0, "digital button release");
    }
    // SDL maps raw signed trigger travel onto the gamepad's 0..32767.
    for (auto a : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}) {
      const unsigned bit = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0x40 : 0x20;
      for (Sint16 raw : {Sint16(-32768), Sint16(0), Sint16(31231), Sint16(31232), Sint16(32767)}) {
        axis(a, raw);
        p = read();
        SDL_Gamepad* observer = SDL_OpenGamepad(id);
        check(observer != nullptr, SDL_GetError());
        const int mapped = SDL_GetGamepadAxis(observer, a);
        SDL_CloseGamepad(observer);
        check((p.buttons & bit) == (mapped >= 32000 ? bit : 0), "trigger digital threshold");
        check((a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? p.trigger_l : p.trigger_r) == mapped * 255 / 32767,
              "trigger analog scaling");
      }
      axis(a, -32768);
      read();
    }
    for (bool c : {false, true}) {
      const auto x = c ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
      const auto y = c ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
      const int radius = c ? 90 : 100;
      axis(x, 3000); axis(y, -3000); p = read();
      check((c ? p.c_x : p.stick_x) == 128 && (c ? p.c_y : p.stick_y) == 128, "radial deadzone");
      axis(x, 32767); axis(y, 0); p = read();
      check((c ? p.c_x : p.stick_x) == 128 + radius, "positive stick endpoint");
      axis(x, -32768); axis(y, -32768); p = read();
      const int diagonal = int(std::lround(radius / std::sqrt(2.0)));
      check((c ? p.c_x : p.stick_x) == 128 - diagonal &&
            (c ? p.c_y : p.stick_y) == 128 + diagonal, "diagonal normalization and Y inversion");
      axis(x, 0); axis(y, 32767); p = read();
      check((c ? p.c_y : p.stick_y) == 128 - radius, "negative Y endpoint");
      axis(x, 16384); axis(y, 0);
      pad_input_set_deadzone(2.0f); p = read();
      check((c ? p.c_x : p.stick_x) == 128, "configured deadzone");
      axis(x, 32767); p = read();
      check((c ? p.c_x : p.stick_x) == 128 + radius, "deadzone upper clamp preserves full reach");
      axis(x, 16384);
      pad_input_set_deadzone(0.15f); p = read();
      check((c ? p.c_x : p.stick_x) > 128 && (c ? p.c_x : p.stick_x) < 128 + radius / 2,
            "deadzone rescales remaining travel");
      axis(x, 0); axis(y, 0);
    }
    detach(id);
    p = read();
    check(p.buttons == 0 && p.stick_x == 128 && p.c_x == 128, "disconnect clears stale state");
    id = attach(); neutral();
    check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true), SDL_GetError());
    check(read().buttons == 0x100, "reconnect selects new pad");
    detach(id); read();
    SDL_Quit();
    std::puts("PASS: synthetic digital mapping, triggers, both sticks/deadzone, disconnect/reconnect");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s (SDL: %s)\n", e.what(), SDL_GetError());
    SDL_Quit();
    return 1;
  }
}
