// SPDX-License-Identifier: GPL-3.0-or-later
//
// Follower Pokemon: the first Pokemon in the party walks behind Wes in the field, the way Rui
// does, and Rui walks one place further back (behind the Pokemon). When Rui is played by a
// second player (the co-op mod), only the Pokemon follows.
//
// How it works (decomp: src/game/hero_move.c, pokemon_range_8011F5FC.c):
//   - heroMoveInit (0x8012FCD4) runs on every floor load: the follower is rebuilt there.
//   - The model comes from pokemonCreateSequence (0x80121C18), the loader the summary screen
//     and evolution use (species, shiny and status textures from the party slot). It waits for
//     the model data to load, so it runs in its own game thread; its return hands us the
//     sequence, whose GSmodel (fn_801DAC3C) we place every frame.
//   - heroMoveMain (0x8012EBD4) runs once per field frame after Wes and Rui move: the follower
//     walks Wes's trail at a fixed distance behind him, as Rui does at 12 units.
//   - Rui's place behind Wes is heroMove member 1's spacing (0x80426BF8, 12.0); doubling it
//     puts her behind the Pokemon.
//   - A tap of Z sends it out of its ball or calls it back, with the ball's own effects (open,
//     release; the red recall beam) and its cry, played the way the game's own scenes do.
//   - Only outdoors: inside buildings (any field archive but <area>_out) it stays in its ball.

#include "moderngekko/mod_abi.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MOD_ID
#define MOD_ID "pokemon-colosseum-follower"
#endif
#ifndef MOD_VERSION
#define MOD_VERSION "0.1.0"
#endif
#ifndef MOD_DISPLAY_NAME
#define MOD_DISPLAY_NAME "Pokemon Colosseum Follower Pokemon"
#endif

enum {
  kHeroMoveInit = 0x8012FCD4u,
  kHeroMoveMain = 0x8012EBD4u,
  kCreateSequence = 0x80121C18u,
  kSequenceModel = 0x801DAC3Cu,   // fn_801DAC3C(seq) -> GSmodel
  kSequenceFree = 0x801DB100u,    // fn_801DB100(seq)
  kSequenceState = 0x801DA4E8u,   // fn_801DA4E8(seq, state); 1 = idle, as gs_task.c does
  kFloorContext = 0x800FF560u,
  kThreadCreate = 0x800F07A8u,    // GSthreadCreate(priority, context, stack, ?, ?, entry)
  kThreadSetArgs = 0x800F0654u,   // (thread, count, arg0)
  kSetPosition = 0x800E43A4u,     // GSmodelSetPosition(model, GSvec*)
  kSetRotation = 0x800E4170u,     // GSmodelSetRotation(model, GSvec*)
  kSetScale = 0x800E407Cu,        // GSmodelSetScale(model, GSvec*)
  kSaveRoot = 0x8047ADB8u,        // party slot i: *root + 0xA0 + i * 0x138
  kPeople = 0x8047B200u,          // people entries, stride 0xDC; +0x28 group, +0x2C index
  kPeopleCount = 0x8047B1F8u,
  kHeroMove = 0x80426BD0u,        // heroMove state: +0x00 leader index; member i at +i*0x20,
                                  // +0x04 u16 flags (bit 0: in the party), +0x08 spacing
  kRuiSpacing = 0x80426BF8u,      // heroMove member 1, +0x08 spacing (f32)
  // The model sequence pool (lbl_80467CC0: array, +4 u16 count; 0x8C per sequence, +0x74
  // in use). Battles and menus create it; the field has none, so we make a one-slot pool the
  // way evolution does (fn_801DADC0(count) after wazaSequenceSysRelease).
  kPool = 0x80467CC0u,
  kPoolCreate = 0x801DADC0u,
  kPoolRelease = 0x801DAC90u,
  // Ball effects, as a battle send-out plays them on the Pokemon's own sequence
  // (fightOutPokemonDasuEffect / ModosuEffect): load (reads files; a game thread), start,
  // poll, free. Ids come from the ball item: itemGetStatus(0, ball, 13 In / 14 Open / 15 Out).
  kEffectLoad = 0x801DDD28u,      // fn_801DDD28(seq, id, 4, 0) -> nonzero when loaded
  kEffectStart = 0x801DA9E8u,     // (seq, id, 4)
  kEffectPlaying = 0x801DA94Cu,   // (seq, id, 4) -> nonzero while playing
  kEffectFree = 0x801DA8C4u,      // (seq, id, 4)
  kItemStatus = 0x80142CF4u,      // itemGetStatus(0, item, field, 0)
  kPokemonStatus = 0x8012640Cu,   // pokemonGetStatus(mon, species, field, index)
  kPlaySound = 0x80166AB8u,       // fn_80166AB8(sound, 0, 0): sound effects and cries
  kSetVisibility = 0x800E4014u,   // GSmodelSetVisibility(model, visible)
  // Plays a list of effects on a sequence, one after another, stepping the effect system
  // (fn_801DB088) each frame until each ends, then frees it: fn_8025DE54(u32 *sequence,
  // u16 (*list)[2] {id, 4}, count, skip_reset, fade, idle_after). The field never steps effects
  // itself, and the step can wait on the game, so this runs in a game thread.
  kEffectsPlay = 0x8025DE54u,
  // Its arguments live in guest memory for as long as the thread runs: the unused tail of the
  // debug move viewer's state (lbl_804673F8, 0x888 bytes; only the debug menu touches it).
  kScratch = 0x804673F8u + 0x878u,
  kPads = 0x80401C10u,            // pad table: 4 entries of 0x6C, +0x00 pad id, +0x24 held
  // The floor being played: floorDataBiosGetCurrentPtr; +0x04 file group (its FSYS archive).
  kFloorCurrent = 0x80115BD8u,
  // Field collision sweep: fn_8010F320(GSvec *from, GSvec *to, f32 radius, GSvec *hit) -> 1
  // when a wall is in the way (heroMove tests Wes's moves with it at chest height).
  kSweep = 0x8010F320u,
};

// The send-out button: a tap of Z. The field reads A (talk), X/Y/Start (menu) and the stick,
// and its pad camera (camera.c) moves on L, R, X and Y; Z alone does nothing. Z is also the
// modifier for the free camera (Z+X, Z+Y, Z+D-pad) and the pause menu (Start+Z), so only a
// short press with no other button counts.
enum { kButtonZ = 0x0010u, kTapFrames = 20 };

enum BallFx { kFxIn, kFxOpen, kFxOut, kFxCount };
enum Ball {
  kBallIdle, kBallThrowing, kBallOpening, kBallAppearing,
  kBallRecallBall,  // recall: the ball flies to the Pokemon and opens (the send-out's ball)
  kBallRecalling,   // then the red beam takes it in
};

// Wes's throw: his field model's clip 7 (a lunge with the Snag Machine arm thrust forward),
// played the way scripts animate people (fn_8018B368(group 0, id 100, clip, frame, loop)),
// with heroMove held still for it (heroMoveSetLockFrame). The ball leaves his hand ~10 frames in.
enum {
  kPeopleAnimate = 0x8018B368u,
  kHeroLock = 0x8012B184u,
  kThrowClip = 7,
  kThrowRelease = 10,
  kThrowLock = 40,
};

