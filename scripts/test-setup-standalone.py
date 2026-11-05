#!/usr/bin/env python3
"""Exercise module setup boundaries without game data or compilation."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("setup_standalone", Path(__file__).with_name("setup-standalone.py"))
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class SetupTests(unittest.TestCase):
    def test_windows_redirected_success_preserves_unicode_path(self):
        output, errors = io.BytesIO(), io.BytesIO()
        stdout = io.TextIOWrapper(output, encoding="cp1252")
        stderr = io.TextIOWrapper(errors, encoding="cp1252")
        module = Path("設置-é-🌍") / "gGC6E01_recomp.dll"
        try:
            with patch.object(setup.sys, "platform", "win32"), \
                 patch.object(setup.sys, "argv", ["setup-standalone.py", "owned.iso"]), \
                 patch.object(setup.sys, "stdout", stdout), \
                 patch.object(setup.sys, "stderr", stderr), \
                 patch.object(setup, "setup", return_value=module):
                setup.main()
                # Diagnostics share the same redirected Windows boundary.
                print(str(module), file=setup.sys.stderr)
            stdout.flush()
            stderr.flush()
            self.assertIn(str(module), output.getvalue().decode("utf-8"))
            self.assertEqual(errors.getvalue().decode("utf-8").strip(), str(module))
        finally:
            stdout.detach()
            stderr.detach()

    def test_direct_tool_wins_over_nested_build_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            filename = "dolrecomp" + (".exe" if setup.os.name == "nt" else "")
            direct = root / filename
            direct.write_bytes(b"tool")
            nested = root / "tools/dolrecomp"
            nested.mkdir(parents=True)
            (nested / filename).write_bytes(b"nested tool")
            self.assertEqual(setup.find_tool(root, "dolrecomp"), direct)

    def test_verified_build_installs_only_module(self):
        with tempfile.TemporaryDirectory(prefix="standalone setup ") as temporary:
            root = Path(temporary)
            host, extracted, output = root / "host", root / "disc", root / "modules"
            host.mkdir()
            (host / "settings.ini").write_text("keep settings")
            (host / "MemoryCardA.USA.raw").write_bytes(b"keep saves")
            (root / "patches").mkdir()
            (root / "patches/GC6E01-native-widescreen.csv").write_text("address,replacement\n80000000,60000000\n")
            (root / "patches/GC6E01-native-substitutions.csv").write_text("guest,symbol\n80000004,native_glyph\n")
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                self.assertNotIn("shell", kwargs)
                if command[1] == "extract":
                    extracted.mkdir()
                if command[1] == "build":
                    game = output / "GC6E01"
                    game.mkdir(parents=True)
                    module = game / "gGC6E01_recomp.dll"
                    module.write_bytes(b"local module")
                    (game / "active-module-c.txt").write_text(str(module))

            with patch.object(setup, "find_tool", side_effect=lambda directory, name: directory / name), patch.object(setup.subprocess, "run", side_effect=run):
                installed = setup.setup(root, root / "owned game.iso", root / "tools", host, extracted, output, 2)
            self.assertEqual(installed.read_bytes(), b"local module")
            self.assertEqual((host / "settings.ini").read_text(), "keep settings")
            self.assertEqual((host / "MemoryCardA.USA.raw").read_bytes(), b"keep saves")
            self.assertEqual(len(calls), 4)
            self.assertEqual(calls[0][1], str(root / "scripts/verify-disc.py"))
            self.assertEqual(calls[2][-1], str(extracted))
            self.assertIn("80000000=60000000", calls[3])
            self.assertIn("80000004:native_glyph", calls[3])

    def test_failed_verification_never_extracts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(setup, "find_tool", return_value=root / "tool"), patch.object(setup.subprocess, "run", side_effect=setup.subprocess.CalledProcessError(1, "verify")) as run:
                with self.assertRaises(setup.subprocess.CalledProcessError):
                    setup.setup(root, root / "bad.iso", root, root, root / "disc", root / "out", 1)
                self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
