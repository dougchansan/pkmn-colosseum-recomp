#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

struct IdleWaitState {
  uint32_t pc, msr, r13, r3, r0;
  uint64_t cycles, vi_due, ai_due, card_due;
  uint32_t decrementer, tick_remainder;
  bool external_pending, decrementer_pending, dsp_mail_pending;
};

// GC6E01's OS idle callback scans background tasks between interrupts. Wait
// only when the complete list proves that none can run. PC/registers stay at
// the return from OSRestoreInterrupts, before the branch back to the scan.
// The ordinary host device tick handles the
// first scheduled event after the wait.
inline uint64_t background_idle_cycles(const uint8_t* ram, size_t size, const IdleWaitState& s) {
  if (!ram || s.pc != 0x800FEB9C || s.r13 != 0x80480820 || !(s.msr & 0x8000) || s.r3 || s.r0 ||
      s.external_pending || s.decrementer_pending || s.dsp_mail_pending || s.tick_remainder >= 12)
    return 0;
  const auto pointer = [&](uint32_t address, size_t bytes) -> const uint8_t* {
    const uint32_t offset = (address & ~0x40000000u) - 0x80000000u;
    return offset <= size && bytes <= size - offset ? ram + offset : nullptr;
  };
  const auto read32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const uint8_t* code = pointer(s.pc - 4, 8);
  const uint8_t* globals = pointer(0x8047AC94, 12);
  if (!code || !globals || read32(code) != 0x4BF9F3CDu || read32(code + 4) != 0x4BFFFEF4u ||
      read32(globals) || read32(globals + 8))
    return 0;
  uint32_t node = read32(globals + 4);
  unsigned visited = 0;
  while (node && visited++ < 128) {
    const uint8_t* task = pointer(node, 0x18);
    if (!task || (node & 3) || (read32(task + 8) == 2 && task[0xD] == 0))
      return 0;
    node = read32(task + 4);
  }
  if (node || s.vi_due <= s.cycles)
    return 0;  // invalid/cyclic list, or a device event already due
  uint64_t deadline = s.vi_due;
  for (uint64_t due : {s.ai_due, s.card_due}) {
    if (due && due <= s.cycles)
      return 0;
    if (due)
      deadline = std::min(deadline, due);
  }
  if (int32_t(s.decrementer) >= 0) {
    const uint64_t delta = (uint64_t(s.decrementer) + 1) * 12 - s.tick_remainder;
    if (delta < deadline - s.cycles)
      deadline = s.cycles + delta;
  }
  return deadline - s.cycles;
}