static const ModernGekkoModHostApi *api;

static const float kDistance = 12.0f;      // behind Wes, as Rui stands by default
static const float kSnap = 60.0f;          // further than this from Wes: start again beside him
static const float kSample = 1.0f;         // trail point every unit of Wes's movement
static const float kThrow = 36.0f;         // the ball lands this far ahead: three places
static const float kRejoin = 2.5f;         // trot speed back to the trail (units per frame)

static struct {
  int want;             // a floor loaded: build the follower
  int loading;          // the loader thread is running
  int in_loader;        // inside our pokemonCreateSequence call
  uint32_t mon;         // party slot the model was made from
  uint16_t species;
  uint32_t pid;         // personality value (form, shininess)
  int rui_spaced;       // Rui's spacing is doubled
  uint32_t sequence;
  uint32_t model;
  uint32_t pool;        // the sequence pool our model lives in
  int own_pool;         // we created it (the field has none of its own)
  float trail[256][3];  // Wes's recent positions, newest first
  int trail_count;
  float pos[3];
  int placed;           // pos has been set for this model
  float yaw;
  unsigned frames;
  // Sent out or in its ball. Kept across floors, menus and battles: the follower comes back
  // the way the player left it.
  int out;
  int ball;             // enum Ball: an effect in progress
  int ball_frames;      // frames in the current stage
  int visible;          // the model's current visibility
  uint32_t fx[kFxCount];   // effect ids for this Pokemon's ball
  int fx_loaded[kFxCount];
  unsigned fx_want;     // effects (1 << BallFx) the throw in progress needs loaded
  int fx_loading;       // an effect loader thread is running
  int in_fx_loader;
  // The effect load in flight: which effect, for which sequence, and for how long. When the
  // follower is put away mid-load (a battle, a menu, a floor change) the load goes stale: its
  // result is dropped, and no new load starts until it has returned (or given up on).
  int fx_loading_which;
  uint32_t fx_loading_seq;
  int fx_stale;
  int loading_stale;    // the same for the model loader
  unsigned load_frames; // frames the current load (model or effect) has been running
  uint16_t prev_held;   // pad 1's buttons last frame
  int z_frames;         // frames Z has been held (0: up)
  int z_combo;          // another button was pressed while Z was held
  int fx_seen;          // the current stage's effect has been seen playing
  int indoors;          // this floor is inside a building: no follower
  int floor_checked;    // indoors has been worked out for this floor
  int landed;           // sent out ahead of Wes: stays put, then trots back to the trail
  float wes_at_throw[3];
} f;

// --- guest memory ---------------------------------------------------------------------------

static uint8_t *memory(CPUState *s, uint32_t a, uint32_t size) {
  const uint32_t offset = a - 0x80000000u;
  if (!s->ram || a < 0x80000000u || offset > s->ram_size || size > s->ram_size - offset)
    return NULL;
  return s->ram + offset;
}

static uint32_t read32(CPUState *s, uint32_t a) {
  const uint8_t *p = memory(s, a, 4);
  return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}

static uint16_t read16(CPUState *s, uint32_t a) {
  const uint8_t *p = memory(s, a, 2);
  return p ? (uint16_t)(((uint32_t)p[0] << 8) | p[1]) : 0;
}

