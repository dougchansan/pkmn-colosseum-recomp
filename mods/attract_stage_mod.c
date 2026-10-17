/* Attract-video remake staging (capture builds only, not shipped to players).
 *
 * Stages the battles and field shots of the title attract movie in-engine so
 * they can be recorded at full resolution (research: C:/dev/colo-perf/
 * research/attract). This first version checks the guest-call bridge
 * (ModernGekkoModHostApi::call_guest): ATTRACT_STAGE_TEST=bridge calls the
 * encounter-record resolver and compares it with the table in memory. */
#include "moderngekko/mod_abi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const ModernGekkoModHostApi *api;
static uint32_t pad_buffer;  /* PADStatus array of the PADRead in progress */
static void message_text(CPUState *s, uint32_t key, char *out, size_t size);
static void parse_dump(const char *line);
static void update_dumps(int shot_poll);
static unsigned frames;
static int bridge_done;

static uint8_t *memory(CPUState *s, uint32_t a, uint32_t size) {
  uint32_t offset = a - 0x80000000u;
  if (!s->ram || a < 0x80000000u || offset > s->ram_size || size > s->ram_size - offset)
    return NULL;
  return s->ram + offset;
}

static uint32_t read32(CPUState *s, uint32_t a) {
  const uint8_t *p = memory(s, a, 4);
  return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}

/* Calls a plain guest function with up to six arguments; returns its r3 (0
   when the bridge failed). */
static uint32_t call_guest6(CPUState *s, uint32_t address, const uint32_t *args, int count,
                            int *ok) {
  CPUState copy = *s;
  for (int i = 0; i < count && i < 6; ++i)
    copy.gpr[3 + i] = args[i];
  *ok = api && api->call_guest && api->call_guest(api->user_data, &copy, address);
  return *ok ? copy.gpr[3] : 0;
}

static uint32_t call_guest(CPUState *s, uint32_t address, uint32_t r3, uint32_t r4,
                           int *ok) {
  const uint32_t args[2] = {r3, r4};
  return call_guest6(s, address, args, 2, ok);
}

/* Starts encounter `id` the way the game's own debug menu does
   (src/game/gs_task.c:1680-1725): a game thread running the battle launcher
   fn_8020DAD0(id), created in the current floor's context. */
static int start_battle(CPUState *s, uint32_t id) {
  int ok = 0;
  if (call_guest(s, 0x801EF63Cu, 0, 0, &ok) != 0 || !ok) {
    fprintf(stderr, "[attract-stage] a battle is already running\n");
    return 0;
  }
  const uint32_t floor_context = call_guest(s, 0x800FF560u, 0, 0, &ok);
  if (!ok)
    return 0;
  const uint32_t create[6] = {1, floor_context, 0x4000, 1, 1, 0x8020DAD0u};
  const uint32_t task = call_guest6(s, 0x800F07A8u, create, 6, &ok);
  if (!ok || !task) {
    fprintf(stderr, "[attract-stage] GSthreadCreate failed\n");
    return 0;
  }
  const uint32_t set_args[3] = {task, 1, id};
  call_guest6(s, 0x800F0654u, set_args, 3, &ok);
  fprintf(stderr, "[attract-stage] battle 0x%X started (thread %08X, floor context %08X)\n", id,
          task, floor_context);
  return ok;
}

static void test_bridge(CPUState *s) {
  /* fightEncountDataBiosGetPtr(id): base = *(0x80478F54), stride 0x38. */
  const uint32_t id = 0x20A;
  const uint32_t expected = read32(s, 0x80478F54u) + id * 0x38u;
  int ok = 0;
  const uint32_t got = call_guest(s, 0x8020E0F8u, id, 0, &ok);
  fprintf(stderr, "[attract-stage] bridge %s: encounter 0x%X record %08X (expected %08X)\n",
          ok && got == expected ? "OK" : "FAILED", id, got, expected);
  int ok2 = 0;
  const uint32_t battle = call_guest(s, 0x801EF63Cu, 0, 0, &ok2);
  fprintf(stderr, "[attract-stage] battle-in-progress getter -> %u (call %s)\n", battle,
          ok2 ? "ok" : "failed");
}

static uint16_t read16(CPUState *s, uint32_t a) {
  const uint8_t *p = memory(s, a, 2);
  return p ? (uint16_t)(((uint32_t)p[0] << 8) | p[1]) : 0;
}

/* Dumps what the staging needs to rewrite: the party, an encounter record,
   its trainers and their Pokemon templates. */
static void dump_tables(CPUState *s, uint32_t encounter) {
  const uint32_t save = read32(s, 0x8047ADB8u);
  for (int i = 0; i < 6; ++i) {
    const uint32_t mon = save + 0xA0u + (uint32_t)i * 0x138u;
    fprintf(stderr, "[attract-stage] party %d @%08X species %u level %u hp %u/%u moves %u %u %u %u\n",
            i, mon, read16(s, mon), memory(s, mon + 0x60, 1) ? *memory(s, mon + 0x60, 1) : 0,
            read16(s, mon + 0x8A), read16(s, mon + 0x8C), read16(s, mon + 0x78),
            read16(s, mon + 0x7C), read16(s, mon + 0x80), read16(s, mon + 0x84));
  }
  const uint32_t record = read32(s, 0x80478F54u) + encounter * 0x38u;
  const uint8_t *r = memory(s, record, 0x38);
  if (!r)
    return;
  fprintf(stderr, "[attract-stage] encounter 0x%X @%08X kind %u type %u stage 0x%X bgm %u\n",
          encounter, record, r[0], r[1], read16(s, record + 4), read32(s, record + 0xC));
  const uint32_t trainers = read32(s, 0x80478F24u), templates = read32(s, 0x80478F14u);
  for (int i = 0; i < 4; ++i) {
    const uint16_t id = read16(s, record + 0x18u + (uint32_t)i * 8u);
    if (!id)
      continue;
    const uint32_t trainer = trainers + id * 0x34u;
    const uint16_t first = read16(s, trainer + 4);
    fprintf(stderr, "[attract-stage]  slot %d trainer %u kind %u first template %u device %u\n", i,
            id, read16(s, trainer + 2), first, read32(s, record + 0x1Cu + (uint32_t)i * 8u));
    for (uint32_t t = first; first && t < first + 6u; ++t) {
      const uint32_t row = templates + t * 0x50u;
      const uint8_t *m = memory(s, row, 0x50);
      if (!m || !read16(s, row + 0xA))
        break;
      fprintf(stderr, "[attract-stage]   template %u @%08X species %u level %u shadow %u moves %u %u %u %u\n",
              t, row, read16(s, row + 0xA), m[4], m[3], read32(s, row + 0x34),
              read32(s, row + 0x3C), read32(s, row + 0x44), read32(s, row + 0x4C));
    }
  }
}

static void write8(CPUState *s, uint32_t a, uint8_t v) {
  uint8_t *p = memory(s, a, 1);
  if (p)
    p[0] = v;
}
static void write16(CPUState *s, uint32_t a, uint16_t v) {
  uint8_t *p = memory(s, a, 2);
  if (p) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
  }
}
static void write32(CPUState *s, uint32_t a, uint32_t v) {
  uint8_t *p = memory(s, a, 4);
  if (p) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
  }
}

/* A staged battle (ATTRACT_STAGE_SCENE file, one item per line):
     encounter A4        hex encounter id to launch
     type 1              0 single, 1 double (fight-type index)
     stage 21            hex stage index (fight-floor table)
     player 405 100 90 89 0 0           species level move1..4 (internal ids)
     enemy 404 100 0 323 57 58 0        species level shadow-id move1..4 */
typedef struct {
  uint16_t species;
  uint8_t level, shadow;
  uint16_t moves[4];
} Member;

