# Standalone distribution

The pinned Dawn component inventory, notices, build-only exclusions and audit
evidence are included under `licenses/DawnPrebuilt`, with provenance in
`SOURCES.json`. Packaging rejects incomplete audits and verifies the native
Dawn link library or runtime DLLs against the audited release hashes. The
Windows package replaces the upstream SDK-derived `dxil.dll` with the exact
official DXC release validator recorded in that inventory.

The standalone host uses GXRuntime/Aurora without the Dolphin runner. Its
distribution contains no disc image, recompiled game module, extracted game
data, texture pack, memory card, settings, shader cache, or Dolphin Sys tree.
Players supply their own GC6E01 disc. Included setup tools generate the module
locally without a Dolphin runner; see SETUP.md. Python, CMake, Ninja and Clang
are prerequisites on the player's machine.

From a recursive checkout, with CMake 3.25+, Ninja and a C++20 compiler:

```sh
cmake -S tools/aurora-mirror -B build/standalone -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCOLOSSEUM_SETUP_RELOCATABLE=ON
cmake --build build/standalone --target colosseum-standalone colosseum-setup-tools --parallel 4
python scripts/package-standalone.py --build build/standalone --output build/dist/ColosseumStandalone
```

On Windows use an MSVC developer shell and add `--windows` to the packaging
command. Runtime DLLs must already be staged beside the executable by CMake.
Before Windows packaging, run `python scripts/fetch-standalone-runtime.py`
to acquire the exact official DXC archive pinned by the audited notice
manifest. This is an explicit download of the existing DXIL validator, cached
under `build/prebuilt-runtime`. Packaging itself stays offline: it verifies
the archive and exact `bin/x64/dxil.dll` hashes, and substitutes that verified
validator for the Dawn package's SDK-derived copy. A missing or mismatched
archive fails packaging. CI performs the acquisition as a separate step.
The packager also requires a complete component and license inventory with
source pins, evidence for build-only exclusions, and no unresolved audit gaps.
It checks the configured Dawn release and source revision against that
inventory, then verifies every listed notice's hash before copying it.
On Linux/macOS also configure `-DCOLOSSEUM_PORTABLE_DEPENDENCIES=ON` in a
fresh build directory. This selects bundled static dependencies instead of
libraries installed under a developer's Homebrew or local prefixes. Static
Dawn/SDL flags alone do not prevent other shared dependencies. The packager
checks all three executables with `otool` or `ldd` and rejects missing or
unbundled runtime libraries before creating the package.
Before packaging, run the Python distribution/setup tests and build
`colosseum-host-tests colosseum-pad-tests colosseum-idle-tests colosseum-path-tests`, then run
`ctest --test-dir build/standalone --output-on-failure --no-tests=error -R '^standalone-'`.
The gamepad test uses a virtual SDL controller and skips when a physical
controller is connected, so it does not interact with user hardware.
The packager copies only the executable, known Aurora runtime DLL names,
documentation, dependency notices and the local module setup tools/source.
This software is based in part on the work of the FreeType Team.
The source selection uses an explicit top-level module-template file list and
tracked runtime files with suffix filters. Generated subtrees are excluded even
when tracked, together with assembly includes, generated chunks and game data.
It refuses existing destinations, so
it cannot refresh a played installation or carry its private state forward.
Use a fresh output name for each candidate. Check `SHA256SUMS` after transfer.

The default `Build player artifacts` workflow routes pull requests to a Linux
standalone build and version tags to Windows, Linux and macOS. Manual runs of
either it or `Build standalone artifacts` preserve platform and source-ref
selection, without a game or module. The historical Dolphin runner pipeline is
available only through the explicit manual `legacy_runner` option for developer
comparisons; it never runs on pull requests or tags. Standalone downloads use
the `ColosseumStandalone-<platform>` artifact names. Linux/macOS artifacts
use static Aurora dependencies where supported; operating-system graphics,
audio and window-system libraries remain prerequisites. These are developer
artifacts, not signed/notarized installers. CI compilation is not gameplay or
display validation. Test a candidate on each target OS with locally supplied
inputs before publishing. Keep the matching source revisions and notices with
any distributed binary; follow the licensing requirements in those notices.

CI uploads a `.tar.gz` on Linux/macOS to preserve executable permissions,
and a `.zip` on Windows. Extract the inner archive before running setup or the
host; downloading a raw artifact directory would lose Unix executable modes.