static void write32(CPUState *s, uint32_t a, uint32_t v) {
  uint8_t *p = memory(s, a, 4);
  if (!p)
    return;
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static float read_f32(CPUState *s, uint32_t a) {
  const uint32_t bits = read32(s, a);
  float v;
  memcpy(&v, &bits, 4);
  return v;
}

static uint32_t f32_bits(float v) {
  uint32_t bits;
  memcpy(&bits, &v, 4);
  return bits;
}

// --- guest calls ----------------------------------------------------------------------------

static uint32_t call(CPUState *s, uint32_t address, const uint32_t *args, int count, int *ok) {
  CPUState c = *s;
  for (int i = 0; i < count && i < 8; ++i)
    c.gpr[3 + i] = args[i];
  const int done = api && api->call_guest && api->call_guest(api->user_data, &c, address);
  if (ok)
    *ok = done;
  return done ? c.gpr[3] : 0;
}

// Calls fn(model, &vec) with the vector in scratch space below the guest stack pointer.
static int call_with_vec(CPUState *s, uint32_t address, uint32_t model, const float v[3]) {
  if (!api || !api->call_guest)
    return 0;
  CPUState c = *s;
  const uint32_t vec = (s->gpr[1] - 0x100u) & ~0xFu;
  for (int k = 0; k < 3; ++k)
    write32(s, vec + 4u * (uint32_t)k, f32_bits(v[k]));
  c.gpr[1] = vec - 0x100u;
  c.gpr[3] = model;
  c.gpr[4] = vec;
  return api->call_guest(api->user_data, &c, address);
}

// Runs entry(args...) (up to 6 arguments) in a new game thread in the floor's context, for
// functions that wait on the game (file loads, effect starts): call_guest cannot return from
// those. The thread starts on the scheduler's next pass.
static int run_in_thread(CPUState *s, uint32_t entry, const uint32_t *args, int count) {
  int ok = 0;
  const uint32_t context = call(s, kFloorContext, NULL, 0, &ok);
  const uint32_t create[6] = {1, context, 0x4000, 1, 1, entry};
  const uint32_t thread = ok ? call(s, kThreadCreate, create, 6, &ok) : 0;
  if (!ok || !thread) {
    fprintf(stderr, "[follower] could not start a thread for %08X\n", entry);
    return 0;
  }
  uint32_t set[8] = {thread, (uint32_t)count};
  for (int i = 0; i < count && i < 6; ++i)
    set[2 + i] = args[i];
  call(s, kThreadSetArgs, set, 2 + count, &ok);
  return 1;
}

// The field's collision sweep from `from` to `to` (radius r): 1 if blocked.
static int blocked(CPUState *s, const float from[3], const float to[3], float r) {
  if (!api || !api->call_guest)
    return 0;
  CPUState c = *s;
  const uint32_t a = (s->gpr[1] - 0x100u) & ~0xFu, b = a + 0x10u, hit = a + 0x20u;
  for (int k = 0; k < 3; ++k) {
    write32(s, a + 4u * (uint32_t)k, f32_bits(from[k]));
    write32(s, b + 4u * (uint32_t)k, f32_bits(to[k]));
    write32(s, hit + 4u * (uint32_t)k, 0);
  }
  c.gpr[1] = a - 0x100u;
  c.gpr[3] = a;
  c.gpr[4] = b;
  c.gpr[5] = hit;
  c.fpr[1] = r;
  return api->call_guest(api->user_data, &c, kSweep) && (c.gpr[3] & 0xFFu) != 0;
}

// --- the field ------------------------------------------------------------------------------

// GSmodel of people (group 0, index), or 0.
static uint32_t people_model(CPUState *s, uint32_t index) {
  const uint32_t base = read32(s, kPeople), count = read32(s, kPeopleCount);
  for (uint32_t i = 0; base && i < count && i < 512; ++i) {
    const uint32_t e = base + i * 0xDCu;
    const uint8_t *active = memory(s, e, 1);
    if (active && *active && read32(s, e + 0x28) == 0 && read32(s, e + 0x2C) == index)
      return read32(s, e + 0x08);
  }
  return 0;
}

static int model_position(CPUState *s, uint32_t model, float out[3]) {
  if (!model || !memory(s, model + 0x18, 12))
    return 0;
  for (int k = 0; k < 3; ++k)
    out[k] = read_f32(s, model + 0x18u + 4u * (uint32_t)k);
  return 1;
}

// The field archive's name: <area>_<place>[_<floor>]... (M1_out, M1_pc_B1, M2_cave_2F_1).
// Copies the name of an FSYS archive at `a` into out if it looks like a field archive.
static int field_archive_name(CPUState *s, uint32_t a, char out[64]) {
  const uint8_t *p = memory(s, a + 0x70u, 64);
  if (!p)
    return 0;
  int n = 0;
  for (; n < 63 && p[n]; ++n) {
    const char c = (char)p[n];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.'))
      return 0;
    out[n] = c;
  }
  out[n] = 0;
  return n >= 4 && strchr("SMDT", out[0]) && out[1] >= '0' && out[1] <= '9' && out[2] == '_';
}

// Inside a building: the loaded field archive's place is anything but "out" (towns, routes and
// the desert are <area>_out). Its FSYS header names the file group the floor data asks for, so
// a stale archive left in memory by the last floor is not mistaken for this one. Unknown counts
// as outdoors.
static int floor_indoors(CPUState *s) {
  const uint32_t floor = call(s, kFloorCurrent, NULL, 0, NULL);
  const uint32_t group = floor ? read32(s, floor + 4u) : 0;
  char best[64] = "", name[64];
  int matched = 0;
  for (uint32_t off = 0x10000u; s->ram && off + 0xB0u <= s->ram_size; off += 4) {
    const uint8_t *p = s->ram + off;
    if (p[0] != 'F' || p[1] != 'S' || p[2] != 'Y' || p[3] != 'S')
      continue;
    const uint32_t a = 0x80000000u + off;
    const uint32_t size = read32(s, a + 0x20u);
    if (size < 0x80u || size > s->ram_size - off || !field_archive_name(s, a, name))
      continue;
    const int match = group && (read32(s, a + 0x08u) == group || read32(s, a + 0x04u) == group ||
                                read32(s, a + 0x0Cu) == group);
    fprintf(stderr, "[follower] field archive %s at %08X (header %08X %08X %08X)%s\n", name, a,
            read32(s, a + 0x04u), read32(s, a + 0x08u), read32(s, a + 0x0Cu),
            match ? " <- this floor" : "");
    if (match && !matched) {
      matched = 1;
      memcpy(best, name, sizeof(best));
    } else if (!best[0]) {
      memcpy(best, name, sizeof(best));
    }
  }
  const char *place = strchr(best, '_');
  const int indoors = place && strncmp(place + 1, "out", 3) != 0;
  const uint8_t *kind = floor ? memory(s, floor, 1) : NULL;
  fprintf(stderr, "[follower] floor %X (%08X) group %08X kind %u: %s (%s)\n",
          call(s, 0x800FF56Cu, NULL, 0, NULL), floor, group,  // floor id: what FOLLOWER_WARP takes
          kind ? (unsigned)(*kind >> 5) & 7u : 0u, best[0] ? best : "unknown",
          indoors ? "indoors: no follower" : "outdoors");
  float wes[3];
  if (model_position(s, people_model(s, 100), wes))  // where FOLLOWER_WARP can put him here
    fprintf(stderr, "[follower]   wes arrives at (%.0f %.0f %.0f)\n", (double)wes[0],
            (double)wes[1], (double)wes[2]);
  return indoors;
}

// Where the thrown ball lands: kThrow ahead of Wes along his facing (model rotation +0x24,
// Euler radians; yaw is y), pulled in until nothing stands in the way. Returns the distance.
static float landing_point(CPUState *s, uint32_t wes_model, const float wes[3], float out[3]) {
  const float yaw = read_f32(s, wes_model + 0x28u);
  const float fx = sinf(yaw), fz = cosf(yaw);
  const float from[3] = {wes[0], wes[1] + 8.5f, wes[2]};
  float d = kThrow;
  for (; d > 6.0f; d -= 6.0f) {
    const float to[3] = {wes[0] + fx * d, wes[1] + 8.5f, wes[2] + fz * d};
    if (!blocked(s, from, to, 4.0f))
      break;
  }
  out[0] = wes[0] + fx * d;
  out[1] = wes[1];
  out[2] = wes[2] + fz * d;
  f.yaw = yaw;
  fprintf(stderr, "[follower] thrown %.0f ahead (yaw %.2f)\n", (double)d, (double)yaw);
  return d;
}

static uint32_t lead_pokemon(CPUState *s, uint16_t *species) {
  const uint32_t root = read32(s, kSaveRoot);
  if (!root)
    return 0;
  const uint32_t mon = root + 0xA0u;
  *species = read16(s, mon);
  return *species ? mon : 0;
}

// Our sequence is still ours while the pool is the one it was made in and its slot is in use;
// a battle or menu releases and remakes the pool, taking the model with it.
static int sequence_alive(CPUState *s) {
  if (!f.sequence)
    return 0;
  const uint8_t *in_use = memory(s, f.sequence + 0x74u, 1);
  return read32(s, kPool) == f.pool && in_use && *in_use;
}

static void free_effects(CPUState *s);

static void release(CPUState *s) {
  if (sequence_alive(s)) {
    free_effects(s);
    const uint32_t args[1] = {f.sequence};
    call(s, kSequenceFree, args, 1, NULL);
  }
  for (int i = 0; i < kFxCount; ++i)
    f.fx_loaded[i] = 0;
  f.ball = 0;
  f.landed = 0;
  if (f.own_pool && read32(s, kPool) == f.pool)
    call(s, kPoolRelease, NULL, 0, NULL);
  f.own_pool = 0;
  f.sequence = 0;
  f.model = 0;
  f.pool = 0;
  if (f.fx_loading)
    f.fx_stale = 1;
  if (f.loading)
    f.loading_stale = 1;
}

// The field has no sequence pool: make a one-slot one. Menus that show a Pokemon (the
// summary's model loader, fn_8010A88C) expect the field to have none and build their own, and
// sharing ours with them froze the game, so menu_model_entry puts the follower away first.
static int ensure_pool(CPUState *s) {
  if (read16(s, kPool + 4u))
    return 1;
  const uint32_t args[1] = {1};
  int ok = 0;
  call(s, kPoolCreate, args, 1, &ok);
  const uint32_t pool = read32(s, kPool);
  const uint32_t count = read16(s, kPool + 4u);
  fprintf(stderr, "[follower] made a sequence pool: %s, array %08X, %u slots\n",
          ok ? "called" : "call failed", pool, count);
  if (!pool || !count)
    return 0;
  f.own_pool = 1;
  return 1;
}

// Starts pokemonCreateSequence(mon) in a game thread, in the floor's context.
static void start_loading(CPUState *s) {
  uint16_t species = 0;
  const uint32_t mon = lead_pokemon(s, &species);
  if (!mon || !ensure_pool(s))
    return;
  const uint32_t args[1] = {mon};
  if (!run_in_thread(s, kCreateSequence, args, 1))
    return;
  f.mon = mon;
  f.species = species;
  f.pid = read32(s, mon + 4u);
  f.loading = 1;
  f.loading_stale = 0;
  f.load_frames = 0;
  fprintf(stderr, "[follower] loading species %u (party slot %08X)\n", species, mon);
}

static void push_trail(const float p[3]) {
  if (f.trail_count) {
    const float dx = p[0] - f.trail[0][0], dz = p[2] - f.trail[0][2];
    if (dx * dx + dz * dz < kSample * kSample) {
      f.trail[0][1] = p[1];
      return;
    }
  }
  const int keep = f.trail_count < 256 ? f.trail_count : 255;
  memmove(f.trail[1], f.trail[0], sizeof(f.trail[0]) * (size_t)keep);
  memcpy(f.trail[0], p, sizeof(f.trail[0]));
  f.trail_count = keep + 1;
}

// The point `distance` along the trail behind the leader at `leader`; 0 if the trail is shorter.
static int trail_point(const float leader[3], float distance, float out[3]) {
  const float *prev = leader;
  for (int i = 0; i < f.trail_count; ++i) {
    const float *p = f.trail[i];
    const float dx = p[0] - prev[0], dy = p[1] - prev[1], dz = p[2] - prev[2];
    const float len = sqrtf(dx * dx + dz * dz);
    if (len >= distance && len > 0.0f) {
      const float t = distance / len;
      out[0] = prev[0] + dx * t;
      out[1] = prev[1] + dy * t;
      out[2] = prev[2] + dz * t;
      f.trail_count = i + 2 < f.trail_count ? i + 2 : f.trail_count;  // drop what lies beyond
      return 1;
    }
    distance -= len;
    prev = p;
  }
  return 0;
}

// --- the ball -------------------------------------------------------------------------------

static void set_visible(CPUState *s, int visible) {
  if (!f.model || f.visible == visible)
    return;
  const uint32_t args[2] = {f.model, (uint32_t)visible};
  call(s, kSetVisibility, args, 2, NULL);
  f.visible = visible;
}

static int fx_call(CPUState *s, uint32_t address, int which) {
  if (!f.fx_loaded[which])
    return 0;
  const uint32_t args[3] = {f.sequence, f.fx[which], 4};
  return (int)call(s, address, args, 3, NULL);
}

static void write16(CPUState *s, uint32_t a, uint16_t v) {
  uint8_t *p = memory(s, a, 2);
  if (p) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
  }
}

