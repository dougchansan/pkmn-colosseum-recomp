# Pokemon Colosseum ModernGekko mods

These packages use ModernGekko's code-mod ABI and directory convention:

```text
<id>.mgm/
  mod.dll        # Windows
  mod.so         # Linux
  mod.dylib      # macOS
  host-patches/  # reproducible upstream host-support source patches
```

The enhanced rendering itself is implemented by the ModernGekko host because
ultrawide projection, texture upscaling, and minimap compositing happen outside
guest PowerPC code. Each package is still a first-class, game-checked mod with
a stable ID, semantic version, capability export, load ordering, and netplay
fingerprint.

Each package is self-contained for source builders. Its `host-patches` folder
contains the same checksum-verified ModernGekko and RecompCore patch bundle,
base revision manifest, and PowerShell apply command. ModernGekko ignores these
documentation/source files when loading the normal platform mod library.

To build from the current supported upstream baseline, clone ModernGekko with
its recursively pinned dependencies and apply the bundle from any one package:

```powershell
git clone --recursive https://github.com/ExpansionPak/ModernGekko.git
pwsh ./pokemon-colosseum-ultrawide.mgm/host-patches/apply-host-patches.ps1 `
  -ModernGekkoSource ./ModernGekko
```

On Linux or macOS:

```sh
git clone --recursive https://github.com/ExpansionPak/ModernGekko.git
sh ./pokemon-colosseum-ultrawide.mgm/host-patches/apply-host-patches.sh ./ModernGekko
```

The compatible RecompCore baseline is ModernGekko's pinned
`moderngekko-vendor` revision, not RecompCore `main`; those upstream histories
are currently separate. `host-patches/manifest.json` records both exact bases.

Release builds keep the packages in `Mods/Optional`. The ImGui launcher settings
select individual package directories, so all three features remain off by
default. A package can also be copied directly to `Mods/<id>.mgm` when managing
mods by hand; its capability export then enables the matching host-rendering
feature without a separate config edit.

Build on Windows with:

```powershell
cmake -S mods -B build/mods -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DMODERNGEKKO_SOURCE_DIR=lib/ModernGekko
cmake --build build/mods
```
