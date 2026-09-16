# Skip whole-range icache invalidation when the range holds no blocks

Draft PR for **ExpansionPak/RecompCore**, branch `moderngekko-vendor`
(`Source/Core/Core/PowerPC/JitCommon/JitCache.{h,cpp}`), then a submodule
bump in ModernGekko. Measured on Pokémon Colosseum (GC6E01), 2026-09-05.

**Deliberately not proposed to dolphin-emu/dolphin.** `git blame` says the
lines are Dolphin's own, so the same inconsistency exists there — but every
measurement below was taken on the StaticRecomp core, where the JIT block
cache is nearly empty and the early-out therefore fires on essentially every
flush. Under Dolphin's normal JIT the game itself is compiled, `valid_block`
is densely populated, and the payoff is unknown and probably much smaller.
Claiming these numbers upstream would be claiming them for a configuration
they were not measured in. Scope is RecompCore.

## The defect

`JitBaseBlockCache::InvalidateICacheInternal` has an early-out for the
single-cache-line case and none for the multi-line case:

```cpp
bool destroy_block = true;
if (length == 32 && (physical_address & 0x1fu) == 0)
{
  if (!valid_block.Test(physical_address / 32))
    destroy_block = false;              // nothing compiled here: skip
  else
    valid_block.Clear(physical_address / 32);
}
else if (length > 32)
{
  // no test at all
  const u32 covered_block_start = (physical_address + 0x1f) / 32;
  const u32 covered_block_end = (physical_address + length) / 32;
  for (u32 i = covered_block_start; i < covered_block_end; ++i)
    valid_block.Clear(i);
}

if (destroy_block)
{
  ErasePhysicalRange(physical_address, length);
  ...
}
```

For any range larger than one cache line the code clears every covered bit and
then calls `ErasePhysicalRange` unconditionally — whether or not a single block
was ever compiled in that range. The comment explains why the bits are cleared,
but not why the range is erased when there is nothing in it.

The cost is proportional to the size of the flush, and guests flush big buffers.

## What it costs, measured

A guest `DCFlushRange` over a staged buffer reaches this path via
`JitInterface::InvalidateICacheLines`. Instrumenting the runtime on Colosseum's
post-battle scene, windowed, in the shipping configuration:

```
~890 DCFlushRange calls/s   3,650,000 cache lines/s
sizes[1|2-7|8-31|32-127|128+] = 0|0|0|0|1785     (every flush is 128+ lines)
0 hit   ~75,000 miss   (100.0% skippable)   ~37,000 invalidations/s
```

Average flush ~4,100 lines (~131 KB). Across the whole run, **not one**
invalidation found a compiled block: 0 hits against ~75,000 misses. The bitset
loop alone runs 3.65M iterations per second doing nothing, and each range also
pays an `ErasePhysicalRange` over an empty range.

This was found from the other end: a time-based guest-PC profile put
`DCFlushRange` at **71% of guest CPU** on that scene, and the cost stayed put
after the routine was replaced with a native implementation — which is what
pointed at the host side of the call rather than the guest loop.

## Why this bites hardest under static recompilation

With a native recompiled module executing the guest, almost nothing goes through
the JIT block cache — only fallback chunks and interpreted regions ever compile.
`valid_block` is therefore nearly empty, and every one of the guest's large
buffer flushes walks thousands of bits that are already clear and then erases a
physical range containing nothing. The counter below records **zero** hits
against ~75,000 invalidations across a whole run.

The same code path exists under Dolphin's normal JIT, but there the block cache
holds the game's own compiled code, so a flush that overlaps compiled regions
does real work and the early-out fires less often. That configuration is not
measured here.

## The change

Add a word-at-a-time range test to `ValidBlockBitSet` and use it to gate both
the clear loop and `destroy_block`:

```cpp
bool TestRange(u32 first, u32 last) const;   // any bit set in [first, last]
```

```cpp
else if (length > 32)
{
  const u32 covered_block_start = (physical_address + 0x1f) / 32;
  const u32 covered_block_end = (physical_address + length) / 32;
  // covered_block_* are the FULLY covered lines, but ErasePhysicalRange below
  // takes the whole byte range, so a block can begin inside a partially covered
  // line at either end. Test every line the range touches.
  const u32 touched_block_start = physical_address / 32;
  const u32 touched_block_end = (physical_address + length - 1) / 32;
  if (!valid_block.TestRange(touched_block_start, touched_block_end))
  {
    destroy_block = false;               // nothing to clear, nothing to erase
  }
  else
  {
    for (u32 i = covered_block_start; i < covered_block_end; ++i)
      valid_block.Clear(i);
  }
}
```

