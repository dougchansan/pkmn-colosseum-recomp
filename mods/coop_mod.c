// hook-scan: skip -- this mod patches moveLeader at 0x8012E7BC, which is
// that function's address in the co-op DOL. On the release DOL it sits at
// 0x8012E7B8 and heroMoveMain still calls partnerMove, so the patch has
// nothing to act on and the hook scanners cannot resolve the address at
// all. Built only with -DCOLOSSEUM_COOP_MOD=ON, against the co-op DOL.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wes/Rui co-op host mod - input routing probe.
//
// Standing problem: driving automation port 0 walks Wes, yet every earlier
// sample of the game's pad table showed `held` (+0x24) and the raw sticks
// (+0x26/+0x27) frozen. Both cannot be true of the same table, so either the
// samples were taken somewhere the game does not write, or the field map was
// wrong. This build stops trusting the map and measures the path end to end.
//
// The map is in fact right; it was re-derived from the shipped DOL rather than
// from the Dolphin notes:
//
//     GSinputGetLeftStickXData  0x800F7A7C
//         lis r5, 0x8040 / lwzu r0, 0x1c10(r5)   -> table 0x80401C10
//         four-way scan at stride 0x6C comparing +0x00 against r3
//         r4 == 1 -> lbz +0x58 (processed), else lbz +0x26 (raw)
//     GSinputGetLeftStickYData  0x800F7A08       same, +0x59 / +0x27
//     GSinputRead               0x800F7F64
//         lhz +0x24 -> stw +0x30, then memcpy(entry+0x24, entry+0x18, 12)
//         so +0x18 is the incoming SI snapshot and +0x24 the latched copy
//
// So the probes are placed at the two ends and at the gate between them:
//
//     0x8008ABA0  gate      -> the driver polls a pad only when this returns 0
//     0x800F7F64  GSinputRead  entry + return, diffing the whole 0x6C entry
//     0x800F7A7C  stick X      entry + return, logging (padId, sel) -> value
//     0x800F7A08  stick Y      likewise
//     0x8000604C  the per-frame driver, for the frame counter and heartbeat
//
// A return hook needs the pad id, which the guest passes in r3 and the return
// value overwrites, so each getter is hooked at both ends and the entry hook
// parks the id in a static. Every function here is non-reentrant on the guest
// side (the getters are leaves; GSinputRead runs once per pad per frame from a
// single task), so a one-deep static is sufficient.

#include "moderngekko/mod_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#define BSWAP16(x) _byteswap_ushort(x)
#define BSWAP32(x) _byteswap_ulong(x)
#else
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#endif

#define GUEST_RAM_BASE 0x80000000u
#define PAD_TABLE_DEFAULT 0x80401C10u
#define PAD_STRIDE 0x6Cu
#define PAD_SLOTS 4u

#define OFF_ID 0x00u
#define OFF_STATUS 0x0Cu
#define OFF_RAW_IN 0x18u
#define OFF_HELD 0x24u
#define OFF_RAW_X 0x26u
#define OFF_RAW_Y 0x27u

#define DIAG_TAG "toggle1"

static uint32_t g_pad_table = PAD_TABLE_DEFAULT;
static uint64_t g_frames;

// ------------------------------------------------------------- the toggle
//
// Co-op is on by default - it is what this build is for - and switchable at
// runtime from `[Coop] enabled` in the user directory's config.ini. The host
// re-reads that file on every get_setting call, so a change from the lobby
// takes effect without a restart; polling it once every 30 frames keeps that
// to one small file read per second. COLO_COOP overrides it for scripted runs,
// where there is no lobby to click.
//
// config.ini is the right home for this: the frontend's writer carries every
// key it does not own across a rewrite, under its original section, which is
// how other mods keep their settings there.

#define COOP_SECTION "Coop"
#define COOP_KEY "enabled"
#define COOP_POLL_FRAMES 30u

static int g_coop_enabled = 1;
static int g_coop_forced = -1;  // COLO_COOP, when set
static const ModernGekkoModHostApi *g_host;

// Accepts what a human or a frontend might write: 1/true/yes/on are true,
// anything else is false. "off" has to be tested before "on"'s leading letter.
static int parse_bool(const char *text, int fallback) {
  static const char *const yes[] = {"1", "true", "yes", "on"};
  if (!text || !*text)
    return fallback;
  for (uint32_t i = 0; i < sizeof(yes) / sizeof(yes[0]); ++i) {
    const char *a = text;
    const char *b = yes[i];
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) {
      ++a;
      ++b;
    }
    if (!*a && !*b)
      return 1;
  }
  return 0;
}

