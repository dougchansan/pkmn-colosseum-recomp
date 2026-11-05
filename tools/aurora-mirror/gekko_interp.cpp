// A Gekko (PowerPC 750CL) interpreter for colosseum-standalone.
//
// It runs guest code the recompiled module does not contain: the exception
// stubs the OS copies into low memory, and the RELs the game loads into the
// heap (every map's script module), which ModernGekko left to Dolphin's JIT.
// Floating-point, paired-single and quantized instructions go through the same
// GXRuntime helpers the generated code calls, so both execute alike.

extern "C" {
#include "core/cpu.h"
}

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kCA = 0x20000000, kOV = 0x40000000, kSO = 0x80000000;

uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
double from_bits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

uint32_t rotl(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

uint32_t mask(uint32_t mb, uint32_t me) {
  const uint32_t begin = 0xFFFFFFFFu >> mb;
  const uint32_t end = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
  return mb <= me ? begin & end : begin | end;
}

void set_cr(CPUState* c, int field, uint32_t value) {
  const int shift = 28 - 4 * field;
  c->cr = (c->cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
}

void record(CPUState* c, uint32_t v) {
  const int32_t s = int32_t(v);
  set_cr(c, 0, (s < 0 ? 8u : s > 0 ? 4u : 2u) | (c->xer >> 31));
}

void record_fp(CPUState* c) { set_cr(c, 1, c->fpscr >> 28); }

void set_ca(CPUState* c, bool ca) { c->xer = ca ? (c->xer | kCA) : (c->xer & ~kCA); }
bool ca(CPUState* c) { return (c->xer & kCA) != 0; }

void set_ov(CPUState* c, bool ov) {
  c->xer = ov ? (c->xer | kOV | kSO) : (c->xer & ~kOV);
}

bool cond(CPUState* c, uint32_t bo, uint32_t bi) {
  if (!(bo & 4))
    --c->ctr;
  const bool ctr_ok = (bo & 4) || ((c->ctr != 0) != ((bo & 2) != 0));
  const bool cond_ok = (bo & 16) || ((((c->cr >> (31 - bi)) & 1) != 0) == ((bo & 8) != 0));
  return ctr_ok && cond_ok;
}

double fsel(double a, double b, double c) {
  return (a >= -0.0) ? c : b;  // NaN compares false: b
}

uint32_t spr_of(uint32_t raw) { return ((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5); }

}  // namespace

// Executes the instruction at cpu->pc. Returns false (pc unchanged) if it is
// not an instruction this knows.
bool gekko_interpret(CPUState* c) {
  const uint32_t cia = c->pc;
  const uint32_t raw = mem_read32(c, cia);
  const uint32_t op = raw >> 26;
  const uint32_t d = (raw >> 21) & 31, a = (raw >> 16) & 31, b = (raw >> 11) & 31, cc = (raw >> 6) & 31;
  const bool rc = raw & 1;
  const int32_t simm = int16_t(raw & 0xFFFF);
  const uint32_t uimm = raw & 0xFFFF;
  uint32_t* r = c->gpr;
  const uint32_t ra0 = a ? r[a] : 0;
  c->pc = cia + 4;

  switch (op) {
  case 3:  // twi
    if (ppc_trap_condition(uint8_t(d), r[a], uint32_t(simm)))
      ppc_program_exception(c, 0x00020000, cia);
    return true;
  case 4: {  // paired singles
    const uint32_t xo10 = (raw >> 1) & 0x3FF, xo5 = (raw >> 1) & 0x1F, xo6 = (raw >> 1) & 0x3F;
    switch (xo6) {
    case 6: case 38: {  // psq_lx, psq_lux / psq_stx, psq_stux (xo6 7/39)
      const bool w = (raw >> 10) & 1;
      const uint8_t gqr = uint8_t((raw >> 7) & 7);
      const uint32_t ea = (xo6 == 38 ? r[a] : ra0) + r[b];
      if (ppc_psq_load(c, uint8_t(d), ea, w, gqr, true, cia) && xo6 == 38)
        r[a] = ea;
      return true;
    }
    case 7: case 39: {
      const bool w = (raw >> 10) & 1;
      const uint8_t gqr = uint8_t((raw >> 7) & 7);
      const uint32_t ea = (xo6 == 39 ? r[a] : ra0) + r[b];
      if (ppc_psq_store(c, uint8_t(d), ea, w, gqr, true, cia) && xo6 == 39)
        r[a] = ea;
      return true;
    }
    }
    switch (xo5) {
    case 10: ppc_ps_sum0(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b)); break;
    case 11: ppc_ps_sum1(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b)); break;
    case 12: ppc_ps_muls0(c, uint8_t(d), uint8_t(a), uint8_t(cc)); break;
    case 13: ppc_ps_muls1(c, uint8_t(d), uint8_t(a), uint8_t(cc)); break;
    case 14: ppc_ps_madds0(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b)); break;
    case 15: ppc_ps_madds1(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b)); break;
    case 18: ppc_ps_div_op(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 20: ppc_ps_sub_op(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 21: ppc_ps_add_op(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 23:  // ps_sel
      c->fpr[d] = fsel(c->fpr[a], c->fpr[b], c->fpr[cc]);
      c->ps1[d] = fsel(c->ps1[a], c->ps1[b], c->ps1[cc]);
      break;
    case 24: ppc_ps_res_op(c, uint8_t(d), uint8_t(b)); break;
    case 25: ppc_ps_mul_op(c, uint8_t(d), uint8_t(a), uint8_t(cc)); break;
    case 26: ppc_ps_rsqrte_op(c, uint8_t(d), uint8_t(b)); break;
    case 28: ppc_ps_madd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, false); break;   // ps_msub
    case 29: ppc_ps_madd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, false); break;  // ps_madd
    case 30: ppc_ps_madd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, true); break;    // ps_nmsub
    case 31: ppc_ps_madd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, true); break;   // ps_nmadd
    default:
      switch (xo10) {
      case 0: ppc_fcmp(c, uint8_t(d >> 2), c->fpr[a], c->fpr[b], false); return true;   // ps_cmpu0
      case 32: ppc_fcmp(c, uint8_t(d >> 2), c->fpr[a], c->fpr[b], true); return true;   // ps_cmpo0
      case 64: ppc_fcmp(c, uint8_t(d >> 2), c->ps1[a], c->ps1[b], false); return true;  // ps_cmpu1
      case 96: ppc_fcmp(c, uint8_t(d >> 2), c->ps1[a], c->ps1[b], true); return true;   // ps_cmpo1
      case 40:  // ps_neg
        c->fpr[d] = from_bits(bits(c->fpr[b]) ^ (1ull << 63));
        c->ps1[d] = from_bits(bits(c->ps1[b]) ^ (1ull << 63));
        break;
      case 72: c->fpr[d] = c->fpr[b]; c->ps1[d] = c->ps1[b]; break;  // ps_mr
      case 136:  // ps_nabs
        c->fpr[d] = from_bits(bits(c->fpr[b]) | (1ull << 63));
        c->ps1[d] = from_bits(bits(c->ps1[b]) | (1ull << 63));
        break;
      case 264:  // ps_abs
        c->fpr[d] = from_bits(bits(c->fpr[b]) & ~(1ull << 63));
        c->ps1[d] = from_bits(bits(c->ps1[b]) & ~(1ull << 63));
        break;
      case 528: { const double x = c->fpr[a], y = c->fpr[b]; c->fpr[d] = x; c->ps1[d] = y; break; }  // ps_merge00
      case 560: { const double x = c->fpr[a], y = c->ps1[b]; c->fpr[d] = x; c->ps1[d] = y; break; }  // ps_merge01
      case 592: { const double x = c->ps1[a], y = c->fpr[b]; c->fpr[d] = x; c->ps1[d] = y; break; }  // ps_merge10
      case 624: { const double x = c->ps1[a], y = c->ps1[b]; c->fpr[d] = x; c->ps1[d] = y; break; }  // ps_merge11
      case 1014: ppc_dcbz_l(c, ra0 + r[b], cia); return true;  // dcbz_l
      default:
        c->pc = cia;
        return false;
      }
    }
    if (rc) record_fp(c);
    return true;
  }
  case 7: r[d] = uint32_t(int32_t(r[a]) * simm); return true;  // mulli
  case 8: {  // subfic
    const uint64_t v = uint64_t(~r[a]) + uint64_t(uint32_t(simm)) + 1;
    r[d] = uint32_t(v);
    set_ca(c, v >> 32);
    return true;
  }
  case 10: {  // cmpli
    const uint32_t x = r[a];
    set_cr(c, int(d >> 2), (x < uimm ? 8u : x > uimm ? 4u : 2u) | (c->xer >> 31));
    return true;
  }
  case 11: {  // cmpi
    const int32_t x = int32_t(r[a]);
    set_cr(c, int(d >> 2), (x < simm ? 8u : x > simm ? 4u : 2u) | (c->xer >> 31));
    return true;
  }
  case 12: case 13: {  // addic, addic.
    const uint64_t v = uint64_t(r[a]) + uint32_t(simm);
    r[d] = uint32_t(v);
    set_ca(c, v >> 32);
    if (op == 13) record(c, r[d]);
    return true;
  }
  case 14: r[d] = ra0 + uint32_t(simm); return true;          // addi
  case 15: r[d] = ra0 + (uimm << 16); return true;            // addis
  case 16: {  // bc
    const int32_t bd = int16_t(raw & 0xFFFC);
    if (raw & 1) c->lr = cia + 4;
    if (cond(c, d, a))
      c->pc = ((raw & 2) ? 0 : cia) + uint32_t(bd);
    return true;
  }
  case 17:  // sc
    ppc_system_call_exception(c, cia);
    return true;
  case 18: {  // b
    int32_t li = int32_t(raw & 0x03FFFFFC);
    if (li & 0x02000000) li |= int32_t(0xFC000000);
    if (raw & 1) c->lr = cia + 4;
    c->pc = ((raw & 2) ? 0 : cia) + uint32_t(li);
    return true;
  }
  case 19: {
    const uint32_t xo = (raw >> 1) & 0x3FF;
    switch (xo) {
    case 0: set_cr(c, int(d >> 2), c->cr >> (28 - 4 * (a >> 2))); return true;  // mcrf
    case 16: {  // bclr
      const uint32_t target = c->lr & ~3u;
      const bool taken = cond(c, d, a);
      if (raw & 1) c->lr = cia + 4;
      if (taken) c->pc = target;
      return true;
    }
    case 528: {  // bcctr
      const bool taken = (d & 16) || ((((c->cr >> (31 - a)) & 1) != 0) == ((d & 8) != 0));
      const uint32_t target = c->ctr & ~3u;
      if (raw & 1) c->lr = cia + 4;
      if (taken) c->pc = target;
      return true;
    }
    case 50: ppc_rfi(c, cia); return true;  // rfi
    case 150: return true;                   // isync
    case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
      const uint32_t x = (c->cr >> (31 - a)) & 1, y = (c->cr >> (31 - b)) & 1;
      uint32_t v = 0;
      switch (xo) {
      case 33: v = !(x | y); break;   // crnor
      case 129: v = x & !y; break;    // crandc
      case 193: v = x ^ y; break;     // crxor
      case 225: v = !(x & y); break;  // crnand
      case 257: v = x & y; break;     // crand
      case 289: v = !(x ^ y); break;  // creqv
      case 417: v = x | !y; break;    // crorc
      case 449: v = x | y; break;     // cror
      }
      c->cr = (c->cr & ~(1u << (31 - d))) | (v << (31 - d));
      return true;
    }
    }
    c->pc = cia;
    return false;
  }
  case 20: {  // rlwimi
    const uint32_t m = mask(cc, (raw >> 1) & 31);
    r[a] = (rotl(r[d], b) & m) | (r[a] & ~m);
    if (rc) record(c, r[a]);
    return true;
  }
  case 21:  // rlwinm
    r[a] = rotl(r[d], b) & mask(cc, (raw >> 1) & 31);
    if (rc) record(c, r[a]);
    return true;
  case 23:  // rlwnm
    r[a] = rotl(r[d], r[b]) & mask(cc, (raw >> 1) & 31);
    if (rc) record(c, r[a]);
    return true;
  case 24: r[a] = r[d] | uimm; return true;                   // ori
  case 25: r[a] = r[d] | (uimm << 16); return true;           // oris
  case 26: r[a] = r[d] ^ uimm; return true;                   // xori
  case 27: r[a] = r[d] ^ (uimm << 16); return true;           // xoris
  case 28: r[a] = r[d] & uimm; record(c, r[a]); return true;  // andi.
  case 29: r[a] = r[d] & (uimm << 16); record(c, r[a]); return true;  // andis.
  case 31: {
    const uint32_t xo = (raw >> 1) & 0x3FF;
    const bool oe = (raw >> 10) & 1;
    const uint32_t xo9 = (raw >> 1) & 0x1FF;
    // Arithmetic with OE (9-bit extended opcode).
    switch (xo9) {
    case 10: {  // addc
      const uint64_t v = uint64_t(r[a]) + r[b];
      if (oe) set_ov(c, ppc_add_overflowed(r[a], r[b], uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 138: {  // adde
      const uint64_t v = uint64_t(r[a]) + r[b] + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(r[a], r[b], uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 202: {  // addze
      const uint64_t v = uint64_t(r[a]) + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(r[a], 0, uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 234: {  // addme
      const uint64_t v = uint64_t(r[a]) + 0xFFFFFFFFull + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(r[a], 0xFFFFFFFF, uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 266: {  // add
      const uint32_t v = r[a] + r[b];
      if (oe) set_ov(c, ppc_add_overflowed(r[a], r[b], v));
      r[d] = v;
      if (rc) record(c, v);
      return true;
    }
    case 8: {  // subfc
      const uint64_t v = uint64_t(~r[a]) + r[b] + 1;
      if (oe) set_ov(c, ppc_add_overflowed(~r[a], r[b], uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 40: {  // subf
      const uint32_t v = r[b] - r[a];
      if (oe) set_ov(c, (((r[b] ^ r[a]) & (r[b] ^ v)) >> 31) != 0);
      r[d] = v;
      if (rc) record(c, v);
      return true;
    }
    case 136: {  // subfe
      const uint64_t v = uint64_t(~r[a]) + r[b] + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(~r[a], r[b], uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 200: {  // subfze
      const uint64_t v = uint64_t(~r[a]) + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(~r[a], 0, uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 232: {  // subfme
      const uint64_t v = uint64_t(~r[a]) + 0xFFFFFFFFull + (ca(c) ? 1 : 0);
      if (oe) set_ov(c, ppc_add_overflowed(~r[a], 0xFFFFFFFF, uint32_t(v)));
      r[d] = uint32_t(v); set_ca(c, v >> 32);
      if (rc) record(c, r[d]);
      return true;
    }
    case 104: {  // neg
      const uint32_t v = uint32_t(-int64_t(int32_t(r[a])));
      if (oe) set_ov(c, r[a] == 0x80000000u);
      r[d] = v;
      if (rc) record(c, v);
      return true;
    }
    case 235: {  // mullw
      const int64_t v = int64_t(int32_t(r[a])) * int32_t(r[b]);
      if (oe) set_ov(c, v < INT32_MIN || v > INT32_MAX);
      r[d] = uint32_t(v);
      if (rc) record(c, r[d]);
      return true;
    }
    case 75: r[d] = uint32_t((int64_t(int32_t(r[a])) * int32_t(r[b])) >> 32); if (rc) record(c, r[d]); return true;  // mulhw
    case 11: r[d] = uint32_t((uint64_t(r[a]) * r[b]) >> 32); if (rc) record(c, r[d]); return true;                   // mulhwu
    case 491: {  // divw
      const int32_t x = int32_t(r[a]), y = int32_t(r[b]);
      const bool bad = y == 0 || (x == INT32_MIN && y == -1);
      if (oe) set_ov(c, bad);
      r[d] = bad ? ((x < 0 && y == 0) || (x == INT32_MIN && y == -1) ? 0xFFFFFFFFu : 0) : uint32_t(x / y);
      if (rc) record(c, r[d]);
      return true;
    }
    case 459: {  // divwu
      if (oe) set_ov(c, r[b] == 0);
      r[d] = r[b] ? r[a] / r[b] : 0;
      if (rc) record(c, r[d]);
      return true;
    }
    }
    switch (xo) {
    case 0: {  // cmp
      const int32_t x = int32_t(r[a]), y = int32_t(r[b]);
      set_cr(c, int(d >> 2), (x < y ? 8u : x > y ? 4u : 2u) | (c->xer >> 31));
      return true;
    }
    case 32: {  // cmpl
      const uint32_t x = r[a], y = r[b];
      set_cr(c, int(d >> 2), (x < y ? 8u : x > y ? 4u : 2u) | (c->xer >> 31));
      return true;
    }
    case 4:  // tw
      if (ppc_trap_condition(uint8_t(d), r[a], r[b]))
        ppc_program_exception(c, 0x00020000, cia);
      return true;
    case 19: r[d] = c->cr; return true;              // mfcr
    case 83: r[d] = c->msr; return true;             // mfmsr
    case 144: {                                       // mtcrf
      const uint32_t crm = (raw >> 12) & 0xFF;
      uint32_t m = 0;
      for (int i = 0; i < 8; ++i)
        if (crm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
      c->cr = (c->cr & ~m) | (r[d] & m);
      return true;
    }
    case 146: c->msr = r[d]; return true;            // mtmsr
    case 210: c->sr[a & 15] = r[d]; return true;     // mtsr
    case 242: c->sr[(r[b] >> 28) & 15] = r[d]; return true;  // mtsrin
    case 595: r[d] = c->sr[a & 15]; return true;     // mfsr
    case 659: r[d] = c->sr[(r[b] >> 28) & 15]; return true;  // mfsrin
    case 339: r[d] = ppc_mfspr(c, uint16_t(spr_of(raw)), cia); return true;   // mfspr
    case 467: ppc_mtspr(c, uint16_t(spr_of(raw)), r[d], cia); return true;    // mtspr
    case 371: r[d] = ppc_mftb(c, uint16_t(spr_of(raw)), cia); return true;    // mftb
    case 512:  // mcrxr
      set_cr(c, int(d >> 2), c->xer >> 28);
      c->xer &= 0x0FFFFFFF;
      return true;
    case 28: r[a] = r[d] & r[b]; if (rc) record(c, r[a]); return true;     // and
    case 60: r[a] = r[d] & ~r[b]; if (rc) record(c, r[a]); return true;    // andc
    case 124: r[a] = ~(r[d] | r[b]); if (rc) record(c, r[a]); return true; // nor
    case 284: r[a] = ~(r[d] ^ r[b]); if (rc) record(c, r[a]); return true; // eqv
    case 316: r[a] = r[d] ^ r[b]; if (rc) record(c, r[a]); return true;    // xor
    case 412: r[a] = r[d] | ~r[b]; if (rc) record(c, r[a]); return true;   // orc
    case 444: r[a] = r[d] | r[b]; if (rc) record(c, r[a]); return true;    // or
    case 476: r[a] = ~(r[d] & r[b]); if (rc) record(c, r[a]); return true; // nand
    case 24: r[a] = (r[b] & 32) ? 0 : r[d] << (r[b] & 31); if (rc) record(c, r[a]); return true;   // slw
    case 536: r[a] = (r[b] & 32) ? 0 : r[d] >> (r[b] & 31); if (rc) record(c, r[a]); return true;  // srw
    case 792: {  // sraw
      const uint32_t n = r[b] & 63;
      const int32_t s = int32_t(r[d]);
      if (n >= 32) {
        r[a] = s < 0 ? 0xFFFFFFFFu : 0;
        set_ca(c, s < 0);
      } else {
        r[a] = uint32_t(s >> n);
        set_ca(c, s < 0 && (uint32_t(s) & ((1u << n) - 1)));
      }
      if (rc) record(c, r[a]);
      return true;
    }
    case 824: {  // srawi
      const int32_t s = int32_t(r[d]);
      r[a] = uint32_t(s >> b);
      set_ca(c, s < 0 && b && (uint32_t(s) & ((1u << b) - 1)));
      if (rc) record(c, r[a]);
      return true;
    }
    case 26: {  // cntlzw
      uint32_t v = r[d], n = 0;
      while (n < 32 && !(v & 0x80000000u)) { v <<= 1; ++n; }
      r[a] = n;
      if (rc) record(c, n);
      return true;
    }
    case 954: r[a] = uint32_t(int32_t(int8_t(r[d]))); if (rc) record(c, r[a]); return true;   // extsb
    case 922: r[a] = uint32_t(int32_t(int16_t(r[d]))); if (rc) record(c, r[a]); return true;  // extsh
    // Loads and stores, indexed.
    case 23: r[d] = mem_read32(c, ra0 + r[b]); return true;                    // lwzx
    case 55: { const uint32_t ea = r[a] + r[b]; r[d] = mem_read32(c, ea); r[a] = ea; return true; }  // lwzux
    case 87: r[d] = mem_read8(c, ra0 + r[b]); return true;                     // lbzx
    case 119: { const uint32_t ea = r[a] + r[b]; r[d] = mem_read8(c, ea); r[a] = ea; return true; }  // lbzux
    case 279: r[d] = mem_read16(c, ra0 + r[b]); return true;                   // lhzx
    case 311: { const uint32_t ea = r[a] + r[b]; r[d] = mem_read16(c, ea); r[a] = ea; return true; } // lhzux
    case 343: r[d] = uint32_t(int32_t(int16_t(mem_read16(c, ra0 + r[b])))); return true;  // lhax
    case 375: { const uint32_t ea = r[a] + r[b]; r[d] = uint32_t(int32_t(int16_t(mem_read16(c, ea)))); r[a] = ea; return true; }  // lhaux
    case 151: mem_write32(c, ra0 + r[b], r[d]); return true;                   // stwx
    case 183: { const uint32_t ea = r[a] + r[b]; mem_write32(c, ea, r[d]); r[a] = ea; return true; }  // stwux
    case 215: mem_write8(c, ra0 + r[b], uint8_t(r[d])); return true;           // stbx
    case 247: { const uint32_t ea = r[a] + r[b]; mem_write8(c, ea, uint8_t(r[d])); r[a] = ea; return true; }  // stbux
    case 407: mem_write16(c, ra0 + r[b], uint16_t(r[d])); return true;         // sthx
    case 439: { const uint32_t ea = r[a] + r[b]; mem_write16(c, ea, uint16_t(r[d])); r[a] = ea; return true; } // sthux
    case 534: { const uint32_t v = mem_read32(c, ra0 + r[b]); r[d] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); return true; }  // lwbrx
    case 790: { const uint16_t v = mem_read16(c, ra0 + r[b]); r[d] = uint16_t((v >> 8) | (v << 8)); return true; }  // lhbrx
    case 662: { const uint32_t v = r[d]; mem_write32(c, ra0 + r[b], (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24)); return true; }  // stwbrx
    case 918: { const uint16_t v = uint16_t(r[d]); mem_write16(c, ra0 + r[b], uint16_t((v >> 8) | (v << 8))); return true; }  // sthbrx
    case 20: ppc_lwarx_op(c, uint8_t(d), ra0 + r[b], cia); return true;        // lwarx
    case 150: ppc_stwcx_op(c, uint8_t(d), ra0 + r[b], cia); return true;       // stwcx.
    case 533: ppc_lswx(c, uint8_t(d), uint8_t(a), uint8_t(b), cia); return true;  // lswx
    case 597: {  // lswi
      uint32_t n = b ? b : 32, ea = ra0, reg = d;
      for (uint32_t i = 0; i < n; ++i) {
        if (i % 4 == 0) { reg = (d + i / 4) & 31; r[reg] = 0; }
        r[reg] |= uint32_t(mem_read8(c, ea + i)) << (24 - 8 * (i % 4));
      }
      return true;
    }
    case 661: ppc_stsw(c, ra0 + r[b], c->xer & 0x7F, uint8_t(d), cia); return true;  // stswx
    case 725: ppc_stsw(c, ra0, b ? b : 32, uint8_t(d), cia); return true;           // stswi
    case 535: if (ppc_lfs_op(c, uint8_t(d), ra0 + r[b], cia)) {} return true;          // lfsx
    case 567: { const uint32_t ea = r[a] + r[b]; if (ppc_lfs_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; }  // lfsux
    case 599: ppc_lfd_op(c, uint8_t(d), ra0 + r[b], cia); return true;            // lfdx
    case 631: { const uint32_t ea = r[a] + r[b]; if (ppc_lfd_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; }  // lfdux
    case 663: ppc_stfs_op(c, uint8_t(d), ra0 + r[b], cia); return true;           // stfsx
    case 695: { const uint32_t ea = r[a] + r[b]; if (ppc_stfs_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; } // stfsux
    case 727: ppc_stfd_op(c, uint8_t(d), ra0 + r[b], cia); return true;           // stfdx
    case 759: { const uint32_t ea = r[a] + r[b]; if (ppc_stfd_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; } // stfdux
    case 983: mem_write32(c, ra0 + r[b], uint32_t(bits(c->fpr[d]))); return true; // stfiwx
    case 86: ppc_cache_control(c, PPC_CACHE_DCBF, ra0 + r[b], cia); return true;   // dcbf
    case 54: ppc_cache_control(c, PPC_CACHE_DCBST, ra0 + r[b], cia); return true;  // dcbst
    case 470: ppc_cache_control(c, PPC_CACHE_DCBI, ra0 + r[b], cia); return true;  // dcbi
    case 982: ppc_cache_control(c, PPC_CACHE_ICBI, ra0 + r[b], cia); return true;  // icbi
    case 1014: {  // dcbz
      const uint32_t ea = (ra0 + r[b]) & ~31u;
      for (uint32_t i = 0; i < 32; i += 8) mem_write64(c, ea + i, 0);
      return true;
    }
    case 278: case 246: case 598: case 854: case 306: case 566: return true;  // dcbt dcbtst sync eieio tlbie tlbsync
    case 310: r[d] = ppc_eciwx(c, ra0 + r[b], cia); return true;   // eciwx
    case 438: ppc_ecowx(c, ra0 + r[b], r[d], cia); return true;    // ecowx
    }
    c->pc = cia;
    return false;
  }
  case 32: r[d] = mem_read32(c, ra0 + uint32_t(simm)); return true;                 // lwz
  case 33: { const uint32_t ea = r[a] + uint32_t(simm); r[d] = mem_read32(c, ea); r[a] = ea; return true; }  // lwzu
  case 34: r[d] = mem_read8(c, ra0 + uint32_t(simm)); return true;                  // lbz
  case 35: { const uint32_t ea = r[a] + uint32_t(simm); r[d] = mem_read8(c, ea); r[a] = ea; return true; }   // lbzu
  case 36: mem_write32(c, ra0 + uint32_t(simm), r[d]); return true;                 // stw
  case 37: { const uint32_t ea = r[a] + uint32_t(simm); mem_write32(c, ea, r[d]); r[a] = ea; return true; }  // stwu
  case 38: mem_write8(c, ra0 + uint32_t(simm), uint8_t(r[d])); return true;         // stb
  case 39: { const uint32_t ea = r[a] + uint32_t(simm); mem_write8(c, ea, uint8_t(r[d])); r[a] = ea; return true; }  // stbu
  case 40: r[d] = mem_read16(c, ra0 + uint32_t(simm)); return true;                 // lhz
  case 41: { const uint32_t ea = r[a] + uint32_t(simm); r[d] = mem_read16(c, ea); r[a] = ea; return true; }  // lhzu
  case 42: r[d] = uint32_t(int32_t(int16_t(mem_read16(c, ra0 + uint32_t(simm))))); return true;  // lha
  case 43: { const uint32_t ea = r[a] + uint32_t(simm); r[d] = uint32_t(int32_t(int16_t(mem_read16(c, ea)))); r[a] = ea; return true; }  // lhau
  case 44: mem_write16(c, ra0 + uint32_t(simm), uint16_t(r[d])); return true;       // sth
  case 45: { const uint32_t ea = r[a] + uint32_t(simm); mem_write16(c, ea, uint16_t(r[d])); r[a] = ea; return true; }  // sthu
  case 46: for (uint32_t i = d, ea = ra0 + uint32_t(simm); i < 32; ++i, ea += 4) r[i] = mem_read32(c, ea); return true;  // lmw
  case 47: for (uint32_t i = d, ea = ra0 + uint32_t(simm); i < 32; ++i, ea += 4) mem_write32(c, ea, r[i]); return true;  // stmw
  case 48: ppc_lfs_op(c, uint8_t(d), ra0 + uint32_t(simm), cia); return true;      // lfs
  case 49: { const uint32_t ea = r[a] + uint32_t(simm); if (ppc_lfs_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; }  // lfsu
  case 50: ppc_lfd_op(c, uint8_t(d), ra0 + uint32_t(simm), cia); return true;      // lfd
  case 51: { const uint32_t ea = r[a] + uint32_t(simm); if (ppc_lfd_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; }  // lfdu
  case 52: ppc_stfs_op(c, uint8_t(d), ra0 + uint32_t(simm), cia); return true;     // stfs
  case 53: { const uint32_t ea = r[a] + uint32_t(simm); if (ppc_stfs_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; } // stfsu
  case 54: ppc_stfd_op(c, uint8_t(d), ra0 + uint32_t(simm), cia); return true;     // stfd
  case 55: { const uint32_t ea = r[a] + uint32_t(simm); if (ppc_stfd_op(c, uint8_t(d), ea, cia)) r[a] = ea; return true; } // stfdu
  case 56: case 57: {  // psq_l, psq_lu
    const int32_t off = (int32_t(raw << 20)) >> 20;
    const bool w = (raw >> 15) & 1;
    const uint8_t gqr = uint8_t((raw >> 12) & 7);
    const uint32_t ea = (op == 57 ? r[a] : ra0) + uint32_t(off);
    if (ppc_psq_load(c, uint8_t(d), ea, w, gqr, false, cia) && op == 57)
      r[a] = ea;
    return true;
  }
  case 60: case 61: {  // psq_st, psq_stu
    const int32_t off = (int32_t(raw << 20)) >> 20;
    const bool w = (raw >> 15) & 1;
    const uint8_t gqr = uint8_t((raw >> 12) & 7);
    const uint32_t ea = (op == 61 ? r[a] : ra0) + uint32_t(off);
    if (ppc_psq_store(c, uint8_t(d), ea, w, gqr, false, cia) && op == 61)
      r[a] = ea;
    return true;
  }
  case 59: {  // single-precision arithmetic
    switch ((raw >> 1) & 0x1F) {
    case 18: ppc_fdivs(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 20: ppc_fsubs(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 21: ppc_fadds(c, uint8_t(d), uint8_t(a), uint8_t(b)); break;
    case 24: ppc_fres_op(c, uint8_t(d), uint8_t(b)); break;
    case 25: ppc_fmuls(c, uint8_t(d), uint8_t(a), uint8_t(cc)); break;
    case 28: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, true, false); break;   // fmsubs
    case 29: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, false, false); break;  // fmadds
    case 30: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, true, true); break;    // fnmsubs
    case 31: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), true, false, true); break;   // fnmadds
    default: c->pc = cia; return false;
    }
    if (rc) record_fp(c);
    return true;
  }
  case 63: {  // double-precision and FPSCR
    const uint32_t xo5 = (raw >> 1) & 0x1F;
    switch (xo5) {
    case 18: ppc_fdiv(c, uint8_t(d), uint8_t(a), uint8_t(b)); if (rc) record_fp(c); return true;
    case 20: ppc_fsub(c, uint8_t(d), uint8_t(a), uint8_t(b)); if (rc) record_fp(c); return true;
    case 21: ppc_fadd(c, uint8_t(d), uint8_t(a), uint8_t(b)); if (rc) record_fp(c); return true;
    case 23: c->fpr[d] = fsel(c->fpr[a], c->fpr[b], c->fpr[cc]); if (rc) record_fp(c); return true;  // fsel
    case 25: ppc_fmul(c, uint8_t(d), uint8_t(a), uint8_t(cc)); if (rc) record_fp(c); return true;
    case 26: ppc_frsqrte_op(c, uint8_t(d), uint8_t(b)); if (rc) record_fp(c); return true;
    case 28: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, true, false); if (rc) record_fp(c); return true;
    case 29: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, false, false); if (rc) record_fp(c); return true;
    case 30: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, true, true); if (rc) record_fp(c); return true;
    case 31: ppc_fmadd_op(c, uint8_t(d), uint8_t(a), uint8_t(cc), uint8_t(b), false, false, true); if (rc) record_fp(c); return true;
    }
    switch ((raw >> 1) & 0x3FF) {
    case 0: ppc_fcmp(c, uint8_t(d >> 2), c->fpr[a], c->fpr[b], false); return true;   // fcmpu
    case 32: ppc_fcmp(c, uint8_t(d >> 2), c->fpr[a], c->fpr[b], true); return true;   // fcmpo
    case 12: ppc_frsp(c, uint8_t(d), uint8_t(b)); break;                              // frsp
    case 14: ppc_fctiw_op(c, uint8_t(d), uint8_t(b), false); break;                   // fctiw
    case 15: ppc_fctiw_op(c, uint8_t(d), uint8_t(b), true); break;                    // fctiwz
    case 40: c->fpr[d] = from_bits(bits(c->fpr[b]) ^ (1ull << 63)); break;            // fneg
    case 72: c->fpr[d] = c->fpr[b]; break;                                            // fmr
    case 136: c->fpr[d] = from_bits(bits(c->fpr[b]) | (1ull << 63)); break;           // fnabs
    case 264: c->fpr[d] = from_bits(bits(c->fpr[b]) & ~(1ull << 63)); break;          // fabs
    case 583: c->fpr[d] = from_bits(0xFFF8000000000000ull | c->fpscr); break;         // mffs
    case 38: ppc_mtfsb1_op(c, uint8_t(d)); break;                                     // mtfsb1
    case 70: ppc_mtfsb0_op(c, uint8_t(d)); break;                                     // mtfsb0
    case 64: {  // mcrfs
      const uint32_t shift = 28 - 4 * (a >> 2);
      set_cr(c, int(d >> 2), c->fpscr >> shift);
      c->fpscr &= ~((0x9FF80700u & (0xFu << shift)));  // exception bits copied are cleared
      ppc_fpscr_updated(c);
      return true;
    }
    case 134: {  // mtfsfi
      const uint32_t field = d >> 2, imm = (raw >> 12) & 0xF;
      const uint32_t shift = 28 - 4 * field;
      c->fpscr = (c->fpscr & ~(0xFu << shift)) | (imm << shift);
      ppc_fpscr_control_updated(c);
      break;
    }
    case 711: {  // mtfsf
      const uint32_t fm = (raw >> 17) & 0xFF;
      uint32_t m = 0;
      for (int i = 0; i < 8; ++i)
        if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
      c->fpscr = (c->fpscr & ~m) | (uint32_t(bits(c->fpr[b])) & m);
      ppc_fpscr_control_updated(c);
      break;
    }
    default:
      c->pc = cia;
      return false;
    }
    if (rc) record_fp(c);
    return true;
  }
  }
  c->pc = cia;
  return false;
}
