#!/usr/bin/env python3
"""Explicitly acquire the pinned existing DXIL runtime; packaging stays offline."""
import hashlib
import importlib.util
from pathlib import Path
import sys
import urllib.request


def main():
    root = Path(__file__).resolve().parent.parent
    spec = importlib.util.spec_from_file_location("standalone_package", root / "scripts/package-standalone.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    data = package.load_prebuilt_manifest(root)
    archive = package.prebuilt_runtime_archive(root, data)
    if archive.exists():
        package.verified_dxil(root, data)
        print(f"Verified cached DXIL runtime: {archive}")
        return
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_suffix(".download")
    try:
        digest = hashlib.sha256()
        size = 0
        with urllib.request.urlopen(data["windows_dxil"]["asset_url"], timeout=120) as response, temporary.open("wb") as stream:
            for block in iter(lambda: response.read(1024 * 1024), b""):
                size += len(block)
                if size > 512 * 1024 * 1024:
                    raise ValueError("DXIL archive exceeds expected download size")
                digest.update(block)
                stream.write(block)
        if digest.hexdigest() != data["windows_dxil"]["asset_sha256"]:
            raise ValueError("downloaded DXIL archive hash mismatch")
        temporary.replace(archive)
        try:
            package.verified_dxil(root, data)
        except Exception:
            archive.unlink()
            raise
        print(f"Verified pinned DXIL runtime: {archive}")
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f"runtime acquisition failed: {exc}")