static void refresh_toggle(void) {
  int wanted = g_coop_enabled;
  if (g_coop_forced >= 0) {
    wanted = g_coop_forced;
  } else if (g_host && g_host->abi_version >= 2u && g_host->get_setting) {
    char value[32];
    if (g_host->get_setting(g_host->user_data, COOP_SECTION, COOP_KEY, value,
                            (uint32_t)sizeof(value)))
      wanted = parse_bool(value, g_coop_enabled);
  }
  if (wanted == g_coop_enabled)
    return;
  g_coop_enabled = wanted;
  fprintf(stderr, "[coop] co-op %s (frame=%llu)\n",
          g_coop_enabled ? "ON - Rui takes pad 2" : "OFF - Rui follows",
          (unsigned long long)g_frames);
  fflush(stderr);
  if (g_host && g_host->abi_version >= 2u && g_host->set_status)
    g_host->set_status(g_host->user_data, MOD_ID,
                       g_coop_enabled ? "Co-op: Rui on pad 2" : "Co-op: off");
}

// ---------------------------------------------------------------- guest reads

static const uint8_t *guest_ptr(CPUState *state, uint32_t address,
                                uint32_t size) {
  const uint32_t offset = address - GUEST_RAM_BASE;
  if (!state->ram || address < GUEST_RAM_BASE)
    return 0;
  if (offset > state->ram_size || size > state->ram_size - offset)
    return 0;
  return (const uint8_t *)state->ram + offset;
}

static uint32_t read_u32(CPUState *state, uint32_t address) {
  const uint8_t *p = guest_ptr(state, address, 4);
  if (!p)
    return 0;
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return BSWAP32(v);
}

static uint16_t read_u16(CPUState *state, uint32_t address) {
  const uint8_t *p = guest_ptr(state, address, 2);
  if (!p)
    return 0;
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return BSWAP16(v);
}

static uint8_t read_u8(CPUState *state, uint32_t address) {
  const uint8_t *p = guest_ptr(state, address, 1);
  return p ? *p : 0u;
}

// Four consecutive entries whose first word is 1,2,3,4 at stride 0x6C: a
// signature nothing else in RAM matches. The disassembly says 0x80401C10 and
// this has agreed with it, but the scan costs nothing and catches a shift.
static uint32_t find_pad_table(CPUState *state) {
  for (uint32_t a = 0x80300000u; a < 0x80500000u; a += 4u) {
    int ok = 1;
    for (uint32_t i = 0; i < PAD_SLOTS; ++i) {
      if (read_u32(state, a + i * PAD_STRIDE) != i + 1u) {
        ok = 0;
        break;
      }
    }
    if (ok)
      return a;
  }
  return 0u;
}

static uint32_t slot_of_id(CPUState *state, uint32_t id) {
  for (uint32_t i = 0; i < PAD_SLOTS; ++i)
    if (read_u32(state, g_pad_table + i * PAD_STRIDE + OFF_ID) == id)
      return i;
  return PAD_SLOTS;
}

// ------------------------------------------------------- 0x8008ABA0, the gate

static uint32_t g_gate_id;
static int32_t g_gate_last[8];

static void on_gate_entry(CPUState *state) { g_gate_id = state->gpr[3]; }

static void on_gate_return(CPUState *state) {
  const uint32_t id = g_gate_id < 8u ? g_gate_id : 0u;
  const int32_t value = (int32_t)(state->gpr[3] & 0xFFu);
  if (g_gate_last[id] != value) {
    g_gate_last[id] = value;
    fprintf(stderr, "[coop] gate(id=%u) -> %d  (%s) frame=%llu\n", g_gate_id,
            value, value == 0 ? "POLLED" : "skipped",
            (unsigned long long)g_frames);
    fflush(stderr);
  }
}

// -------------------------------------------- 0x800F7F64, GSinputRead (latch)

static uint32_t g_read_id;
static uint8_t g_read_before[PAD_STRIDE];
static int g_read_have_before;
static uint32_t g_read_calls[8];
// Cumulative set of entry offsets ever seen to change across one call, per id.
static uint8_t g_changed[8][PAD_STRIDE];

