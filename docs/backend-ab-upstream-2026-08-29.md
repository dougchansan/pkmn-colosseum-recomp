# C vs LLVM on current upstream — 2026-08-29

Rebuild onto current upstream, a root-cause fix for the LLVM backend, and a
re-test of the two generated-code backends for the ultrawide / 60 fps target.

## Verdict: keep the C backend

C is faster on every scene where LLVM runs at all, and LLVM hangs on two of the
seven. There is no configuration here in which LLVM is the right module to ship.

## The LLVM backend was broken by a CPUState ABI mismatch — fixed

Every LLVM module segfaulted. Root cause: GXRuntime's `CPUState` was missing
`s64 cycle_budget`, which DolRecomp's has between `downcount` and `exram`. One
8-byte field, shifting `exram` and `exram_size` by 8.

It broke *only* LLVM because of an asymmetry in how the backends reach guest
state:

- The **C backend** emits `cpu->exram` — a field name, compiled against
  GXRuntime's header. Self-correcting; it never noticed.
- The **LLVM backend** emits `offsetof(CPUState, exram)` — a raw byte offset,
  baked in from DolRecomp's header at recompile time.

So generated code loaded `exram_size` and its neighbours as the MEM2 base
pointer and dereferenced it. GXRuntime's own header states the rule that was
broken: *"Do not insert fields into the mirrored prefix without coordinating a
generated-code ABI bump with DolRecomp."* The vendored DolRecomp had been
advanced to current upstream without the matching GXRuntime update.

Fixed by adding the field to both ModernGekko mirrors of the struct and bumping
`MODERNGEKKO_CPU_ABI_VERSION` 3 -> 4, which correctly invalidates every cached
module. Both structs are now 3536 bytes with identical offsets.

**This fix already exists upstream** (ModernGekko master, and the merged-vendor
RecompCore lineage in `G:\dolrecomp-rebase-20260820`). It is a local sync gap,
not an upstream bug, and not PR material. `scripts/check-cpu-abi.sh` compiles
both headers and diffs every offset the LLVM backend emits; run it after each
vendored-DolRecomp bump, because nothing else catches this — the module links
cleanly and segfaults at runtime.

## Results

Two comparisons, because they answer different questions. Headless, mods off,
3 pairs per scene, order flipped each pair, `smc=0` on every run.

### Fair backend comparison — no substitutions on either arm

This is the backend-vs-backend number. Median frames per 20 s.

| scene | C | LLVM + state-in-memory | C/LLVM |
|---|---|---|---|
| field-outskirt | 1200 | 728 | 1.65x |
| stadium | 1075 | 741 | 1.45x |
| field-indoor | 960 | 706 | 1.36x |
| battle-start | 906 | 675 | 1.34x |
| field-phenac | 818 | 680 | 1.20x |
| battle-mid | 666 | **59 / 86 / 90** | not a ratio |
| postbattle-anchor | 591 | **STALLED 3/3** | not a ratio |

**C faster on all 5 healthy scenes, median 1.36x.** Within-arm spread on this
run: median 8.4%, worst 15.4% — so the per-scene gaps (20-65%) clear the noise,
though field-phenac's 1.20x is the least comfortable margin.

Run conditions recorded: CPU load 20% at start, 4% at end, 3 retries.

### Shipping-config comparison — C keeps its substitutions

| scene | C | LLVM | C/LLVM |
|---|---|---|---|
| field-outskirt | 1268 | 802 | 1.58x |
| field-indoor | 1207 | 745 | 1.62x |
| stadium | 1105 | 755 | 1.46x |
| battle-start | 1093 | 713 | 1.53x |
| field-phenac | 931 | 719 | 1.29x |

Median 1.53x. The difference between the two tables is the native
substitutions, which the LLVM backend cannot use.

### What the substitutions are worth

Comparing the C arm across the two sweeps (different sweeps, so weaker evidence
than a paired run): median **1.21x**, range 1.03-1.27x. That accounts for most
of the 1.53x -> 1.36x difference between the tables above.

## Windowed, mods on — the real player configuration

Vulkan, native ultrawide, HD text, minimap, fastmath, the 722 MB community HD
texture pack, `ultrawide_efb_scale=5`, 60 Hz target, uncapped, no substitutions
on either arm. Machine at 7% load falling to 3%.