// Plays the effects in `which` (enum BallFx), one after another, in a game thread via
// kEffectsPlay. It frees each one when it ends, so they are loaded again afterwards.
static int fx_play(CPUState *s, const int *which, int n) {
  f.ball_frames = 0;
  f.fx_seen = 0;
  int count = 0;
  write32(s, kScratch, f.sequence);
  for (int k = 0; k < n; ++k) {
    const int i = which[k];
    if (!f.fx_loaded[i])
      continue;
    write16(s, kScratch + 4u + 4u * (uint32_t)count, (uint16_t)f.fx[i]);
    write16(s, kScratch + 6u + 4u * (uint32_t)count, 4);
    ++count;
  }
  const uint32_t args[6] = {kScratch, kScratch + 4u, (uint32_t)count, 1, 0, 0};
  return count && run_in_thread(s, kEffectsPlay, args, 6);
}

// The ball itself: the send-out's model nodes (kind 2: a GSmodel the effect places and
// animates; the Poke Ball flying in and opening). It is true to size next to the Pokemon,
// which leaves it a few pixels from the field camera (battles film it close up), so while the
// send-out plays it is drawn kBallGrow times larger about its own origin, which also stretches
// its flight back toward Wes. The sequence's effect lists: WazaEffect +0x68 and +0x6C; nodes
// from +0x24, kind +0x04, model +0xA4, next +0xA8. (The recall effect has no ball near the
// Pokemon - in battle it sits in the trainer's hand, off camera - so a recall replays this one.)
static void grow_ball(CPUState *s) {
  const char *env = getenv("FOLLOWER_BALL_GROW");  // testing: another size
  const float grow = env && *env ? (float)atof(env) : 3.0f;
  const uint32_t lists[2] = {read32(s, f.sequence + 0x68u), read32(s, f.sequence + 0x6Cu)};
  for (int l = 0; l < 2; ++l) {
    if (!lists[l] || (l == 1 && lists[1] == lists[0]))
      continue;
    uint32_t node = read32(s, lists[l] + 0x24u);
    for (int n = 0; node && n < 64; ++n, node = read32(s, node + 0xA8u)) {
      const uint32_t kind = read32(s, node + 0x04u), model = read32(s, node + 0xA4u);
      if (kind == 2 && model && grow != 1.0f) {
        const float scale[3] = {grow, grow, grow};
        call_with_vec(s, kSetScale, model, scale);
      }
    }
  }
}

// The effect `which` has played out: seen playing and now stopped, or never seen within ~1 s
// (the thread did not get to it), or ~6 s in all. Never leaves the Pokemon invisible.
static int fx_done(CPUState *s, int which) {
  const int playing = fx_call(s, kEffectPlaying, which);
  if (playing)
    f.fx_seen = 1;
  return f.ball_frames > 180 || (!playing && (f.fx_seen || f.ball_frames > 30));
}

// The thread freed what it played; the next throw loads them again.
static void fx_played(void) {
  for (int i = 0; i < kFxCount; ++i)
    f.fx_loaded[i] = 0;
  f.fx_want = 0;
  f.ball = kBallIdle;
}

// The ball's effect ids, from the item the Pokemon was caught in.
static void read_ball_effects(CPUState *s) {
  const uint32_t ball_args[4] = {f.mon, 0, 0x73, 0};
  const uint32_t ball = call(s, kPokemonStatus, ball_args, 4, NULL) & 0xFFFFu;
  static const uint32_t fields[kFxCount] = {13, 14, 15};
  for (int i = 0; i < kFxCount; ++i) {
    const uint32_t args[4] = {0, ball ? ball : 4u, fields[i], 0};  // 4: Poke Ball
    f.fx[i] = call(s, kItemStatus, args, 4, NULL);
    f.fx_loaded[i] = 0;
  }
  f.fx_want = 0;
  fprintf(stderr, "[follower] ball %u: effects in %X open %X out %X\n", ball, f.fx[kFxIn],
          f.fx[kFxOpen], f.fx[kFxOut]);
}

