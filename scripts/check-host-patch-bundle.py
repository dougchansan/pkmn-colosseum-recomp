#!/usr/bin/env python3
"""Fail release builds when the canonical and optional host patches drift."""

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CANONICAL = ROOT / "mods" / "host-patches"
PACKAGES = (
    "pokemon-colosseum-ultrawide.mgm",
    "pokemon-colosseum-hd-text.mgm",
    "pokemon-colosseum-minimap.mgm",
)
COMPONENTS = (
    ("modernGekko", "MODERN", "moderngekko.patch"),
    ("recompCore", "RECOMP", "recompcore.patch"),
    ("dolRecomp", "DOLRECOMP", "dolrecomp.patch"),
)


def check_bundle(bundle: Path, canonical: dict) -> list[str]:
    errors = []
    try:
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        script = (bundle / "apply-host-patches.sh").read_text(encoding="utf-8")
    except (OSError, ValueError) as exc:
        return [f"{bundle}: {exc}"]
    if manifest != canonical:
        errors.append(f"{bundle}: manifest differs from canonical bundle")
    for key, shell_name, filename in COMPONENTS:
        try:
            expected_hash = manifest[key]["sha256"].lower()
            expected_base = manifest[key]["baseCommit"]
            actual_hash = hashlib.sha256((bundle / filename).read_bytes()).hexdigest()
        except (KeyError, OSError) as exc:
            errors.append(f"{bundle}: {filename}: {exc}")
            continue
        if actual_hash != expected_hash:
            errors.append(f"{bundle}: {filename}: SHA-256 mismatch")
        for variable, expected in ((f"{shell_name}_SHA", expected_hash),
                                   (f"{shell_name}_BASE", expected_base)):
            match = re.search(rf"^{variable}=([0-9a-fA-F]+)$", script, re.MULTILINE)
            if not match or match.group(1).lower() != expected.lower():
                errors.append(f"{bundle}: {variable} differs from manifest")
    return errors


def main() -> int:
    try:
        canonical = json.loads((CANONICAL / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"cannot read canonical host patch manifest: {exc}", file=sys.stderr)
        return 1
    errors = check_bundle(CANONICAL, canonical)
    canonical_files = {name: (CANONICAL / name).read_bytes() for name in
                       ("apply-host-patches.sh", "apply-host-patches.ps1",
                        "moderngekko.patch", "recompcore.patch", "dolrecomp.patch")}
    for package in PACKAGES:
        bundle = ROOT / "mods" / "Optional" / package / "host-patches"
        errors.extend(check_bundle(bundle, canonical))
        for name, expected in canonical_files.items():
            try:
                if (bundle / name).read_bytes() != expected:
                    errors.append(f"{bundle}: {name} differs from canonical bundle")
            except OSError as exc:
                errors.append(f"{bundle}: {name}: {exc}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Host patch bundles and checksums agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
