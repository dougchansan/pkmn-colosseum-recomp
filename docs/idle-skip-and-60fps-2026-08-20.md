# Guest idle skip, and the push for 60 fps on every scene — August 2026

Campaign to get all seven mods-on benchmark scenes holding the game's own 60 fps
target, measured against the 2026-08-19 baseline in
[`build/bench-mods-20260819/RESULTS.md`](../build/bench-mods-20260819/RESULTS.md).

**Outcome: six of seven scenes hold 60**, up from five, from a single change.
`postbattle-anchor` does not and is not reachable with the levers tried here.

## Result

Capped, mods on, C backend, three rounds per cell, median. "Task idle" is the
change described below.

| scene | idle off | idle on | holds 60 |
|---|---|---|---|
| field-phenac | 59.9 | 59.9 | yes |
| field-outskirt | 59.8 | 59.8 | yes |
| field-indoor | 59.9 | 60.4 | yes |
| stadium | 60.0 | 59.8 | yes |
| battle-start | 59.9 | 59.9 | yes |
| **battle-mid** | **55.1** | **60.0** | **yes — fixed** |
| postbattle-anchor | 43.0 | 43.2 | no |

Uncapped, the same change moves `battle-mid` from 59.0 to 67.9 — so it clears
the target with real margin rather than sitting exactly on it.

## The change

`gappBackgroundCallback` at `0x800FEA74` is the guest's `OSSetIdleFunction`
body: a bare `for(;;)` that walks a task list, runs any runnable callback, then
splices in whatever a pending list accumulated. When nothing is runnable the
iteration does no work, and on some scenes the guest spends a large fraction of
its time there.

The existing `StaticRecompIdlePC` mechanism cannot fire on this loop.
`StaticRecompCore_Run.cpp` requires the dispatcher to regain control with
`m_guest.pc == m_idle_pc`, but the loop's back edge (`800FEB9C: b 0x800FEA90`)
is an intra-chunk `goto` in the generated C — the dispatcher never sees it.
Pointing the existing keys at the loop body records zero idle events.

What the dispatcher *does* see is the return from each `bl`, because a call out
of the chunk is emitted as `ctx->lr = ...; ctx->pc = target; return;`. So a
second trigger was added, keyed on `0x800FEB9C` — the instruction after
`bl OSRestoreInterrupts` at the bottom of the loop. That point is chosen
deliberately over anything earlier: it is *after* interrupts are re-enabled, so
`CoreTiming::Idle()` cannot advance emulated time while the guest believes
interrupts are masked.

When the trigger fires, a predicate mirroring the guest's own logic decides
whether to idle:

- pending list head (`0x8047AC9C`) non-zero → do not idle, a splice is due;
- any node in the active list (head `0x8047AC98`, stride `0x18`, `next` at
  `+0x4`, `state` at `+0x8`, `blocked` at `+0xD`) with `state == 2 &&
  blocked == 0` → do not idle, there is real work;
- otherwise the iteration is a no-op and idling is correct.

Three safety properties are deliberate: any unreadable guest address answers
"not empty" (a bad read costs performance, never correctness); the list walk is
bounded at 256 nodes so a corrupt or mid-update list cannot hang the dispatcher;
and the feature is off unless configured.

Both guest addresses were confirmed three independent ways — the decompilation
(`src/game/gs_gapp.c`), the generated chunk (`r13-23432` and `r13-23428`), and a
runtime trace showing `r13 = 0x80480820`.

### Configuration

Three keys, all defaulting to `0`, so an unconfigured build behaves exactly as
before and the feature is a pure config flip on one binary:

```ini
[Core]
StaticRecompTaskIdlePC = 0x800FEB9C
StaticRecompTaskListHead = 0x8047AC98
StaticRecompTaskPendingHead = 0x8047AC9C
```

These are game-specific. Put them in `<user>/GameSettings/GC6E01.ini`
(the LocalGame layer), which outranks `Sys/GameSettings/GC6E01.ini` — that file
already sets `StaticRecompIdlePC` for the unrelated startup wait loop at
`0x800D31E0`, and both mechanisms coexist.

## Correctness

Matched-window differential lockstep against the interpreter, same scene and
window, feature off vs on:

```
off:  checks=2011  reports=73  undercharges=0  max_deficit=0
on:   checks= 722  reports=29  undercharges=0  max_deficit=0

diverging blocks in both: 29    only-off: 44    ONLY-ON: 0
```

Every block that diverges with idle enabled also diverges with it disabled — the
`on` set is a strict subset — and the divergence classes are identical
(`CTRLFLOW`, `fpscr`, `r0`, `r25`, `r3`). No block diverges only with the
feature on. Cycle accounting is exact both ways, which is the check that would
catch idle stealing emulated time.

