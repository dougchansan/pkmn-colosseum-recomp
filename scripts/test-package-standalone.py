#!/usr/bin/env python3
"""Distribution boundary tests using synthetic binaries, never game data."""

import importlib.util
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import zipfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("package", Path(__file__).with_name("package-standalone.py"))
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def runtime_notice_fixture(build, base=None):
    build.mkdir(parents=True, exist_ok=True)
    base = base or build / "_deps"
    for _, (name, _, files) in package.RUNTIME_NOTICES.items():
        for filename in files:
            target = base / (name + "-src") / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"actual {name} notice {filename}")
    sdl = base / "sdl3_prebuilt-src/licenses/SDL3/LICENSE.txt"
    sdl.parent.mkdir(parents=True, exist_ok=True)
    sdl.write_text("actual SDL3 notice")
    for member in ("lib/libwebgpu_dawn.a", "bin/dxcompiler.dll", "bin/webgpu_dawn.dll"):
        target = base / "dawn_prebuilt-src" / member
        target.parent.mkdir(parents=True, exist_ok=True)
        content = ("synthetic Dawn " + member).encode()
        target.write_bytes(content)
        if member.startswith("bin/"):
            (build / Path(member).name).write_bytes(content)
    build.mkdir(parents=True, exist_ok=True)
    (build / "CMakeCache.txt").write_text(f"FETCHCONTENT_BASE_DIR:PATH={base.as_posix()}\nAURORA_SDL3_PROVIDER:STRING=package\n"
                                         "AURORA_DAWN_PROVIDER:STRING=package\nAURORA_DAWN_VERSION:STRING=v1\n"
                                         f"AURORA_DAWN_REF:STRING={'a' * 40}\n")
    return base


