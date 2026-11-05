# Playing with colosseum-standalone

`colosseum-standalone` runs the recompiled Pokemon Colosseum (USA, GC6E01)
without Dolphin. You need your own disc image (`.iso` or `.gcm`) and the
recompiled module `gGC6E01_recomp.dll` (`.so` on Linux, `.dylib` on macOS).
For a local build, run `python scripts/setup-standalone.py <ISO>` from the
repository root; [SETUP.md](SETUP.md) explains the toolchain and build steps.

## Running it

Windows needs Windows 10 version 1903 or later.

Double-click `colosseum-standalone.exe` (or run it with no arguments). It looks for:

- **the disc image**: `iso =` in the settings file, then `game.iso` / `game.gcm`
  next to the exe, then any other `*.iso` / `*.gcm` there. If none is found, a
  message box offers a file dialog, and the file you choose is remembered.
- **the module**: `module =` in the settings file, then `gGC6E01_recomp.dll`
  next to the exe, then the settings folder. The setup script installs the
  module beside the standalone executable. Linux uses `.so`; macOS uses `.dylib`.

You can also pass them on the command line:
`colosseum-standalone [--verbose] [<game.iso> [<gGC6E01_recomp.dll>]]`. If no
disc image is remembered yet, the one you pass is saved for later launches.

Windows host and setup builds use UTF-8 file paths, including non-ASCII disc,
module, settings, card, font and texture-pack locations. Their embedded UTF-8
process manifest requires Windows 10 version 1903 or newer; no system locale
change is needed.

## Controls

A gamepad is used when one is connected; the keyboard works when none is.

| GameCube   | Gamepad (Xbox layout)          | Keyboard        |
|------------|--------------------------------|-----------------|
| A / B      | A / B (south / east)           | X / Z           |
| X / Y      | X / Y (west / north)           | C / S           |
| Start      | Start (Menu)                   | Enter           |
| Z          | Right bumper                   | D               |
| L / R      | Left / right trigger (analog)  | Q / W           |
| Main stick | Left stick                     | Arrow keys      |
| C stick    | Right stick                    | I / J / K / L   |
| D-pad      | D-pad                          | T / F / G / H   |

## Hotkeys

- **Esc** or the gamepad's **Back/View** button opens the pause menu (Resume,
  Fullscreen, Volume, pause when the window loses focus, Aspect ratio, Frame rate, HD text,
  Save settings, Quit).
  The game, its clock and its sound stop while the menu is open. Use the mouse,
  the arrow keys and Space/Enter, or the gamepad (A selects, B or Back resumes).
- **F11** or **Alt+Enter** switches between a window and borderless fullscreen.
- Closing the window quits. The memory card is written out on the way out.

## Picture

- **Aspect ratio** (`aspect =`): `auto` (the default) fills the window's shape,
  e.g. 21:9 on an ultrawide; `16:9` shows a 16:9 picture, with bars if the
  window is shaped differently; `4:3` is the original picture, pillarboxed.
  Widescreen is ModernGekko's Hor+ mode: the game is shown wider at the same
  height and vertical field of view, so nothing is stretched, and it lays out
  its own 2D (HUD, title, menus) for the wider frame. A few screens the game
  only draws for 4:3 stay 4:3 with bars: the FMVs, field submenus (Pokemon,
  items, the PDA) and character naming. Naming keeps its full 4:3 canvas and
  fills the outer margins with the stock striped backdrop in a wider window.
  Ported from ModernGekko's runtime
  (`presentation.cpp`); the scene detection reads the same guest words.
- **Frame rate** (`frame_rate =`): `native` (default) preserves the
  original output; `60` interpolates 30 FPS gameplay to 60 FPS, and `120`
  interpolates it to 120 FPS. `display`
  targets the current display, up to four presents per game frame (120 FPS
  during 30 FPS gameplay, 240 FPS during 60 FPS scenes).
  Guest logic and audio keep the original 59.94 Hz clock. Movies, naming and
  complete authored menus use native rendering; slow rendering can reduce
  interpolation automatically. Inserted frames are temporarily omitted when
  emulation falls behind its real-time clock, then restored after it catches up.
  On Windows, presentation locks to compositor
  vblanks when refresh rates are compatible, with a clock fallback elsewhere.
- **Texture packs** (`texture_pack =`): set a replacement-texture directory in
  `settings.ini` while the game is closed, then restart. Leave empty for the
  original textures. This uses Aurora's existing texture-name format.
- **HD text** (`hd_text =`, on by default): the game's text
  is drawn from a Windows font (MS PGothic from `msgothic.ttc`) at the output
  resolution instead of upscaled from the game's small bitmaps. The game still
  lays out every line; only the letters are redrawn (`native_text.cpp`, from
  ModernGekko's HD dynamic text). `STANDALONE_TEXT_FONT`, `STANDALONE_TEXT_FACE`
  and `STANDALONE_TEXT_SIZE` (pixels per 22-pixel game line, default 24) choose
  another font. The module must carry the glyph hook
  (`patches/GC6E01-native-substitutions.csv`, `dolrecomp_native_msg_glyph_blit`);
  with an older module the option changes nothing.

Aspect ratio, frame rate and HD text can be changed in the pause menu while playing; Save settings keeps them.

## Where things live

Everything is in `%APPDATA%\Pokemon Colosseum (Aurora)\`, the same folder
Aurora keeps its shader caches in:

- `settings.ini`: settings (disc image, module, memory card path, fullscreen,
  window size, vsync, sound on/off, volume, stick deadzone, pause on focus
  loss, verbose logging, aspect ratio, frame rate, HD text, texture-pack directory). Edit it while the game is
  closed. The window size, fullscreen state, volume, focus option, aspect ratio
  frame rate and HD text are saved again whenever you quit.
- `MemoryCardA.USA.raw`: the slot A memory card (a Dolphin-format raw image,
  so you can copy one from Dolphin). Set `memcard =` to use another file.
- `standalone.log`: the log when the game was started by double-click.

## For testing

macOS keeps rendering on the main thread for SDL/Cocoa video initialization
and runs guest emulation on a worker, so rendering and guest logic overlap.
`AURORA_NATIVE_ASYNC` applies only to Windows and Linux.

Linux x86-64 uses a shared logical memory view for current game modules.
The reserved 4 GiB is virtual address space, not 4 GiB of physical RAM.
`STANDALONE_FASTMEM=0` selects the checked-memory fallback for diagnostics.
Older modules remain compatible but use checked memory; rerun setup with the
current tools to enable the updated memory helpers.

These environment variables override the settings: `STANDALONE_PREF_DIR`
(another settings folder), `STANDALONE_CARD`, `STANDALONE_AUDIO=0|1`,
`STANDALONE_AUDIO_VOLUME`, `AURORA_NATIVE_SIZE=<w>x<h>`, `STANDALONE_VERBOSE=1`
(status lines), `STANDALONE_NO_DIALOGS=1` (no message boxes),
`STANDALONE_PRESENTATION_TRACE=1` (each widescreen/4:3 decision and the guest
words behind it, on stderr). In an input
script (`STANDALONE_INPUT_SCRIPT`), `<field>+<n> MENU` opens the pause menu for
n repaints, `QUIT` quits the way the menu does, and `CLOSE` quits the way closing
the window does.

`AURORA_PRESENT_INTERVALS=1` logs host present-call completion interval statistics
(mean, median, p95 and range every 240 presents). These measure the host
submission path; use PresentMon with the display awake to measure scanout.