Two caveats. The `on` arm checks fewer blocks because idle skips dispatches, so
raw divergence *counts* between arms are not comparable — only the sets are.
And **lockstep is not clean at baseline**: 73 diverging blocks with the feature
off is a pre-existing property of the recompiler, unrelated to this change. Any
plan that gates on "reports must be 0" is gating on something that has never
held; diff against a baseline run instead.

## Levers that did not work

Measured paired against a same-session baseline, both run orderings, with a
null candidate (a rebuild of the baseline) establishing the noise floor.

| lever | result |
|---|---|
| `--arch-flags "-mfma"` | ~0%, at or below the null candidate |
| `--opt-level 3` | ~0%, and +1.4 MB module size |
| inline cache ops | +2.2–3.5%, real but small |
| inline FP level 2 | +1.4% against a +1.2% null — unresolved |

`-mfma` is worth recording as a negative result because the reasoning behind it
was sound: `module-template/CMakeLists.txt` documents that `fmaf()` is a
software libm call on baseline x86-64, and the native substitutions are the hot
path. It simply does not show up in measurement, and it would have cost
pre-Haswell/pre-Piledriver compatibility for nothing.

## Why `postbattle-anchor` is not reachable

It sits at ~43 capped and needs 60 — **+39%**. Everything measured compounds to
roughly +13%, landing near 51. The scene is not graphics-bound: a nine-variant
config probe moved nothing outside ±2.3%, and dropping `ultrawide_efb_scale`
from 5 to 3 was +0.0%.

**A profile share is not a speedup bound.** The idle loop was ~15% of attributed
samples on this scene and returned +3.7% there, because that time was already
partly being yielded to the host. The same reasoning applies to any remaining
candidate: a function worth 12% of samples will not return 12%.

### The profiler's candidate list is not trustworthy

The planned next step was native substitution of `fn_80009178`, reported at
12.3% of attributed samples. It is **menu cursor navigation** —
`windowGetKeyInfo()`, row/column movement, walking a menu item list — which runs
at most once per frame and cannot be 12% of a battle scene.

The cause is in `attribute-samples.py`: it recovers the guest PC by scanning the
disassembly for `ctx->pc = 0x...` immediate stores, but the optimiser does not
emit one at every instruction, so a sample is charged to the last PC that
happened to be stored and then bisects to the preceding symbol. Neighbouring
`fn_800096B4` is 0x23E0 bytes, which is enough to absorb a lot of misattributed
samples.

Re-deriving attribution — an instrumented or no-LTO module, or emitting a PC
store per basic block in profiling builds only — is the prerequisite for any
future substitution work. Without it, candidate selection is guesswork.

## Known issues found along the way

- **`scripts/train-pgo.ps1` is interactive**, so it cannot cover benchmark
  savestates. It installs an instrumented module and asks a human to play, and
  `postbattle-anchor` -- the scene that needs the help -- is a savestate nobody
  reaches by playing. Its build arguments are otherwise correct.
- **DOL patches: pass them explicitly.** `extracted/.../sys/main.dol` is already
  patched in place, because the runner applies
  `patches/GC6E01-native-widescreen.csv` to it persistently at startup
  (`moderngekko_run.cpp:410`); all four addresses were verified to hold their
  patched values. So a module built *without* `--dol-patch` still contains the
  widescreen patches -- which is why the benchmark module
  (`patches=a94cca99aeb8ed93`) is valid despite omitting them. It is valid only
  by luck: against a freshly extracted, unpatched DOL the same command silently
  yields a module with **no native ultrawide**, and nothing flags it.
  `package-imgui-launcher.ps1` and `train-pgo.ps1` both pass the patches
  explicitly, which is the correct, self-healing pattern. Re-applying them to an
  already-patched DOL writes identical bytes, so only the `patches=` cache key
  differs, not the module content.
- **The 2026-08-19 benchmark did not load the HD texture pack.** See the
  correction at the top of that report.
- **`bench-mods.ps1` aborts a whole sweep** on a `$proc.Kill()` race with an
  already-exiting process, because `ErrorActionPreference` is `Stop`.
- **Benchmark hygiene:** the harness retains a full package copy per run, which
  is enough to exhaust a disk over a long sweep and silently corrupt the
  results — failed runs report as `never reached a presenting state`. Round 1 of
  any scene is also systematically slow (cold page cache on a 45 MB module:
  31.2 vs 45.2 on the *same* module), so pre-warm the modules and use a settle
  window of at least 20 s.
