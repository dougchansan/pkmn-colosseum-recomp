# Third-party notices

Pokemon Colosseum Recomp version 1.3.3 combines original project source and
patches with upstream open-source projects that are fetched at setup time.
Copyright and license notices in upstream source files remain in effect.

## Project source and patches

Copyright (C) 2026 Douglas Whittingham

The original project source and patches are licensed under the GNU General
Public License, version 3 or (at your option) any later version. See `LICENSE`.

## Upstream projects

- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) is distributed under
  the GNU General Public License; its files retain their SPDX identifiers and
  copyright notices.
- [ModernGekko](https://github.com/ExpansionPak/ModernGekko) is distributed
  under the GNU General Public License and contains Dolphin-derived runtime
  code; its files retain their SPDX identifiers and copyright notices.
- [RecompCore](https://github.com/ExpansionPak/RecompCore) states that most
  original Dolphin source is GPL-2.0-or-later and that the repository as a
  whole is GPLv3-compatible. Per-file SPDX identifiers and the upstream
  `LICENSES` directory specify additional licenses. Binary artifacts include
  copies of these licenses and notice files from vendored dependencies under
  `licenses/`.

- Roboto Medium (`Sys/Resources/Roboto-Medium.ttf`), the launcher's font until
  the game's own fonts are read from the player's disc, is Copyright 2011
  Google Inc. and licensed under the Apache License, Version 2.0; it is taken
  unmodified from Dear ImGui's `misc/fonts`. Binary packages carry the license
  under `licenses/Roboto/`.
- [Zig](https://ziglang.org) 0.14.1 is not distributed with this project. The
  launcher downloads it from ziglang.org, checks it against the published
  SHA-256, and uses its bundled Clang/LLD toolchain to build the game module on
  the player's machine; it remains under its own licenses (MIT for Zig, with
  the licenses of its bundled LLVM and C library components).
- Binary packages carry, under `Tools/ModuleSource/`, the ModernGekko and
  Dolphin-derived runtime sources (GPL) that are compiled into the locally
  built game module.

The setup scripts identify the upstream revisions used by this release and
apply the game-specific host patches. When distributing object code, provide
the complete corresponding source for the exact binary being distributed,
including this project's matching revision, the upstream revisions and patches
used to build it, this notice, the applicable licenses, and build instructions.
Pointing only to a moving default branch is insufficient for a reproducible
source offer.

## Optional community assets

The community HD texture pack for Pokemon Colosseum is the work of **Sephie**
and is published by its author at <https://www.reddit.com/r/DolphinEmulator/s/IgXkBDodBC>.
It is **not** distributed with this project in source or binary form; the
launcher only loads it if a player installs it themselves. All rights in that
pack remain with its author.

## Game data and marks

No disc image, extracted game data, cryptographic key, save data, generated
recompilation module, or replacement game texture is part of the source
release. Those materials must not be added to a public source archive.

Pokemon, Pokemon Colosseum, Nintendo, Game Freak, and Creatures are trademarks
or copyrights of their respective owners. This project is an unaffiliated fan
project and is not endorsed by those owners.