class DistributionTest(unittest.TestCase):
    def prebuilt_fixture(self, root):
        source = root / "third_party/notices/dawn"
        source.mkdir(parents=True)
        files = {}
        for i in range(5):
            name = f"notice-{i}.txt"
            content = f"pinned notice {i}".encode()
            (source / name).write_bytes(content)
            files[name] = hashlib.sha256(content).hexdigest()
        archive = root / "test-runtime.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("bin/x64/dxil.dll", b"verified synthetic validator")
            stream.writestr("../private.iso", b"must never extract")
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        cached = root / "build/prebuilt-runtime" / archive_hash / "runtime.zip"
        cached.parent.mkdir(parents=True)
        archive.replace(cached)
        data = {
            "schema_version": 2,
            "files": files,
            "audit": {"complete": True, "gaps": [], "scope": "synthetic audited runtime"},
            "dawn": {"commit": "a" * 40, "release": "v1", "assets": [
                {"asset_name": asset, "link_inputs_and_runtime": [
                    {"member": "./" + member, "sha256": hashlib.sha256(("synthetic Dawn " + member).encode()).hexdigest(),
                     "size": len(("synthetic Dawn " + member).encode())} for member in
                    (("bin/dxcompiler.dll", "bin/webgpu_dawn.dll") if "windows" in asset else ("lib/libwebgpu_dawn.a",))]}
                for asset in ("dawn-windows-amd64.tar.gz", "dawn-linux-x86_64.tar.gz", "dawn-linux-aarch64.tar.gz",
                              "dawn-darwin-arm64.tar.gz", "dawn-darwin-x86_64.tar.gz")]},
            "components": [{"id": "dxc-validator", "revision": "b" * 40, "source_url": "https://example.com/pinned",
                            "platforms": ["windows"], "role": "runtime", "evidence": ["pinned build input"],
                            "licenses": [{"file": name, "url": "https://example.com/pinned/" + name, "sha256": digest}
                                         for name, digest in files.items()]}],
            "excluded_components": [{"id": "tests", "reason": "build-only", "evidence": ["disabled target"]}],
            "windows_dxil": {"source": "official-dxc-release", "license_component": "dxc-validator", "release": "v1",
                             "release_commit": "b" * 40, "asset_name": "runtime.zip", "asset_sha256": archive_hash,
                             "asset_url": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1/runtime.zip",
                             "member": "bin/x64/dxil.dll", "binary_sha256": hashlib.sha256(b"verified synthetic validator").hexdigest()},
        }
        (source / "SOURCES.json").write_text(json.dumps(data))
        (source / "private.iso").write_bytes(b"not a notice")
        return source, files

    def test_prebuilt_notices_copy_only_manifest_and_verified_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prebuilt_fixture(root)
            output = root / "output"
            package.copy_prebuilt_notices(root, output)
            self.assertEqual(len(list(output.iterdir())), 6)
            self.assertFalse((output / "private.iso").exists())
            self.assertIn("synthetic audited runtime", (output / "SOURCES.json").read_text())

    def test_invalid_prebuilt_notices_fail_before_copy(self):
        for invalid in ("hash", "path", "inventory", "incomplete", "pin", "member", "url"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source, files = self.prebuilt_fixture(root)
                data = json.loads((source / "SOURCES.json").read_text())
                if invalid == "hash":
                    (source / "notice-4.txt").write_bytes(b"altered")
                elif invalid == "path":
                    data["files"]["../private.iso"] = data["files"].pop("notice-4.txt")
                    data["components"][0]["licenses"][-1]["file"] = "../private.iso"
                elif invalid == "inventory":
                    data["components"][0]["licenses"].pop()
                elif invalid == "incomplete":
                    data["audit"]["gaps"] = ["missing transitive dependency"]
                elif invalid == "pin":
                    data["windows_dxil"]["release_commit"] = "main"
                elif invalid == "member":
                    data["windows_dxil"]["member"] = "../dxil.dll"
                else:
                    data["windows_dxil"]["asset_url"] = "https://example.com/unpinned.zip"
                (source / "SOURCES.json").write_text(json.dumps(data))
                output = root / "output"
                with self.assertRaises(ValueError):
                    package.copy_prebuilt_notices(root, output)
                self.assertFalse(output.exists())

    def test_dxil_staging_is_offline_exact_member_and_hash_verified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prebuilt_fixture(root)
            data = package.load_prebuilt_manifest(root)
            self.assertEqual(package.verified_dxil(root, data), b"verified synthetic validator")
            self.assertFalse((root / "private.iso").exists())
            data["windows_dxil"]["binary_sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "binary hash mismatch"):
                package.verified_dxil(root, data)
            archive = package.prebuilt_runtime_archive(root, data)
            archive.write_bytes(b"corrupt archive")
            with self.assertRaisesRegex(ValueError, "archive hash mismatch"):
                package.verified_dxil(root, data)
            archive.unlink()
            with self.assertRaisesRegex(ValueError, "missing pinned DXIL archive"):
                package.verified_dxil(root, data)

    def test_dawn_build_must_match_audited_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prebuilt_fixture(root)
            build = root / "build"
            runtime_notice_fixture(build)
            data = package.load_prebuilt_manifest(root)
            package.verify_dawn_build(build, data)
            for key, value in (("AURORA_DAWN_VERSION", "unreviewed"), ("AURORA_DAWN_REF", "b" * 40),
                               ("AURORA_DAWN_PROVIDER", "system"), ("AURORA_DAWN_PACKAGE_URL", "https://example.com/custom")):
                with self.subTest(key=key), patch.object(package, "read_cmake_cache", return_value={
                        "AURORA_DAWN_VERSION": "v1", "AURORA_DAWN_REF": "a" * 40,
                        "AURORA_DAWN_PROVIDER": "package", key: value}):
                    with self.assertRaisesRegex(ValueError, "does not match audited"):
                        package.verify_dawn_build(build, data)

    def test_native_dawn_library_and_staged_runtime_must_match_hashes(self):
        for target, machine in (("win32", "AMD64"), ("linux", "x86_64"), ("darwin", "arm64")):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.prebuilt_fixture(root)
                build = root / "build"
                base = runtime_notice_fixture(build)
                data = package.load_prebuilt_manifest(root)
                with patch.object(package.sys, "platform", target), patch.object(package.platform, "machine", return_value=machine):
                    package.verify_dawn_build(build, data)
                    member = "bin/webgpu_dawn.dll" if target == "win32" else "lib/libwebgpu_dawn.a"
                    if target == "win32":
                        staged = build / "webgpu_dawn.dll"
                        staged.write_bytes(b"wrong staged DLL")
                        with self.assertRaisesRegex(ValueError, "hash mismatch"):
                            package.verify_dawn_build(build, data)
                        shutil.copyfile(base / "dawn_prebuilt-src" / member, staged)
                    # A correct label and source pin cannot hide altered bytes.
                    (base / "dawn_prebuilt-src" / member).write_bytes(b"different library")
                    with self.assertRaisesRegex(ValueError, "hash mismatch"):
                        package.verify_dawn_build(build, data)

    def test_dawn_binary_mismatch_precedes_package_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prebuilt_fixture(root)
            build = root / "build"
            runtime_notice_fixture(build)
            for name in ("colosseum-standalone.exe", "colosseum-module-tool.exe", "dolrecomp.exe"):
                (build / name).write_bytes(b"synthetic executable")
            (build / "dxcompiler.dll").write_bytes(b"wrong staged compiler")
            output = root / "output"
            with patch.object(package.sys, "platform", "win32"), patch.object(package.platform, "machine", return_value="AMD64"):
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    package.package(root, build, output, True)
            self.assertFalse(output.exists())

    def test_system_zlib_requires_positive_platform_library_cache(self):
        for platform, library in (("linux", "/usr/lib/x86_64-linux-gnu/libz.so"),
                                  ("linux", "/lib64/libz.so.1"),
                                  ("darwin", "/Applications/Xcode.app/SDKs/MacOSX.sdk/usr/lib/libz.tbd"),
                                  ("darwin", "/usr/lib/libz.1.dylib")):
            with self.subTest(platform=platform, library=library), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = root / "build"
                base = runtime_notice_fixture(build)
                shutil.rmtree(base / "zlib-src")
                with (build / "CMakeCache.txt").open("a") as cache:
                    cache.write(f"ZLIB_LIBRARY_RELEASE:FILEPATH={library}\n")
                output = root / "licenses"
                with patch.object(package.sys, "platform", platform):
                    package.copy_runtime_notices(build, output)
                self.assertFalse((output / "zlib").exists())
                self.assertIn(library, (output / "SystemLibraries.txt").read_text())
                self.assertTrue((output / "Tracy/LICENSE").is_file())

    def test_unknown_or_bundled_zlib_still_requires_notice(self):
        for library in ("", "ZLIB_LIBRARY_RELEASE-NOTFOUND", "/opt/sdk/lib/libz.so", "/usr/lib/libz.a"):
            with self.subTest(library=library), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = root / "build"
                base = runtime_notice_fixture(build)
                shutil.rmtree(base / "zlib-src")
                with (build / "CMakeCache.txt").open("a") as cache:
                    cache.write(f"ZLIB_LIBRARY_RELEASE:FILEPATH={library}\n")
                with patch.object(package.sys, "platform", "linux"):
                    with self.assertRaisesRegex(ValueError, "missing required runtime notice"):
                        package.copy_runtime_notices(build, root / "licenses")

    def test_system_zlib_does_not_hide_missing_bundled_notice(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            base = runtime_notice_fixture(build)
            (base / "zlib-src/LICENSE.md").unlink()
            with (build / "CMakeCache.txt").open("a") as cache:
                cache.write("ZLIB_LIBRARY:FILEPATH=/usr/lib/libz.so\n")
            with patch.object(package.sys, "platform", "linux"):
                with self.assertRaisesRegex(ValueError, "missing required runtime notice"):
                    package.copy_runtime_notices(build, root / "licenses")

    def test_runtime_notices_use_actual_cache_sources_and_explicit_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            base = runtime_notice_fixture(build, root / "shared dependencies")
            custom = root / "override abseil"
            custom.mkdir()
            (custom / "LICENSE").write_text("override notice")
            (custom / "game.iso").write_bytes(b"private")
            with (build / "CMakeCache.txt").open("a") as cache:
                cache.write(f"FETCHCONTENT_SOURCE_DIR_ABSEIL-CPP:PATH={custom.as_posix()}\n")
            output = root / "licenses"
            package.copy_runtime_notices(build, output)
            self.assertEqual((output / "Abseil/LICENSE").read_text(), "override notice")
            self.assertTrue((output / "FreeType/docs/FTL.TXT").is_file())
            self.assertEqual((output / "PNG/LICENSE").read_text(), "actual png notice LICENSE")
            self.assertEqual(len([p for p in output.rglob("*") if p.is_file()]), 12)
            self.assertFalse(list(output.rglob("*.iso")))

    def test_runtime_notices_support_default_deps_aliases_and_vendor_sdl(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            base = runtime_notice_fixture(build)
            alias = root / "actual fmt"
            alias.mkdir()
            (alias / "LICENSE").write_text("alias notice")
            sdl = base / "sdl3-src"
            sdl.mkdir()
            (sdl / "LICENSE.txt").write_text("vendor SDL3 notice")
            (build / "CMakeCache.txt").write_text(f"FMT_SOURCE_DIR:STATIC={alias.as_posix()}\nSDL3_SOURCE_DIR:STATIC={sdl.as_posix()}\nAURORA_SDL3_PROVIDER:STRING=vendor\n")
            output = root / "licenses"
            package.copy_runtime_notices(build, output)
            self.assertEqual((output / "fmt/LICENSE").read_text(), "alias notice")
            self.assertEqual((output / "SDL3/LICENSE.txt").read_text(), "vendor SDL3 notice")

    def test_runtime_notices_fail_closed_before_copying(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            base = runtime_notice_fixture(build)
            (base / "tracy-src/LICENSE").unlink()
            output = root / "licenses"
            with self.assertRaisesRegex(ValueError, "missing required runtime notice"):
                package.copy_runtime_notices(build, output)
            self.assertFalse(output.exists())

    def test_macos_allows_only_system_libraries(self):
        output = "host:\n\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0)\n\t/System/Library/Frameworks/Metal.framework/Versions/A/Metal (compatibility version 1.0.0)\n"
        with patch.object(package.sys, "platform", "darwin"), \
             patch.object(package.subprocess, "check_output", return_value=output) as inspect:
            package.verify_unix_dependencies(Path("host"))
            self.assertEqual(inspect.call_args.args[0], ["otool", "-L", "host"])
        for library in ("/opt/homebrew/opt/fmt/lib/libfmt.11.dylib", "@rpath/libSDL3.dylib",
                        "/usr/local/lib/libpng16.dylib"):
            with self.subTest(library=library), patch.object(package.sys, "platform", "darwin"), \
                 patch.object(package.subprocess, "check_output", return_value=f"host:\n\t{library} (compatibility version 1.0.0)\n"):
                with self.assertRaisesRegex(ValueError, "nonportable"):
                    package.verify_unix_dependencies(Path("host"))

    def test_linux_allows_platform_libraries(self):
        output = "linux-vdso.so.1 (0x1234)\nlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x1234)\nlibstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x1234)\nlibX11.so.6 => /usr/lib/libX11.so.6 (0x1234)\nlibwayland-client.so.0 => /usr/lib/libwayland-client.so.0 (0x1234)\nlibz.so.1 => /usr/lib/libz.so.1 (0x1234)\n/lib64/ld-linux-x86-64.so.2 (0x1234)\n"
        with patch.object(package.sys, "platform", "linux"), \
             patch.object(package.subprocess, "check_output", return_value=output):
            package.verify_unix_dependencies(Path("host"))

    def test_linux_rejects_missing_local_and_unbundled_libraries(self):
        outputs = ["libSDL3.so.0 => not found\n",
                   "libcustom.so => /home/builder/libcustom.so (0x1234)\n",
                   "libcustom.so => /opt/sdk/libcustom.so (0x1234)\n",
                   "libcustom.so => /usr/local/lib/libcustom.so (0x1234)\n"]
        outputs += [f"lib{name}.so => /usr/lib/lib{name}.so (0x1234)\n"
                    for name in ("SDL3", "webgpu_dawn", "absl_strings", "fmt", "png16", "freetype",
                                 "zstd", "harfbuzz", "brotlidec")]
        for output in outputs:
            with self.subTest(output=output), patch.object(package.sys, "platform", "linux"), \
                 patch.object(package.subprocess, "check_output", return_value=output):
                with self.assertRaises(ValueError):
                    package.verify_unix_dependencies(Path("host"))

    def test_unix_dependency_failure_precedes_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            binaries = [build / name for name in ("colosseum-standalone", "colosseum-module-tool", "dolrecomp")]
            for binary in binaries:
                binary.write_bytes(b"synthetic")
            output = root / "output"
            with patch.object(package, "verify_unix_dependencies", side_effect=[None, None, ValueError("nonportable")]) as inspect:
                with self.assertRaisesRegex(ValueError, "nonportable"):
                    package.package(root, build, output, False)
            self.assertEqual([call.args[0] for call in inspect.call_args_list], binaries)
            self.assertFalse(output.exists())

    def test_existing_destination_is_never_modified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, "must not exist"):
                package.package(root, root, root, True)

    def test_only_explicit_runtime_inputs_are_copied(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            for name in ("colosseum-standalone.exe", "colosseum-module-tool.exe", "dolrecomp.exe", "SDL3.dll", "game.iso",
                         "gGC6E01_recomp.dll", "MemoryCardA.USA.raw", "settings.ini",
                         "aurora-native.dll"):
                (build / name).write_bytes(b"synthetic")
            for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "SOURCE.md", "VERSION",
                         "tools/aurora-mirror/STANDALONE.md", "docs/STANDALONE-DISTRIBUTION.md",
                         "docs/STANDALONE-SETUP.md", "scripts/setup-standalone.py", "scripts/verify-disc.py",
                         "patches/GC6E01-native-widescreen.csv", "patches/GC6E01-native-substitutions.csv",
                         "lib/ModernGekko/vendor/dolphin/COPYING", "lib/ModernGekko/LICENSE",
                         "lib/ModernGekko/vendor/dolphin/DolRecomp/LICENSE",
                         "lib/ModernGekko/vendor/dolphin/GXRuntime/graphics/aurora/LICENSE"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("synthetic notice")
            collector = root / "scripts/collect-third-party-notices.py"
            collector.parent.mkdir(exist_ok=True)
            collector.write_text("def collect(source, destination): return 20\n")
            runtime_notice_fixture(build)
            self.prebuilt_fixture(root)
            output = root / "output"
            with patch.object(package.subprocess, "check_output", return_value="synthetic revision\n"), \
                 patch.object(package, "copy_module_source"), \
                 patch.object(package, "verify_unix_dependencies") as inspect:
                package.package(root, build, output, True)
            inspect.assert_not_called()
            self.assertTrue((output / "SDL3.dll").is_file())
            self.assertTrue((output / "colosseum-standalone.exe").is_file())
            self.assertTrue((output / "Tools/colosseum-module-tool.exe").is_file())
            self.assertTrue((output / "Tools/dolrecomp.exe").is_file())
            self.assertEqual({p.name for p in output.glob("*.dll")}, {"SDL3.dll", "dxil.dll", "dxcompiler.dll", "webgpu_dawn.dll"})
            self.assertEqual((output / "dxil.dll").read_bytes(), b"verified synthetic validator")
            for name in ("game.iso", "MemoryCardA.USA.raw", "settings.ini"):
                self.assertFalse((output / name).exists())
            self.assertIn("colosseum-standalone.exe", (output / "SHA256SUMS").read_text())

    def test_module_sources_exclude_game_files_and_assembly_includes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "output"
            names = ["module-template/" + name for name in package.MODULE_TEMPLATE_FILES] + list(package.MODULE_SOURCE_EXTRA_FILES) + ["module-template/private.inc",
                     "module-template/generated/generated.h", "GXRuntime/include/generated/game.h",
                     "module-template/main.dol", "GXRuntime/include/gxruntime/cpu.h",
                     "GXRuntime/src/core/cpu.c",
                     "Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h"]
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("synthetic")
            with patch.object(package.subprocess, "check_output", side_effect=lambda args, **kwargs:
                              "\n".join(n for n in names if n.startswith(args[-1] + "/") and n not in package.MODULE_SOURCE_EXTRA_FILES)):
                package.copy_module_source(root, output)
            self.assertTrue((output / "GXRuntime/src/core/cpu.c").is_file())
            self.assertTrue((output / package.MODULE_SOURCE_EXTRA_FILES[0]).is_file())
            self.assertFalse(list(output.rglob("*.inc")))
            self.assertFalse(list(output.rglob("*.dol")))
            self.assertFalse((output / "module-template/generated").exists())
            self.assertFalse((output / "GXRuntime/include/generated").exists())


if __name__ == "__main__":
    unittest.main()
