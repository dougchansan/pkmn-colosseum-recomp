#!/usr/bin/env python3
"""Assemble an asset-free standalone distribution from explicit inputs only."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import zipfile


RUNTIME_DLLS = (
    "SDL3.dll", "webgpu_dawn.dll", "dxcompiler.dll", "dxil.dll",
    "libpng16.dll", "zlib1.dll",
)

MODULE_SOURCE_TREES = {
    "GXRuntime/include": {".h"},
    "GXRuntime/src/core": {".c", ".h"},
}
MODULE_SOURCE_EXTRA_FILES = ("GXRuntime/include/gxruntime/utf8_file.h",)
MODULE_TEMPLATE_FILES = (
    "CMakeLists.txt", "gen_module_tables.py", "module_export.c", "module.exports",
    "native_call_guard.c", "native_call_guard.h", "sdk_leaf_guard.c", "sdk_leaf_guard.h",
)
RUNTIME_NOTICES = {
    "Abseil": ("abseil-cpp", (("absl_SOURCE_DIR", "."),), ("LICENSE",)),
    "Tracy": ("tracy", (("Tracy_SOURCE_DIR", "."),), ("LICENSE",)),
    "FreeType": ("freetype", (("freetype_SOURCE_DIR", "."),), ("LICENSE.TXT", "docs/FTL.TXT", "docs/GPLv2.TXT")),
    "PNG": ("png", (("libpng_SOURCE_DIR", "."),), ("LICENSE",)),
    "zlib": ("zlib", (("zlib_SOURCE_DIR", "."),), ("LICENSE.md",)),
    "fmt": ("fmt", (("FMT_SOURCE_DIR", "."),), ("LICENSE",)),
    "ImGui": ("imgui", (("imgui_SOURCE_DIR", "."),), ("LICENSE.txt",)),
    "xxHash": ("xxhash", (("xxHash_SOURCE_DIR", ".."),), ("LICENSE",)),
    "zstd": ("zstd", (("zstd_SOURCE_DIR", "../.."),), ("LICENSE",)),
}


def load_prebuilt_manifest(root: Path) -> dict:
    """Require an audited component inventory and exact runtime provenance."""
    source = root / "third_party/notices/dawn"
    manifest = source / "SOURCES.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError(f"missing prebuilt notice manifest: {manifest}")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    files = data.get("files") if isinstance(data, dict) else None
    if not isinstance(data, dict) or data.get("schema_version") != 2:
        raise ValueError("prebuilt notice manifest requires schema_version 2")
    audit = data.get("audit", {})
    if not isinstance(audit, dict) or audit.get("complete") is not True or audit.get("gaps") != [] or not audit.get("scope"):
        raise ValueError("prebuilt component audit is incomplete")
    if not isinstance(files, dict) or not files:
        raise ValueError("prebuilt notice manifest requires explicit files")
    dawn = data.get("dawn", {})
    if not isinstance(dawn, dict) or not re.fullmatch(r"[0-9a-f]{40}", dawn.get("commit", "")) or not dawn.get("release"):
        raise ValueError("prebuilt Dawn source/release pins are missing")
    components = data.get("components")
    if not isinstance(components, list) or not components:
        raise ValueError("prebuilt component inventory is missing")
    ids, referenced = set(), set()
    for component in components:
        if not isinstance(component, dict) or not all(component.get(key) for key in ("id", "revision", "source_url", "platforms", "role", "evidence", "licenses")):
            raise ValueError("prebuilt component provenance is incomplete")
        if component["id"] in ids or component["role"] not in ("linked", "compiled-header", "runtime"):
            raise ValueError("duplicate or invalid prebuilt component")
        ids.add(component["id"])
        for notice in component["licenses"]:
            if not isinstance(notice, dict) or not notice.get("url") or files.get(notice.get("file")) != notice.get("sha256"):
                raise ValueError("component license does not match pinned notice manifest")
            referenced.add(notice["file"])
    if referenced != set(files):
        raise ValueError("prebuilt notice files must match component license inventory")
    exclusions = data.get("excluded_components")
    if not isinstance(exclusions, list) or any(not isinstance(c, dict) or not all(c.get(k) for k in ("id", "reason", "evidence")) for c in exclusions):
        raise ValueError("prebuilt build-only exclusions lack evidence")
    dxil = data.get("windows_dxil", {})
    if not isinstance(dxil, dict) or dxil.get("source") != "official-dxc-release" or dxil.get("license_component") not in ids:
        raise ValueError("DXIL provenance requires an audited official DXC release")
    if not re.fullmatch(r"[0-9a-f]{40}", dxil.get("release_commit", "")) or not dxil.get("release"):
        raise ValueError("DXIL source/release pins are missing")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*\.zip", dxil.get("asset_name", "")) or dxil.get("member") != "bin/x64/dxil.dll":
        raise ValueError("DXIL archive/member is not allowlisted")
    expected_url = f"https://github.com/microsoft/DirectXShaderCompiler/releases/download/{dxil['release']}/{dxil['asset_name']}"
    if dxil.get("asset_url") != expected_url or any(not re.fullmatch(r"[0-9a-f]{64}", dxil.get(k, "")) for k in ("asset_sha256", "binary_sha256")):
        raise ValueError("DXIL official archive/binary pins are missing")
    for name, expected in files.items():
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.casefold() == "sources.json":
            raise ValueError(f"invalid prebuilt notice basename: {name}")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected):
            raise ValueError(f"invalid prebuilt notice hash: {name}")
        filename = source / name
        if filename.is_symlink() or not filename.is_file():
            raise ValueError(f"missing prebuilt notice: {filename}")
        if hashlib.sha256(filename.read_bytes()).hexdigest() != expected.lower():
            raise ValueError(f"prebuilt notice hash mismatch: {filename}")
    return data


def copy_prebuilt_notices(root: Path, destination: Path) -> None:
    """Copy only the audited, hash-verified prebuilt dependency notices."""
    data = load_prebuilt_manifest(root)
    source = root / "third_party/notices/dawn"
    files = data["files"]
    for name in files:
        copy_file(source / name, destination / name)
    copy_file(source / "SOURCES.json", destination / "SOURCES.json")


def prebuilt_runtime_archive(root: Path, data: dict) -> Path:
    dxil = data["windows_dxil"]
    return root / "build/prebuilt-runtime" / dxil["asset_sha256"] / dxil["asset_name"]


def read_cmake_cache(build: Path) -> dict:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    return cache


def verify_dawn_build(build: Path, data: dict) -> None:
    cache = read_cmake_cache(build)
    if (cache.get("AURORA_DAWN_PROVIDER") != "package" or
            cache.get("AURORA_DAWN_VERSION") != data["dawn"]["release"] or
            cache.get("AURORA_DAWN_REF") != data["dawn"]["commit"] or
            cache.get("AURORA_DAWN_PACKAGE_URL")):
        raise ValueError("Dawn build dependency does not match audited prebuilt release pins")
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in ("amd64", "x86_64"):
        asset_name = "dawn-windows-amd64.tar.gz"
    elif sys.platform == "darwin" and machine in ("arm64", "aarch64", "x86_64", "amd64"):
        asset_name = "dawn-darwin-" + ("arm64" if machine in ("arm64", "aarch64") else "x86_64") + ".tar.gz"
    elif sys.platform.startswith("linux") and machine in ("arm64", "aarch64", "x86_64", "amd64"):
        asset_name = "dawn-linux-" + ("aarch64" if machine in ("arm64", "aarch64") else "x86_64") + ".tar.gz"
    else:
        raise ValueError(f"no audited native Dawn asset for {sys.platform}/{machine}")
    assets = [asset for asset in data["dawn"].get("assets", []) if asset.get("asset_name") == asset_name]
    if len(assets) != 1:
        raise ValueError(f"missing or duplicate audited Dawn asset: {asset_name}")

    def cache_path(value):
        result = Path(value)
        return result if result.is_absolute() else build / result

    if cache.get("Dawn_DIR") and not cache["Dawn_DIR"].endswith("-NOTFOUND"):
        # DawnConfig's install prefix is the root of the imported library paths.
        package_root = cache_path(cache["Dawn_DIR"]).parent.parent.parent
    elif cache.get("FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT"):
        package_root = cache_path(cache["FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT"])
    else:
        base = cache_path(cache["FETCHCONTENT_BASE_DIR"]) if cache.get("FETCHCONTENT_BASE_DIR") else build / "_deps"
        package_root = base / "dawn_prebuilt-src"
    expected_members = ({"bin/dxcompiler.dll", "bin/webgpu_dawn.dll"} if sys.platform == "win32"
                        else {"lib/libwebgpu_dawn.a"})
    inputs = assets[0].get("link_inputs_and_runtime", [])
    members = [str(PurePosixPath(item.get("member", ""))) for item in inputs]
    if len(members) != len(expected_members) or set(members) != expected_members:
        raise ValueError("audited Dawn native link/runtime member inventory is incomplete")
    for item, member in zip(inputs, members):
        expected = item.get("sha256", "")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError(f"missing Dawn input hash: {member}")
        filename = package_root / member
        candidates = [filename]
        if sys.platform == "win32":
            candidates.append(build / Path(member).name)
        for candidate in candidates:
            if candidate.is_symlink() or not candidate.is_file():
                raise ValueError(f"missing audited Dawn input: {candidate}")
            if candidate.stat().st_size != item.get("size") or hashlib.sha256(candidate.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Dawn link/runtime hash mismatch: {candidate}")


def verified_dxil(root: Path, data: dict) -> bytes:
    """Read exactly one verified DLL from an already cached official archive."""
    archive = prebuilt_runtime_archive(root, data)
    if archive.is_symlink() or not archive.is_file():
        raise ValueError("missing pinned DXIL archive; run scripts/fetch-standalone-runtime.py first")
    dxil = data["windows_dxil"]
    if hashlib.sha256(archive.read_bytes()).hexdigest() != dxil["asset_sha256"]:
        raise ValueError("DXIL archive hash mismatch")
    with zipfile.ZipFile(archive) as stream:
        members = [info for info in stream.infolist() if info.filename == dxil["member"]]
        if len(members) != 1 or members[0].file_size > 32 * 1024 * 1024 or (members[0].external_attr >> 16) & 0o170000 == 0o120000:
            raise ValueError("DXIL archive must contain exactly one regular allowlisted member")
        binary = stream.read(members[0])
    if hashlib.sha256(binary).hexdigest() != dxil["binary_sha256"]:
        raise ValueError("DXIL binary hash mismatch")
    return binary


def copy_runtime_notices(build: Path, destination: Path) -> None:
    """Copy allowlisted notices from the dependency sources used by CMake."""
    cache = read_cmake_cache(build)

    def path(value: str) -> Path:
        candidate = Path(value)
        return candidate if candidate.is_absolute() else build / candidate

    base = path(cache["FETCHCONTENT_BASE_DIR"]) if cache.get("FETCHCONTENT_BASE_DIR") else build / "_deps"

    def source(name: str, aliases: tuple) -> Path:
        override = cache.get("FETCHCONTENT_SOURCE_DIR_" + name.upper())
        if override:
            return path(override)
        for alias, relative in aliases:
            if cache.get(alias):
                return path(cache[alias]) / relative
        return base / (name + "-src")

    notices = []
    system_zlib = None
    for label, (name, aliases, files) in RUNTIME_NOTICES.items():
        root = source(name, aliases)
        if name == "zlib" and not root.exists() and not cache.get("FETCHCONTENT_SOURCE_DIR_ZLIB") and not cache.get("zlib_SOURCE_DIR"):
            # Linux and macOS may link the OS's shared zlib rather than fetch
            # source. Require an explicit CMake library result; a missing
            # source directory by itself is never evidence of system linkage.
            for key in ("ZLIB_LIBRARY_RELEASE", "ZLIB_LIBRARY"):
                library = cache.get(key, "")
                linux_system = sys.platform.startswith("linux") and re.fullmatch(
                    r"/(?:usr/)?lib(?:64)?/(?:[^/]+/)*libz\.so(?:\.[0-9]+)*", library)
                mac_system = sys.platform == "darwin" and re.fullmatch(
                    r"(?:/usr/lib/libz(?:\.1)?\.(?:dylib|tbd)|/.+\.sdk/usr/lib/libz(?:\.1)?\.tbd)", library)
                if linux_system or mac_system:
                    system_zlib = library
                    break
            if system_zlib:
                continue
        for filename in files:
            notices.append((root / filename, destination / label / filename))
    # SDL's prebuilt package and vendored source use different layouts.
    prebuilt = source("sdl3_prebuilt", ())
    vendor = source("sdl3", (("SDL3_SOURCE_DIR", "."),))
    if cache.get("AURORA_SDL3_PROVIDER") == "package" or (
            not cache.get("SDL3_SOURCE_DIR") and (prebuilt / "licenses/SDL3/LICENSE.txt").is_file()):
        notices.append((prebuilt / "licenses/SDL3/LICENSE.txt", destination / "SDL3/LICENSE.txt"))
    else:
        notices.append((vendor / "LICENSE.txt", destination / "SDL3/LICENSE.txt"))
    # Validate the whole list before writing; no directory traversal or broad
    # source-tree copying can include generated or game-specific files.
    for filename, _ in notices:
        if filename.is_symlink() or not filename.is_file():
            raise ValueError(f"missing required runtime notice: {filename}")
    for filename, target in notices:
        copy_file(filename, target)
    if system_zlib:
        (destination / "SystemLibraries.txt").write_text(
            f"zlib: {system_zlib}\nProvided by the operating system; this library is not included in the package.\n",
            encoding="utf-8")


def verify_unix_dependencies(binary: Path) -> None:
    """Reject dependencies absent from the asset-free, library-free package."""
    if sys.platform == "darwin":
        output = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        dependencies = output.splitlines()[1:]
        if not dependencies:
            raise ValueError(f"cannot inspect runtime dependencies: {binary}")
        for line in dependencies:
            library = line.strip().split(" (", 1)[0]
            if not library.startswith(("/usr/lib/", "/System/Library/")):
                raise ValueError(f"nonportable runtime dependency in {binary}: {library}")
    elif sys.platform.startswith("linux"):
        try:
            output = subprocess.check_output(["ldd", str(binary)], text=True, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            if "statically linked" in (exc.output or ""):
                return
            raise
        if "not found" in output:
            raise ValueError(f"missing runtime dependency in {binary}: {output.strip()}")
        if "statically linked" in output:
            return
        dependencies = [line.strip() for line in output.splitlines() if line.strip()]
        if not dependencies:
            raise ValueError(f"cannot inspect runtime dependencies: {binary}")
        bundled_family = re.compile(r"^lib(?:sdl3|webgpu_dawn|absl|fmt|png|freetype|zstd|harfbuzz|brotli)", re.I)
        for line in dependencies:
            name = line.split()[0]
            if bundled_family.match(Path(name).name):
                raise ValueError(f"unbundled runtime dependency in {binary}: {name}")
            resolved = line.split("=>", 1)[-1].strip().split()[0]
            if resolved.startswith("/") and not resolved.startswith(("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")):
                raise ValueError(f"nonportable runtime dependency in {binary}: {resolved}")
    else:
        raise ValueError(f"unsupported Unix packaging platform: {sys.platform}")


def copy_module_source(dolphin: Path, destination: Path) -> None:
    # The module compiler needs portable runtime source, never game chunks.
    # Ask git for tracked files so local/generated source cannot enter releases.
    for name in MODULE_TEMPLATE_FILES:
        relative = Path("module-template") / name
        copy_file(dolphin / relative, destination / relative)
    for subtree, suffixes in MODULE_SOURCE_TREES.items():
        files = subprocess.check_output(
            ["git", "ls-files", "--", subtree], cwd=dolphin, text=True).splitlines()
        if not files:
            raise ValueError(f"missing tracked module source: {subtree}")
        for name in files:
            relative = Path(name)
            if relative.suffix in suffixes and "generated" not in relative.parts:
                copy_file(dolphin / relative, destination / relative)
    # Explicit new portable runtime headers are safe even before first commit.
    # Never broaden the tracked-tree filter to arbitrary local source.
    for name in MODULE_SOURCE_EXTRA_FILES:
        copy_file(dolphin / name, destination / name)
    abi = "Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h"
    copy_file(dolphin / abi, destination / abi)


def copy_file(source: Path, destination: Path) -> None:
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"expected a regular file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def package(root: Path, build: Path, destination: Path, windows: bool) -> None:
    # Never refresh an existing release or copy a directory with player state.
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"destination must not exist: {destination}")
    executable = "colosseum-standalone" + (".exe" if windows else "")
    binaries = [executable] + [name + (".exe" if windows else "")
                               for name in ("colosseum-module-tool", "dolrecomp")]
    for name in binaries:
        if not (build / name).is_file():
            raise ValueError(f"missing standalone executable: {build / name}")
        if not windows:
            verify_unix_dependencies(build / name)
    prebuilt = load_prebuilt_manifest(root)
    verify_dawn_build(build, prebuilt)
    replacement_dxil = verified_dxil(root, prebuilt) if windows else None
    destination.mkdir(parents=True)
    copy_runtime_notices(build, destination / "licenses/AuroraDependencies")
    copy_prebuilt_notices(root, destination / "licenses/DawnPrebuilt")
    copy_file(build / executable, destination / executable)
    if windows:
        for name in RUNTIME_DLLS:
            if name == "dxil.dll":
                (destination / name).write_bytes(replacement_dxil)
                continue
            if (build / name).exists():
                copy_file(build / name, destination / name)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "SOURCE.md", "VERSION"):
        copy_file(root / name, destination / name)
    copy_file(root / "tools/aurora-mirror/STANDALONE.md", destination / "README.md")
    copy_file(root / "docs/STANDALONE-DISTRIBUTION.md", destination / "BUILDING.md")
    copy_file(root / "docs/STANDALONE-SETUP.md", destination / "SETUP.md")
    for name in ("setup-standalone.py", "verify-disc.py"):
        copy_file(root / "scripts" / name, destination / "scripts" / name)
    for name in ("GC6E01-native-widescreen.csv", "GC6E01-native-substitutions.csv"):
        copy_file(root / "patches" / name, destination / "patches" / name)
    for name in ("colosseum-module-tool", "dolrecomp"):
        binary = name + (".exe" if windows else "")
        copy_file(build / binary, destination / "Tools" / binary)
    dolphin = root / "lib/ModernGekko/vendor/dolphin"
    copy_module_source(dolphin, destination / "Tools/ModuleSource/vendor/dolphin")
    copy_file(dolphin / "COPYING", destination / "licenses/Dolphin/COPYING")
    copy_file(root / "lib/ModernGekko/LICENSE", destination / "licenses/ModernGekko/LICENSE")
    copy_file(root / "lib/ModernGekko/vendor/dolphin/DolRecomp/LICENSE", destination / "licenses/DolRecomp/LICENSE")
    # Reuse the repository's notice collector instead of copying dependency trees.
    spec = importlib.util.spec_from_file_location("notices", root / "scripts/collect-third-party-notices.py")
    notices = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(notices)
    notices.collect(dolphin / "Externals", destination / "licenses/Externals")
    copy_file(dolphin / "GXRuntime/graphics/aurora/LICENSE", destination / "licenses/Aurora/LICENSE")
    revisions = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True)
    revisions += subprocess.check_output(["git", "submodule", "status", "--recursive"], cwd=root, text=True)
    (destination / "SOURCE_REVISIONS.txt").write_text(revisions, encoding="utf-8")
    hashes = []
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            hashes.append(f"{digest}  {path.relative_to(destination).as_posix()}\n")
    (destination / "SHA256SUMS").write_text("".join(hashes), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new directory; existing paths are refused")
    parser.add_argument("--windows", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        package(root, args.build.resolve(), args.output.absolute(), args.windows)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"standalone packaging failed: {exc}\n")


if __name__ == "__main__":
    main()
