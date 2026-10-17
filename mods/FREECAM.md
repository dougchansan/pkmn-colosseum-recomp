# C-stick third-person and free camera (GC6E01)

Build the optional `colosseum_freecam_mod` target with the commands in
[README.md](README.md). The package is generated at
`build/mods/packages/pokemon-colosseum-freecam.mgm` (or under your chosen build
directory). It requires the current ModernGekko CPU ABI, like the fastmath mod.
The local play helper uses `build/freecam-arrival-mods` for the updated package,
allowing an older free-camera session to remain running during the build.

Build that package from Developer PowerShell with:

```powershell
cmake -S mods -B build/freecam-arrival-mods -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DMODERNGEKKO_SOURCE_DIR=(Resolve-Path lib/ModernGekko).Path
cmake --build build/freecam-arrival-mods --target colosseum_freecam_mod colosseum_freecam_test
```

The runner also needs the included `freecam-runtime.patch`: it prevents the
fallback JIT from combining calls and returns across mod hook boundaries.
Apply it to ModernGekko's Dolphin/RecompCore checkout and rebuild the runner:

```powershell
git -C lib/ModernGekko/vendor/dolphin apply (Resolve-Path mods/freecam-runtime.patch)
cmake --build lib/ModernGekko/build --target moderngekko-run
```

This workspace already has the patch applied on `codex/c-stick-freecam` in the
runtime checkout. Existing prebuilt runners need rebuilding before this mod
can work. The change is limited to the static-recompilation JIT fallback;
ordinary Dolphin JIT behavior is preserved. ARM64 has the equivalent source
fix but requires validation on that platform.

Older C game modules with unguarded cross-chunk direct calls also bypass hooks.
Build a compatible C module with `-DCMAKE_C_FLAGS=-DDOLRECOMP_C_MAX_CALL_DEPTH=0`
in the module-template CMake configure command. This preserves the generated
game code and forces cross-chunk calls back through the runtime dispatcher.
The local test module is at `build/freecam-module/gGC6E01_recomp.dll`; it is
generated from private disc data and must not be distributed. This conservative
configuration may cost performance compared with the normal module.

Launch the local build with:

```powershell
./scripts/play-freecam.ps1
# Or resume a private test checkpoint:
./scripts/play-freecam.ps1 -LoadState ./states/part1-phenac-nw.sav
```

The helper uses a separate user/card directory at `build/freecam-user` and
copies the repository's `User/Config` controller settings on the first launch;
pass `-UserDirectory` to choose your normal save directory and controller
configuration. It also accepts `-Game` and `-Module` overrides. Existing
configuration is preserved on subsequent launches.

Copy the package directory to your portable installation's
`Mods/pokemon-colosseum-freecam.mgm`, or pass the package directly:

```powershell
./lib/ModernGekko/build/moderngekko-run-display.exe --module ./build/freecam-module/gGC6E01_recomp.dll `
  --mods ./build/freecam-arrival-mods/packages/pokemon-colosseum-freecam.mgm
