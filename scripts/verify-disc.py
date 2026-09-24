#!/usr/bin/env python3
"""Verify the exact GC6E01 dump supported by this release."""

import hashlib
import sys
from pathlib import Path


DISC_ID = b"GC6E01"
ISO_SHA256 = "78c4074429fa598b97bfac296bf8e0f7e93f5cb6d8679f8d2653ca902820f6c5"
DOL_SHA256 = "7e6c00a3bd632126d5466cbab856c63ac3c52a5da20b60dba7e756538152c9f4"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify(iso: Path, game_root=None) -> None:
    if sha256(iso) != ISO_SHA256:
        raise ValueError("unsupported ISO: this release requires the verified GC6E01 USA dump")

    if game_root is None:
        return

    boot = game_root / "sys" / "boot.bin"
    with boot.open("rb") as stream:
        disc_id = stream.read(6)
    if disc_id != DISC_ID:
        raise ValueError(f"unsupported extracted disc ID: {disc_id!r}; expected GC6E01")

    main_dol = game_root / "sys" / "main.dol"
    if sha256(main_dol) != DOL_SHA256:
        raise ValueError(
            "extracted main.dol differs from the verified GC6E01 revision; "
            "check for a stale or modified extraction"
        )


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: verify-disc.py <iso> [extracted-game-root]")
    try:
        verify(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) == 3 else None)
    except (OSError, ValueError) as exc:
        raise SystemExit(f"disc verification failed: {exc}") from exc
    print("Verified GC6E01 USA ISO" + (" and original main.dol" if len(sys.argv) == 3 else ""))
