# Colosseum host-support patches

The `.mgm` library in the parent directory is a normal ModernGekko code-mod
package. These additional files reproduce the host renderer/runtime support
that the Colosseum visual features currently require.

The patches do not run when ModernGekko loads the mod. Apply them once to a
source checkout before building:

```powershell
git clone --recursive https://github.com/ExpansionPak/ModernGekko.git
pwsh ./apply-host-patches.ps1 -ModernGekkoSource ./ModernGekko
```

On Linux or macOS:

```sh
git clone --recursive https://github.com/ExpansionPak/ModernGekko.git
sh ./apply-host-patches.sh ./ModernGekko
```

ModernGekko pins the compatible RecompCore revision in `vendor/dolphin`. Do not
replace that submodule with RecompCore `main`: the upstream `main` branch and
the `moderngekko-vendor` integration line are separate histories. The manifest
records the exact upstream-compatible revisions used to generate this bundle.

The apply command is idempotent and refuses dirty tracked files, the wrong
RecompCore line, or a patch that no longer applies cleanly. Use `-CheckOnly` to
verify a checkout without changing it.