static void on_read_entry(CPUState *state) {
  g_read_id = state->gpr[3];
  g_read_have_before = 0;
  const uint32_t slot = slot_of_id(state, g_read_id);
  if (slot >= PAD_SLOTS)
    return;
  const uint8_t *p =
      guest_ptr(state, g_pad_table + slot * PAD_STRIDE, PAD_STRIDE);
  if (!p)
    return;
  memcpy(g_read_before, p, PAD_STRIDE);
  g_read_have_before = 1;
}

static void on_read_return(CPUState *state) {
  const uint32_t id = g_read_id < 8u ? g_read_id : 0u;
  g_read_calls[id]++;
  if (!g_read_have_before)
    return;
  const uint32_t slot = slot_of_id(state, g_read_id);
  if (slot >= PAD_SLOTS)
    return;
  const uint8_t *p =
      guest_ptr(state, g_pad_table + slot * PAD_STRIDE, PAD_STRIDE);
  if (!p)
    return;

  // Report an offset the first time it is ever seen to move for this pad, so
  // the log names the live fields once instead of every frame.
  char line[512];
  int n = 0;
  for (uint32_t o = 0; o < PAD_STRIDE; ++o) {
    if (p[o] == g_read_before[o] || g_changed[id][o])
      continue;
    g_changed[id][o] = 1;
    if (n < (int)sizeof(line) - 24)
      n += snprintf(line + n, sizeof(line) - (size_t)n, " +0x%02X:%02X->%02X",
                    o, g_read_before[o], p[o]);
  }
  if (n > 0) {
    fprintf(stderr, "[coop] read(id=%u) new live bytes:%s  frame=%llu\n",
            g_read_id, line, (unsigned long long)g_frames);
    fflush(stderr);
  }
}

// --------------------------------------- 0x800F7A7C / 0x800F7A08, the getters

// [axis][id][sel]; axis 0 = X, 1 = Y.
static uint32_t g_get_id[2];
static uint32_t g_get_sel[2];
static int32_t g_get_last[2][8][2];
static int g_get_seen[2][8][2];

static void getter_entry(CPUState *state, int axis) {
  g_get_id[axis] = state->gpr[3];
  g_get_sel[axis] = state->gpr[4];
}

static void getter_return(CPUState *state, int axis) {
  const uint32_t id = g_get_id[axis] < 8u ? g_get_id[axis] : 0u;
  const uint32_t sel = g_get_sel[axis] == 1u ? 1u : 0u;
  const int32_t value = (int32_t)(state->gpr[3] & 0xFFu);
  if (g_get_seen[axis][id][sel] && g_get_last[axis][id][sel] == value)
    return;
  g_get_seen[axis][id][sel] = 1;
  g_get_last[axis][id][sel] = value;
  fprintf(stderr, "[coop] stick%c(id=%u, sel=%u) -> %d  frame=%llu\n",
          axis == 0 ? 'X' : 'Y', g_get_id[axis], g_get_sel[axis], value,
          (unsigned long long)g_frames);
  fflush(stderr);
}

static void on_stickx_entry(CPUState *state) { getter_entry(state, 0); }
static void on_stickx_return(CPUState *state) { getter_return(state, 0); }
static void on_sticky_entry(CPUState *state) { getter_entry(state, 1); }
static void on_sticky_return(CPUState *state) { getter_return(state, 1); }

// ------------------------------------------------ 0x8012E388, heroReadStick
//
// The field's stick reader, called once per moved member from
// moveLeader__F15HEROMOVE_MEMBER. r3 is the member index, and the co-op patch
// maps member -> logical pad as member + 1. Counting calls per member says
// directly whether heroMoveMain's co-op branch runs for Rui (member 1), which
// is the one thing the pad-table trace cannot show.

static uint32_t g_stick_member[4];

static void on_read_stick(CPUState *state) {
  const uint32_t member = state->gpr[3] < 4u ? state->gpr[3] : 3u;
  if (g_stick_member[member]++ == 0u) {
    fprintf(stderr, "[coop] heroReadStick(member=%u) first call frame=%llu\n",
            state->gpr[3], (unsigned long long)g_frames);
    fflush(stderr);
  }
}

// --------------------------- 0x8012E7BC, moveLeader__F15HEROMOVE_MEMBER
//
// The co-op build's heroMoveMain calls this for the non-leader member and
// again for the leader (retail called partnerMove for the first). Counting it
// per member separates "heroMoveMain never reaches the co-op call" from
// "moveLeader runs for Rui but bails before heroReadStick".
//
// The address is the CO-OP build's: this is the first symbol the co-op edit
// moves (retail 0x8012E7B8 -> 0x8012E7BC), because heroReadStick grows by one
// instruction ahead of it. Everything else hooked here sits below the edit
// point and is identical in both builds.

