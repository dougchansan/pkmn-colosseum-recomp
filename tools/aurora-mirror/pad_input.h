// Controller port 1's state from the host: an SDL gamepad, the keyboard when no
// gamepad is connected, or a script (STANDALONE_INPUT_SCRIPT) for test runs.
#pragma once

#include <cstdint>

struct PadInput {
  uint16_t buttons = 0;  // SDK bit layout (PAD_BUTTON_*)
  uint8_t stick_x = 0x80, stick_y = 0x80, c_x = 0x80, c_y = 0x80;
  uint8_t trigger_l = 0, trigger_r = 0;
};

// Reads the script if STANDALONE_INPUT_SCRIPT names one: lines of
// "<field>[+<fields held, default 6>] <button>..." with buttons A B X Y Z L R
// START UP DOWN LEFT RIGHT (d-pad) and STICK_UP/DOWN/LEFT/RIGHT (main stick).
void pad_input_init();
// The state for this field. With a script, only the script counts.
PadInput pad_input_read(uint64_t field);
// Radial stick deadzone, 0..0.9 of full deflection (default 0.15).
void pad_input_set_deadzone(float deadzone);
// Host actions a script asks for at this field (test hooks): MENU opens the
// pause menu for its hold count of repaints (~1/60 s each), QUIT quits the
// way the menu's Quit does, CLOSE as closing the window does.
struct PadScriptAction {
  uint64_t menu_repaints = 0;
  bool quit = false;
  bool close = false;  // CLOSE: as if the window were closed
};
PadScriptAction pad_input_script_action(uint64_t field);
