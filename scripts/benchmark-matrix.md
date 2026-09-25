# Cross-platform scene benchmark

`benchmark-matrix.py` uses only Python's standard library. Run it separately on
the Linux host and the MacBook with each machine's own build, extracted game,
compatible module, savestates, built mod packages, and community HD ZIP.
Keep those inputs outside the repository.

Create a local JSON manifest, for example:

```json
{
  "scenes": [
    {"name": "phenac-field", "state": "/absolute/path/to/phenac-field.sav"},
    {"name": "battle", "state": "/absolute/path/to/battle.sav"}
  ]
}
```

Run from a terminal with a graphical desktop session:

```sh
python3 scripts/benchmark-matrix.py \
  --runner /absolute/path/to/moderngekko-run \
  --game /absolute/path/to/extracted-game \
  --module /absolute/path/to/game-module \
  --mods-dir /absolute/path/to/built-mod-packages \
  --hd-pack '/absolute/path/to/Pokemon Colosseum HD Pack.zip' \
  --manifest /absolute/path/to/scenes.json \
  --output /absolute/path/to/new-benchmark-results
```

Use `--states-dir /absolute/path/to/states` in place of `--manifest` to include
every `.sav` file in that directory and its subdirectories. Scene names derive
from relative filenames. Names must be unique after punctuation is replaced.

The output directory must be new. The default matrix covers every scene with
minimap off/on, community HD pack off/on, 1x through 6x internal resolution,
and three separate trials of each case. Linux defaults to Vulkan and macOS to
Metal; `--backend` overrides this. The selected backend is written into each
fresh user configuration and passed to the runner.
Each scene runs all configurations once per trial, reversing their order on
alternate trials to reduce thermal and background-load bias.
Use `--only-scene NAME` and `--scales 1 2` for a smoke run. Add `--base-mod
pokemon-colosseum-ultrawide.mgm` if the same non-minimap package should run in
every arm. Each case uses a fresh user directory and an explicit mod directory.
The generated user directory is removed after each run so the HD pack cannot
fill the host disk; the effective config, logs, and measurements remain.
On macOS, keep the desktop unlocked and lid open. The runner rejects locked
desktop measurements and stops the sweep if the screen locks.

Status snapshots older than one second, implausibly future-dated, or modified
in place during a read are rejected. Contents and timestamps come from one
opened file, so atomic pathname replacement does not reject a valid snapshot.
The start waits for a fresh snapshot; an invalid or
stale snapshot during measurement fails the case instead of reporting inflated
FPS. `measurement-samples.json` preserves the accepted start, each measured
sample, snapshot age/mtime, and rejected samples for diagnosis. This is a
status-sampled throughput measurement, not a GPU frametime profiler. Earlier
results lacking freshness checks must not be treated as validated timings.

On a host where disk latency stalls status publication, `--telemetry-root /dev/shm`
can isolate the small telemetry files on an existing memory-backed directory.
It does not move the game or HD pack. Each case uses a unique temporary directory;
after the runner exits its telemetry is copied into the case evidence directory
before that temporary directory is removed. The selected root is recorded in
metadata and must be disclosed when comparing runs.

`results.csv` and `results.json` contain one row per trial. `summary.csv` and the
JSON summary contain medians only when all required trials passed. `status=failed`
and a nonzero exit code identify failed runs; failures do not become zero-FPS
measurements. `unique_fps` is the change in the runner's unique `frame_count`
over elapsed wall time. `status_fps` and `status_vps` are averages of the runner's
live status readings; `ms_per_frame` is derived from unique FPS. Per-case logs
are kept alongside the results. The JSON includes host, platform, configuration,
source commit, and hashes for the runner, module, HD ZIP, and each savestate.
Use the same scene manifest,
durations, and repetition count on both hosts when comparing results.