static struct {
  uint32_t encounter;
  uint32_t seed;
  int has_seed;
  int auto_a;    /* press A through the battle menus and text */
  int running;   /* the staged battle has been started */
  int miss, crit, variance;  /* forced rolls: "miss", "crit", "variance N" (0 = 100%) */
  int trainer[4];  /* "trainer <slot> <id>": override an encounter slot's trainer */
  /* "cmd <moveSlot> <target>" / "switch <partySlot>" queue; a switch is
     stored as moveSlot -1 - partySlot */
  int command[32][2], commands;
  int player_hp[6][2];  /* "playerhp <slot> <cur> <max>": HUD numbers as in the original */
  int swap;  /* exchange the encounter's slot 0/1 trainers (Battle Mode's second
                player then stands on the HUD-left side, as in the attract) */
  int type, stage;
  Member player[6], enemy[6];
  int players, enemies;
} scene;

static int load_scene(const char *path) {
  FILE *f = path && *path ? fopen(path, "r") : NULL;
  if (!f)
    return 0;
  memset(&scene, 0, sizeof(scene));
  scene.type = scene.stage = -1;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    parse_dump(line);
    unsigned a[8] = {0};
    if (sscanf(line, "encounter %x", &a[0]) == 1)
      scene.encounter = a[0];
    else if (sscanf(line, "seed %x", &a[0]) == 1) {
      scene.seed = a[0];
      scene.has_seed = 1;
    } else if (strncmp(line, "auto", 4) == 0)
      scene.auto_a = 1;
    else if (strncmp(line, "miss", 4) == 0)
      scene.miss = 1;
    else if (strncmp(line, "swap", 4) == 0)
      scene.swap = 1;
    else if (sscanf(line, "playerhp %u %u %u", &a[0], &a[1], &a[2]) == 3 && a[0] < 6) {
      scene.player_hp[a[0]][0] = (int)a[1];
      scene.player_hp[a[0]][1] = (int)a[2];
    } else if (sscanf(line, "cmd %u %d", &a[0], (int *)&a[1]) == 2 && scene.commands < 32) {
      scene.command[scene.commands][0] = (int)a[0];
      scene.command[scene.commands][1] = (int)a[1];
      ++scene.commands;
    } else if (sscanf(line, "switch %u", &a[0]) == 1 && a[0] < 6 && scene.commands < 32) {
      scene.command[scene.commands][0] = -1 - (int)a[0];
      scene.command[scene.commands][1] = 0;
      ++scene.commands;
    } else if (sscanf(line, "trainer %u %u", &a[0], &a[1]) == 2 && a[0] < 4)
      scene.trainer[a[0]] = (int)a[1];
    else if (strncmp(line, "crit", 4) == 0)
      scene.crit = 1;
    else if (sscanf(line, "variance %u", &a[0]) == 1)
      scene.variance = (int)(a[0] & 15u);
    else if (sscanf(line, "type %u", &a[0]) == 1)
      scene.type = (int)a[0];
    else if (sscanf(line, "stage %x", &a[0]) == 1)
      scene.stage = (int)a[0];
    else if (sscanf(line, "player %u %u %u %u %u %u", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) >= 2 &&
             scene.players < 6) {
      Member *m = &scene.player[scene.players++];
      m->species = (uint16_t)a[0];
      m->level = (uint8_t)a[1];
      for (int i = 0; i < 4; ++i)
        m->moves[i] = (uint16_t)a[2 + i];
    } else if (sscanf(line, "enemy %u %u %u %u %u %u %u", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5],
                      &a[6]) >= 3 &&
               scene.enemies < 6) {
      Member *m = &scene.enemy[scene.enemies++];
      m->species = (uint16_t)a[0];
      m->level = (uint8_t)a[1];
      m->shadow = (uint8_t)a[2];
      for (int i = 0; i < 4; ++i)
        m->moves[i] = (uint16_t)a[3 + i];
    }
  }
  fclose(f);
  return scene.encounter != 0;
}

/* Player side (trainer kind 1 copies the saved party): rebuild party slots
   with the game's own constructor, pokemonCreate(ptr, species, level,
   attest), so stats, HP, experience and name are consistent, then set moves.
   The original-trainer block comes from the last party slot, which is
   rebuilt or cleared afterwards. */
static void stage_player(CPUState *s) {
  const uint32_t save = read32(s, 0x8047ADB8u);
  const uint32_t party = save + 0xA0u;
  if (!scene.players || !memory(s, party, 6 * 0x138u))
    return;
  int ok = 0;
  const uint32_t last = party + 5u * 0x138u;
  const uint32_t status_args[4] = {last, 0, 0x70, 0};
  const uint32_t attest = call_guest6(s, 0x8012640Cu, status_args, 4, &ok);
  if (!ok || !memory(s, attest, 4)) {
    fprintf(stderr, "[attract-stage] no original-trainer block\n");
    return;
  }
  for (int i = 0; i < scene.players && i < 5; ++i) {
    const Member *m = &scene.player[i];
    const uint32_t mon = party + (uint32_t)i * 0x138u;
    const uint32_t create[4] = {mon, m->species, m->level, attest};
    call_guest6(s, 0x801240C4u, create, 4, &ok);
    for (int k = 0; k < 4; ++k) {
      write16(s, mon + 0x78u + 4u * (uint32_t)k, m->moves[k]);
      write8(s, mon + 0x7Au + 4u * (uint32_t)k, m->moves[k] ? 20 : 0);
      write8(s, mon + 0x7Bu + 4u * (uint32_t)k, 0);
    }
    if (scene.player_hp[i][1] > 0) {
      write16(s, mon + 0x8C, (uint16_t)scene.player_hp[i][1]);  /* max HP */
      write16(s, mon + 0x8A, (uint16_t)scene.player_hp[i][0]);  /* current HP */
    }
    fprintf(stderr, "[attract-stage] player %d: species %u level %u hp %u (create %s)\n", i,
            read16(s, mon), memory(s, mon + 0x60, 1) ? *memory(s, mon + 0x60, 1) : 0,
            read16(s, mon + 0x8A), ok ? "ok" : "failed");
  }
  for (int i = scene.players; i < 6; ++i) {
    const uint32_t init[1] = {party + (uint32_t)i * 0x138u};
    call_guest6(s, 0x80124A60u, init, 1, &ok);  /* pokemonInit: empty slot */
  }
}

/* Opponent side: the encounter's second trainer builds its party from
   consecutive Pokemon templates (stride 0x50); rewrite them and end the list. */
static void stage_enemy(CPUState *s, uint32_t record) {
  if (!scene.enemies)
    return;
  /* The template-built trainer: slot 1 normally, slot 0 after "swap". */
  const uint16_t id = read16(s, record + 0x18u + (scene.swap ? 0u : 8u));
  const uint32_t trainer = read32(s, 0x80478F24u) + id * 0x34u;
  const uint16_t first = read16(s, trainer + 4);
  const uint32_t templates = read32(s, 0x80478F14u);
  if (!id || !first)
    return;
  for (int i = 0; i <= scene.enemies && i < 6; ++i) {
    const uint32_t row = templates + (first + (uint32_t)i) * 0x50u;
    if (i == scene.enemies) {
      write16(s, row + 0xA, 0);  /* end of party */
      break;
    }
    const Member *m = &scene.enemy[i];
    write16(s, row + 0xA, m->species);
    write8(s, row + 4, m->level);
    write8(s, row + 3, m->shadow);
    write32(s, row + 0x14, 0);  /* no nickname message (else the old name shows) */
    for (int k = 0; k < 4; ++k) {
      write8(s, row + 0x30u + 8u * (uint32_t)k, 0);
      /* 0 = no move; 0xFFFFFFFF would mean "fill from the learnset". */
      write32(s, row + 0x34u + 8u * (uint32_t)k, m->moves[k]);
    }
    fprintf(stderr, "[attract-stage] enemy %d (trainer %u template %u): species %u level %u\n", i,
            id, first + i, m->species, m->level);
  }
}