// Loads the next effect the throw needs, in a game thread (fn_801DDD28 reads files and waits).
//
// Only during a throw, while Wes stands locked in it: a load's thread is ended by any scene
// change (a battle, a door), and one cut off part-way left the game's file loading broken - the
// next effect load after the battle never finished and the game froze. Loaded in the background
// after every throw, as they first were, a trainer or a door could cut one off at any time.
static void load_next_effect(CPUState *s) {
  if (f.fx_loading)
    return;
  int which = -1;
  for (int i = 0; i < kFxCount && which < 0; ++i)
    if ((f.fx_want & (1u << i)) && !f.fx_loaded[i] && f.fx[i])
      which = i;
  if (which < 0)
    return;
  const uint32_t args[4] = {f.sequence, f.fx[which], 4, 0};
  if (!run_in_thread(s, kEffectLoad, args, 4)) {
    f.fx_want &= ~(1u << which);
    return;
  }
  f.fx_loading = 1;
  f.fx_loading_which = which;
  f.fx_loading_seq = f.sequence;
  f.fx_stale = 0;
  f.load_frames = 0;
}

// Every effect the throw needs is loaded (or failed and was dropped from the list).
static int fx_ready(void) {
  if (f.fx_loading)
    return 0;
  for (int i = 0; i < kFxCount; ++i)
    if ((f.fx_want & (1u << i)) && !f.fx_loaded[i] && f.fx[i])
      return 0;
  return 1;
}

// A load whose thread never came back (a floor change ends the floor's threads): after ~15 s
// stop waiting for it, so the follower and its effects can load again.
static void expire_loads(void) {
  if (!f.loading && !f.fx_loading) {
    f.load_frames = 0;
    return;
  }
  if (++f.load_frames < 900)
    return;
  fprintf(stderr, "[follower] a %s load never returned; giving up on it\n",
          f.loading ? "model" : "effect");
  f.loading = f.in_loader = f.loading_stale = 0;
  f.fx_loading = f.in_fx_loader = f.fx_stale = 0;
  f.load_frames = 0;
  f.want = 1;
}

static void free_effects(CPUState *s) {
  for (int i = 0; i < kFxCount; ++i) {
    if (f.fx_loaded[i])
      fx_call(s, kEffectFree, i);
    f.fx_loaded[i] = 0;
  }
  f.fx_want = 0;
  f.ball = kBallIdle;
}

static void play_cry(CPUState *s) {
  const uint32_t voice_args[4] = {0, f.species, 0x61, 0};
  const uint32_t voice = call(s, kPokemonStatus, voice_args, 4, NULL);
  if (voice) {
    const uint32_t args[3] = {voice, 0, 0};
    call(s, kPlaySound, args, 3, NULL);
  }
}

// True on the frame a Z tap ends: Z released within kTapFrames with nothing else pressed.
static int z_tapped(CPUState *s) {
  uint16_t held = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t entry = kPads + i * 0x6Cu;
    if (read32(s, entry) == 1) {
      held = read16(s, entry + 0x24u);
      break;
    }
  }
  int tapped = 0;
  if (held & kButtonZ) {
    if (held & ~kButtonZ & ~f.prev_held)
      f.z_combo = 1;
    if (!f.z_frames)
      f.z_combo = (held & ~kButtonZ) != 0;
    ++f.z_frames;
  } else if (f.z_frames) {
    tapped = !f.z_combo && f.z_frames <= kTapFrames;
    f.z_frames = 0;
    f.z_combo = 0;
  }
  f.prev_held = held;
  return tapped;
}

// Wes's people entry (group 0, index 100), or 0.
static uint32_t people_entry(CPUState *s, uint32_t index) {
  const uint32_t base = read32(s, kPeople), count = read32(s, kPeopleCount);
  for (uint32_t i = 0; base && i < count && i < 512; ++i) {
    const uint32_t e = base + i * 0xDCu;
    const uint8_t *active = memory(s, e, 1);
    if (active && *active && read32(s, e + 0x28) == 0 && read32(s, e + 0x2C) == index)
      return e;
  }
  return 0;
}

// The people animator also records the clip in Wes's entry (+0x48 the motion clip his walk and
// idle logic think is playing, +0xD8 its rate). Left as the throw, his walk broke; so they are
// kept here and put back, with his own clip playing again, when the throw ends.
static struct {
  int frames;           // frames until the throw ends (0: none in progress)
  uint32_t clip, rate;
} wes_saved;

// Wes's throw (or the recall gesture): his lunge clip, with heroMove held for its length.
static void wes_throw(CPUState *s) {
  const uint32_t entry = people_entry(s, 100);
  if (!entry)
    return;
  if (!wes_saved.frames) {
    wes_saved.clip = read32(s, entry + 0x48u);
    wes_saved.rate = read32(s, entry + 0xD8u);
  }
  wes_saved.frames = kThrowLock;
  const uint32_t lock[1] = {kThrowLock};
  call(s, kHeroLock, lock, 1, NULL);
  const uint32_t args[5] = {0, 100, kThrowClip, 0, 0};
  call(s, kPeopleAnimate, args, 5, NULL);
}

static void wes_throw_end(CPUState *s) {
  if (!wes_saved.frames || --wes_saved.frames)
    return;
  const uint32_t entry = people_entry(s, 100);
  if (!entry)
    return;
  if ((int32_t)wes_saved.clip >= 0) {
    const uint32_t args[5] = {0, 100, wes_saved.clip, 0, 1};  // his own clip, looping
    call(s, kPeopleAnimate, args, 5, NULL);
  }
  write32(s, entry + 0x48u, wes_saved.clip);
  write32(s, entry + 0xD8u, wes_saved.rate);
}

// The ball leaves Wes's hand: send out (open, release) or call back (the red beam).
static const int kSendOut[2] = {kFxOpen, kFxOut};

static void release_ball(CPUState *s) {
  const int waited = f.ball_frames;
  f.ball_frames = 0;
  if (!f.out) {
    f.out = 1;
    const char *no_fx = getenv("FOLLOWER_NO_FX");  // testing: appear without effects
    f.ball = kBallOpening;  // before the start: camera_start_patch keys on it
    if (!f.fx_loaded[kFxOpen] || !f.fx_loaded[kFxOut] || (no_fx && no_fx[0] == '1') ||
        !fx_play(s, kSendOut, 2)) {  // no effects to play: just appear
      f.ball = kBallIdle;
      set_visible(s, 1);
      play_cry(s);
    }
    fprintf(stderr, "[follower] sent out (%d frames after the tap)\n", waited);
  } else {
    // The ball comes back to the Pokemon and opens, then the beam: the send-out's ball, played
    // first, then the recall. Without the ball effect, just the beam.
    static const int with_ball[2] = {kFxOpen, kFxIn}, beam_only[1] = {kFxIn};
    const int ball = f.fx_loaded[kFxOpen];
    f.ball = ball ? kBallRecallBall : kBallRecalling;
    if (!f.fx_loaded[kFxIn] || !fx_play(s, ball ? with_ball : beam_only, ball ? 2 : 1)) {
      f.ball = kBallIdle;
      f.landed = 0;
      f.out = 0;
      set_visible(s, 0);
    }
    fprintf(stderr, "[follower] called back (%d frames after the tap)\n", waited);
  }
}

