#!/usr/bin/env python3
"""Run a reproducible local ModernGekko scene/feature/resolution matrix.

Manifest JSON: {"scenes": [{"name": "phenac", "state": "/private/phenac.sav"}]}
Savestates, game files, and the community HD archive stay outside this repo.
"""

import argparse
import csv
import hashlib
import itertools
import json
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path

FIELDS = ("host", "scene", "minimap", "hd_pack", "scale", "trial", "status", "error",
          "elapsed_s", "frames", "unique_fps", "ms_per_frame", "status_fps",
          "status_vps", "status_speed", "samples", "log")
SUMMARY_FIELDS = ("host", "scene", "minimap", "hd_pack", "scale", "status",
                  "ok_trials", "required_trials", "median_unique_fps",
                  "median_ms_per_frame", "median_status_fps", "median_status_vps",
                  "failed_trials")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_commit():
    root = Path(__file__).resolve().parent.parent
    result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def read_status(path):
    try:
        values = dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines()
                      if "=" in line)
        if values.get("booted") != "1":
            return None
        return {key: float(values[key]) for key in ("fps", "vps", "speed")} | {
            "frame_count": int(values["frame_count"]), "state": values.get("state", ""),
            "last_error": values.get("last_error", "")}
    except (OSError, ValueError, KeyError):
        return None


def wait_status(path, process, deadline, min_frames=None):
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"runner exited early ({process.returncode})")
        status = read_status(path)
        if status and status["state"] not in ("stopped", "error"):
            if min_frames is None or status["frame_count"] > min_frames:
                if status["last_error"]:
                    raise RuntimeError(status["last_error"])
                return status
        time.sleep(0.2)
    raise RuntimeError("timed out waiting for advancing frame_count")


def prepare_case(path, scale, minimap, hd, pack, mods, backend, base_mods):
    path.mkdir(parents=True, exist_ok=False)
    user = path / "user"
    user.mkdir()
    # Each run gets a fresh user profile. Nothing from a previous mod or pack
    # installation can leak into the opposite arm.
    (user / "config.ini").write_text(
        f"[Video]\nresolution={640 * scale}x{528 * scale}\nbackend={backend}\n"
        f"hires_textures={'true' if hd else 'false'}\n"
        f"community_hd_texture_pack={'true' if hd else 'false'}\n"
        f"navigation_overlay={'1' if minimap else '0'}\n"
        "[QualityOfLife]\nsixty_fps=true\n", encoding="utf-8")
    mod_dir = path / "mods"
    mod_dir.mkdir()
    selected = [mods / name for name in base_mods]
    if minimap:
        selected.append(mods / "pokemon-colosseum-minimap.mgm")
    if minimap and not selected[-1].is_dir():
        raise RuntimeError("minimap package missing from --mods-dir")
    for source in selected:
        if not source.is_dir():
            raise RuntimeError(f"base mod package missing: {source}")
        if not any((source / f"mod.{ext}").is_file() for ext in ("so", "dylib", "dll")):
            raise RuntimeError(f"mod binary missing: {source}")
        # Package symlinks avoid copying binary payloads and preserve a precise
        # package set; the runner is pointed only at this directory.
        try:
            (mod_dir / source.name).symlink_to(source.resolve(), target_is_directory=True)
        except OSError:
            shutil.copytree(source, mod_dir / source.name)
    if hd:
        archive_dir = user / "ResourcePacks"
        archive_dir.mkdir()
        target = archive_dir / "Pokemon Colosseum HD Pack.zip"
        try:
            os.link(pack, target)
        except OSError:
            shutil.copy2(pack, target)
        if target.stat().st_size != pack.stat().st_size:
            raise RuntimeError("HD archive staging failed")
    return user, mod_dir