static uint32_t g_move_leader[4];

// Retail's heroMoveMain gives the non-leader member partnerMove (the follower
// AI); the co-op build gives it moveLeader (human control from its own pad).
// The toggle picks between the two without touching the guest at all.
//
// It has to be done this way. The obvious guest-side flag is what the old
// coopFieldActive was, and testing it inside heroMoveMain grows .text, which
// moves .bss - and the game reaches some globals through addresses baked into
// code that no relocation rewrites. A +192 build put the pad table's symbol at
// 0x80401CD0 while the game kept filling 0x80401C10. So heroMoveMain keeps
// calling moveLeader unconditionally and the switch lives here, where it
// costs the guest nothing.
//
// A patch that leaves pc on the function's entry declines the call and the
// original runs (mod_abi.h), so "co-op on" is one host call per invocation and
// nothing else. To turn co-op off, point pc at partnerMove and leave lr alone:
// that is a tail call, and partnerMove returns straight to heroMoveMain's
// continuation. moveLeader's f32 result is discarded at the non-leader call
// site, so partnerMove returning void is fine.
#define PARTNER_MOVE 0x8012DE94u
#define HERO_MOVE_WORK 0x80426BD0u

static void coop_move_leader(CPUState *state) {
  const uint32_t member = state->gpr[3] < 4u ? state->gpr[3] : 3u;
  g_move_leader[member]++;
  // The leader is a human either way; only the partner changes hands.
  if (g_coop_enabled ||
      (int32_t)state->gpr[3] == (int32_t)read_u32(state, HERO_MOVE_WORK))
    return;
  state->pc = PARTNER_MOVE;
}

// ---------------------------- 0x8012EBD8, heroMoveMain (co-op address)
//
// Counts frames in which the field mover actually runs, and records the member
// flags as heroMoveMain itself sees them, rather than as a heartbeat samples
// them between frames.

static uint32_t g_main_calls;
static uint16_t g_main_flags[2];

static void on_hero_move_main(CPUState *state) {
  g_main_calls++;
  g_main_flags[0] = read_u16(state, 0x80426BD0u + 0x04u);
  g_main_flags[1] = read_u16(state, 0x80426BD0u + 0x24u);
}

// ------------------------------------------- 0x8000604C, the per-frame driver

// HeroMoveWork lbl_80426BD0: +0x00 leader, then member[2] at stride 0x20 from
// +0x04 whose u16 flags bit 0 marks an active party member. heroMoveMain moves
// a non-leader member only when that bit is set.
// Where each field member actually is, which the runner's own telemetry does
// not report: navigation_player_x/z is the leader only, so it can never show
// Rui moving. The game's own route, from moveLeader's prologue:
//
//     getPos(member) = GSmodelGetPosition(GSresGetResource(0, resId[member]))
//     GSresGetResource (0x800F9318) walks *(r13-0x5BC4), *(r13-0x5BC0) entries
//         of stride 0x14, matching +0x08 == group and +0x0C == id, and
//         returns +0x04
//     GSmodelGetPosition (0x800E3D98) copies the VECTOR at model + 0x18
//     the two resource ids live in sdata2 at r2-0x6670 and r2-0x666C
//
// Walking that from the host keeps the probe read-only: no guest calls, so
// nothing can perturb the thing being measured.
#define RES_COUNT_SDA 0x5BC0u
#define RES_TABLE_SDA 0x5BC4u
#define RES_ENTRY_STRIDE 0x14u
#define MODEL_POSITION 0x18u

static float read_f32(CPUState *state, uint32_t address) {
  const uint32_t bits = read_u32(state, address);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t resource_of(CPUState *state, uint32_t group, uint32_t id) {
  const uint32_t count = read_u32(state, state->gpr[13] - RES_COUNT_SDA);
  const uint32_t table = read_u32(state, state->gpr[13] - RES_TABLE_SDA);
  if (!table || count > 0x4000u)
    return 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * RES_ENTRY_STRIDE;
    if (!read_u32(state, entry + 0x04u))
      continue;
    if (read_u32(state, entry + 0x08u) == group &&
        read_u32(state, entry + 0x0Cu) == id)
      return read_u32(state, entry + 0x04u);
  }
  return 0u;
}

