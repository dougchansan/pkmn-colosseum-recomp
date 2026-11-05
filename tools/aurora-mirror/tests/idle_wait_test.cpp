#include "idle_wait.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

void check(bool condition) { if (!condition) throw std::runtime_error("idle wait regression"); }
int main() {
  std::vector<uint8_t> ram(0x1800000);
  const auto write = [&](uint32_t address, uint32_t value) {
    auto* p = ram.data() + (address & 0x1ffffff);
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (24 - 8*i));
  };
  write(0x800FEB98, 0x4BF9F3CD); write(0x800FEB9C, 0x4BFFFEF4);
  write(0x8047AC98, 0x80410000);
  IdleWaitState s{0x800FEB9C, 0x8000, 0x80480820, 0, 0,
                  100, 1000, 800, 0, 1000, 0, false, false, false};
  const auto wait = [&] { return background_idle_cycles(ram.data(), ram.size(), s); };
  check(wait() == 700); s.card_due = 500; check(wait() == 400);
  s.decrementer = 9; s.tick_remainder = 3; check(wait() == 117);
  s.decrementer = 1000; s.tick_remainder = 0; s.card_due = 0;
  write(0x80410008, 2); check(wait() == 0);  // runnable background callback
  ram[0x41000D] = 1; check(wait() == 700); // blocked background callback
  write(0x8047AC94, 0x80410000); check(wait() == 0); write(0x8047AC94, 0);
  write(0x8047AC9C, 0x80410000); check(wait() == 0); write(0x8047AC9C, 0);
  s.external_pending = true; check(wait() == 0); s.external_pending = false;
  s.decrementer_pending = true; check(wait() == 0); s.decrementer_pending = false;
  s.dsp_mail_pending = true; check(wait() == 0); s.dsp_mail_pending = false;
  s.msr = 0; check(wait() == 0); s.msr = 0x8000;
  s.ai_due = 1; check(wait() == 0); s.ai_due = 800;
  s.card_due = 100; check(wait() == 0); s.card_due = 0;
  write(0x80410004, 0x80410000); check(wait() == 0); // cyclic list
  write(0x80410004, 0xDEADBEEF); check(wait() == 0); // invalid pointer
  write(0x80410004, 0); s.r3 = 0x80420000; check(wait() == 0); s.r3 = 0;
  write(0x800FEB9C, 0); check(wait() == 0); // different guest code: no optimization
  write(0x800FEB9C, 0x4BFFFEF4); check(wait() == 700);
  s.r0 = 1; check(wait() == 0); s.r0 = 0;
  s.pc = 0x800FEAEC; check(wait() == 0); s.pc = 0x800FEB9C;
  write(0x80410008, 2); ram[0x41000D] = 0; check(wait() == 0);
  std::puts("idle eligibility, list validation and device deadlines passed");
}