def run_case(args, scene, minimap, hd, scale, trial, outdir):
    name = f"{scene['name']}-mini{int(minimap)}-hd{int(hd)}-{scale}x-trial{trial}"
    row = dict.fromkeys(FIELDS, "")
    row.update(host=socket.gethostname(), scene=scene["name"], minimap=int(minimap),
               hd_pack=int(hd), scale=scale, trial=trial, status="failed",
               log=str(outdir / name / "runner.log"))
    process = None
    try:
        user, mod_dir = prepare_case(outdir / name, scale, minimap, hd,
                                     args.hd_pack, args.mods_dir, args.backend, args.base_mod)
        automation = outdir / name / "automation"
        automation.mkdir()
        command = [str(args.runner), "--game", str(args.game), "--module", str(args.module),
                   "--user-dir", str(user), "--load-state", scene["state"],
                   "--automation-dir", str(automation), "--graphics", args.backend,
                   "--no-mods", "--mods", str(mod_dir)]
        with Path(row["log"]).open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            status_path = automation / "status.txt"
            first = wait_status(status_path, process, time.monotonic() + args.boot_timeout)
            # A booted core may still be loading the savestate. Require frames
            # to advance before the warmup starts.
            wait_status(status_path, process, time.monotonic() + args.boot_timeout,
                        first["frame_count"])
            time.sleep(args.warmup)
            start = wait_status(status_path, process, time.monotonic() + 5)
            start_time = time.monotonic()
            samples = []
            while time.monotonic() - start_time < args.duration:
                if process.poll() is not None:
                    raise RuntimeError(f"runner exited during measurement ({process.returncode})")
                time.sleep(min(1, max(0.05, args.duration - (time.monotonic() - start_time))))
                value = read_status(status_path)
                if value is None or value["last_error"] or value["state"] in ("stopped", "error"):
                    raise RuntimeError("invalid runner status during measurement")
                samples.append(value)
            end_time = time.monotonic()
            end = samples[-1]
            if hd and "community HD texture pack unavailable" in Path(row["log"]).read_text(
                    encoding="utf-8", errors="replace"):
                raise RuntimeError("runner rejected community HD pack; see log")
            frames = end["frame_count"] - start["frame_count"]
            elapsed = end_time - start_time
            if frames <= 0 or elapsed <= 0:
                raise RuntimeError("frame_count did not advance")
            fps = frames / elapsed
            row.update(status="ok", elapsed_s=round(elapsed, 3), frames=frames,
                       unique_fps=round(fps, 3), ms_per_frame=round(1000 / fps, 3),
                       status_fps=round(statistics.mean(s["fps"] for s in samples), 3),
                       status_vps=round(statistics.mean(s["vps"] for s in samples), 3),
                       status_speed=round(statistics.mean(s["speed"] for s in samples), 3),
                       samples=len(samples))
    except (OSError, RuntimeError, ValueError) as exc:
        row["error"] = str(exc)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        # The HD pack can install gigabytes of textures into a fresh user
        # profile. Keep the effective config and logs, then remove only this
        # case's generated profile before starting the next of hundreds of runs.
        case_dir = outdir / name
        user_dir = case_dir / "user"
        if user_dir.is_dir() and not user_dir.is_symlink():
            try:
                config = user_dir / "config.ini"
                if config.is_file():
                    shutil.copy2(config, case_dir / "effective-config.ini")
                shutil.rmtree(user_dir)
            except OSError as exc:
                row["status"] = "failed"
                row["error"] = f"could not remove generated user profile: {exc}"
    return row


