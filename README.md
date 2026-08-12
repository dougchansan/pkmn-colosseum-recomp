# Pokemon Colosseum Recomp

<div align="center">

[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-blue)](#platform-status)
[![Release](https://img.shields.io/badge/release-v1.0.0-success)](#versioning)
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

### GitHub Release ZIP or tar.gz

Download `Source code (zip)` or `Source code (tar.gz)` from the GitHub Release,
extract it, and open a terminal in the extracted folder. These archives contain
the project source, not Pokemon Colosseum or a ready-to-run game module, so you
must provide the path to your own legally obtained USA (`GC6E01`) ISO.

Windows PowerShell:

```powershell
Set-Location "C:\path\to\pkmn-colosseum-recomp-1.0.0"
.\setup.ps1 -Iso "D:\path\to\Pokemon Colosseum.iso"
```

Linux or macOS:

```sh
cd /path/to/pkmn-colosseum-recomp-1.0.0
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
on Windows). Setup validates `GC6E01`, extracts the disc, builds the stable C
module, and creates a portable player directory.

Normal boot is the default even when savestates exist. In-game saves use the
portable `User/GC` memory card and survive package/setup refreshes. Savestate
loading remains available as an explicit launcher selection.

## Recommended launcher options

For the complete enhanced presentation, enable these three independent mods:

- `Native ultrawide (Hor+)`
- `Upscale dynamic game text (4x)`
- `High-contrast minimap colors`

The optional `60 FPS gameplay patch` can be enabled separately. The original
30 FPS behavior is the default, and changing the frame-rate patch takes effect
on the next game launch. `Hold Space to fast-forward` is a separate temporary
emulation-speed control and also starts disabled.

All enhancements start disabled, so the plain recomp can be verified first.

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

The portable C build is the supported player release. LLVM remains available
as an optimized developer backend in DolRecomp.

## Versioning

The current release is **1.0.0**. Releases follow
[Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

- `MAJOR` changes can break an existing user's setup or integrations. For
  example, `2.0.0` would be appropriate if the launcher stopped reading 1.x
  configuration files, existing saves or mod packages needed conversion, or
  the setup command changed incompatibly.
- `MINOR` changes add a feature while remaining backward compatible. That
  means a 1.x setup that worked before should keep working without conversion
  or changed commands. Adding an optional photo mode or a new graphics toggle
  could therefore be `1.1.0`.
- `PATCH` changes fix behavior without changing the supported interface. Here,
  compatible means existing commands, configuration, saves, and mods keep the
  same meaning. Fixing a stretched menu or a crash while loading the same save
  could therefore be `1.0.1`.

Git release tags use the conventional `v` prefix, such as `v1.0.0`.

## Platform status

| Platform | Status |
|---|---|
| Windows 11 x64 | Launcher, package refresh, C/LLVM builds, memory-card persistence, and gameplay verified. |
| Linux x64 | Native host/module/`.mgm` builds and headed Vulkan gameplay verified on Ubuntu 26.04 WSL2/WSLg (X11 and Wayland). |
| macOS arm64 | Native host/module/`.mgm` builds and headed Metal gameplay verified on macOS 26.1 with AppleClang 17. |

## Legal

Bring your own legally obtained USA disc dump. The source release does not
include or download disc images, extracted game files, keys, saves, generated
recompilation modules, or replacement game textures. Do not redistribute a
locally generated `gGC6E01_recomp` module, extracted files, memory cards, or a
complete built player directory.

Original project source and patches are licensed under
[GPL-3.0-or-later](LICENSE). DolRecomp, ModernGekko, RecompCore, and their
dependencies retain their upstream copyright notices and per-file licenses;
see [third-party notices](THIRD_PARTY_NOTICES.md). A binary release must be
accompanied by its matching source snapshot, license, notices, and build
instructions.

Pokemon, Pokemon Colosseum, Nintendo, Game Freak, and Creatures are trademarks
or copyrights of their respective owners. This fan project is not affiliated
with or endorsed by them.