`TestRange` scans 32 lines per word and returns on the first non-zero word, so
the common case costs a handful of loads instead of thousands of stores.

Behaviour is unchanged whenever anything is compiled in the range. When nothing
is, clearing already-clear bits and erasing a range containing no blocks are
both no-ops, so skipping them is not observable.

## Results

Pokémon Colosseum, windowed, real player configuration (Vulkan, ultrawide, HD
texture pack, 60 Hz), identical recompiled module in both arms, three
interleaved pairs per scene, arms differing only by the feature flag:

| scene | off | on | change | paired t | 95% CI | pairs favouring |
| --- | --- | --- | --- | --- | --- | --- |
| post-battle | 45.7 fps | 54.8 fps | **+19.9%** | 10.5 | +16.5% .. +24.1% | 6/6 |
| mid-battle | 67.3 fps | 72.5 fps | **+7.8%** | 2.6 | +1.7% .. +13.2% | 4/6 |

Six interleaved pairs per scene, analysed pairwise because this machine drifts
between runs by more than the effect on the smaller scene. The second scene is a
control: it was already above its target and it moved anyway, which is what says
this is a general effect rather than one savestate's quirk. Its wider interval
and 4-of-6 split are consistent with a real but smaller gain on a scene that
flushes less.

The size of the win tracks how much a title flushes, so it should be visible in
any game that stages large buffers for the GPU or DMA, and invisible in one that
does not. Only one title has been measured.

## Correctness

### A bug this review found, and the test that catches it

The first formulation gated on the FULLY covered cache lines,
`(pa + 0x1f)/32 .. (pa + length)/32`. Those bounds exclude a partially covered
line at each end — but `ErasePhysicalRange(physical_address, length)` takes the
whole byte range. A block lying in a partial edge line, with no fully covered
line holding a bit, would therefore have escaped erasure: stale compiled code,
silently.

Every check passed anyway, because `InvalidateICacheLines` always passes a
32-byte-aligned address and a multiple-of-32 length: the benchmark, the lockstep
run and the 8.4M-case range test never entered the path where the two
formulations differ. A game with self-modifying code would not have found it
either — the gap was not "no title has SMC", it was "no caller here is
unaligned".

`upstream-jitcache-range-earlyout-decision-test.cpp` models the decision rather
than the bitset primitive, with deliberately unaligned addresses and
non-multiple-of-32 lengths, and checks the one-directional safety property: the
early-out may fire only when no touched line holds a block.

```
checked 400000 cases
  fixed formulation:    0 unsafe skips
  original formulation: 5232 unsafe skips  (expected > 0)
```

It fails against the original and passes against the fix, which is why it is
included rather than the property merely being asserted.


**Range test, exhaustively.** `TestRange` is checked against a naive per-bit
reference over 8,474,272 cases with 0 failures: every single-bit population
against every range of a 256-bit set (which covers all first-word, last-word and
single-word mask boundaries), the empty set against every range, and 20,000
randomised multi-bit populations. The boundary masks are the whole risk in this
patch and the benchmark scene never exercises them, since it has no blocks in
any flushed range at all — so they are tested directly rather than inferred.

**Differential execution.** The static recompiler's lockstep verifier, which
checks each recompiled block against the interpreter, produces **identical
divergence sets with the feature on and off** — 81 reports each, matching line
for line after normalising report numbering. (This title has pre-existing
baseline divergences in `OSDisableInterrupts`/`OSRestoreInterrupts` on
srr0/srr1/fpscr, so the gate is a baseline diff, not a zero count.)

**What is not established:** the scene used reports no self-modifying code, so
the case where a flushed range *does* contain compiled code is covered by the
argument above and by the range test, not by an end-to-end measurement. A title
with genuine SMC over large flushed ranges would be a better test and has not
been run.

## Reproducing

The runtime instrumentation used to find this is behind
`MODERNGEKKO_DCF_STATS=1`, which reports the flush-size histogram and the
hit/miss counts every ~2 seconds. The change itself is behind
`MODERNGEKKO_JITCACHE_RANGE_SKIP` (default on; `=0` restores the previous
behaviour), so on and off are one variable on one binary.