static int member_position(CPUState *state, uint32_t member, float *x,
                           float *z) {
  const uint32_t id =
      read_u32(state, state->gpr[2] - (member == 0u ? 0x6670u : 0x666Cu));
  const uint32_t model = resource_of(state, 0u, id);
  if (!model)
    return 0;
  *x = read_f32(state, model + MODEL_POSITION);
  *z = read_f32(state, model + MODEL_POSITION + 8u);
  return 1;
}

// One line per sample, both members on it, so a run's before/after is a grep
// away. Wes is the leader and the runner's own navigation_player_x/z follows
// him; Rui appears in no host telemetry, which is why this exists.
static void report_positions(CPUState *state) {
  float wx = 0.0f, wz = 0.0f, rx = 0.0f, rz = 0.0f;
  const int wes = member_position(state, 0u, &wx, &wz);
  const int rui = member_position(state, 1u, &rx, &rz);
  fprintf(stderr, "[coop] pos frame=%llu wes=", (unsigned long long)g_frames);
  if (wes)
    fprintf(stderr, "(%.3f, %.3f)", wx, wz);
  else
    fprintf(stderr, "(no model)");
  fprintf(stderr, " rui=");
  if (rui)
    fprintf(stderr, "(%.3f, %.3f)", rx, rz);
  else
    fprintf(stderr, "(no model)");
  fprintf(stderr, "\n");
  fflush(stderr);
}

#define HERO_MEMBER0_FLAGS (HERO_MOVE_WORK + 0x04u)
#define HERO_MEMBER1_FLAGS (HERO_MOVE_WORK + 0x24u)

static int g_table_found;

static void report(CPUState *state, const char *why) {
  fprintf(stderr, "[coop] %-9s frame=%llu table=0x%08X\n", why,
          (unsigned long long)g_frames, g_pad_table);
  for (uint32_t i = 0; i < PAD_SLOTS; ++i) {
    const uint32_t base = g_pad_table + i * PAD_STRIDE;
    const uint32_t id = read_u32(state, base + OFF_ID);
    fprintf(stderr,
            "[coop]   slot%u id=%u st=%u held=0x%04X raw=(%3u,%3u) "
            "proc=(%3u,%3u) in=%02X%02X%02X%02X reads=%u gate=%d\n",
            i, id, read_u32(state, base + OFF_STATUS),
            read_u16(state, base + OFF_HELD), read_u8(state, base + OFF_RAW_X),
            read_u8(state, base + OFF_RAW_Y), read_u8(state, base + 0x58u),
            read_u8(state, base + 0x59u), read_u8(state, base + OFF_RAW_IN),
            read_u8(state, base + OFF_RAW_IN + 1u),
            read_u8(state, base + OFF_RAW_IN + 2u),
            read_u8(state, base + OFF_RAW_IN + 3u),
            id < 8u ? g_read_calls[id] : 0u, id < 8u ? g_gate_last[id] : -1);
  }
  fprintf(stderr,
          "[coop]   heroMove leader=%d flags=[0x%04X 0x%04X] "
          "(at main: [0x%04X 0x%04X])  main=%u "
          "moveLeader=[%u %u %u %u] readStick=[%u %u %u %u]\n",
          (int32_t)read_u32(state, HERO_MOVE_WORK),
          read_u16(state, HERO_MEMBER0_FLAGS),
          read_u16(state, HERO_MEMBER1_FLAGS), g_main_flags[0],
          g_main_flags[1], g_main_calls, g_move_leader[0], g_move_leader[1],
          g_move_leader[2], g_move_leader[3], g_stick_member[0],
          g_stick_member[1], g_stick_member[2], g_stick_member[3]);
  report_positions(state);
  fflush(stderr);
}

// Which heroMoveMain is actually in RAM.
//
// A savestate restores the whole MEM1 image, code included, so loading one
// that was captured on a different DOL silently replaces the running game's
// text with that DOL's. The co-op edit lives entirely in heroMoveMain's member
// loop, where the retail build calls partnerMove (the follower AI) and the
// co-op build calls moveLeader a second time:
//
//     retail   8012EE28  bl 0x8012DE94   partnerMove
//     co-op    8012EE2C  bl 0x8012E7BC   moveLeader
//
// A `bl` is 0x48000001 | (displacement & 0x03FFFFFC), so the target is
// recoverable from the word alone and this needs no symbol table.
static int g_code_checked;

