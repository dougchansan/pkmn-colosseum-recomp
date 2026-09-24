# Third-party notices

Pokemon Colosseum Recomp version 1.1.0 combines original project source and
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
  `LICENSES` directory specify additional licenses.

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