**The LLVM backend does not run at all. It stalled on 7 of 7 scenes**, every
rep, 21 retries — all of them failures. A representative LLVM run: `state=running`,
`booted=1`, all four mods loaded, `speed=0.11`, `frame_count` frozen at 57737,
**`present_count=5`**. It boots, loads the state, and then produces essentially
nothing. C on the same scene the same minute: `speed=1.25`, frame counter
advancing normally.

So the headless stalls on battle-mid and postbattle-anchor were the visible edge
of something broader: under the real render path the failure generalises to
every scene. Whatever the defect is, GX command processing and the present path
provoke it far more readily than guest CPU work alone.

### C, windowed, mods on, 60 Hz, uncapped

Median across 3 runs per scene; within-arm spread 2.2% median, 8.0% worst.

| scene | fps | speed | holds 60? |
|---|---|---|---|
| field-outskirt | 80.0 | 1.32 | yes |
| stadium | 70.5 | 1.19 | yes |
| field-indoor | 64.2 | 1.07 | yes |
| battle-start | 62.6 | 1.03 | yes, barely |
| field-phenac | 53.2 | 0.87 | **no** |
| battle-mid | 45.5 | 0.73 | **no** |
| postbattle-anchor | 35.2 | 0.56 | **no** |

**4 of 7 hold 60 in this configuration**, and these are the honest numbers: real
render path, every enhancement on, HD pack actually loaded.

Note these arms carry **no native substitutions**, which are worth ~1.21x to the
C backend. Scaling by that puts field-phenac near 64 and battle-mid near 55, so
the shipping build should hold 60 on 5 of 7. postbattle-anchor projects to ~43
and remains out of reach, consistent with every earlier campaign. That scaling
is an estimate across sweeps, not a measurement — the shipping C arm has not
been run windowed.

### Two configuration traps found here

1. **Mods silently rejected after an ABI bump.** The first windowed attempt
   logged `mod rejected: ... CPU ABI mismatch` for every mod and ran on without
   them, which would have measured a mods-off configuration while reporting
   mods-on. Rebuild `mods/` after any `MODERNGEKKO_CPU_ABI_VERSION` change; see
   `build-mods.bat`.
2. **`mods/Optional/` in the source tree is stale.** It holds three packages; the
   build produces four, including `pokemon-colosseum-fastmath.mgm`, and the
   packaging script ships all of them. Benchmark against
   `build/mods/packages`, not the checked-in directory.

## At a 60 Hz target the gap narrows sharply — but C still wins

The 30 Hz tables above are the wrong frame target for a 60 fps project. Re-run
with `sixty_fps=true`, fair arms, quiet machine:

| scene | C speed | LLVM speed | C/LLVM at 60 Hz | (was, at 30 Hz) |
|---|---|---|---|---|
| field-outskirt | 1.73 | 1.23 | 1.41x | 1.57x |
| stadium | 1.22 | 0.96 | 1.27x | 1.47x |
| field-indoor | 1.22 | 1.00 | 1.22x | 1.31x |
| battle-start | 1.08 | 0.94 | 1.15x | 1.50x |
| field-phenac | 0.99 | 1.06 | **0.93x** | 1.17x |
| battle-mid | 0.77 | STALLED | — | — |
| postbattle-anchor | 0.75 | STALLED | — | — |

**C faster on 4/5, median 1.22x — down from 1.47x at 30 Hz.** The gap narrows on
every single scene, and on field-phenac LLVM actually goes ahead by 6%.

The mechanism is consistent: at 30 Hz a large share of the frame is the guest
idle loop, where LLVM does badly; doubling the frame target makes real work
dominate. C degrades more than LLVM as load rises (field-phenac 1.35 -> 0.99
against LLVM 1.15 -> 1.06). Extrapolating past 60 Hz would be unwarranted, but
the direction is unambiguous across all five scenes.

**This does not change the recommendation.** C is still ahead on 4 of 5 scenes,
and LLVM stalls outright on both battle-mid and postbattle-anchor at 60 Hz —
battle-mid degrades from crawling at 30 Hz to a full stall. The stalls have now
reproduced in four sweeps, three configurations and both frame targets.

