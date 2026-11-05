// colosseum-standalone's window handling and pause menu, drawn with the
// Dear ImGui that Aurora runs (on Aurora's render thread, through
// aurora_native_set_overlay).
//
// Esc or the gamepad's Back/View button opens and closes the menu; F11 or
// Alt+Enter toggles borderless fullscreen; with pause_on_focus_loss, the menu
// opens when the window loses focus.
#pragma once

#include "host_config.h"

// Before aurora_native_init: takes the window settings and installs the
// overlay and the SDL event watch.
void host_ui_init(const HostSettings& settings);
// After Aurora started (the event watch needs SDL's events up).
void host_ui_start();

bool host_ui_menu_open();
void host_ui_set_menu(bool open);
// The menu's Save settings was chosen since the last call.
bool host_ui_take_save_request();
// Tells the menu the save went through (it shows "Saved").
void host_ui_saved(bool ok);
// The window and menu state into the settings (fullscreen, size, volume,
// pause_on_focus_loss, aspect, hd_text).
void host_ui_store(HostSettings* settings);

// The menu's picture settings, live: aspect 0 = 4:3, 1 = 16:9, 2 = auto.
int host_ui_frame_rate();
int host_ui_aspect();
bool host_ui_hd_text();
// The window's drawable width / height (fullscreen included).
float host_ui_window_aspect();