// A Z tap: Wes throws; the Pokemon comes out where the ball lands (or goes back in), with the
// ball's effects; then it walks on behind him.
static void update_ball(CPUState *s, int tapped, uint32_t wes_model, const float wes[3]) {
  if (!f.model)
    return;
  if (tapped && f.ball == kBallIdle) {
    memcpy(f.wes_at_throw, wes, sizeof(f.wes_at_throw));
    f.landed = 1;  // held where it is (or where it lands) until Wes walks on
    if (!f.out) {  // the effects play at the model's position: put it where the ball lands
      landing_point(s, wes_model, wes, f.pos);
      call_with_vec(s, kSetPosition, f.model, f.pos);
    } else {
      // Facing away from Wes, as when it was sent out: the ball's flight is laid out behind the
      // Pokemon, where its trainer stands in battle, so it comes from Wes.
      const float ax = f.pos[0] - wes[0], az = f.pos[2] - wes[2];
      if (ax * ax + az * az > 0.01f)
        f.yaw = atan2f(ax, az);
    }
    // The effects this throw plays, loaded now while Wes is held in it (load_next_effect).
    f.fx_want = f.out ? (1u << kFxIn) | (1u << kFxOpen) : (1u << kFxOpen) | (1u << kFxOut);
    wes_throw(s);
    f.ball = kBallThrowing;
    f.ball_frames = 0;
  }
  if (f.ball == kBallThrowing)
    load_next_effect(s);
  if (f.ball == kBallIdle) {
    f.ball_frames = 0;
    return;
  }
  ++f.ball_frames;
  if (f.ball == kBallOpening || f.ball == kBallAppearing || f.ball == kBallRecallBall)
    grow_ball(s);
  switch (f.ball) {
  case kBallThrowing:
    // The ball leaves his hand once the effects are in; he stays held in the throw until then
    // (~2 s at most: then it goes ahead with whatever has loaded).
    if (f.ball_frames >= kThrowRelease && (fx_ready() || f.ball_frames > 120)) {
      f.fx_want = 0;
      release_ball(s);
    } else if (f.ball_frames >= kThrowLock - 2) {
      const uint32_t lock[1] = {2};
      call(s, kHeroLock, lock, 1, NULL);
      if (wes_saved.frames && wes_saved.frames < 2)
        wes_saved.frames = 2;  // and in the throw pose
    }
    break;
  case kBallOpening:  // the ball opens; when the release starts the Pokemon appears and cries
    if (fx_call(s, kEffectPlaying, kFxOut) || f.ball_frames > 180) {
      set_visible(s, 1);
      play_cry(s);
      f.ball = kBallAppearing;
      f.ball_frames = 0;
      f.fx_seen = 1;
    }
    break;
  case kBallAppearing:
    if (fx_done(s, kFxOut))
      fx_played();
    break;
  case kBallRecallBall:  // the ball arrives and opens; the beam starts when it is done
    {
      // The open effect hides its Pokemon (on a send-out it is still in the ball): keep it in
      // view until the beam takes it.
      const uint32_t args[2] = {f.model, 1};
      call(s, kSetVisibility, args, 2, NULL);
      f.visible = 1;
    }
    if (fx_call(s, kEffectPlaying, kFxIn) || f.ball_frames > 180) {
      f.ball = kBallRecalling;
      f.ball_frames = 0;
      f.fx_seen = 1;
    }
    break;
  case kBallRecalling:  // the red beam; then it is gone
    if (fx_done(s, kFxIn)) {
      set_visible(s, 0);
      f.out = 0;
      f.landed = 0;
      fx_played();
    }
    break;
  default:
    break;
  }
}

// The ball effects carry the battle camera's moves: starting one hands the scene camera to
// battleCameraStartWaza (0x801D30BC), which in the field swung it to a close-up. While one of
// our effects plays, the call returns at once; otherwise the game's own code runs.
static void camera_start_patch(CPUState *s) {
  if (f.ball == kBallIdle || !f.sequence)
    return;  // not ours: leave pc alone and the original runs
  fprintf(stderr, "[follower] effect camera skipped (owner %08X sequence %08X)\n", s->gpr[3],
          s->gpr[4]);
  s->pc = s->lr;
}

// --- hooks ----------------------------------------------------------------------------------

static void effect_load_entry(CPUState *s) {
  if (f.fx_loading && !f.in_fx_loader && s->gpr[3] == f.fx_loading_seq)
    f.in_fx_loader = 1;
}

static void effect_load_return(CPUState *s) {
  if (!f.in_fx_loader)
    return;
  f.in_fx_loader = 0;
  f.fx_loading = 0;
  if (f.fx_stale) {  // for a sequence that is gone: drop it
    f.fx_stale = 0;
    fprintf(stderr, "[follower] a stale effect load returned\n");
    return;
  }
  const int which = f.fx_loading_which;
  if (which < 0 || which >= kFxCount)
    return;
  f.fx_loaded[which] = (s->gpr[3] & 0xFFu) != 0;
  fprintf(stderr, "[follower] effect %X %s\n", f.fx[which],
          f.fx_loaded[which] ? "loaded" : "failed to load");
  if (!f.fx_loaded[which])
    f.fx_want &= ~(1u << which);
}

static void floor_init_return(CPUState *s) {
  release(s);
  f.want = 1;
  f.trail_count = 0;
  f.frames = 0;
  f.floor_checked = 0;
  f.indoors = 0;
  wes_saved.frames = 0;  // a new floor sets Wes up afresh
  fprintf(stderr, "[follower] floor loaded\n");
}

// A battle sets up its own sequence pool (fn_801DAEF8, battle_FightStart) over whatever is
// there: put ours away first so its memory is not lost.
static void battle_pool_entry(CPUState *s) {
  if (!f.sequence && !f.own_pool)
    return;
  release(s);
  f.want = 1;
  f.frames = 0;
  fprintf(stderr, "[follower] a battle starts: follower put away\n");
}

// A menu is about to show a Pokemon model: give it the field as the game leaves it, with no
// sequence pool. The follower comes back on the next field frame.
static void menu_model_entry(CPUState *s) {
  if (!f.sequence && !f.own_pool)
    return;
  release(s);
  f.want = 1;
  f.frames = 0;
  fprintf(stderr, "[follower] a menu shows a Pokemon: follower put away\n");
}

static void create_entry(CPUState *s) {
  if (f.loading && s->gpr[3] == f.mon)
    f.in_loader = 1;
}