```

During normal field exploration on controller port 1:

| Control | Action |
| --- | --- |
| C-stick | Orbit horizontally/vertically around the player by default |
| Left stick | Walk in the direction of the current camera view |
| Hold D-pad Up / Down | Zoom the third-person camera in / out |
| Z + X (right shoulder + X) | Toggle first-person view; C-stick looks, left stick walks |
| Z + Y (right shoulder + Y) | Toggle detached free camera from the orbit view |
| C-stick in free camera | Look horizontally and vertically |
| Left stick in free camera | Fly forward/back and strafe |
| L / R in free camera | Descend / ascend (digital or analog trigger) |
| Z + D-pad Down | Leave free camera and recenter the third-person view |

The third-person view follows the player's normal camera target and retains
the initial camera distance. Moving the C-stick changes the viewing angle;
moving the left stick turns and walks the player relative to that angle.
A/B, triggers, and menu navigation keep normal behavior in third-person mode.
X/Y are reserved for camera controls and are masked from the guest game after
the mod reads their shoulder chords, so they cannot open conflicting game menus.
Returning from free flight resumes the previous third-person angle. Recenter
resets it to the game's authored angle and distance. Door transitions preserve
yaw, pitch, zoom, and the camera's distance from the player, then reattach the
view to the destination's player target. Travel between different areas carries
that chosen yaw offset into the destination's authored camera orientation,
so a differently oriented city does not reverse the view. In first-person,
every field arrival instead starts level and facing the player's destination
spawn direction, straight out from the entrance. C-stick looking resumes normally
once the transition completes. First-person mode
also survives field transitions; Z + X returns to third person with the current
look direction and retained zoom. Its eye follows the player's ground transform
at a 14-unit height and 2-unit forward offset; tight walls and the player model
can still clip, because the mod does not add camera collision. First-person
rendering collapses only Wes's head joint to remove head/hair occlusion, retaining
the body for downward views. All joint transforms and cached scale are restored
after each draw; third-person, NPCs, and non-field cameras retain normal models.
The eye follows the live player model position, including elevated indoor floors. Loading and scripted cameras suspend
the override; the retained view resumes when normal field control returns.
Zoom is continuous while held, limited to 35–250% of the authored camera
distance. Only explicit recenter restores 100% and adopts the current room's authored
distance. Zoom is retained when
switching to free flight and back; detached flight uses its existing movement
controls. Up/Down are consumed for zoom during field play and remain available
for menu navigation when the game's menu-active flag is set.

Camera controls are consumed during detached flight, so moving the free camera does not
move the player. The game continues running; this is not a pause mode. Free
camera automatically turns off on floor changes, scripted camera modes,
animation, or controller disconnection. Enable it again after the transition.
Only the main field camera is overridden; menus, battle cameras, and minimaps
are excluded. Fixed indoor cameras without interpolation-bank entries are supported too.

Movement reads the game's retrace delta through the host's locked-cache memory
access, with a 12-count stick deadzone and a
pitch limit to prevent flipping. Speed is 250 game units per second; look speed
is 2 radians per second. These constants are in `freecam_mod.c`.

The mod uses entry/return hooks instead of replacing game functions. It applies
temporary eye, interest, and up-vector values while the field view matrix is
built, then restores the game's camera values. Later rendering passes reapply
the override only to the identified main camera. The rendering camera's cached
view matrix remains active for drawing. A dirty flag ensures the original view
is rebuilt when the orbit is recentered. Player movement reads the orbit heading
through two scoped hooks on `cameraGetRotY`; other users of that getter retain
the authored heading. Floor reset ends detached flight but retains the third-person view. Mod state is
not serialized in savestates: disable the camera before manually loading one.

Orbit has no added camera collision solver, so tight rooms can clip walls.
Moving beyond the authored camera can expose unloaded geometry, missing faces,
or camera-dependent culling. This is a debugging/exploration mod and does not
change collision, room loading, or visibility logic. Other camera mods may
conflict; multiplayer use is not validated.

Run the native regression tests with:

```powershell
cmake --build build/freecam-arrival-mods --target colosseum_freecam_test
ctest --test-dir build/freecam-arrival-mods --output-on-failure
```

Hook addresses and memory layouts come from the companion
[Colosseum decompilation](https://github.com/dougchansan/pkmn-colosseum):
`config/GC6E01/symbols.txt`, `src/game/camera.c`,
`include/game/gs_render_util.h`, `include/game/gs_gfx.h`, and
`include/dolphin/pad/Pad.h`. Other regions are unsupported.

Validation on Windows: Release regression tests, Clang warnings/format checks,
MSVC static analysis, and the runtime's mod-loader/CPU ABI tests pass. A live
headless Phenac City test verified default orbit centered on the player,
following while walking, and walking aligned with the camera (dot product
0.99995). Detached flight still leaves the player stationary. Regression tests
also cover movement-only heading reads, normal input preservation, mode
transitions, frame timing, scene reset and controller disconnection.
A supplied USA GCI also boots into `M3_pc_1F` (Pokemon Center), including its
fixed camera with zero bank entries. Orbit and zoom are verified there too.
Physical controller feel, wider room coverage,
Linux/macOS, and the ARM64 runtime fix remain unvalidated.

Door continuity validation: native floor data confirms `M3_pc_1F` (42) to
`M3_out` (36). Orientation error was below 0.000001 and camera distance
remained 84.7538 game units relative to the player, including a 40-unit ground
height change. Loading and controller-not-ready polls retain the view; explicit
recenter still adopts the destination camera. Wider transitions remain untested.

The Windows runner also has an [in-game Esc menu](menu-host/README.md) with
camera sensitivity/invert Y/recenter, controller selection, graphics/audio
settings, checkpoints and quit confirmation. Open it with Esc or Start + Z.
