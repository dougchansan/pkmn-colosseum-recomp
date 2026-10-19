# Pokemon Colosseum Recomp

<div align="center">

[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-blue)](#platform-status)
[![Release](https://img.shields.io/badge/release-v1.3.3-success)](#versioning)
[![Backends](https://img.shields.io/badge/backends-C%20%7C%20LLVM-8A2BE2)](#source-and-mods)
[![Recompiler](https://img.shields.io/badge/recompiler-DolRecomp-informational)](https://github.com/ExpansionPak/DolRecomp)
[![Runtime](https://img.shields.io/badge/runtime-ModernGekko-informational)](https://github.com/ExpansionPak/ModernGekko)
[![Decomp](https://img.shields.io/badge/decomp-dougchansan%2Fpkmn--colosseum-brightgreen)](https://github.com/dougchansan/pkmn-colosseum)
[![Decomp progress](https://decomp.dev/dougchansan/pkmn-colosseum.svg?mode=shield)](https://decomp.dev/dougchansan/pkmn-colosseum)

</div>

A release-focused static recompilation of the USA version of Pokemon
Colosseum (`GC6E01`) using [DolRecomp](https://github.com/ExpansionPak/DolRecomp)
and [ModernGekko](https://github.com/ExpansionPak/ModernGekko).

The stable player uses the portable C backend at the original 30 FPS. Native
Hor+ ultrawide, dynamic HD text, and a high-contrast minimap are independent
ModernGekko mods. No Nintendo game data is included or downloaded; bring your
own legally obtained USA disc dump.

## Install

The setup scripts build the player but do not install system packages. Install
Git, CMake, Ninja, Python 3, and Clang first. Windows also needs Visual Studio
2022 or newer C++ Build Tools and the Windows SDK; macOS needs current Xcode
command-line tools; Linux needs ModernGekko/Dolphin's SDL, GTK, audio, Vulkan,
X11, and Wayland development packages. Setup stops with the name of any
missing command.

Setup verifies the supported USA ISO (SHA-256
`78c4074429fa598b97bfac296bf8e0f7e93f5cb6d8679f8d2653ca902820f6c5`)
and its original `main.dol` before applying game patches. A stale or modified
extraction is rejected with an explanation.

### Release packages (easiest)

Download the package for your system from the GitHub Release
(`PokemonColosseumRecomp-v…-win64.zip`, `-linux-x86_64.zip` or `-macos-arm64.zip`),
unzip it, and open `PokemonColosseumRecomp` (`.exe` on Windows, the `.app` on
macOS). Choose your own USA (`GC6E01`) disc image (ISO, RVZ or WBFS) and press
**Extract and play**. Nothing else needs installing:

1. The launcher extracts the disc next to itself.
2. It downloads a compiler once — [Zig](https://ziglang.org) 0.14.1, 45 to 80 MB
   depending on the platform, checked against its published SHA-256 — into
   `User/Toolchains`.
3. It builds the game module from your extracted disc on your computer (about 5
   to 15 minutes, with a progress bar), then starts the game.

The packages contain no game code: the module is built from your disc each time
it is set up, which is why the first start takes a while. Later starts are
immediate. If the build stops (for example, no internet for the compiler
download), **Build game** on the Play page retries it; the details are in
`User/Logs/module-build.log`.

### GitHub Release ZIP or tar.gz (source)

Download `Source code (zip)` or `Source code (tar.gz)` from the GitHub Release,
extract it, and open a terminal in the extracted folder. These archives contain
the project source, not Pokemon Colosseum or a ready-to-run game module, so you
must provide the path to your own legally obtained USA (`GC6E01`) ISO.

Windows PowerShell:

```powershell
Set-Location "C:\path\to\pkmn-colosseum-recomp-1.3.3"
.\setup.ps1 -Iso "D:\path\to\Pokemon Colosseum.iso"
```

Linux or macOS:

```sh
cd /path/to/pkmn-colosseum-recomp-1.3.3
sh ./setup.sh "/path/to/Pokemon Colosseum.iso"
```

### Git checkout

Windows PowerShell:

```powershell
git clone https://github.com/dougchansan/pkmn-colosseum-recomp.git; cd pkmn-colosseum-recomp; .\setup.ps1 -Iso "C:\path\to\Pokemon Colosseum.iso"
```

Linux or macOS:

```sh
git clone https://github.com/dougchansan/pkmn-colosseum-recomp.git && cd pkmn-colosseum-recomp && sh ./setup.sh /path/to/Pokemon-Colosseum-USA.iso
```

Launch `build/release/PokemonColosseumRecomp/PokemonColosseumRecomp` (`.exe`
on Windows). On macOS, the release packages also carry
`PokemonColosseumRecomp.app`: open that instead, so macOS can ask once for
Local Network access (online battles, netplay and GBA link play need it). A
downloaded app is blocked by Gatekeeper the first time: Control-click it and
choose Open. Setup validates `GC6E01`, extracts the disc, builds the stable C
module, and creates a portable player directory.

Normal boot is the default even when savestates exist. In-game saves use the
portable `User/GC` memory card and survive package/setup refreshes. Savestate
loading remains available as an explicit launcher selection.

## Recommended launcher options

For the complete enhanced presentation, enable these four independent options:

- `Native ultrawide (Hor+)`
- `Upscale dynamic game text (4x)`
- `High-contrast minimap colors`
- `60 FPS gameplay patch`

`60 FPS` doubles the game's own frame target, so it is worth knowing what it
does and does not deliver. Field and menu scenes hold a locked 60; the heaviest
battle scenes do not, and land between roughly 47 and 55 FPS on the machine
these figures came from. See [Performance](#performance) for per-scene numbers.
A scene that cannot keep up does not fall back to a clean 30. The frame
target is held at 60 by the host each VI field, so when the machine cannot
sustain it the whole emulation slows down: the heaviest battle scene measured
here runs its VI at about 34 fields per second against hardware's 59.94, which
is roughly 57% of real time rather than full-speed 30 FPS. Turning the patch off
does not change that scene's rate, so this is emulation cost, not a cost of the
patch.

The original 30 FPS behavior remains the shipped default, and changing the
frame-rate patch takes effect on the next game launch. `Hold Space to
fast-forward` is a separate temporary emulation-speed control and also starts
disabled.

The launcher options above start disabled, so the plain recomp can be verified
first. The HD texture packs below are the exception: setup installs and enables
them.

### HD texture packs

Setup downloads two community texture packs and switches them on. Neither is
stored in this repository. [`mods/hd-packs.json`](mods/hd-packs.json) pins each
one -- a SHA-256 for the zip, a commit for the git pack -- and a download that
does not match is discarded. Downloads are kept in `build/hd-packs`, so running
setup again does not fetch them twice.

- **UI textures from [OrreHD](https://github.com/OrreHD/ColosseumHD-Texturepack)**
  (78 files, 139 MB): the title screen, menus, menu text, PC and box screens,
  battle icons, PDA and world map, installed to `User/Load/Textures/GC6E01`.
  Only these files are fetched; OrreHD's scenery, character and Pokemon
  textures are left out.
- **Community HD texture pack** by **Sephie** (689 MB): everything else --
  Pokemon, characters, effects and locations -- distributed by its author
  ([Sephie's post](https://www.reddit.com/r/DolphinEmulator/s/IgXkBDodBC)),
  installed to `User/ResourcePacks` and shown in the launcher as
  `Community HD texture pack (Sephie)`.

Both packs cover the UI. `Load/Textures/GC6E01` is searched before the
community pack's `GC6` folder and the first file for a texture wins, so
OrreHD's versions are used for the UI and the community pack fills in
everything else.

To skip both downloads, run `.\setup.ps1 -Iso ... -NoHdPacks` on Windows or
`HD_PACKS=0 sh ./setup.sh ...` on Linux and macOS. If a download fails, setup
warns and finishes without that pack. A `User/config.ini` from an earlier setup
keeps its own `community_hd_texture_pack` setting; tick the option in the
launcher to turn the pack on there.

The community pack costs performance -- roughly 4% on the heaviest battle scene
when it was last measured -- so turn it off first if a scene runs short of full
speed.

## Performance

The game module replaces a handful of hot guest functions with native ones:
the paired-single matrix routines and `memcpy`/`memset`, listed in
`patches/GC6E01-native-substitutions.csv`. They are semantically identical to
the code they replace, reproducing the Gekko's fused multiply-adds exactly, and
any call they cannot handle is declined and runs the original guest code.

Measured across seven savestates covering field, menu and battle scenes, with
the 60 FPS patch on, in unique frames per second:

| scene | without substitutions | shipped |
|---|---|---|
| Phenac field (ultrawide) | 51.2 | 57.7 |
| Phenac field (16:9) | 53.6 | 59.9 |
| Battle Now menu | 60.0 | 59.9 |
| Pokemon select | 42.8 | 48.4 |
| battle arena | 45.9 | 50.5 |
| battle, alternate | 26.9 | 41.8 |
| in-battle | 59.9 | 59.9 |
| **mean** | **48.6** | **54.0** |

Numbers are from one machine and are meant for comparing the two columns, not
as a target for yours.

The trainer-introduction scene at the start of a battle is heavier than any
entry above: it measured about 34 unique FPS, roughly 57% of real time. That
one is bound by the recompiled guest code and not by anything you can configure.
Dropping the internal resolution from 5120x2880 to 1280x720 -- sixteen times
fewer pixels -- bought about 7%; switching the graphics backend to Null, which
removes all GPU work, changed the emulation speed by nothing at all
(0.564 either way); turning off the HD texture pack changed nothing; and 16:9
instead of 32:9 gained about 4%. Profile-guided optimization below is the only
lever that moves it, and it is worth about 5%.

Frame rate is quoted as unique game frames rather than emulation speed on
purpose. Emulation speed is measured against whatever frame target the game is
currently using, and the 60 FPS patch changes that target at runtime, so a
scene that drops to the 30 FPS path can report a *better* speed while
delivering far fewer frames.

### Optional: profile-guided optimization

A further ~5% is available by optimizing the module against a profile of real
play. The profile cannot be shipped with the project: it is instrumentation
counters over the recompiled game code, so it is derived from the game itself.
Anyone who wants it trains their own, locally:

```powershell
scripts\train-pgo.ps1 -Phase train     # builds an instrumented module
# play normally for 10-15 minutes across a few areas, including a battle
scripts\train-pgo.ps1 -Phase finish    # merges, rebuilds, installs
```

```sh
scripts/train-pgo.sh train
# ... play, then quit ...
scripts/train-pgo.sh finish
```

The instrumented module used during training is much slower than the normal
one; that is the cost of recording and is not what you keep. Train on what you
actually play — a profile recorded entirely in one town optimizes for that
town, and will be worth less than the figure above.

## Default keyboard controls

The Windows package includes a keyboard fallback. Selecting an SDL gamepad in
the launcher replaces it with ModernGekko's generated controller profile.

| GameCube control | Keyboard mapping |
|---|---|
| Main Stick Up / Down / Left / Right | `W` / `S` / `A` / `D` |
| C-Stick Up / Down / Left / Right | `I` / `K` / `J` / `L` |
| A | `X` |
| B | `Z` |
| X | `C` |
| Y | `V` |
| Z | `Left Shift` |
| Start / Pause | `Enter` |
| L | `Q` |
| R | `E` |
| D-Pad Up / Down / Left / Right | Arrow keys |

## Source and mods

The setup scripts clone the official upstream DolRecomp and ModernGekko
repositories, check out the public revisions recorded by the release, then
apply the game-specific, checksum-pinned host patches from
`mods/host-patches`. The release therefore uses only public upstream sources
while retaining Colosseum's renderer integration.

The optional packages follow ModernGekko's convention:

```text
Mods/Optional/<id>.mgm/
  mod.dll      # Windows
  mod.so       # Linux
  mod.dylib    # macOS
  host-patches/
```

Each package has a stable ID, semantic version, game/ABI checks, capability
export, dependency ordering, and netplay fingerprint. See
[`mods/README.md`](mods/README.md) for package IDs and patch details.

The portable C build is the supported player release. The LLVM backend also
builds a working module, but it is **not** faster: measured windowed with every
enhancement enabled, C beats it by ~1.8x (per-scene 1.62x-2.00x across seven
scenes), and the LLVM module is 4.4x larger (190 MiB from 4890 objects against
43 MiB from 154 chunks). Treat LLVM as an alternative code path for development,
not as an optimisation. These figures are from the August 2026 seven-scene
local A/B comparison; its generated report lives under the ignored `build/`
directory and is not part of the source release.

## Versioning

The current release is **1.3.3**. Releases follow
[Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

- `MAJOR` changes can break an existing user's setup or integrations. For
  example, `2.0.0` would be appropriate if the launcher stopped reading 1.x
  configuration files, existing saves or mod packages needed conversion, or
  the setup command changed incompatibly.
- `MINOR` changes add a feature while remaining backward compatible. That
  means a 1.x setup that worked before should keep working without conversion
  or changed commands. Adding an optional photo mode or a new graphics toggle
  could therefore be `1.4.0`.
- `PATCH` changes fix behavior without changing the supported interface. Here,
  compatible means existing commands, configuration, saves, and mods keep the
  same meaning. Fixing a stretched menu or a crash while loading the same save
  could therefore be `1.1.1`.

Git release tags use the conventional `v` prefix, such as `v1.3.3`.

## Platform status

| Platform | Status |
|---|---|
| Windows 11 x64 | Launcher, package refresh, C/LLVM builds, memory-card persistence, and gameplay verified. |
| Linux x64 | Native host/module/`.mgm` builds and headed Vulkan gameplay verified on Ubuntu 26.04 WSL2/WSLg (X11 and Wayland). |
| macOS arm64 | Native host/module/`.mgm` builds and headed Metal gameplay verified on macOS 26.1 with AppleClang 17. |
| Raspberry Pi 4 (aarch64) | Native host and module builds verified on Debian 13 (trixie), kernel 6.18, with the V3D Vulkan driver. Expect frame rates far below the x64 figures in [Performance](#performance) — that section compares module builds on one x64 machine and is not a target for this board. See the [August 2026 Pi performance campaign](docs/pi4-performance-2026-08-20.md) for guarded A/B results and their limits. |

## Legal

Bring your own legally obtained USA disc dump. The source release does not
include or download disc images, extracted game files, keys, saves, generated
recompilation modules, or replacement game textures. Do not redistribute a
locally generated `gGC6E01_recomp` module, extracted files, memory cards, or a
complete built player directory.

Original project source and patches are licensed under
[GPL-3.0-or-later](LICENSE). DolRecomp, ModernGekko, RecompCore, and their
dependencies retain their upstream copyright notices and per-file licenses;
see [third-party notices](THIRD_PARTY_NOTICES.md). Anyone distributing a binary
must provide its complete corresponding source, including the exact project
revision, upstream revisions, and patches used to build it, with license,
notices, and build instructions. See [security reporting](SECURITY.md) for
how to report a vulnerability.

Pokemon, Pokemon Colosseum, Nintendo, Game Freak, and Creatures are trademarks
or copyrights of their respective owners. This fan project is not affiliated
with or endorsed by them.