### How to enable 60 Hz (and how not to)

Use the runner config: `config.ini` `[QualityOfLife] sixty_fps=true`. Verified at
64.9 Hz of emulated time against 30 with it off, `smc=0`.

Do **not** build it into the module with `--dol-patch 80005D98=38600000`. That
address lands in chunk `[0x800055E0,0x800095E0)`; the benchmark savestates carry
a 30 Hz RAM image that contradicts it, the runtime drops the chunk to the
interpreter, and both arms come back `smc=1`. The measurement then describes the
interpreter, and the frame target does not even rise (33 Hz measured).

Note the Action Replay 60 FPS code *is* inert under static recompilation, as
previously recorded — that is a different mechanism from this config switch, and
conflating the two is what made an earlier pass here report the patch as dead.

## Does the recent upstream work make LLVM faster? No.

The recollection that recent DolRecomp/ModernGekko/RecompCore work got LLVM
ahead traces to `notes/BEST-BACKEND-PER-TITLE.md`, where GC6E01 `sim-msr` came
out **+4.0% over C**. That measurement is sound but is not about the new work:

- Its lineage's vendored DolRecomp is **pre-rewrite** — no `native_abi`, and
  state-in-memory still selected by the `DOLRECOMP_STATE_MEMORY` environment
  variable rather than the `--state-in-memory` flag. It measured the *old* LLVM
  backend plus state-in-memory plus the MSR region-leader fix.
- It used 8 different scenes (clean-*/play-*), and the interleaved PowerShell
  harness at +/-1.7%, which is sharper than this one.

So there is no contradiction to resolve: +4.0% was the old backend on a
different scene set, and on current upstream with a fair comparison the new
backend is 1.36x *behind* C on these scenes.

Note also that `--state-in-memory` is now largely inert on this workload:
`register_state.cpp` gates it as `if (!state_in_memory_ || native_abi_)`, so any
function that qualifies for the native ABI ignores it. The lever that produced
+4.0% is not the lever being pulled here.

## The substitutions confound

Native substitutions are implemented only in `src/backend/emitter.c`, the C
emitter; the LLVM backend silently ignores `--native-subst`. Any comparison that
passes them must therefore report both tables above, or say which it is. The
archived 1.8x figure from 2026-08-19 carries the same confound: both arms were
passed the same flags, but only C honoured them.

## Harness

Two defects fixed during this run, both of which produced fake numbers:

1. **Silent early exit misread as a slow boot.** Force-killing the runner leaves
   its named shared-memory arena held briefly; launching into that window makes
   the next instance exit within seconds — one log line, no `status.txt`, no
   stray process. A fixed 3 s sleep did not cover it. Teardown is now verified by
   polling, plus a settle margin. This affected both arms at random.
2. **Measuring from a fixed sleep after `booted=1`.** The savestate may still be
   loading with the frame counter static, producing windows of exactly zero
   frames. The harness now waits for the counter to actually move, and reports
   `NOBOOT` / `STALL` / `STALLED_MID` / `DIED` by name rather than scoring a
   failure as 0 — a failure averaged in as zero reads as a performance result.

## Reproducing

    bash scripts/check-cpu-abi.sh          # must pass before trusting any LLVM build
    ./build-llvm-toolchain.bat             # MSVC builds the runtime; LLVM 20 is
                                           # only DolRecomp's backend dependency
    ./build-backend-arm.bat c    c    0
    ./build-backend-arm.bat llvm-sim llvm 1
    ARM_A=c ARM_B=llvm-sim PAIRS=3 bash bench-headless-ab.sh

Build arms from `extracted\Pokemon-Colosseum-USA` with no explicit `--dol-patch`:
that DOL is patched in place and is the image the seven savestates were captured
against. Building from the fresh `Colo-Fresh` extract with patches passed
explicitly makes the runtime drop chunk `[0x800055E0,0x800095E0)` to the
interpreter and the benchmark silently measures that.

These are headless, mods-off numbers: guest CPU only, no GX command processing,
EFB/texture work, present, or ultrawide projection. They overstate real headroom
and cannot classify a scene as CPU- or GPU-bound.