static void create_return(CPUState *s) {
  if (!f.in_loader)
    return;
  f.in_loader = 0;
  f.loading = 0;
  if (f.loading_stale) {  // started before a battle or menu took the pool: not ours to use
    f.loading_stale = 0;
    f.want = 1;
    fprintf(stderr, "[follower] a stale model load returned\n");
    return;
  }
  f.sequence = s->gpr[3];
  if (!f.sequence) {
    // Which of fn_801DE190's early returns: model index, resource table, residency, pool.
    int ok = 0;
    const uint32_t status[4] = {0, f.species, 0x66, 0};
    const uint32_t index = call(s, 0x8012640Cu, status, 4, &ok) & 0xFFFFu;
    const uint32_t group = read32(s, 0x80370BD0u + index * 12u);
    const uint32_t resource = read32(s, 0x80370BD0u + index * 12u + 4u);
    const uint32_t state_args[1] = {group};
    const uint32_t state = call(s, 0x8017B2CCu, state_args, 1, NULL);
    const uint32_t res_args[2] = {group, resource};
    const uint32_t res = call(s, 0x800F9318u, res_args, 2, NULL);
    const uint32_t pooled = call(s, 0x801DB154u, NULL, 0, NULL);
    if (pooled) {
      const uint32_t free_args[1] = {pooled};
      call(s, kSequenceFree, free_args, 1, NULL);
    }
    const uint32_t pool = read32(s, 0x80467CC0u);
    const uint32_t slots = read16(s, 0x80467CC4u);
    uint32_t used = 0;
    for (uint32_t i = 0; pool && i < slots && i < 64; ++i)
      used += memory(s, pool + i * 0x8Cu + 0x74u, 1) && *memory(s, pool + i * 0x8Cu + 0x74u, 1);
    fprintf(stderr,
            "[follower] the loader returned no model: index %u (limit %u) group %u resource %08X "
            "state %d resource ptr %08X pool %08X (array %08X, %u slots, %u in use)\n",
            index, read32(s, 0x80478CD0u), group, resource, (int)state, res, pooled, pool, slots,
            used);
    return;
  }
  f.pool = read32(s, kPool);
  f.placed = 0;
  const uint32_t args[2] = {f.sequence, 1};
  f.model = call(s, kSequenceModel, args, 1, NULL);
  call(s, kSequenceState, args, 2, NULL);
  fprintf(stderr, "[follower] species %u ready: sequence %08X model %08X\n", f.species,
          f.sequence, f.model);
  // In its ball until sent out (or back out, if it was out before the floor or menu change).
  f.visible = 1;
  set_visible(s, f.out);
  read_ball_effects(s);
}

// FOLLOWER_TEST_BATTLE=<hex encounter id> (testing): start that battle once, 5 s after the
// Pokemon is out, the way the game's debug menu does (a thread running fn_8020DAD0(id)).
static void debug_battle(CPUState *s) {
  static int done;
  if (done || f.frames < 150)
    return;
  const char *env = getenv("FOLLOWER_TEST_BATTLE");
  const char *in_ball = getenv("FOLLOWER_TEST_BATTLE_IN_BALL");  // testing: don't wait for out
  if (env && *env && !(in_ball && *in_ball) && !(f.model && f.out && f.ball == kBallIdle))
    return;  // with the Pokemon out, to see it put away and brought back
  if (!env || !*env) {
    done = 1;
    return;
  }
  done = 1;
  const uint32_t id = (uint32_t)strtoul(env, NULL, 16);
  const uint32_t args[1] = {id};
  const int thread = run_in_thread(s, 0x8020DAD0u, args, 1);
  fprintf(stderr, "[follower] test battle 0x%X %s (heroMove leader word %08X, Rui flags %04X)\n",
          id, thread ? "started" : "failed", read32(s, kHeroMove), read16(s, kHeroMove + 0x24u));
}

// The walk. Colosseum's Pokemon models are battle models: idle, attacks, a hit, a faint and a
// few poses, and no walk cycle (Umbreon's nine clips run 5 to 150 frames; none is a loop to walk
// to). So the idle keeps playing and the gait is drawn on top of it: a hop every kStride units
// travelled, as high as the pace allows, settling smoothly when it stops.
static const float kStride = 9.0f;      // units per hop
static const float kHop = 1.6f;         // hop height at a walk
static void gait(const float pos[3], float shown[3]) {
  static float last[3], phase, amount;
  const float mx = pos[0] - last[0], mz = pos[2] - last[2];
  float step = sqrtf(mx * mx + mz * mz);
  memcpy(last, pos, sizeof(last));
  if (step > 8.0f)  // a placement or a snap, not a step
    step = 0.0f;
  const float pi = 3.14159265f;
  if (step > 0.05f) {
    // Walking: hops as high as the pace, eased in so a start is not a jolt.
    const float pace = step > 1.2f ? 1.0f : step / 1.2f;
    amount += (pace - amount) * 0.35f;
    phase += step * (pi / kStride);
  } else if (phase > 0.0f) {
    // Stopped mid-hop: finish it at a walk's speed and land, rather than hang in the air.
    const float ground = ceilf(phase / pi) * pi;
    phase += pi / 6.0f;
    if (phase >= ground)
      phase = 0.0f;
  }
  if (phase == 0.0f)
    amount = 0.0f;  // on the ground; the next walk eases in again
  if (phase > 2.0f * pi)
    phase -= 2.0f * pi;
  memcpy(shown, pos, sizeof(float) * 3);
  shown[1] += fabsf(sinf(phase)) * kHop * amount;
}

// FOLLOWER_WARP="<floor id hex> <x> <y> <z>[;<next>...]" (testing): go to each in turn, the
// first 3 s into the field and the rest 20 s apart, the way the game's own exits do
// (floorChangePos(floor, direction, x, y, z)).
static void debug_warp(CPUState *s) {
  static unsigned ticks;
  static int next;
  const char *env = getenv("FOLLOWER_WARP");
  if (!env || ++ticks < 90u + 600u * (unsigned)next)
    return;
  const char *stop = env;
  for (int i = 0; stop && i < next; ++i) {
    stop = strchr(stop, ';');
    if (stop)
      ++stop;
  }
  ++next;
  unsigned floor_id = 0;
  float x = 0, y = 0, z = 0;
  if (!stop || sscanf(stop, "%x %f %f %f", &floor_id, &x, &y, &z) != 4 || !api || !api->call_guest)
    return;
  CPUState c = *s;
  c.gpr[3] = floor_id;
  c.gpr[4] = 0;
  c.fpr[1] = x;
  c.fpr[2] = y;
  c.fpr[3] = z;
  const int ok = api->call_guest(api->user_data, &c, 0x80113778u);  // floorChangePos
  fprintf(stderr, "[follower] warp to floor %X (%.0f %.0f %.0f) %s\n", floor_id, (double)x,
          (double)y, (double)z, ok ? "requested" : "failed");
}

// heroMoveMain's own early-outs: a menu (which builds its own model pool, so ours must be gone
// first) or scene mode 6.
static int field_paused(CPUState *s) {
  const uint32_t menu_args[1] = {0xCA};
  if (call(s, 0x80102620u, menu_args, 1, NULL) != 0)  // menuIsCheck(0xCA)
    return 1;
  if (call(s, 0x801337B0u, NULL, 0, NULL) != 0)       // dbgMenuIsOpen
    return 1;
  return call(s, 0x80177A38u, NULL, 0, NULL) == 6;    // GSscene_GetMode
}

