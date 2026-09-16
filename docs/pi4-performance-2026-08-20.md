# Raspberry Pi 4 performance campaign — 2026-08-20

This records the guarded A/B runs made on the project Pi before their working
files in `/tmp` disappear. The numbers are the runner's displayed FPS metric,
sampled 20 times at three-second intervals after a fixed settle. They are useful
for comparing paired arms on this machine; they are not portable performance
targets.

## Test conditions

- Raspberry Pi 4, Debian 13, aarch64
- 1920x1080, V3D Vulkan through Mesa 25.2
- `MODERNGEKKO_UBERSHADERS=0`
- Colosseum USA (`GC6E01`), runtime DOL SHA-256
  `7e6c00a3bd632126d5466cbab856c63ac3c52a5da20b60dba7e756538152c9f4`
- 150-second settle from boot before each Colosseum sample window
- alternating arms, explicit `CPUThread` setting, and four or five pairs
- a run was accepted only when the module executed, the emulator remained
  alive, frames changed, the captured frame was non-black, and the scene was
  stable

The benchmark harnesses and module build helper live on the Pi at
`/home/recomp/{colo_ab.sh,core_ab.sh,bench_guarded.sh,build_module.sh}`.

## Colosseum results

| comparison | arm A mean | arm B mean | change |
|---|---:|---:|---:|
| baseline module → optimized LLVM module | 12.84 | 15.74 | +22.6% |
| optimized LLVM module → same-scene PGO | 15.69 | 19.65 | +25.3% |
| PGO, single-core → PGO, dual-core | 19.71 | 22.78 | +15.5% |
| original baseline → PGO plus dual-core | 12.84 | 22.78 | +77.4% |

The PGO row excludes pair 2: both arms left the stable scene and reported
standard deviations of 3.24 and 6.06. The other four alternating pairs were
stable. The harness was tightened after that run to reject this condition.

The PGO profile was trained on the same self-driving intro sequence used for
measurement. It is therefore an upper bound for that sequence, not evidence of
a 25% gain throughout the game. A representative field/menu/battle profile and
held-out scenes are required before changing the project's general PGO claim.
The local profile is derived from the game and remains only on the Pi:

- profile: `/home/recomp/pgo-pc/pc.profdata`
- 14,520 instrumented functions
- profile SHA-256:
  `af870c8d6c1024d6472d26e3b4d0311deb8d1c7bd1c6bd163486e5d43771a914`

Reproducible module identities:

| module | path on Pi | SHA-256 |
|---|---|---|
| baseline | `/home/recomp/pcfix/build-aot/gGC6E01_recomp.so` | `534cdc43178d924c13d418068c06d9964d270373e2f27ddf3248860b975d8964` |
| optimized | `/home/recomp/pc-opt/build/gGC6E01_recomp.so` | `4206303dc8bcfd3cee2a65eb22640f3845bd568556e369c1d2269a3ca4efe2a8` |
| PGO use | `/home/recomp/pc-pgouse/build/gGC6E01_recomp.so` | `7ce1549494529d25827813f61dae355b3525c0c63f7b3a96fc08f86aa2852dc3` |

The optimized and PGO builds use identical generated C/header inputs. Their
difference is the profile-guided compilation of the emitted LLVM chunk objects,
not a different guest binary.

## Other compiler experiments

Luigi's Mansion was used as a faster cross-title test for changes in the shared
compiler. These paired runs established the direction of several levers:

| change | result |
|---|---:|
| guest-state/guest-memory alias metadata | +15.0% |
| same-scene PGO after alias metadata | +21.9% |
| budget-check hoisting after PGO | +1.7% |
| accumulated budget-counter guard | -0.8% |

A later five-pair Colosseum test of the revised guard also lost slightly:
15.857 FPS baseline versus 15.820 FPS guarded (-0.23%), with the guarded arm
winning only two pairs. Do not carry either guard experiment forward.

## Profile finding and next gate

A 30-second `perf` sample of the Colosseum module recorded about 16,000 samples
with none lost. The largest attributed symbol was the emitted state machine
rooted at guest `GSgappInit` (21.7%). Instruction-level samples within that
symbol resolve to `gappBackgroundCallback`, particularly its linked-list scan
of background tasks. This is the game's idle/background scheduler, not init
work despite the enclosing symbol name.

Replacing that scheduler natively is high risk: it disables/restores guest
interrupts, mutates the task list, processes pending tasks, and invokes dynamic
guest callbacks. The failed guard experiments also show that adding more state
to this hot generated path can erase the intended gain. The next safe gates are:

1. Validate the existing PGO build on held-out gameplay scenes.
2. Soak-test `CPUThread=True` for timing, audio, save, and transition faults.
3. Only then consider a smaller code-generation change around guest-state
   reloads in the scheduler loop; do not replace the scheduler wholesale.