static void check_hero_move_code(CPUState *state) {
  static const struct {
    uint32_t site;
    uint32_t target;
    const char *name;
  } variants[] = {
      {0x8012EE28u, 0x8012DE94u, "RETAIL (partnerMove: follower AI)"},
      {0x8012EE2Cu, 0x8012E7BCu, "CO-OP (moveLeader: human control)"},
  };
  for (uint32_t i = 0; i < 2u; ++i) {
    const uint32_t word = read_u32(state, variants[i].site);
    int32_t displacement = (int32_t)(word & 0x03FFFFFCu);
    if (displacement & 0x02000000)
      displacement -= 0x04000000;
    const uint32_t target = variants[i].site + (uint32_t)displacement;
    fprintf(stderr, "[coop] code 0x%08X = 0x%08X  %s -> 0x%08X  %s\n",
            variants[i].site, word,
            (word & 0xFC000003u) == 0x48000001u ? "bl" : "??", target,
            (word & 0xFC000003u) == 0x48000001u && target == variants[i].target
                ? variants[i].name
                : "(not this variant)");
  }
  fflush(stderr);
}

static void on_frame(CPUState *state) {
  g_frames++;

  if ((g_frames % COOP_POLL_FRAMES) == 0u)
    refresh_toggle();

  // After the savestate has had a chance to land.
  if (!g_code_checked && g_frames == 60u) {
    g_code_checked = 1;
    check_hero_move_code(state);
  }

  if (!g_table_found && (g_frames % 30u) == 0u) {
    const uint32_t found = find_pad_table(state);
    if (found) {
      if (found != g_pad_table)
        fprintf(stderr, "[coop] pad table found at 0x%08X (was 0x%08X)\n", found,
                g_pad_table);
      g_pad_table = found;
      g_table_found = 1;
      report(state, "found");
    }
  }

  if ((g_frames % 30u) == 0u)
    report_positions(state);

  if ((g_frames % 300u) == 0u)
    report(state, "heartbeat");
}

static const ModernGekkoModPatch patches[] = {
    RECOMP_PATCH(0x8012E7BCu, coop_move_leader),
};

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(0x8008ABA0u, on_gate_entry),
    RECOMP_HOOK_RETURN(0x8008ABA0u, on_gate_return),
    RECOMP_HOOK(0x800F7F64u, on_read_entry),
    RECOMP_HOOK_RETURN(0x800F7F64u, on_read_return),
    RECOMP_HOOK(0x800F7A7Cu, on_stickx_entry),
    RECOMP_HOOK_RETURN(0x800F7A7Cu, on_stickx_return),
    RECOMP_HOOK(0x800F7A08u, on_sticky_entry),
    RECOMP_HOOK_RETURN(0x800F7A08u, on_sticky_return),
    RECOMP_HOOK(0x8012E388u, on_read_stick),
    RECOMP_HOOK(0x8012EBD8u, on_hero_move_main),
    RECOMP_HOOK(0x8000604Cu, on_frame),
};

static void on_load(const ModernGekkoModHostApi *api) {
  g_host = api;
  const char *coop = getenv("COLO_COOP");
  if (coop && *coop)
    g_coop_forced = parse_bool(coop, 1);
  refresh_toggle();
  if (g_host && g_host->abi_version >= 2u && g_host->set_status)
    g_host->set_status(g_host->user_data, MOD_ID,
                       g_coop_enabled ? "Co-op: Rui on pad 2" : "Co-op: off");
  const char *override = getenv("COLO_COOP_PAD_TABLE");
  if (override && *override) {
    const unsigned long value = strtoul(override, 0, 0);
    if (value >= GUEST_RAM_BASE)
      g_pad_table = (uint32_t)value;
  }
  for (uint32_t i = 0; i < 8u; ++i)
    g_gate_last[i] = -1;
  fprintf(stderr, "[coop] loaded " DIAG_TAG "; co-op %s; pad table 0x%08X%s\n",
          g_coop_enabled ? "on" : "off",
          g_pad_table,
          g_pad_table == PAD_TABLE_DEFAULT ? " (retail default)" : " (override)");
  fflush(stderr);
}

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
    hooks,
    (uint32_t)(sizeof(hooks) / sizeof(hooks[0])),
    0,
    0u,
    0,
    0u,
    0,
    0u,
    0,
    0u,
    on_load,
    0,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
  return &descriptor;
}
