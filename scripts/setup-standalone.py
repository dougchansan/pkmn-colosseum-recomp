#!/usr/bin/env python3
"""Generate a standalone game module locally from your verified GC6E01 disc."""

import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import sys


def find_tool(directory: Path, name: str) -> Path:
    filename = name + (".exe" if os.name == "nt" else "")
    direct = directory / filename
    if direct.is_file():
        return direct
    candidates = sorted(directory.rglob(filename))
    if not candidates:
        raise ValueError(f"missing {filename} under {directory}; build colosseum-setup-tools first")
    if len(candidates) != 1:
        raise ValueError(f"multiple {filename} files under {directory}; use a narrower --tools directory")
    return candidates[0]


def build_command(root: Path, tool: Path, extracted: Path, output: Path) -> list[str]:
    command = [str(tool), "build", str(extracted), "--backend", "c",
               "--toolchain", "clang", "--opt-level", "2", "--output", str(output)]
    for manifest, flag, left, right, separator in (
        ("native-widescreen", "--dol-patch", "address", "replacement", "="),
        ("native-substitutions", "--native-subst", "guest", "symbol", ":"),
    ):
        with (root / f"patches/GC6E01-{manifest}.csv").open(newline="", encoding="utf-8-sig") as stream:
            for row in csv.DictReader(stream):
                command.extend([flag, f"{row[left]}{separator}{row[right]}"])
    return command


def setup(root: Path, iso: Path, tools: Path, host: Path, extracted: Path,
          output: Path, jobs: int) -> Path:
    extractor = find_tool(tools, "dolrecomp")
    builder = find_tool(tools, "colosseum-module-tool")
    if not host.is_dir():
        raise ValueError(f"host/output directory does not exist: {host}")
    # Verify before extraction, then check any existing extraction before reuse.
    verify = [sys.executable, str(root / "scripts/verify-disc.py"), str(iso)]
    subprocess.run(verify, check=True, cwd=root)
    if not extracted.exists():
        subprocess.run([str(extractor), "extract", str(iso), str(extracted)], check=True, cwd=root)
    subprocess.run(verify + [str(extracted)], check=True, cwd=root)
    env = os.environ.copy()
    env["MODERNGEKKO_BUILD_JOBS"] = str(jobs)
    bundled_source = root / "Tools/ModuleSource"
    if bundled_source.is_dir():
        env["MODERNGEKKO_SOURCE_DIR"] = str(bundled_source)
    subprocess.run(build_command(root, builder, extracted, output), check=True, cwd=root, env=env)
    marker = output / "GC6E01/active-module-c.txt"
    value = marker.read_text(encoding="utf-8").strip()
    if not value:
        raise ValueError(f"empty module marker: {marker}")
    module = Path(value)
    if not module.is_absolute():
        module = root / module
    if not module.is_file():
        raise ValueError(f"module builder did not produce {module}")
    destination = host / module.name
    if module.resolve() != destination.resolve():
        # Copy only the module: never touch settings, cards, or the source ISO.
        temporary = destination.with_name(destination.name + ".setup-tmp")
        try:
            shutil.copy2(module, temporary)
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    return destination


def main() -> None:
    if sys.platform == "win32":
        # Redirected Windows streams otherwise use the legacy ANSI encoding.
        for stream in (sys.stdout, sys.stderr):
            if hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8")
    root = Path(__file__).resolve().parent.parent
    packaged = (root / "Tools/ModuleSource").is_dir()
    default_tools = root / ("Tools" if packaged else "build/aurora-native-win")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path, help="your original GC6E01 USA ISO or GCM")
    parser.add_argument("--tools", type=Path, default=default_tools)
    parser.add_argument("--host-dir", type=Path, help="install beside the host; defaults to --tools")
    parser.add_argument("--extracted", type=Path, default=root / "extracted/Pokemon-Colosseum-USA")
    parser.add_argument("--output", type=Path, default=root / "build/standalone-modules", help="local generated module build directory")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        module = setup(root, args.iso.resolve(), args.tools.resolve(),
                       (args.host_dir or (root if packaged else args.tools)).resolve(), args.extracted.resolve(),
                       args.output.resolve(), args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"standalone setup failed: {exc}\n")
    print(f"Standalone module ready: {module}")
    print("Launch colosseum-standalone with your ISO, or select it on first launch.")


if __name__ == "__main__":
    main()
