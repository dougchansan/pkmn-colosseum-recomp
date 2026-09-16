// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native replacements for the paired-single matrix routines.
//
// These are the hottest guest code in the game once it runs at 60 FPS: a
// wall-clock profile of guest PC put ~7% of frame time in the two 256-byte
// buckets covering PSMTXConcat alone. They are emulated one Gekko paired-single
// instruction at a time, each becoming a helper call operating on the guest
// register file in memory, so a single 3x4 matrix concat costs far more than
// the dozen host instructions it actually needs.
//
// They are also the safest possible HLE targets: pure functions over guest
// memory with no side effects, no global state, and semantics fixed by the
// Dolphin SDK. Addresses come from the Colosseum decomp's symbol table
// (config/GC6E01/symbols.txt).
//
// Guest floats are big-endian; every load and store swaps. The arithmetic is
// deliberately written in the same order and at the same precision as the
// guest performs it - these matrices feed skinning and the camera, so a result
// that differs in the last bit is a visible artefact, not a rounding detail.

#include "moderngekko/mod_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The mod packages build with MSVC on Windows and clang/gcc elsewhere, so the
// byteswap cannot use a compiler-specific builtin unguarded.
#if defined(_MSC_VER)
#include <stdlib.h>
#define BSWAP32(x) _byteswap_ulong(x)
#else
#define BSWAP32(x) __builtin_bswap32(x)
#endif

#define GUEST_RAM_BASE 0x80000000u

static inline float *guest_ptr(CPUState *state, uint32_t address) {
  const uint32_t offset = address - GUEST_RAM_BASE;
  if (!state->ram || address < GUEST_RAM_BASE || offset >= state->ram_size)
    return 0;
  return (float *)(void *)(state->ram + offset);
}

static inline float load_be(const float *p) {
  uint32_t bits;
  memcpy(&bits, p, sizeof(bits));
  bits = BSWAP32(bits);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static inline void store_be(float *p, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  bits = BSWAP32(bits);
  memcpy(p, &bits, sizeof(bits));
}

// void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
//
// ab = a * b for 3x4 row-major matrices. The SDK tolerates ab aliasing a or b,
// so the result is accumulated into locals before any store.
static void ps_mtx_concat(CPUState *state) {
#if defined(FASTMATH_TRACE)
  {
    static unsigned long long calls = 0;
    if ((calls++ & 0xFFFFFu) == 0) {
      fprintf(stderr, "[fastmath] PSMTXConcat native call #%llu\n", calls);
      fflush(stderr);
    }
  }
#endif
  const float *a = guest_ptr(state, state->gpr[3]);
  const float *b = guest_ptr(state, state->gpr[4]);
  float *ab = guest_ptr(state, state->gpr[5]);
  if (!a || !b || !ab) {
    state->pc = state->lr;
    return;
  }

  float av[12], bv[12], out[12];
  for (int i = 0; i < 12; i++) {
    av[i] = load_be(a + i);
    bv[i] = load_be(b + i);
  }

  for (int row = 0; row < 3; row++) {
    const float r0 = av[row * 4 + 0];
    const float r1 = av[row * 4 + 1];
    const float r2 = av[row * 4 + 2];
    const float r3 = av[row * 4 + 3];
    out[row * 4 + 0] = r0 * bv[0] + r1 * bv[4] + r2 * bv[8];
    out[row * 4 + 1] = r0 * bv[1] + r1 * bv[5] + r2 * bv[9];
    out[row * 4 + 2] = r0 * bv[2] + r1 * bv[6] + r2 * bv[10];
    out[row * 4 + 3] = r0 * bv[3] + r1 * bv[7] + r2 * bv[11] + r3;
  }

  for (int i = 0; i < 12; i++)
    store_be(ab + i, out[i]);

  state->pc = state->lr;
}

// void PSMTXCopy(const Mtx src, Mtx dst)
static void ps_mtx_copy(CPUState *state) {
  const float *src = guest_ptr(state, state->gpr[3]);
  float *dst = guest_ptr(state, state->gpr[4]);
  if (src && dst && src != dst)
    memcpy(dst, src, 12 * sizeof(float));  // byte copy: endianness irrelevant
  state->pc = state->lr;
}

// void PSMTXIdentity(Mtx m)
static void ps_mtx_identity(CPUState *state) {
  float *m = guest_ptr(state, state->gpr[3]);
  if (m) {
    for (int i = 0; i < 12; i++)
      store_be(m + i, 0.0f);
    store_be(m + 0, 1.0f);
    store_be(m + 5, 1.0f);
    store_be(m + 10, 1.0f);
  }
  state->pc = state->lr;
}

// void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst)
//
// dst = m * src with an implied src.w of 1. src and dst may alias.
static void ps_mtx_mult_vec(CPUState *state) {
  const float *m = guest_ptr(state, state->gpr[3]);
  const float *src = guest_ptr(state, state->gpr[4]);
  float *dst = guest_ptr(state, state->gpr[5]);
  if (!m || !src || !dst) {
    state->pc = state->lr;
    return;
  }

  const float x = load_be(src + 0);
  const float y = load_be(src + 1);
  const float z = load_be(src + 2);
  float mv[12];
  for (int i = 0; i < 12; i++)
    mv[i] = load_be(m + i);

  const float ox = mv[0] * x + mv[1] * y + mv[2] * z + mv[3];
  const float oy = mv[4] * x + mv[5] * y + mv[6] * z + mv[7];
  const float oz = mv[8] * x + mv[9] * y + mv[10] * z + mv[11];

  store_be(dst + 0, ox);
  store_be(dst + 1, oy);
  store_be(dst + 2, oz);

  state->pc = state->lr;
}

// Addresses from the decomp symbol table; sizes are recorded there too and the
// runtime rejects a patch that does not land on a known function entry.
static const ModernGekkoModPatch patches[] = {
    RECOMP_PATCH(0x800A2D98u, ps_mtx_concat),    // PSMTXConcat,  size 0xCC
    RECOMP_PATCH(0x800A2D64u, ps_mtx_copy),      // PSMTXCopy,    size 0x34
    RECOMP_PATCH(0x800A2D38u, ps_mtx_identity),  // PSMTXIdentity, size 0x2C
    RECOMP_PATCH(0x800A37CCu, ps_mtx_mult_vec),  // PSMTXMultVec, size 0x54
};

static void feature_marker(CPUState *state) {
  moderngekko_mod_return_u32(state, 1u);
}

static const ModernGekkoModExportEntry exports[] = {
    RECOMP_EXPORT("moderngekko.feature.fast_matrix", feature_marker),
};

static const ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    0,
    0u,
    patches,
    (uint32_t)(sizeof(patches) / sizeof(patches[0])),
    0,
    0u,
    exports,
    1u,
    0,
    0u,
    0,
    0u,
    0,
    0u,
    0,
    0,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
#if defined(FASTMATH_TRACE)
  fprintf(stderr, "[fastmath] descriptor requested: %u patches\n",
          (unsigned)(sizeof(patches) / sizeof(patches[0])));
  fflush(stderr);
#endif
  return &descriptor;
}