static void stage_scene(CPUState *s) {
  const uint32_t record = read32(s, 0x80478F54u) + scene.encounter * 0x38u;
  if (!memory(s, record, 0x38))
    return;
  if (scene.type >= 0)
    write8(s, record + 1, (uint8_t)scene.type);
  if (scene.stage >= 0)
    write16(s, record + 4, (uint16_t)scene.stage);
  for (int i = 0; i < 4; ++i)
    if (scene.trainer[i] > 0) {
      write16(s, record + 0x18u + 8u * (uint32_t)i, (uint16_t)scene.trainer[i]);
      fprintf(stderr, "[attract-stage] slot %d trainer -> %d\n", i, scene.trainer[i]);
    }
  if (scene.swap) {
    /* Trainer ids (+0x18 + 8*i) and input devices (+0x1C + 8*i) together. */
    const uint16_t a = read16(s, record + 0x18), b = read16(s, record + 0x20);
    const uint32_t da = read32(s, record + 0x1C), db = read32(s, record + 0x24);
    write16(s, record + 0x18, b);
    write16(s, record + 0x20, a);
    write32(s, record + 0x1C, db);
    write32(s, record + 0x24, da);
    fprintf(stderr, "[attract-stage] trainers swapped: slot 0 = %u (device %u), slot 1 = %u (device %u)\n",
            b, db, a, da);
  }
  /* The game's integer RNG state (pointer slot 0x80478C94): a fixed seed,
     set before the player's Pokemon are built (their IVs and personality are
     rolled), makes the battle repeatable, so a wanted outcome can be found by
     trying seeds. */
  if (scene.has_seed) {
    const uint32_t state = read32(s, 0x80478C94u);
    write32(s, state, scene.seed);
    fprintf(stderr, "[attract-stage] RNG seed %08X\n", scene.seed);
  }
  stage_player(s);
  stage_enemy(s, record);
  scene.running = start_battle(s, scene.encounter);
}

/* Battle roll control (needs 0x800E0C54 hooked in the game module). The
   game's integer RNG returns next>>16 with next = seed*0x343FD + 0x269EC3.
   For battle-mechanics callers (identified by return address), the seed is
   swapped for one whose next value gives the wanted roll R, then restored,
   so cosmetic randomness (particles) is untouched:
     accuracy (LR 0x80228BAC): R%100+1 <= accuracy hits -> R=0 always hits
     critical (LR 0x80227FC4, Shadow Rush extra 0x80228000, preview
       0x802114D8/0x80211514): crit if R%denominator==0 -> R=1 never crits
     damage variance (LR 0x802272A0): 100-(R%16) percent -> scene "variance" */
static struct {
  int active;
  uint32_t state_address, saved;
} roll;

static int forced_roll(uint32_t lr, uint32_t *r) {
  if (!scene.running)
    return 0;
  switch (lr) {
  case 0x80228BACu:  /* accuracy */
  case 0x8021AE14u:  /* one-hit KO: R%100+1 < 30 + level difference */
    *r = scene.miss ? 99 : 0;
    return 1;
  case 0x80227FC4u:
  case 0x802114D8u:
    *r = scene.crit ? 0 : 1;
    return 1;
  case 0x80228000u:
  case 0x80211514u:
    *r = 99;  /* Shadow Rush's extra crit roll fails at >= 90 */
    return 1;
  case 0x802272A0u:
    *r = (uint32_t)scene.variance;
    return 1;
  default:
    return 0;
  }
}

/* Capture windows: the runtime's frame dumper is switched on/off through
   automation commands (ATTRACT_STAGE_COMMANDS) so a capture run records only
   the frames a shot needs.
     dump <from> <to>                    field shot polls
     dumptext <msgId> <nth> <offset> <length>
                                         battles: start <offset> polls after the
                                         nth appearance of a battle message */
typedef struct {
  int text;  /* 0 = field window, 1 = text-anchored */
  uint32_t id;
  int nth, offset, length, from, to;
  int seen, start, done;
} DumpWindow;
static DumpWindow dumps[8];
static int dump_count, dumping;
/* "snapevery <from> <to> <step>": a screenshot every <step> polls between two
   global polls (counted from boot, as in the text log), in battles too; for
   probing where a battle's moments fall. Same command / snapshot directories as
   the field shots' snap lines. */
static int snap_from = -1, snap_to = -1, snap_step;
/* "statesave <poll>": a save state (SNAPDIR/state-<poll>.sav) at a global poll. */
static int state_poll = -1;
static uint32_t last_message, last_logged;

static void set_dumping(int on) {
  if (on == dumping)
    return;
  dumping = on;
  const char *commands = getenv("ATTRACT_STAGE_COMMANDS");
  if (!commands)
    return;
  static int n;
  char path[512];
  snprintf(path, sizeof(path), "%s/dump-%04d.txt", commands, n++);
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "command=dump_frames\nenable=%d\n", on);
    fclose(f);
  }
  fprintf(stderr, "[attract-stage] capture %s at poll %u\n", on ? "ON" : "OFF", frames);
  fflush(stderr);  /* stderr is block-buffered when redirected; capture scripts wait on this */
}

static void parse_dump(const char *line) {
  unsigned id = 0;
  int a = 0, b = 0, c = 0;
  if (sscanf(line, "snapevery %d %d %d", &a, &b, &c) == 3 && c > 0) {
    snap_from = a;
    snap_to = b;
    snap_step = c;
    return;
  }
  if (sscanf(line, "statesave %d", &a) == 1) {
    state_poll = a;
    return;
  }
  if (dump_count >= 8)
    return;
  DumpWindow *d = &dumps[dump_count];
  memset(d, 0, sizeof(*d));
  d->start = -1;
  if (sscanf(line, "dumptext %x %d %d %d", &id, &a, &b, &c) == 4) {
    d->text = 1;
    d->id = id;
    d->nth = a;
    d->offset = b;
    d->length = c;
    ++dump_count;
  } else if (sscanf(line, "dump %d %d", &a, &b) == 2) {
    d->from = a;
    d->to = b;
    ++dump_count;
  }
}

/* Called every poll: shot_poll < 0 outside a field shot. */
static void update_dumps(int shot_poll) {
  if (snap_step > 0 && (int)frames >= snap_from && (int)frames <= snap_to &&
      ((int)frames - snap_from) % snap_step == 0) {
    const char *commands = getenv("ATTRACT_STAGE_COMMANDS");
    const char *dir = getenv("ATTRACT_STAGE_SNAPDIR");
    if (commands && dir) {
      char path[512];
      snprintf(path, sizeof(path), "%s/snapg-%06u.txt", commands, frames);
      FILE *f = fopen(path, "w");
      if (f) {
        fprintf(f, "command=screenshot\npath=%s/snapg-%06u.png\n", dir, frames);
        fclose(f);
      }
    }
  }
  if (state_poll >= 0 && (int)frames == state_poll) {
    const char *commands = getenv("ATTRACT_STAGE_COMMANDS");
    const char *dir = getenv("ATTRACT_STAGE_SNAPDIR");
    if (commands && dir) {
      char path[512];
      snprintf(path, sizeof(path), "%s/state-%06u.txt", commands, frames);
      FILE *f = fopen(path, "w");
      if (f) {
        fprintf(f, "command=save_state\npath=%s/state-%06u.sav\n", dir, frames);
        fclose(f);
      }
    }
  }
  int want = 0, open = 0;
  for (int i = 0; i < dump_count; ++i) {
    DumpWindow *d = &dumps[i];
    if (d->done)
      continue;
    ++open;
    if (!d->text) {
      if (shot_poll >= d->from && shot_poll < d->to)
        want = 1;
      else if (shot_poll >= d->to)
        d->done = 1;
    } else if (d->start >= 0) {
      if ((int)frames < d->start + d->length) {
        if ((int)frames >= d->start)
          want = 1;
      } else {
        d->done = 1;
      }
    }
  }
  set_dumping(want);
  /* Every window closed: a battle capture has several, so "capture OFF" alone
     does not mean the run is finished. */
  static int announced;
  if (dump_count > 0 && open == 0 && !dumping && !announced) {
    announced = 1;
    fprintf(stderr, "[attract-stage] all captures done at poll %u\n", frames);
    fflush(stderr);
  }
}