static void hero_move_return(CPUState *s) {
  if (field_paused(s)) {
    if (f.sequence || f.own_pool || f.loading) {
      if (!f.loading) {
        release(s);
        fprintf(stderr, "[follower] menu open: follower put away\n");
      }
      f.want = 1;
    }
    f.frames = 0;
    return;
  }
  ++f.frames;
  expire_loads();
  if (f.sequence && !sequence_alive(s)) {
    // A battle or menu took the pool: forget the model (it is not ours to free) and rebuild.
    fprintf(stderr, "[follower] the sequence pool changed; rebuilding the follower\n");
    for (int i = 0; i < kFxCount; ++i)
      f.fx_loaded[i] = 0;
    f.ball = kBallIdle;
    f.sequence = 0;
    f.model = 0;
    f.pool = 0;
    f.own_pool = 0;
    f.want = 1;
    f.frames = 0;
  }
  // A different lead (the party was reordered or the Pokemon evolved): rebuild.
  if (f.sequence && f.frames % 30 == 0) {
    uint16_t species = 0;
    const uint32_t mon = lead_pokemon(s, &species);
    if (mon != f.mon || species != f.species || read32(s, mon + 4u) != f.pid) {
      fprintf(stderr, "[follower] the lead changed (species %u -> %u): rebuilding\n", f.species,
              species);
      release(s);
      f.want = 1;
      f.frames = 0;
    }
  }
  // Only outdoors: inside a building the Pokemon stays in its ball (and is back out on the
  // next outdoor floor if it was out before).
  if (!f.floor_checked && f.frames > 1) {
    f.floor_checked = 1;
    f.indoors = floor_indoors(s);
    const char *anywhere = getenv("FOLLOWER_INDOORS");  // testing: allow it indoors too
    if (anywhere && anywhere[0] == '1')
      f.indoors = 0;
    if (f.indoors && !f.loading)
      release(s);
  }
  if (f.indoors && f.sequence && !f.loading)
    release(s);
  if (f.want && !f.indoors && f.floor_checked && !f.loading && !f.sequence && f.frames > 2) {
    f.want = 0;
    start_loading(s);
  }
  float wes[3];
  const uint32_t wes_model = people_model(s, 100);
  if (!model_position(s, wes_model, wes))
    return;
  static float wes_last[3];
  const float sx = wes[0] - wes_last[0], sz = wes[2] - wes_last[2];
  float wes_step = sqrtf(sx * sx + sz * sz);  // how far Wes moved this frame
  if (wes_step > 10.0f)
    wes_step = 0.0f;
  memcpy(wes_last, wes, sizeof(wes_last));
  push_trail(wes);
  debug_battle(s);
  debug_warp(s);
  wes_throw_end(s);

  // Rui one place further back, behind the Pokemon - only while she follows Wes (not when she
  // leads, or is played by someone else in co-op).
  const int rui_follows = read32(s, kHeroMove) == 0 && (read16(s, kHeroMove + 0x24u) & 1u);
  const int pressed = z_tapped(s);
  const int want_spacing = f.model && f.out && rui_follows;
  if (want_spacing != f.rui_spaced) {
    write32(s, kRuiSpacing, f32_bits(want_spacing ? 2.0f * kDistance : kDistance));
    f.rui_spaced = want_spacing;
  }

  if (!f.model)
    return;
  if (f.placed) {  // Wes moved a long way at once (a warp within the floor): catch up
    const float jx = wes[0] - f.pos[0], jz = wes[2] - f.pos[2];
    if (jx * jx + jz * jz > kSnap * kSnap) {
      f.placed = 0;
      f.landed = 0;
      f.trail_count = 0;
      push_trail(wes);
    }
  }
  if (!f.placed) {  // just loaded: start where Rui stands, behind Wes
    float rui[3];
    if (!model_position(s, people_model(s, 101), rui)) {
      memcpy(rui, wes, sizeof(rui));
      rui[2] -= kDistance;
    }
    memcpy(f.pos, rui, sizeof(f.pos));
    f.placed = 1;
  }
  update_ball(s, pressed, wes_model, wes);  // a send-out puts it where the ball lands
  float target[3];
  if (!trail_point(wes, kDistance, target))
    memcpy(target, f.pos, sizeof(target));
  const float dx = target[0] - f.pos[0], dz = target[2] - f.pos[2];
  const float gap = sqrtf(dx * dx + dz * dz);
  if (f.landed) {
    // Where the ball landed until Wes walks on; then it trots back onto his trail, a little
    // faster than he moves, and follows from there.
    const float mx = wes[0] - f.wes_at_throw[0], mz = wes[2] - f.wes_at_throw[2];
    if (f.ball == kBallIdle && mx * mx + mz * mz > 4.0f) {
      const float step = wes_step + kRejoin;
      if (gap <= step) {
        memcpy(f.pos, target, sizeof(f.pos));
        f.landed = 0;
      } else {
        f.pos[0] += dx * step / gap;
        f.pos[1] += (target[1] - f.pos[1]) * step / gap;
        f.pos[2] += dz * step / gap;
      }
      if (gap > 0.01f)
        f.yaw = atan2f(dx, dz);
    }
  } else {
    if (gap > 0.01f)
      f.yaw = atan2f(dx, dz);
    memcpy(f.pos, target, sizeof(f.pos));
  }
  float shown[3];
  gait(f.pos, shown);
  const float rotation[3] = {0.0f, f.yaw, 0.0f};
  call_with_vec(s, kSetPosition, f.model, shown);
  call_with_vec(s, kSetRotation, f.model, rotation);
  if (f.frames % 120 == 0)
    fprintf(stderr, "[follower] wes (%.1f %.1f %.1f) pokemon (%.1f %.1f %.1f) yaw %.2f trail %d\n",
            (double)wes[0], (double)wes[1], (double)wes[2], (double)f.pos[0], (double)f.pos[1],
            (double)f.pos[2], (double)f.yaw, f.trail_count);
}

static void on_load(const ModernGekkoModHostApi *host) {
  api = host;
  fprintf(stderr, "[follower] loaded (guest calls %s)\n",
          host && host->call_guest ? "available" : "unavailable");
}

static const ModernGekkoModPatch patches[] = {
    RECOMP_PATCH(0x801D30BCu, camera_start_patch),  // battleCameraStartWaza
};

// Literal addresses: the hook-aware module builds read them from this table.
static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK_RETURN(0x8012FCD4u, floor_init_return),  // heroMoveInit
    RECOMP_HOOK(0x80121C18u, create_entry),              // pokemonCreateSequence
    RECOMP_HOOK_RETURN(0x80121C18u, create_return),
    RECOMP_HOOK_RETURN(0x8012EBD4u, hero_move_return),   // heroMoveMain
    RECOMP_HOOK(0x8010A88Cu, menu_model_entry),          // menu Pokemon model loader
    RECOMP_HOOK(0x801DAEF8u, battle_pool_entry),         // battle sequence pool setup
    RECOMP_HOOK(0x801DDD28u, effect_load_entry),         // sequence effect loader
    RECOMP_HOOK_RETURN(0x801DDD28u, effect_load_return),
};

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    NULL,
    0,
    patches,
    sizeof(patches) / sizeof(patches[0]),
    hooks,
    sizeof(hooks) / sizeof(hooks[0]),
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    on_load,
    NULL,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
  return &descriptor;
}
