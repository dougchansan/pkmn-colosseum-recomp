#!/usr/bin/env python3
"""Download and verify the HD texture packs listed in mods/hd-packs.json.

Packs land in build/hd-packs, and nothing is moved into place until it is
verified, so setup can treat "it exists" as "the pack is good":

- a resource-pack zip must match its size and SHA-256. An interrupted download
  resumes from its .part file.
- a git-files pack is a sparse checkout of the listed files at a pinned
  commit, which git verifies. Only those files are fetched.

A pack that is already present and verified is not downloaded again.

Each pack is fetched independently. One that fails is reported and skipped,
and the exit status is 1, but the others are still installed: the packs are
enhancements, and a dead link must not stop the game from being set up.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CHUNK = 1024 * 1024
USER_AGENT = "pkmn-colosseum-recomp-setup"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def verified(path: Path, pack: dict) -> bool:
    return (
        path.is_file()
        and path.stat().st_size == pack["size"]
        and sha256(path) == pack["sha256"]
    )


def download(pack: dict, target: Path) -> None:
    partial = target.with_name(target.name + ".part")
    offset = partial.stat().st_size if partial.is_file() else 0
    if offset > pack["size"]:
        partial.unlink()
        offset = 0
    if offset == pack["size"]:
        finish(partial, pack, target)
        return

    request = urllib.request.Request(pack["url"], headers={"User-Agent": USER_AGENT})
    if offset:
        request.add_header("Range", f"bytes={offset}-")
    with urllib.request.urlopen(request, timeout=60) as response:
        if offset and response.status != 206:
            # The host ignored the range; start over rather than append.
            offset = 0
        if "text/html" in response.headers.get("Content-Type", ""):
            raise ValueError(
                "the link returned a web page instead of the file; "
                f"check that it is still shared publicly: {pack['page']}"
            )
        total = pack["size"]
        done = offset
        reported = -1
        with partial.open("ab" if offset else "wb") as stream:
            for block in iter(lambda: response.read(CHUNK), b""):
                stream.write(block)
                done += len(block)
                percent = done * 100 // total
                if percent // 10 != reported // 10:
                    reported = percent
                    print(f"  {pack['id']}: {percent}% of {total // (1024 * 1024)} MB", flush=True)
    finish(partial, pack, target)


def finish(partial: Path, pack: dict, target: Path) -> None:
    if partial.stat().st_size != pack["size"] or sha256(partial) != pack["sha256"]:
        partial.unlink()
        raise ValueError("download does not match the size and SHA-256 in mods/hd-packs.json")
    partial.replace(target)


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git {args[0]} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def fetch_git_files(pack: dict, dest: Path) -> bool:
    """Check out only the listed files at the pinned commit. Returns False if
    they were already in place."""
    output = dest / pack["id"] / pack["folder"]
    marker = dest / pack["id"] / "commit.txt"
    names = sorted(pack["files"])
    if (marker.is_file() and marker.read_text().strip() == pack["commit"]
            and sorted(p.name for p in output.glob("*")) == names):
        return False

    source = dest / f"{pack['id']}-src"
    if not (source / ".git").is_dir():
        shutil.rmtree(source, ignore_errors=True)
        git("init", "-q", str(source))
        git("-C", str(source), "remote", "add", "origin", pack["repository"])
    # Sparse patterns go in before the checkout, so only these blobs download.
    git("-C", str(source), "config", "core.sparseCheckout", "true")
    patterns = source / ".git" / "info" / "sparse-checkout"
    patterns.parent.mkdir(parents=True, exist_ok=True)
    patterns.write_text("".join(f"/{pack['folder']}/{name}\n" for name in names))
    git("-C", str(source), "fetch", "-q", "--depth", "1", "--filter=blob:none",
        "origin", pack["commit"])
    git("-C", str(source), "checkout", "-q", "--force", "--detach", pack["commit"])
    if git("-C", str(source), "rev-parse", "HEAD") != pack["commit"]:
        raise ValueError("checkout is not at the pinned commit")

    staging = output.with_name(output.name + ".tmp")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for name in names:
        shutil.copy2(source / pack["folder"] / name, staging / name)
    shutil.rmtree(output, ignore_errors=True)
    staging.replace(output)
    marker.write_text(pack["commit"] + "\n")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, default=ROOT / "mods" / "hd-packs.json")
    parser.add_argument("--dest", type=Path, default=ROOT / "build" / "hd-packs")
    args = parser.parse_args()

    packs = json.loads(args.manifest.read_text(encoding="utf-8"))["packs"]
    args.dest.mkdir(parents=True, exist_ok=True)
    failed = []
    for pack in packs:
        try:
            print(f"{pack['id']}: {pack['name']} by {pack['author']}", flush=True)
            if pack["kind"] == "git-files":
                fresh = fetch_git_files(pack, args.dest)
            else:
                target = args.dest / pack["file"]
                fresh = not verified(target, pack)
                if fresh:
                    download(pack, target)
            print(f"{pack['id']}: {'ready' if fresh else 'already downloaded and verified'}")
        except Exception as error:  # noqa: BLE001 - report and keep going
            failed.append(pack["id"])
            print(f"{pack['id']}: skipped ({error})", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
