# In-game Esc menu

The Windows runner now opens an in-game menu with **Esc** or **Start + Z**
(Plus + right shoulder on the current USB profile). Esc resumes from any page.
Arrows, D-pad, or the left stick navigate; Enter/A selects; Backspace/B goes
back; Left/Right adjusts values. Mouse selection is supported too.

The menu offers Resume, Settings, Controls, Save checkpoint and Quit. Settings
include 30/60 FPS, HD textures, sharper text, input display, autosave, mute,
volume, camera sensitivity, invert Y, recenter and fullscreen. Render resolution
can be changed live from 1x to 6x native; it controls the internal render target,
independently of window size. The chosen scale survives scene changes and restarts.
VSync, performance display, frame-time graphs and detailed render statistics
are also available. Metrics fonts, graphs, and statistics panels scale with the
physical game-window height, using 1080p as the reference (4K uses 2x), with
Windows DPI as a minimum readable size. Scaling updates when resizing or moving
the game between displays and is independent of internal render resolution.
Controller
selection switches compatible SDL/XInput devices while keeping existing
bindings; this is not a button-remapping editor. Camera options require the
updated camera mod and are disabled when it is absent.

Gameplay pauses while the menu redraws the last game frame. Closing it restores
the prior pause state, and held navigation buttons are consumed until release.
Quit requires confirmation with Keep playing selected first. Checkpoints are
separate from normal memory-card saves. Settings and actions work without an
automation directory. Camera preferences persist in the user's `config.ini`.

## HD textures and performance testing

The local Sephie HD Pack V2.00 was imported into the two free-camera profiles
under `ResourcePacks/Pokemon Colosseum HD Pack.zip`. The runtime installs its
7,045 DDS files into `Load/Textures/GC6` on first boot. The private pack is not
committed. `community_hd_texture_pack` and `hires_textures` are enabled in those
profiles; texture preloading remains off to avoid preloading the whole pack.

The performance display shows FPS, mean frame interval and variation, emulation
speed, and actual internal resolution. Graphs expose frame-pacing spikes;
render statistics show draw calls and texture/shader activity. These are not
separate CPU/GPU execution timings. Ignore readings while paused or loading.
Compare the same scene and camera after a few seconds of warmup, first at 3x
and 6x, then with HD textures off/on. A lower scale improving speed indicates
sensitivity to graphics workload; it does not prove GPU saturation. Full game
speed is 100%; a scene's authored cadence may be 30 FPS or 60 FPS.

Optional CSV capture while playing (run the monitor in another terminal):

```powershell
.\scripts\play-freecam.ps1 -PerformanceDirectory .\build\freecam-performance
.\scripts\monitor-freecam-performance.ps1 -PerformanceDirectory .\build\freecam-performance -Seconds 60
```

For the imported save, pass `-UserDirectory .\build\freecam-save-test\User`
and `-LoadState .\build\freecam-save-test\loaded-gci.sav` to the launcher,
and the same `-UserDirectory` to the monitor. CSV resolution and texture values
are saved settings, not GPU timings or a guarantee that a transition has finished.

These additive source patches preserve the existing host bundle. Apply the
normal `host-patches` bundle first, then the camera's `freecam-runtime.patch`
from its main documentation, followed by these menu patches:

```powershell
git -C lib/ModernGekko/vendor/dolphin apply (Resolve-Path mods/menu-host/recompcore-menu.patch)
git -C lib/ModernGekko apply (Resolve-Path mods/menu-host/moderngekko-menu.patch)
git -C lib/ModernGekko/vendor/dolphin apply (Resolve-Path mods/menu-host/display-overlay.patch)
cmake -S lib/ModernGekko -B lib/ModernGekko/build `
  -DMODERNGEKKO_RUNNER_OUTPUT_NAME=moderngekko-run-display -DBUILD_TESTING=ON
cmake --build lib/ModernGekko/build --target moderngekko-run moderngekko_pause_menu_policy_test
ctest --test-dir lib/ModernGekko/build -R moderngekko.pause_menu_policy --output-on-failure
```

The current workspace already has these patches applied. Build the updated
mod in `build/freecam-arrival-mods` using the camera build instructions with that
directory substituted. `scripts/play-freecam.ps1` uses this package and the
side-by-side `moderngekko-run-display.exe`, preserving existing running sessions.

Validation: Release navigation policy and camera-export regression tests pass.
Live Vulkan tests verified Esc pause/resume, keyboard/controller navigation,
mouse checkpoint creation, quit cancellation, frozen guest frames while menus
redraw, and persistent camera/audio settings with automation disabled. Visual
review covers Home, Settings, Controls and confirmation at the tested desktop
size. Resolution/HD comparisons in the loaded Pokemon Center scene held a median
59.94 FPS and approximately 100% speed at 1x, 3x and 6x with HD textures, and
at 3x with original textures (eight one-second samples per case after warmup).
This is a short stationary-scene check, not a game-wide performance guarantee.
Other platforms, online/netplay menu use and physical button-remapping
are unsupported/unvalidated by this update.

Fullscreen stress coverage and measured limits are documented in
[the stress report](../../docs/freecam-ultrawide-stress.md). A universal 60 FPS
lock is not yet validated; do not infer it from a static interior benchmark.
