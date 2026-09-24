#!/usr/bin/env python3
"""Copy vendored dependency notice files into a binary distribution."""

import re
import shutil
import sys
from pathlib import Path


NOTICE_NAME = re.compile(r"^(?:LICENSE|LICENCE|COPYING|NOTICE)(?:$|[._-])", re.IGNORECASE)


def collect(source: Path, destination: Path) -> int:
    if not source.is_dir():
        raise ValueError(f"missing dependency source: {source}")
    count = 0
    for path in source.rglob("*"):
        if path.is_symlink() or not path.is_file() or not NOTICE_NAME.match(path.name):
            continue
        if path.stat().st_size > 1024 * 1024:
            raise ValueError(f"unexpectedly large notice file: {path}")
        target = destination / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        count += 1
    if count < 20:
        raise ValueError(f"only {count} dependency notices found; check recursive submodules")
    return count


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: collect-third-party-notices.py <Externals> <output-directory>")
    try:
        found = collect(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"notice collection failed: {exc}") from exc
    print(f"Collected {found} dependency notices")
