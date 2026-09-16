// Exhaustive + randomised check of ValidBlockBitSet::TestRange against a naive
// per-bit reference. The early-out in InvalidateICacheInternal is only correct
// if TestRange never returns false while a bit in [first,last] is set, so the
// boundary masks are the whole risk and the benchmark scene never exercises
// them (it has no JIT blocks in any flushed range at all).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using u32 = uint32_t;

struct BitSet
{
  std::vector<u32> m_valid_block;
  explicit BitSet(u32 words) : m_valid_block(words, 0) {}
  void Set(u32 bit) { m_valid_block[bit / 32] |= 1u << (bit % 32); }
  void Clear(u32 bit) { m_valid_block[bit / 32] &= ~(1u << (bit % 32)); }
  bool Test(u32 bit) const { return (m_valid_block[bit / 32] & (1u << (bit % 32))) != 0; }

  // The implementation under test, copied verbatim from the patch.
  bool TestRange(u32 first, u32 last) const
  {
    if (first > last)
      return false;
    const u32 first_word = first / 32;
    const u32 last_word = last / 32;
    if (first_word == last_word)
    {
      const u32 mask = (0xFFFFFFFFu << (first % 32)) &
                       (0xFFFFFFFFu >> (31u - (last % 32)));
      return (m_valid_block[first_word] & mask) != 0;
    }
    if ((m_valid_block[first_word] & (0xFFFFFFFFu << (first % 32))) != 0)
      return true;
    for (u32 w = first_word + 1; w < last_word; ++w)
    {
      if (m_valid_block[w] != 0)
        return true;
    }
    return (m_valid_block[last_word] & (0xFFFFFFFFu >> (31u - (last % 32)))) != 0;
  }

  bool Reference(u32 first, u32 last) const
  {
    if (first > last)
      return false;
    for (u32 b = first; b <= last; ++b)
      if (Test(b))
        return true;
    return false;
  }
};

int main()
{
  const u32 WORDS = 8;      // 256 bits: enough to cover every mask boundary case
  const u32 BITS = WORDS * 32;
  long long checked = 0, failures = 0;

  // 1. Exhaustive: every single-bit population against every range.
  for (u32 set_bit = 0; set_bit < BITS; ++set_bit)
  {
    BitSet bs(WORDS);
    bs.Set(set_bit);
    for (u32 first = 0; first < BITS; ++first)
      for (u32 last = first; last < BITS; ++last)
      {
        ++checked;
        if (bs.TestRange(first, last) != bs.Reference(first, last))
        {
          if (failures < 5)
            std::printf("FAIL single set=%u range=[%u,%u] got=%d want=%d\n", set_bit, first, last,
                        (int)bs.TestRange(first, last), (int)bs.Reference(first, last));
          ++failures;
        }
      }
  }

  // 2. Empty bitset: every range must be false.
  {
    BitSet bs(WORDS);
    for (u32 first = 0; first < BITS; ++first)
      for (u32 last = first; last < BITS; ++last)
      {
        ++checked;
        if (bs.TestRange(first, last)) { ++failures; }
      }
  }

  // 3. Randomised populations, including dense ones.
  std::mt19937 rng(12345);
  for (int trial = 0; trial < 20000; ++trial)
  {
    BitSet bs(WORDS);
    const int n = rng() % 12;
    for (int i = 0; i < n; ++i)
      bs.Set(rng() % BITS);
    const u32 first = rng() % BITS;
    const u32 last = first + rng() % (BITS - first);
    ++checked;
    if (bs.TestRange(first, last) != bs.Reference(first, last))
    {
      if (failures < 5)
        std::printf("FAIL random range=[%u,%u]\n", first, last);
      ++failures;
    }
  }

  std::printf("checked %lld cases, %lld failures\n", checked, failures);
  return failures != 0;
}