static void note_message(uint32_t id) {
  if (id == last_message)
    return;
  last_message = id;
  for (int i = 0; i < dump_count; ++i) {
    DumpWindow *d = &dumps[i];
    if (d->text && d->start < 0 && d->id == id && ++d->seen == d->nth)
      d->start = (int)frames + d->offset;
  }
}

/* Battle text log: fn_800FBE7C(message id) renders each message; logging the
   decoded template (placeholders show as '?') with the poll gives a readable
   timeline of a staged battle. */
static void message_log(CPUState *s) {
  if (!scene.running)
    return;
  note_message(s->gpr[3]);
  if (s->gpr[3] == last_logged)
    return;  /* the renderer calls this every frame a message is up */
  last_logged = s->gpr[3];
  char text[64];
  message_text(s, s->gpr[3], text, sizeof(text));
  fprintf(stderr, "[attract-stage] text %08X at poll %u: %s\n", s->gpr[3], frames, text);
}

static void rng_entry(CPUState *s) {
  uint32_t r;
  /* ATTRACT_STAGE_RNGLOG=1: log each distinct caller once (and every 64th
     call) while the staged battle runs, to identify roll sites. */
  static uint32_t seen[256];
  static unsigned seen_count, calls[256];
  if (scene.running && getenv("ATTRACT_STAGE_RNGLOG")) {
    unsigned i = 0;
    while (i < seen_count && seen[i] != s->lr)
      ++i;
    if (i == seen_count && seen_count < 256)
      seen[seen_count++] = s->lr;
    if (i < 256 && (calls[i]++ % 64) == 0)
      fprintf(stderr, "[attract-stage] rng lr %08X call %u at poll %u\n", s->lr, calls[i], frames);
  }
  if (!forced_roll(s->lr, &r))
    return;
  const uint32_t state = read32(s, 0x80478C94u);
  if (!memory(s, state, 4))
    return;
  const uint32_t wanted = (r << 16) | 0x5A5Au;
  roll.active = 1;
  roll.state_address = state;
  roll.saved = read32(s, state);
  write32(s, state, (wanted - 0x269EC3u) * 0xB9B33155u);
}

static void rng_return(CPUState *s) {
  if (!roll.active)
    return;
  roll.active = 0;
  write32(s, roll.state_address, roll.saved);
}

/* Scripted battle menus (needs 0x800117BC / 0x80011288 routed in the game
   module). "cmd <moveSlot> <target>" lines form a queue consumed by the
   player's successive menu calls (in a double battle: actor 0 then actor 1
   each turn); the last entry repeats. The move menu (menuFightOpenWaza)
   returns the slot, the target menu (menuFightOpenTarget) the relative
   target id, without showing either menu. */
static int next_command, pending_target = -1, pending_party = -1;

/* The command menu (fn_80011A1C; 0 Fight, 1 Items, 2 Pokemon, 3 Call): a
   queued "switch" picks Pokemon, anything else Fight, which the move menu
   then reads. */
static void top_menu_patch(CPUState *s) {
  int mode = 0;
  if (scene.commands) {
    const int i = next_command < scene.commands ? next_command : scene.commands - 1;
    if (scene.command[i][0] < 0) {
      pending_party = -1 - scene.command[i][0];
      ++next_command;
      mode = 2;
    }
  }
  fprintf(stderr, "[attract-stage] command menu -> %s\n", mode == 2 ? "switch" : "fight");
  s->gpr[3] = (uint32_t)mode;
  s->pc = s->lr;
}

/* The switch party menu (menuFightOpenPokemon): the queued party slot. */
static void party_menu_patch(CPUState *s) {
  const int slot = pending_party >= 0 ? pending_party : 0;
  pending_party = -1;
  fprintf(stderr, "[attract-stage] party menu -> slot %d\n", slot);
  s->gpr[3] = (uint32_t)slot;
  s->pc = s->lr;
}

static void waza_menu_patch(CPUState *s) {
  int slot = 0;
  if (scene.commands) {
    const int i = next_command < scene.commands ? next_command : scene.commands - 1;
    slot = scene.command[i][0];
    pending_target = scene.command[i][1];
    ++next_command;
  }
  fprintf(stderr, "[attract-stage] move menu -> slot %d (command %d)\n", slot, next_command);
  s->gpr[3] = (uint32_t)slot;
  s->pc = s->lr;
}

static void target_menu_patch(CPUState *s) {
  const int target = pending_target >= 0 ? pending_target : 0;
  fprintf(stderr, "[attract-stage] target menu -> %d\n", target);
  s->gpr[3] = (uint32_t)target;
  s->pc = s->lr;
}

/* Battle autopilot: tap A every 24 polls while the staged battle runs. Each
   Pokemon has one move, so A always picks it (Fight, move, target). */
static void autopilot(CPUState *s) {
  static unsigned polls, seen_battle, ended;
  if (!scene.running || !scene.auto_a)
    return;
  /* Report how the staged battle ended (battle-in-progress byte 0x8047B5DA,
     result u16 0x8047B5D8), for seed searches. */
  const uint8_t *in_battle = memory(s, 0x8047B5DAu, 1);
  if (in_battle && *in_battle)
    seen_battle = 1;
  else if (seen_battle && !ended) {
    ended = 1;
    fprintf(stderr, "[attract-stage] battle ended after %u polls, result %u\n", polls,
            read16(s, 0x8047B5D8u));
  }
  uint8_t *p = memory(s, pad_buffer, 12);
  if (!p)
    return;
  ++polls;
  if (polls % 24 < 3) {
    const uint16_t buttons = (uint16_t)(((uint16_t)p[0] << 8) | p[1]) | 0x0100u;  /* A */
    p[0] = (uint8_t)(buttons >> 8);
    p[1] = (uint8_t)buttons;
    p[8] = 0xFF;  /* analog A */
  }
}

/* One-bit story flag (same resolution as the runtime's Shadow tracker):
   definition (bank = top 2 bits, bit offset u16 +4) -> bank (words, buffer). */
static int story_flag(CPUState *s, uint32_t id) {
  if (!id)
    return 1;
  const uint32_t defs = read32(s, 0x80478F9Cu), banks = read32(s, 0x80478EECu);
  const uint8_t *kind = memory(s, defs + id * 8u, 1);
  if (!kind)
    return -1;
  const uint32_t bit = read16(s, defs + id * 8u + 4), bank = (uint32_t)(*kind >> 6);
  const uint32_t words = read32(s, banks + bank * 8u), buffer = read32(s, banks + bank * 8u + 4);
  if ((bit >> 5) >= words)
    return -1;
  return (int)((read32(s, buffer + 4u * (bit >> 5)) >> (bit & 31)) & 1u);
}

/* Game text by message id (same lookup as the runtime's Shadow tracker):
   groups linked from *(U32(0x80478B08)+8); id (u16 +0), count (u16 +4),
   sorted (key, offset) pairs at +0x10; text = 16-bit chars at node+offset. */