def summarize(rows, repetitions):
    groups = {}
    for row in rows:
        key = (row["host"], row["scene"], row["minimap"], row["hd_pack"], row["scale"])
        groups.setdefault(key, []).append(row)
    summary = []
    for key, group in groups.items():
        ok = [row for row in group if row["status"] == "ok"]
        failed = [row["trial"] for row in group if row["status"] != "ok"]
        complete = len(ok) == repetitions and len(group) == repetitions
        item = dict(zip(SUMMARY_FIELDS[:5], key))
        item.update(status="ok" if complete else "failed", ok_trials=len(ok),
                    required_trials=repetitions, failed_trials=",".join(map(str, failed)))
        for source, target in (("unique_fps", "median_unique_fps"),
                               ("ms_per_frame", "median_ms_per_frame"),
                               ("status_fps", "median_status_fps"),
                               ("status_vps", "median_status_vps")):
            item[target] = round(statistics.median(row[source] for row in ok), 3) if complete else ""
        summary.append(item)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("runner", "game", "module", "mods-dir", "hd-pack", "output"):
        parser.add_argument("--" + key, type=Path, required=True)
    scene_source = parser.add_mutually_exclusive_group(required=True)
    scene_source.add_argument("--manifest", type=Path)
    scene_source.add_argument("--states-dir", type=Path)
    parser.add_argument("--backend", choices=("Metal", "Vulkan", "OGL"),
                        default="Metal" if platform.system() == "Darwin" else "Vulkan")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--warmup", type=float, default=10)
    parser.add_argument("--boot-timeout", type=float, default=120)
    parser.add_argument("--scales", type=int, nargs="+", default=list(range(1, 7)))
    parser.add_argument("--only-scene", action="append")
    parser.add_argument("--base-mod", action="append", default=[],
                        help="package directory name to include in every arm")
    args = parser.parse_args()
    if not args.scales or any(x < 1 or x > 6 for x in args.scales):
        parser.error("scales must be between 1 and 6")
    if args.repetitions < 1:
        parser.error("repetitions must be positive")
    if args.duration <= 0 or args.warmup < 0 or args.boot_timeout <= 0:
        parser.error("duration and boot-timeout must be positive; warmup must be nonnegative")
    for key in ("runner", "game", "module", "mods_dir", "hd_pack"):
        if not getattr(args, key).exists():
            parser.error(f"--{key.replace('_', '-')} does not exist")
    if not args.runner.is_file() or not args.module.is_file() or not args.game.is_dir() or not args.mods_dir.is_dir():
        parser.error("runner/module must be files and game/mods-dir must be directories")
    if not args.hd_pack.is_file() or args.hd_pack.suffix.lower() != ".zip":
        parser.error("--hd-pack must name the community pack zip archive")
    try:
        with zipfile.ZipFile(args.hd_pack) as archive:
            if archive.testzip() is not None or not any(
                    name.lower().endswith("manifest.json") for name in archive.namelist()):
                parser.error("--hd-pack is corrupt or lacks a manifest")
    except (OSError, zipfile.BadZipFile) as exc:
        parser.error(f"invalid HD archive: {exc}")
    for name in args.base_mod:
        if Path(name).name != name or not name.endswith(".mgm") or "minimap" in name.lower():
            parser.error("--base-mod requires a package directory name other than minimap")
    try:
        if args.manifest:
            scenes = json.loads(args.manifest.read_text(encoding="utf-8"))["scenes"]
        else:
            if not args.states_dir.is_dir():
                raise ValueError("--states-dir must be a directory")
            states = sorted((path for path in args.states_dir.rglob("*")
                             if path.is_file() and path.suffix.lower() == ".sav"),
                            key=lambda path: str(path).lower())
            scenes = [{"name": re.sub(r"[^A-Za-z0-9_-]", "_",
                                      str(path.relative_to(args.states_dir).with_suffix(""))),
                       "state": str(path.resolve())} for path in states]
        if not scenes or any(not isinstance(s, dict) or
                             not isinstance(s.get("name"), str) or
                             not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", s["name"]) or
                             not isinstance(s.get("state"), str) or
                             not Path(s["state"]).is_absolute() or
                             not Path(s["state"]).is_file() for s in scenes):
            raise ValueError("scenes need safe unique names and absolute existing savestates")
        if len({s["name"] for s in scenes}) != len(scenes):
            raise ValueError("scene names must be unique")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(f"invalid scene source: {exc}")
    if args.only_scene:
        scenes = [s for s in scenes if s["name"] in args.only_scene]
        if not scenes:
            parser.error("no requested scenes in manifest")
    args.runner = args.runner.resolve()
    args.game = args.game.resolve()
    args.module = args.module.resolve()
    args.mods_dir = args.mods_dir.resolve()
    args.hd_pack = args.hd_pack.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {"timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "source_commit": source_commit(),
                "host": socket.gethostname(), "platform": platform.platform(),
                "machine": platform.machine(), "processor": platform.processor(),
                "python": platform.python_version(), "runner_sha256": sha256(args.runner),
                "module_sha256": sha256(args.module), "hd_pack_sha256": sha256(args.hd_pack),
                "backend": args.backend, "duration_s": args.duration,
                "warmup_s": args.warmup, "boot_timeout_s": args.boot_timeout,
                "scales": args.scales, "repetitions": args.repetitions,
                "base_mods": args.base_mod,
                "scene_source": str((args.manifest or args.states_dir).resolve()),
                "scenes": [{"name": scene["name"],
                            "state_sha256": sha256(Path(scene["state"]))} for scene in scenes]}
    rows = []
    try:
        configurations = list(itertools.product((False, True), (False, True), args.scales))
        for scene in scenes:
            for trial in range(1, args.repetitions + 1):
                # Reverse every other pass so scale and mod order do not always
                # coincide with thermal drift on long sweeps.
                ordered = configurations if trial % 2 else reversed(configurations)
                for minimap, hd, scale in ordered:
                    row = run_case(args, scene, minimap, hd, scale, trial, args.output)
                    rows.append(row)
                    print(f"{row['scene']} mini={int(minimap)} hd={int(hd)} {scale}x "
                          f"trial={trial}: {row['status']} "
                          f"{row['unique_fps'] or row['error']}", flush=True)
                    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as target:
                        writer = csv.DictWriter(target, fieldnames=FIELDS)
                        writer.writeheader()
                        writer.writerows(rows)
                    summary = summarize(rows, args.repetitions)
                    with (args.output / "summary.csv").open("w", newline="", encoding="utf-8") as target:
                        writer = csv.DictWriter(target, fieldnames=SUMMARY_FIELDS)
                        writer.writeheader()
                        writer.writerows(summary)
                    (args.output / "results.json").write_text(
                        json.dumps({"metadata": metadata, "runs": rows, "summary": summary}, indent=2) + "\n",
                        encoding="utf-8")
    except KeyboardInterrupt:
        print("interrupted; completed rows preserved", file=sys.stderr)
        return 130
    return 1 if any(r["status"] != "ok" for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
