# Changelog

All notable release changes are documented here. This project follows
[Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-09-15

Performance release. The recompiler moved onto current upstream, and three
measured wins that the packaged build was not actually getting are now wired
into it. On the seven-scene benchmark set, with the 60 FPS patch and the mod
packages on, six of seven scenes hold 60; `postbattle-anchor` does not - see
Known limitations.

Figures below were measured on one Windows machine (a multi-die CPU and an
RX 9070 XT) against the 2026-08-19 baseline in `docs/`. They are the shape of
the change, not a promise for any particular system.

- Guest idle skip. The game's `OSSetIdleFunction` body spins on a task list that
  is usually empty, and the existing idle mechanism could never fire on it
  because the loop's back edge never reaches the dispatcher. A second,
  task-list-aware trigger reads the two guest list heads and idles only when the
  coming iteration provably has no work. Worth battle-mid 55.1 to 60.0 FPS
  capped, and 59.0 to 67.9 uncapped. Unreadable guest addresses answer "not
  empty", the list walk is bounded at 256 nodes, and the feature is inert unless
  the three `StaticRecompTask*` keys in `User/GameSettings/GC6E01.ini` name the
  loop.
- The guest idle skip is a launcher option, on by default. It is a trade rather
  than a free win -- see Known limitations -- so it can be turned off without
  hand-editing an ini. The runtime owns the three game-specific keys and
  rewrites `User/GameSettings/GC6E01.ini` from the toggle on every boot.
- The launcher now pins the runner to the largest shared cache before spawning
  it, on parts where one cache domain is genuinely bigger than the rest. Mean
  +15.9% across the benchmark scenes, 95% CI [+13.7%, +18.1%], 21 of 21
  interleaved pairs favouring pinned. On a part whose dies are identical it
  declines and changes nothing. A bare `moderngekko-run` is untouched, so
  existing measurements stay comparable.
- `DCFlushRange` joins the native substitution manifest. It was the largest
  single guest cost remaining in battle scenes by PC sampling, and batches the
  per-line instruction-cache invalidation into one range call.
- The recompiler and runtime are rebased onto current upstream DolRecomp and
  ModernGekko through the vendored tree.
- Releases are built with `package-imgui-launcher.ps1 -Distribution`, which
  generates a package from nothing under `build/dist`. An ordinary run refreshes
  `build/release` and deliberately preserves its `User` directory, so on a
  machine that has played the game that directory accumulates a memory card,
  savestates and that player's own settings. The distribution mode writes to its
  own path, never touches `build/release`, excludes the locally generated game
  module, and fails if `User` contains anything but the generated config, game
  settings and controller profile. No replacement game textures are included.
- Benchmark and profiling harnesses used for the above: A/B runners that hold
  the module byte-identical across arms, a PC-sample attributor, a cold-boot
  harness, and a CPU ABI guard.

- The community HD texture pack by Sephie is now pointed at from the places a
  player looks: the README, the third-party notices, and the launcher's own
  "Not installed" line, which previously said where to put the zip without
  saying where to get it. The pack is not redistributed here -- it is its
  author's work and its author's download.
- The curated UI and title-screen texture overrides are not shipped in this
  release. They are replacement game textures and are excluded from public
  source archives and distribution packages.

### Fixed

- The launcher's texture-pack panel asserted a hardcoded nine curated UI
  overrides. It now counts what is actually installed, excluding the files
  the community pack itself provides, and says nothing about overrides when
  there are none.

- Story battles were pillarboxed to 4:3 on ultrawide displays.
- Enabling the 60 FPS option cost a permanently interpreted chunk of guest code.
  The optional module was built with `--dol-patch 80005D98=38600000`, baking in
  the 60 FPS Action Replay instruction, but the runner patches the player's DOL
  from `GC6E01-native-widescreen.csv`, which does not list that address. Guest
  RAM therefore kept the original `886D85B0` while the module held `38600000`,
  the static recompiler's hash guard saw the mismatch, and chunk
  `[0x800055E0,0x800095E0)` ran in the interpreter for the whole session. The
  patch bought nothing even when it matched: that instruction is never executed,
  and 60 FPS comes from the runner's `sixty_fps` switch. Removed from all six
  places that applied it: `scripts/package-imgui-launcher.ps1`,
  `scripts/setup-unix.sh`, and both build sites in each of `scripts/train-pgo.ps1`
  and `scripts/train-pgo.sh` -- so source builds and PGO-trained modules on every
  platform were affected, not just the Windows package. The 60 FPS module is now
  byte-identical to the stable one apart from its link timestamp, and the option
  still doubles the guest frame rate.
- The idle trigger the release shipped with was compiled in but never
  configured, so the packaged build ran without it.
- The host-patch bundle carried a submodule pointer for `DolRecomp` naming a
  commit that exists only on a local branch. The supported setup path applies
  these patches ignoring submodules, so it was inert there, but anyone applying
  the bundle by hand would have had their `DolRecomp` checkout pointed at a
  commit they cannot fetch. Same defect as the `vendor/dolphin` one fixed in
  1.0.0, in a second bundle.
- The host-patch bundle still added `tools/agent_move_names.inc`, a generated
  move-name table built for the private development harness by a script that is
  not part of this project. Its only consumer was removed with the harness, so
  nothing referenced it. The patch checksums in `mods/host-patches/manifest.json`
  are updated for both regenerated bundles.
- The per-mod copies of the host-patch bundle had drifted behind the canonical
  one in `mods/host-patches`. Packaging overwrote them anyway, so no shipped
  build was affected, but the repository copies were stale.
- `default-game.txt` is no longer written into player packages. Its relative
  path pointed outside the package at the build machine's extracted game, so a
  downloaded copy named a directory that does not exist. A package with no such
  file opens on the disc picker, which is the correct first run. The in-repo
  developer install still gets one.

### Known limitations

- The guest idle skip is a trade, not a free win, and the capped measurement
  above hides one side of it. Measured uncapped on the headless guest-CPU
  screen, pinned, five interleaved pairs with the first excluded and a
  byte-identical module in both arms: `field-phenac` runs **8.3% slower with the
  idle trigger on** (0 of 4 pairs favour it, sd 3.0 points, 95% CI [-13.0%,
  -3.5%]). On a field scene the task list is rarely empty, so each pass pays for
  the bounded list walk and then declines to idle. The capped table shows
  59.9 to 59.9 because that scene sits at the 60 cap with headroom to spare, so
  the cost comes out of headroom nobody was using. The same sweep could not
  resolve the battle-mid gain either way (paired mean +7.2% but 95% CI [-11.2%,
  +25.6%]); the capped windowed figure above remains the measurement of record.
  On hardware that cannot hold 60 on field scenes the trade may not be worth it
  there, which is why it is a launcher toggle rather than a fixed decision.

- `postbattle-anchor`, the most demanding of the seven benchmark scenes, runs
  at roughly 46 FPS with every option on and does not reach 60. Its cost is a
  broad spread of small guest calls rather than one hot function. A block-cache
  range early-out prototyped during this cycle measured +17.8% on that scene but
  is not correctness-verified and is deliberately not included; it is written up
  in `docs/upstream-jitcache-range-earlyout.md` for upstream.

## [1.0.0] - 2026-08-12

Initial player release with the portable C backend, the ModernGekko ImGui
launcher, normal memory-card progression, and optional native Hor+ ultrawide,
dynamic HD text, and high-contrast minimap mod packages.

- Native Hor+ ultrawide rendering, including correctly proportioned authored
  menus and character naming.
- Optional 4x dynamic game-text upscaling and high-contrast navigation map.
- Optional 60 FPS gameplay patch, with the original 30 FPS behavior as the
  default.
- ImGui launcher with portable controller, graphics, mod, and save settings.
- Normal in-game memory-card progression with explicit savestate loading.
- Source setup for Windows, Linux, and macOS using public ExpansionPak
  DolRecomp, ModernGekko, and RecompCore revisions.
- Raspberry Pi 4 (aarch64) added to the platform list: native host and module
  builds verified on Debian 13, at frame rates far below the x64 figures.
- Native replacements for the hottest guest routines - the paired-single matrix
  functions and `memcpy`/`memset` - listed in
  `patches/GC6E01-native-substitutions.csv` and applied at recompile time.
  Measured over seven field, menu, and battle savestates with the 60 FPS patch
  on: mean 48.6 to 54.0 unique frames per second, no scene slower. The
  substitutions reproduce the Gekko's fused multiply-adds exactly, and any call
  they cannot serve is declined and runs the original guest code.
- Optional profile-guided optimization through `scripts/train-pgo.ps1` and
  `scripts/train-pgo.sh`, worth roughly a further 5%. The profile is trained
  locally on your own play because it is derived from the recompiled game code
  and cannot be distributed with the project.
- `moderngekko-port` gains `--native-subst`, `--arch-flags`, `--pgo-generate`,
  `--pgo-use`, and `--profile-runtime`, and folds all of them into the module
  cache identity so builds that differ in any of them cannot share a cached
  module.

### Fixed

- Character and world shadows oscillated on ultrawide displays: the square
  offscreen shadow viewport was being scaled by the output aspect correction,
  which does not apply to a buffer authored square and sampled through a
  projector.
- Aspect-ratio flicker on the Battle Now menus when moving between single and
  double battle or backing out of a dropdown.
- The 60 FPS option now takes effect. The Action Replay instruction it relied on
  is never executed, because the function containing it is superseded during
  boot; the frame period is now set host-side instead.
- The host-patch bundle no longer carries a stray submodule pointer that
  silently repointed `vendor/dolphin` when applied.