static void message_text(CPUState *s, uint32_t key, char *out, size_t size) {
  out[0] = 0;
  if (!key || size < 2)
    return;
  uint32_t node = read32(s, read32(s, 0x80478B08u) + 8);
  const uint32_t group = key >> 20, local = key & 0xFFFFFu;
  for (int guard = 0; node && guard < 64; ++guard) {
    const uint32_t id = read16(s, node), count = read16(s, node + 4);
    if (id == group) {
      for (uint32_t i = 0; i < count && i < 0x10000u; ++i) {
        if (read32(s, node + 0x10 + i * 8) != local)
          continue;
        const uint32_t offset = read32(s, node + 0x14 + i * 8);
        size_t n = 0;
        for (uint32_t k = 0; k < 80 && n + 1 < size; ++k) {
          const uint32_t c = read16(s, node + offset + k * 2);
          if (!c || c == 0xFFFF)
            break;
          out[n++] = c < 0x80 ? (char)c : '?';
        }
        out[n] = 0;
        return;
      }
    }
    node = read32(s, node + 8);
  }
}

/* Every trainer record: id, class, name and the resource words (+0x24..+0x30;
   the model comes from one of these). */
static void dump_trainers(CPUState *s) {
  const uint32_t base = read32(s, 0x80478F24u), count = read32(s, read32(s, 0x80478F20u));
  fprintf(stderr, "[attract-stage] %u trainers\n", count);
  for (uint32_t id = 0; id < count && id < 2000; ++id) {
    const uint32_t t = base + id * 0x34u;
    char name[32];
    message_text(s, read32(s, t + 8), name, sizeof(name));
    fprintf(stderr, "[attract-stage] trainer %u kind %u name '%s' res %08X %08X %08X %08X\n", id,
            read16(s, t + 2), name, read32(s, t + 0x24), read32(s, t + 0x28), read32(s, t + 0x2C),
            read32(s, t + 0x30));
  }
}

/* World-map destinations (U32(0x80478DDC), 0x10 entries: +0 required flag,
   +4 floor id) and whether this save has them unlocked. */
static void dump_progress(CPUState *s) {
  const uint32_t table = read32(s, 0x80478DDCu);
  const uint32_t count = read32(s, read32(s, 0x80478DD8u));
  fprintf(stderr, "[attract-stage] world map: %u destinations, current floor 0x%X\n", count,
          read32(s, read32(s, read32(s, 0x8047ACC8u)) + 0xC));
  for (uint32_t i = 0; i < count && i < 32; ++i) {
    const uint32_t flag = read32(s, table + i * 0x10u), floor = read32(s, table + i * 0x10u + 4);
    fprintf(stderr, "[attract-stage]  destination floor 0x%03X flag %u -> %s\n", floor, flag,
            story_flag(s, flag) == 1 ? "UNLOCKED" : story_flag(s, flag) == 0 ? "locked" : "?");
  }
}

/* Field shots (ATTRACT_STAGE_TEST=shot, ATTRACT_STAGE_SCENE=<file>):
     warp 001 1                  floor (hex) and spawn index
     settle 90                   polls after arriving before the shot starts
     cam 0 ex ey ez tx ty tz     camera keyframe at shot poll 0: eye and look-at,
                                 relative to Wes's position at shot start
     walk 30 100 dx dz [run]     at shot poll 30 people index 100 (Wes) walks
                                 to (dx, dz) relative to the start position
     length 360                  shot length in polls
   The camera is the freecam's free camera (moderngekko.camera.set_state). */
typedef struct {
  int poll;
  float eye[3], target[3];
} CamKey;
typedef struct {
  int poll, people, run, done;
  float x, z;
} WalkCmd;
static struct {
  uint32_t floor, spawn;
  int settle, length, keys, walks, hides, absolute, snaps;
  int snap[32];  /* shot polls at which to take a screenshot */
  struct { int poll, people, done; float pos[3]; } place[8];
  struct { int poll, people, done; float value; } speed[8];
  int places, speeds;
  float fov;     /* vertical FOV in degrees (0 = the game camera's) */
  int first_person;  /* firstperson line: freecam first-person view */
  int hide[8];  /* people indices hidden at shot start (101 = Rui) */
  CamKey key[32];
  WalkCmd walk[16];
  /* runtime */
  int warped, arrived_poll, start_poll, finished;
  float origin[3];
} shot;

static int load_shot(const char *path) {
  FILE *f = path && *path ? fopen(path, "r") : NULL;
  if (!f)
    return 0;
  memset(&shot, 0, sizeof(shot));
  shot.settle = 90;
  shot.length = 300;
  shot.arrived_poll = shot.start_poll = -1;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    parse_dump(line);
    unsigned a = 0, b = 0;
    int i = 0, j = 0;
    float v[6];
    char mode[16] = {0};
    if (sscanf(line, "warp %x %u", &a, &b) == 2) {
      shot.floor = a;
      shot.spawn = b;
    } else if (sscanf(line, "settle %d", &i) == 1)
      shot.settle = i;
    else if (sscanf(line, "length %d", &i) == 1)
      shot.length = i;
    else if (sscanf(line, "snap %d", &i) == 1 && shot.snaps < 32)
      shot.snap[shot.snaps++] = i;
    else if (shot.places < 8 &&
             sscanf(line, "place %d %d %f %f %f", &i, &j, &v[0], &v[1], &v[2]) == 5) {
      shot.place[shot.places].poll = i;
      shot.place[shot.places].people = j;
      memcpy(shot.place[shot.places].pos, v, sizeof(float) * 3);
      ++shot.places;
    } else if (shot.speeds < 8 && sscanf(line, "speed %d %d %f", &i, &j, &v[0]) == 3) {
      shot.speed[shot.speeds].poll = i;
      shot.speed[shot.speeds].people = j;
      shot.speed[shot.speeds].value = v[0];
      ++shot.speeds;
    } else if (sscanf(line, "fov %f", &v[0]) == 1)
      shot.fov = v[0];  /* vertical degrees, like the original's lens */
    else if (strncmp(line, "firstperson", 11) == 0)
      shot.first_person = 1;
    else if (strncmp(line, "absolute", 8) == 0)
      shot.absolute = 1;  /* camera keys and walks in world coordinates */
    else if (sscanf(line, "hide %d", &i) == 1 && shot.hides < 8)
      shot.hide[shot.hides++] = i;
    else if (shot.keys < 32 && sscanf(line, "cam %d %f %f %f %f %f %f", &i, &v[0], &v[1], &v[2],
                                      &v[3], &v[4], &v[5]) == 7) {
      CamKey *k = &shot.key[shot.keys++];
      k->poll = i;
      memcpy(k->eye, v, sizeof(k->eye));
      memcpy(k->target, v + 3, sizeof(k->target));
    } else if (shot.walks < 16 &&
               sscanf(line, "walk %d %d %f %f %15s", &i, &j, &v[0], &v[1], mode) >= 4) {
      WalkCmd *w = &shot.walk[shot.walks++];
      w->poll = i;
      w->people = j;
      w->x = v[0];
      w->z = v[1];
      w->run = strcmp(mode, "run") == 0;
    }
  }
  fclose(f);
  return shot.floor != 0;
}

static float read_f32(CPUState *s, uint32_t a) {
  const uint32_t bits = read32(s, a);
  float f;
  memcpy(&f, &bits, 4);
  return f;
}
static uint32_t f32_bits(float f) {
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}

/* People entry (array U32(0x8047B200), stride 0xDC, +0x28 group, +0x2C index)
   -> GSmodel (+0x08) -> position (+0x18). */
static int people_position(CPUState *s, uint32_t index, float out[3]) {
  const uint32_t base = read32(s, 0x8047B200u), count = read32(s, 0x8047B1F8u);
  for (uint32_t i = 0; base && i < count && i < 512; ++i) {
    const uint32_t e = base + i * 0xDCu;
    const uint8_t *active = memory(s, e, 1);
    if (!active || !*active || read32(s, e + 0x28) != 0 || read32(s, e + 0x2C) != index)
      continue;
    const uint32_t model = read32(s, e + 0x08);
    if (!memory(s, model + 0x18, 12))
      return 0;
    for (int k = 0; k < 3; ++k)
      out[k] = read_f32(s, model + 0x18u + 4u * (uint32_t)k);
    return 1;
  }
  return 0;
}

