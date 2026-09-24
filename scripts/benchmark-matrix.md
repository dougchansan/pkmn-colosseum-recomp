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
Use `--only-scene NAME` and `--scales 1 2` for a smoke run. Add `--base-mod
pokemon-colosseum-ultrawide.mgm` if the same non-minimap package should run in
every arm. Each case uses a fresh user directory and an explicit mod directory.

`results.csv` and `results.json` contain one row per trial. `summary.csv` and the
JSON summary contain medians only when all required trials passed. `status=failed`
and a nonzero exit code identify failed runs; failures do not become zero-FPS
measurements. `unique_fps` is the change in the runner's unique `frame_count`
over elapsed wall time. `status_fps` and `status_vps` are averages of the runner's
live status readings; `ms_per_frame` is derived from unique FPS. Per-case logs
are kept alongside the results. The JSON includes host, platform, configuration,
and hashes for the runner, module, and HD ZIP. Use the same scene manifest,
durations, and repetition count on both hosts when comparing results.
