# Build your local standalone module

The standalone host needs your own original GC6E01 USA disc image and a module
generated from it. Modules and extracted game files are local outputs, never
release contents. Setup uses DolRecomp and the standalone module tool; it does
not launch or install a Dolphin runner.

Install Python 3, CMake, Ninja, and LLVM Clang on PATH. Windows host builds also
need the Visual Studio C++ build tools and a Developer Command Prompt. Initialize
the repository submodules before configuring the standalone host as described in
STANDALONE-DISTRIBUTION.md.

Windows setup tools require Windows 10 version 1903 or newer for Unicode paths.
Their embedded manifest selects UTF-8 for that process; no system locale change,
temporary ISO copy, or path alias is required. Keep `windows-utf8.manifest` with
the repository sources when rebuilding the tools. This uses the documented
[per-process Windows UTF-8 code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).

Build the host and setup tools in that build tree:

```text
cmake --build build/aurora-native-win --target colosseum-standalone colosseum-setup-tools
python scripts/setup-standalone.py "D:/Games/Pokemon Colosseum.iso" --tools build/aurora-native-win
```

The script verifies the ISO with `scripts/verify-disc.py`, extracts it into
`extracted/Pokemon-Colosseum-USA`, verifies the extracted disc ID and original
main DOL, and generates the C backend at optimization level 2 using Clang. It
applies both `GC6E01-native-widescreen.csv` and
`GC6E01-native-substitutions.csv`, including the HD text hook. Generated compiler
inputs and caches live in `build/standalone-modules`. The resulting module is
copied beside the standalone executable. Existing settings and memory cards are
preserved. Close the game before replacing its module.

The standalone module tool selects the host's memory-access profile and records
it in its build-cache identity. Linux x86-64 modules use the shared logical RAM
view only when they advertise the alias-safe helper capability; older modules
stay on the compatible checked path. Other hosts use inline checked access.
Rerun setup after upgrading the tools to receive this profile. The larger
checked-access module on x86-64 can take longer to compile and link.

Use `--host-dir <directory>` to install into a separate host/package directory,
`--output <directory>` for the generated build/cache directory, `--extracted
<directory>` for a separate extraction, and `--jobs N` to limit compilation
memory use. Paths with spaces are supported. On Unix use your configured Unix
build directory for `--tools` and `--host-dir`. An existing extraction is reused
only after verification; a stale or modified extraction fails rather than being
silently replaced. Choose a new `--extracted` path to rebuild from the disc.

For a binary distribution containing the setup tools and source bundle, run the
same script from its `scripts` directory with `--tools Tools --host-dir .`.
Python, CMake, Ninja, and Clang are still required locally. The source bundle at
`Tools/ModuleSource` is selected automatically. Setup never downloads tools or
game content. Keep your generated module and ISO private.

Launch with `colosseum-standalone <path-to-your-ISO>`, or use the first-launch
disc picker. Setup does not depend on the old Dolphin release directory.