/* Freecam first-person view (CAMERA_STATE_ORBIT | CAMERA_STATE_FIRST_PERSON),
   keeping its orbit fields; for tests in first person. */
static void set_first_person(CPUState *s) {
  ModernGekkoModFunction get = NULL, set = NULL;
  if (api && api->find_export) {
    get = api->find_export(api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.get_state");
    set = api->find_export(api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.set_state");
  }
  if (!get || !set)
    return;
  CPUState c = *s;
  get(&c);
  c.gpr[3] = 1u | 2u;
  set(&c);
  fprintf(stderr, "[attract-stage] first-person view on\n");
}

static void set_free_camera(CPUState *s, const float eye[3], const float target[3]) {
  static ModernGekkoModFunction get, set;
  if (!set && api && api->find_export) {
    get = api->find_export(api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.get_state");
    set = api->find_export(api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.set_state");
  }
  if (!get || !set)
    return;
  CPUState c = *s;
  get(&c);  /* keep the orbit fields as they are */
  const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
  const float yaw = atan2f(dx, dz), pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
  c.gpr[3] = 1u | 4u;  /* ORBIT | FREE */
  c.gpr[8] = f32_bits(eye[0]);
  c.gpr[9] = f32_bits(eye[1]);
  c.gpr[10] = f32_bits(eye[2]);
  c.gpr[11] = f32_bits(yaw);
  c.gpr[12] = f32_bits(pitch);
  set(&c);
}

/* Native scripted walk: fn_80184190(group, index, points) reserves the walk
   list, peopleAddWalkList 0x80183FC4(group, index, x, y, z) adds a point (f1-f3),
   fn_80183E5C(group, index, run) starts it. */
static void start_walk(CPUState *s, const WalkCmd *w) {
  int ok = 0;
  const uint32_t reserve[3] = {0, (uint32_t)w->people, 1};
  call_guest6(s, 0x80184190u, reserve, 3, &ok);
  CPUState c = *s;
  c.gpr[3] = 0;
  c.gpr[4] = (uint32_t)w->people;
  c.fpr[1] = shot.origin[0] + w->x;
  c.fpr[2] = shot.origin[1];
  c.fpr[3] = shot.origin[2] + w->z;
  int ok2 = api && api->call_guest && api->call_guest(api->user_data, &c, 0x80183FC4u);
  const uint32_t go[3] = {0, (uint32_t)w->people, (uint32_t)w->run};
  int ok3 = 0;
  call_guest6(s, 0x80183E5Cu, go, 3, &ok3);
  fprintf(stderr, "[attract-stage] walk people %d to (%.1f, %.1f) %s (%d%d%d)\n", w->people,
          (double)(shot.origin[0] + w->x), (double)(shot.origin[2] + w->z), w->run ? "run" : "walk",
          ok, ok2, ok3);
}

/* Native placement: fn_8018C0A8(group, index, GSvec*) sets a person's
   position (dirtying the model). The vector lives in a scratch area on the
   guest stack: the call runs with its stack pointer moved below it. */
static void place_people(CPUState *s, int people, const float pos[3]) {
  if (!api || !api->call_guest)
    return;
  CPUState c = *s;
  const uint32_t vec = (s->gpr[1] - 0x100u) & ~0xFu;
  for (int k = 0; k < 3; ++k)
    write32(s, vec + 4u * (uint32_t)k, f32_bits(pos[k]));
  c.gpr[1] = vec - 0x100u;
  c.gpr[3] = 0;
  c.gpr[4] = (uint32_t)people;
  c.gpr[5] = vec;
  const int ok = api->call_guest(api->user_data, &c, 0x8018C0A8u);
  fprintf(stderr, "[attract-stage] place people %d at (%.1f, %.1f, %.1f) %s\n", people,
          (double)pos[0], (double)pos[1], (double)pos[2], ok ? "ok" : "failed");
}

/* fn_80184948(group, index, speed f1): walk-list movement speed. */
static void set_people_speed(CPUState *s, int people, float speed) {
  if (!api || !api->call_guest)
    return;
  CPUState c = *s;
  c.gpr[3] = 0;
  c.gpr[4] = (uint32_t)people;
  c.fpr[1] = speed;
  const int ok = api->call_guest(api->user_data, &c, 0x80184948u);
  fprintf(stderr, "[attract-stage] speed people %d = %.2f %s\n", people, (double)speed,
          ok ? "ok" : "failed");
}

static void run_shot(CPUState *s, unsigned poll) {
  if (!shot.floor || shot.finished)
    return;
  const uint32_t floor = read32(s, read32(s, read32(s, 0x8047ACC8u)) + 0xC);
  if (!shot.warped) {
    if (poll < 300)
      return;
    shot.warped = 1;
    int ok = 0;
    call_guest(s, 0x80113828u, shot.floor, shot.spawn, &ok);
    fprintf(stderr, "[attract-stage] shot: warp to 0x%X spawn %u %s\n", shot.floor, shot.spawn,
            ok ? "requested" : "failed");
    return;
  }
  if (shot.arrived_poll < 0) {
    if (floor == shot.floor)
      shot.arrived_poll = (int)poll;
    return;
  }
  if (shot.start_poll < 0) {
    if ((int)poll - shot.arrived_poll < shot.settle || !people_position(s, 100, shot.origin))
      return;
    shot.start_poll = (int)poll;
    if (shot.absolute)
      shot.origin[0] = shot.origin[1] = shot.origin[2] = 0;
    if (shot.first_person)
      set_first_person(s);
    if (shot.fov > 0 && api && api->find_export) {
      /* The freecam takes horizontal degrees at 16:9. */
      ModernGekkoModFunction set_fov = api->find_export(
          api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.set_free_fov");
      const float half = shot.fov * 0.5f * 3.14159265f / 180.0f;
      const float horizontal = 2.0f * atanf(tanf(half) * 16.0f / 9.0f) * 180.0f / 3.14159265f;
      if (set_fov) {
        CPUState c = *s;
        c.gpr[3] = (uint32_t)(horizontal + 0.5f);
        set_fov(&c);
      }
      fprintf(stderr, "[attract-stage] free camera FOV %.1f vertical (%.0f at 16:9) %s\n",
              (double)shot.fov, (double)horizontal, set_fov ? "set" : "unavailable");
    }
    for (int i = 0; i < shot.hides; ++i) {
      int ok = 0;
      const uint32_t args[3] = {0, (uint32_t)shot.hide[i], 0};
      call_guest6(s, 0x8018C1E8u, args, 3, &ok);  /* fn_8018C1E8(group, index, visible) */
      fprintf(stderr, "[attract-stage] hide people %d %s\n", shot.hide[i], ok ? "ok" : "failed");
    }
    fprintf(stderr, "[attract-stage] shot start at poll %u (clock %ld ms), Wes at (%.2f, %.2f, %.2f)\n",
            poll, (long)clock(), (double)shot.origin[0], (double)shot.origin[1],
            (double)shot.origin[2]);
  }
  const int t = (int)poll - shot.start_poll;
  if (t >= shot.length) {
    shot.finished = 1;
    fprintf(stderr, "[attract-stage] shot end at poll %u (clock %ld ms)\n", poll, (long)clock());
    fflush(stderr);
    return;
  }
  /* Frame-exact screenshots: an automation command file for the runtime
     (ATTRACT_STAGE_COMMANDS = <automation dir>/commands, ATTRACT_STAGE_SNAPDIR). */
  for (int i = 0; i < shot.snaps; ++i)
    if (t == shot.snap[i]) {
      const char *commands = getenv("ATTRACT_STAGE_COMMANDS");
      const char *dir = getenv("ATTRACT_STAGE_SNAPDIR");
      if (commands && dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/snap-%04d.txt", commands, t);
        FILE *f = fopen(path, "w");
        if (f) {
          fprintf(f, "command=screenshot\npath=%s/snap-%04d.png\n", dir, t);
          fclose(f);
        }
      }
    }
  for (int i = 0; i < shot.places; ++i)
    if (!shot.place[i].done && t >= shot.place[i].poll) {
      shot.place[i].done = 1;
      float pos[3];
      for (int k = 0; k < 3; ++k)
        pos[k] = shot.origin[k] + shot.place[i].pos[k];
      place_people(s, shot.place[i].people, pos);
    }
  for (int i = 0; i < shot.speeds; ++i)
    if (!shot.speed[i].done && t >= shot.speed[i].poll) {
      shot.speed[i].done = 1;
      set_people_speed(s, shot.speed[i].people, shot.speed[i].value);
    }
  for (int i = 0; i < shot.walks; ++i)
    if (!shot.walk[i].done && t >= shot.walk[i].poll) {
      shot.walk[i].done = 1;
      start_walk(s, &shot.walk[i]);
    }
  if (shot.keys) {
    int k = 0;
    while (k + 1 < shot.keys && shot.key[k + 1].poll <= t)
      ++k;
    const CamKey *a = &shot.key[k], *b = k + 1 < shot.keys ? &shot.key[k + 1] : a;
    float u = b->poll > a->poll ? (float)(t - a->poll) / (float)(b->poll - a->poll) : 0.0f;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    u = u * u * (3.0f - 2.0f * u);  /* ease in and out */
    float eye[3], target[3];
    for (int i = 0; i < 3; ++i) {
      eye[i] = shot.origin[i] + a->eye[i] + (b->eye[i] - a->eye[i]) * u;
      target[i] = shot.origin[i] + a->target[i] + (b->target[i] - a->target[i]) * u;
    }
    set_free_camera(s, eye, target);
  }
}

/* Controller record/replay for field shots: a player performs the shot at
   normal speed with ATTRACT_STAGE_RECORD=<file> (port 0, 12 bytes per
   completed PADRead), and the capture run replays it with
   ATTRACT_STAGE_REPLAY=<file> while dumping frames at full resolution. The
   free camera runs on game frame time, so the camera replays too. */
static struct {
  uint64_t time;
  uint32_t pc, sp;
  int seen;
} last_poll;
static FILE *record_file, *replay_file;
static unsigned replay_polls;

static void pad_entry(CPUState *s) { pad_buffer = s->gpr[3]; }

static void record_replay(CPUState *s) {
  static int opened;
  if (!opened) {
    opened = 1;
    const char *record = getenv("ATTRACT_STAGE_RECORD");
    const char *replay = getenv("ATTRACT_STAGE_REPLAY");
    if (replay && *replay && !(replay_file = fopen(replay, "rb")))
      fprintf(stderr, "[attract-stage] cannot open replay %s\n", replay);
    else if (record && *record && !(record_file = fopen(record, "wb")))
      fprintf(stderr, "[attract-stage] cannot open recording %s\n", record);
    if (replay_file)
      fprintf(stderr, "[attract-stage] replaying %s\n", replay);
    if (record_file)
      fprintf(stderr, "[attract-stage] recording to %s\n", record);
  }
  uint8_t *p = memory(s, pad_buffer, 12);
  if (!p)
    return;
  if (replay_file) {
    uint8_t buffer[12];
    if (fread(buffer, 1, 12, replay_file) == 12) {
      memcpy(p, buffer, 12);
      ++replay_polls;
    } else {
      memset(p, 0, 10);  /* end of recording: neutral controller */
      p[2] = p[3] = p[4] = p[5] = 0;
    }
  } else if (record_file) {
    fwrite(p, 1, 12, record_file);
    fflush(record_file);
  }
}

/* ATTRACT_STAGE_BOUNDS=1: fn_80191118 (HSD bound test against the 4:3 screen)
   reports every box visible, to tell its culling from other causes. Needs a
   module built with this hook (gen-hookaware10 / build-gp10). */
static unsigned bound_calls, bound_rejects;
static void bound_test_return(CPUState *s) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *v = getenv("ATTRACT_STAGE_BOUNDS");
    enabled = v && v[0] == '1';
  }
  bound_calls++;
  if (s->gpr[3] == 0) {
    bound_rejects++;
    if (enabled)
      s->gpr[3] = 1;
  }
}

/* ATTRACT_STAGE_JOINTLOG=1: per GSmodel, joints in its render JObj tree
   (+0x08; JObj child +0x10, next +0x08, flags +0x14) and how many carry the
   HSD hidden bit (0x10), plus a hash of which, every 60 polls. */
static void walk_joints(CPUState *s, uint32_t j, int depth, unsigned *n, unsigned *hidden,
                        uint32_t *hash) {
  while (j && depth < 64 && *n < 4096) {
    const uint32_t flags = read32(s, j + 0x14);
    (*n)++;
    if (flags & 0x10u) {
      (*hidden)++;
      *hash = (*hash * 31u) ^ *n;
    }
    walk_joints(s, read32(s, j + 0x10), depth + 1, n, hidden, hash);
    j = read32(s, j + 0x08);
  }
}

static void joint_log(CPUState *s) {
  const uint32_t table = read32(s, 0x8047AB74u), count = read32(s, 0x8047AB78u);
  if (!table || count > 4096u)
    return;
  float pos[3] = {0, 0, 0};
  const int has_pos = people_position(s, 100, pos);
  fprintf(stderr, "[attract-stage] joints poll %u floor 0x%X wes %s(%.1f %.1f %.1f):", frames,
          read32(s, read32(s, read32(s, 0x8047ACC8u)) + 0xC), has_pos ? "" : "none ",
          (double)pos[0], (double)pos[1], (double)pos[2]);
  for (uint32_t i = 0; i < count && i < 8; i++) {
    const uint32_t m = table + i * 0x170u;
    if (!(read32(s, m) & 1u))
      continue;
    unsigned n = 0, hidden = 0;
    uint32_t hash = 0;
    walk_joints(s, read32(s, m + 8), 0, &n, &hidden, &hash);
    fprintf(stderr, " m%u %u/%u %08X", i, hidden, n, hash);
    if (n >= 200) {
      /* The arena: animation state (+0x8C type, +0x90 index, +0x98 frame,
         +0xA4/+0xA8/+0xB0 texture animation) and the root's first children's Y. */
      fprintf(stderr, " [anim %u %u %.1f tex %u %u %.1f ys", read32(s, m + 0x8C), read32(s, m + 0x90),
              (double)read_f32(s, m + 0x98), read32(s, m + 0xA4), read32(s, m + 0xA8),
              (double)read_f32(s, m + 0xB0));
      uint32_t j = read32(s, read32(s, m + 8) + 0x10);
      for (int k = 0; j && k < 12; ++k, j = read32(s, j + 0x08))
        fprintf(stderr, " %.0f", (double)read_f32(s, j + 0x3C));
      fprintf(stderr, "]");
    }
  }
  fprintf(stderr, "\n");
}

static void pad_return(CPUState *s) {
  /* A completed PADRead can be delivered twice (physical and cached PC). */
  if (last_poll.seen && last_poll.time == s->timebase && last_poll.pc == s->pc &&
      last_poll.sp == s->gpr[1])
    return;
  last_poll.seen = 1;
  last_poll.time = s->timebase;
  last_poll.pc = s->pc;
  last_poll.sp = s->gpr[1];
  record_replay(s);
  autopilot(s);
  if (frames % 60 == 0 && getenv("ATTRACT_STAGE_JOINTLOG"))
    joint_log(s);
  if (frames % 120 == 0 && getenv("ATTRACT_STAGE_BOUNDLOG")) {
    fprintf(stderr, "[attract-stage] bounds poll %u: %u tests, %u rejected\n", frames, bound_calls,
            bound_rejects);
    bound_calls = bound_rejects = 0;
  }
  /* ATTRACT_STAGE_MODELLOG=1: the GSmodel table's flags every 120 polls. */
  if (frames % 120 == 0 && getenv("ATTRACT_STAGE_MODELLOG")) {
    const uint32_t table = read32(s, 0x8047AB74u), count = read32(s, 0x8047AB78u);
    if (table && count <= 4096u) {
      fprintf(stderr, "[attract-stage] models poll %u count %u:", frames, count);
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t flags = read32(s, table + i * 0x170u);
        if (flags & 1u)
          fprintf(stderr, " %u:%08X", i, flags);
      }
      fprintf(stderr, "\n");
    }
  }
  update_dumps(shot.start_poll >= 0 && !shot.finished ? (int)frames - shot.start_poll : -1);
  ++frames;
  const char *test = getenv("ATTRACT_STAGE_TEST");
  /* ATTRACT_STAGE_TEST=encdump: every encounter record as id:type:stage:trainer ids. */
  if (frames == 120 && test && strcmp(test, "encdump") == 0) {
    const uint32_t base = read32(s, 0x80478F54u);
    for (uint32_t id = 0; id < 566; ++id) {
      const uint32_t r = base + id * 0x38u;
      if (!memory(s, r, 0x38))
        break;
      fprintf(stderr, "[attract-stage] enc %03X type %u stage %02X trainers %u %u\n", id,
              *memory(s, r + 1, 1), read16(s, r + 4), read16(s, r + 0x18), read16(s, r + 0x20));
    }
  }
  if (!bridge_done && frames > 600 && test && strcmp(test, "bridge") == 0) {
    bridge_done = 1;
    test_bridge(s);
  }
  /* ATTRACT_STAGE_YAWLOG=1: log the freecam's yaw (orbit and free) every 30
     polls, to measure camera turn speed. */
  if (frames % 30 == 0 && getenv("ATTRACT_STAGE_YAWLOG") && api && api->find_export) {
    static ModernGekkoModFunction get;
    if (!get)
      get = api->find_export(api->user_data, "pokemon-colosseum-freecam", "moderngekko.camera.get_state");
    if (get) {
      CPUState c = *s;
      get(&c);
      float orbit_yaw, free_yaw;
      memcpy(&orbit_yaw, &c.gpr[4], 4);
      memcpy(&free_yaw, &c.gpr[11], 4);
      fprintf(stderr, "[attract-stage] yaw poll %u flags %u orbit %.3f free %.3f\n", frames,
              c.gpr[3], (double)orbit_yaw, (double)free_yaw);
    }
  }
  /* Any test: log the runtime's ultrawide projection multiplier
     (0x8047E724 = applied aspect / the game's authored aspect) now and then. */
  if (frames % 600 == 300) {
    const uint32_t bits = read32(s, 0x8047E724u);
    float scale;
    memcpy(&scale, &bits, 4);
    fprintf(stderr, "[attract-stage] projection multiplier %.4f at poll %u\n", (double)scale, frames);
  }
  /* ATTRACT_STAGE_TEST=shot with ATTRACT_STAGE_SCENE=<field shot file> */
  if (test && strcmp(test, "shot") == 0) {
    static int loaded;
    if (!loaded) {
      loaded = 1;
      if (!load_shot(getenv("ATTRACT_STAGE_SCENE")))
        fprintf(stderr, "[attract-stage] shot file missing or invalid\n");
    }
    run_shot(s, frames);
  }
  /* ATTRACT_STAGE_TEST=scene with ATTRACT_STAGE_SCENE=<file> */
  if (!bridge_done && frames > 300 && test && strcmp(test, "scene") == 0) {
    bridge_done = 1;
    if (load_scene(getenv("ATTRACT_STAGE_SCENE")))
      stage_scene(s);
    else
      fprintf(stderr, "[attract-stage] scene file missing or invalid\n");
  }
  /* ATTRACT_STAGE_TEST=warp:<hex floor>:<spawn index> via floorLink */
  if (!bridge_done && frames > 300 && test && strncmp(test, "warp:", 5) == 0) {
    bridge_done = 1;
    char *end = NULL;
    const uint32_t floor = (uint32_t)strtoul(test + 5, &end, 16);
    const uint32_t spawn = end && *end == ':' ? (uint32_t)strtoul(end + 1, NULL, 10) : 0;
    int ok = 0;
    fprintf(stderr, "[attract-stage] floor manager state %u, request %u\n",
            read32(s, 0x8047ACD8u), read32(s, 0x8047ACDCu));
    if (read32(s, 0x8047ACD8u) != 2) {
      fprintf(stderr, "[attract-stage] floor manager not running, no warp\n");
    } else {
      call_guest(s, 0x80113828u, floor, spawn, &ok);
      fprintf(stderr, "[attract-stage] warp to floor 0x%X spawn %u %s\n", floor, spawn,
              ok ? "requested" : "failed");
    }
  }
  if (frames % 120 == 0 && test && strncmp(test, "warp:", 5) == 0)
    fprintf(stderr, "[attract-stage] floor now 0x%X\n",
            read32(s, read32(s, read32(s, 0x8047ACC8u)) + 0xC));
  if (!bridge_done && frames > 300 && test && strcmp(test, "trainers") == 0) {
    bridge_done = 1;
    dump_trainers(s);
  }
  if (!bridge_done && frames > 300 && test && strcmp(test, "progress") == 0) {
    bridge_done = 1;
    dump_progress(s);
  }
  /* ATTRACT_STAGE_TEST=dump:<hex encounter id> */
  if (!bridge_done && frames > 300 && test && strncmp(test, "dump:", 5) == 0) {
    bridge_done = 1;
    dump_tables(s, (uint32_t)strtoul(test + 5, NULL, 16));
  }
  /* ATTRACT_STAGE_TEST=battle:<hex encounter id> */
  if (!bridge_done && frames > 300 && test && strncmp(test, "battle:", 7) == 0) {
    bridge_done = 1;
    start_battle(s, (uint32_t)strtoul(test + 7, NULL, 16));
  }
}

static void on_load(const ModernGekkoModHostApi *host) {
  api = host;
  fprintf(stderr, "[attract-stage] loaded (guest calls %s)\n",
          host && host->call_guest ? "available" : "unavailable");
}

/* Only active with ATTRACT_STAGE_MENUS=1 (the patches replace the menus for
   the whole session); see waza_menu_patch. */
static const ModernGekkoModPatch patches[] = {
    RECOMP_PATCH(0x800117BCu, waza_menu_patch),
    RECOMP_PATCH(0x80011A1Cu, top_menu_patch),
    RECOMP_PATCH(0x800114A4u, party_menu_patch),
    RECOMP_PATCH(0x80011288u, target_menu_patch),
};

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(0x800FBE7Cu, message_log),
    RECOMP_HOOK(0x800E0C54u, rng_entry),
    RECOMP_HOOK_RETURN(0x800E0C54u, rng_return),
    RECOMP_HOOK(0x800AB150u, pad_entry),
    RECOMP_HOOK_RETURN(0x800AB150u, pad_return),
    RECOMP_HOOK_RETURN(0x80191118u, bound_test_return),
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
    NULL,
    0,
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
  const char *menus = getenv("ATTRACT_STAGE_MENUS");
  if (menus && menus[0] == '1') {
    descriptor.patches = patches;
    descriptor.num_patches = sizeof(patches) / sizeof(patches[0]);
  }
  return &descriptor;
}
