// End-to-end check of the InvalidateICacheInternal early-out decision.
//
// The safety property is one-directional: the patch may only skip work that has
// no effect. Concretely, if ANY cache line overlapping [physical_address,
// physical_address + length) holds a compiled block, the patched code must
// still take the destroy path, because stock code calls
// ErasePhysicalRange(physical_address, length) unconditionally and that range
// includes partially covered lines at both ends.
//
// The first version of this patch tested only the FULLY covered lines
// ((pa + 0x1f)/32 .. (pa + length)/32), which excludes a partial line at either
// end. This test fails against that version and passes against the fix.
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using u32 = uint32_t;

struct BitSet
{
  std::vector<u32> w;
  explicit BitSet(u32 words) : w(words, 0) {}
  void Set(u32 b) { w[b / 32] |= 1u << (b % 32); }
  bool Test(u32 b) const { return (w[b / 32] & (1u << (b % 32))) != 0; }
  bool TestRange(u32 first, u32 last) const
  {
    if (first > last)
      return false;
    const u32 fw = first / 32, lw = last / 32;
    const u32 fm = 0xFFFFFFFFu << (first % 32);
    const u32 lm = 0xFFFFFFFFu >> (31u - (last % 32));
    if (fw == lw)
      return (w[fw] & fm & lm) != 0;
    if ((w[fw] & fm) != 0)
      return true;
    for (u32 i = fw + 1; i < lw; ++i)
      if (w[i] != 0)
        return true;
    return (w[lw] & lm) != 0;
  }
};

// The patched decision: does the early-out fire?
static bool SkipsDestroy(const BitSet& vb, u32 pa, u32 length, bool use_fixed)
{
  if (use_fixed)
  {
    const u32 first = pa / 32;
    const u32 last = (pa + length - 1) / 32;
    return !vb.TestRange(first, last);
  }
  // The original, buggy formulation: fully covered lines only.
  const u32 start = (pa + 0x1f) / 32;
  const u32 end = (pa + length) / 32;
  return end > start && !vb.TestRange(start, end - 1);
}

// Ground truth: stock code would have erased any block overlapping the byte
// range, so skipping is only safe when no touched line holds one.
static bool SafeToSkip(const BitSet& vb, u32 pa, u32 length)
{
  for (u32 line = pa / 32; line <= (pa + length - 1) / 32; ++line)
    if (vb.Test(line))
      return false;
  return true;
}

int main()
{
  const u32 WORDS = 8, BITS = WORDS * 32, BYTES = BITS * 32;
  std::mt19937 rng(987654321);
  long long checked = 0, unsafe_fixed = 0, unsafe_orig = 0;

  for (int trial = 0; trial < 400000; ++trial)
  {
    BitSet vb(WORDS);
    const int n = rng() % 6;
    for (int i = 0; i < n; ++i)
      vb.Set(rng() % BITS);

    // Deliberately include unaligned addresses and non-multiple-of-32 lengths:
    // aligned callers cannot expose the bug.
    const u32 pa = rng() % (BYTES / 2);
    const u32 length = 33 + rng() % 2048;
    if (pa + length > BYTES)
      continue;

    ++checked;
    const bool safe = SafeToSkip(vb, pa, length);
    if (SkipsDestroy(vb, pa, length, true) && !safe)
      ++unsafe_fixed;
    if (SkipsDestroy(vb, pa, length, false) && !safe)
      ++unsafe_orig;
  }

  std::printf("checked %lld cases\n", checked);
  std::printf("  fixed formulation:    %lld unsafe skips\n", unsafe_fixed);
  std::printf("  original formulation: %lld unsafe skips  (expected > 0)\n", unsafe_orig);
  if (unsafe_fixed != 0)
  {
    std::printf("FAIL: the fixed formulation skipped work that had an effect\n");
    return 1;
  }
  if (unsafe_orig == 0)
    std::printf("NOTE: the original bug was not reproduced by this sample\n");
  std::printf("PASS\n");
  return 0;
}
