/* Colosseum battles between two Colosseum saves.
 *
 * Colosseum Battle > Gang Battle > "GC controller vs GBA" normally boots a
 * Game Boy Advance in socket 2 and reads its Ruby/Sapphire party. This mod
 * replaces that step: player 2 becomes another Colosseum trainer (name, ID,
 * team) taken from a trainer file, and because that trainer comes from
 * Colosseum (home place 0) the game itself treats player 2 as a GameCube
 * player -- team selection and the battle menus are Colosseum's own, on
 * controller 2. No GBA, BIOS or link cable is involved.
 *
 * Milestone 1 (one PC). With COLO_ONLINE_DIR set:
 *   <dir>/me.hero        written each time the connect step runs: this
 *                        player's trainer (0xB18 bytes, as in game memory)
 *   <dir>/opponent.hero  if present, becomes player 2
 * Without opponent.hero the game's own GBA connection runs as usual.
 *
 * Online (online_battle_net.c): with COLO_ONLINE_HOST=<port> on one side and
 * COLO_ONLINE_JOIN=<host>:<port> on the other, the trainers, the battle
 * seed, the team picks and every turn's choice travel over TCP instead.
 * Both games lay the battle out the same way -- the host's trainer is player
 * 1, the guest's player 2 -- and each maps its own player to controller 1 and
 * the other player to controller 2, whose choices come from the network.
 *
 * Addresses: GC6E01 (USA). Decomp references: src/game/menu/cardesavedata.c
 * (fn_800849B4), src/game/menu/menu_middle.c (fn_8006A824). */
#include "moderngekko/mod_abi.h"
#include "online_battle_net.h"
#include "online_battle_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void wait_ms(unsigned ms) { Sleep(ms); }
#else
#include <unistd.h>
static void wait_ms(unsigned ms) { usleep(ms * 1000u); }
#endif

enum {
  kConnect = 0x800849B4u,             /* fn_800849B4(mode, command, heroes[4], out) */
  kSavedataGetStatus = 0x80129280u,   /* savedataGetStatus(save, 2) -> hero */
  kHeroBiosCopy = 0x8012AC64u,        /* heroBiosCopy(dst, src) */
  kHeroBiosSetHomePlace = 0x8012A774u, /* 0 Colosseum, 1 Ruby/Sapphire, 2 FR/LG */
  kHeroBiosSetSexDataId = 0x8012AA44u, /* Colosseum trainers are male (asserted) */
  kGbaVsGcCommand = 0x1Au,            /* Colosseum Battle: player 1 GameCube, ports 2+ GBA */
  kHeroSize = 0xB18u,
  kTeamSelectState = 0x803A9F08u,     /* lbl_803A9F08; +4+player = ready */
  kEntryOrderCount = 0x8025D560u,     /* toolentryTaisengetEtnryPokemonOrderNum(player) */
  kEntryPokemonCount = 0x8025D89Cu,   /* toolentryTaisenGetPokemonNum(player) */
  kRulePokemonCount = 0x8006B1D4u,    /* how many each player brings */
  kTrainerGetStatus = 0x801FB1C0u,    /* fightTrainerGetStatus(trainer, data id, field, index) */
};

static const ModernGekkoModHostApi *api;
/* Set while the other player is a Colosseum trainer (file or network). */
static int s_virtual_p2;
static void seed_battle_rng(void);
static void new_battle(void);
static void write32(CPUState *s, uint32_t a, uint32_t v);
static unsigned s_start_frames;
static int s_local_switch_menu; /* our replacement menu is open */
static uint32_t s_turn;

static int online(void) { return net_role() != NET_OFF; }

/* Battle player index (0 or 1) of the other player: the guest sees the host
   as player 1 (index 0). */
static uint32_t remote_player(void) { return net_role() == NET_GUEST ? 0u : 1u; }

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

static uint32_t call(CPUState *s, uint32_t address, uint32_t r3, uint32_t r4, int *ok) {
  CPUState copy = *s;
  copy.gpr[3] = r3;
  copy.gpr[4] = r4;
  *ok = api && api->call_guest && api->call_guest(api->user_data, &copy, address);
  return *ok ? copy.gpr[3] : 0;
}

static const char *data_dir(void) {
  const char *dir = getenv("COLO_ONLINE_DIR");
  return dir && dir[0] ? dir : NULL;
}

static FILE *open_file(const char *name, const char *mode) {
  const char *dir = data_dir();
  if (!dir)
    return NULL;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return fopen(path, mode);
}

/* The trainer's name: the GS-string at hero+0 (UTF-16BE, up to 10). */
static void hero_name(CPUState *s, uint32_t hero, char *out, size_t size) {
  const uint8_t *p = memory(s, hero, kHeroSize);
  size_t n = 0;
  for (uint32_t i = 0; p && i < 10 && n + 1 < size; ++i) {
    const uint16_t c = (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
    if (!c)
      break;
    out[n++] = c < 0x80 ? (char)c : '?';
  }
  out[n] = 0;
}

/* ---- Settings and status (host ABI 2) -------------------------------------
   Player 1's [OnlineBattle] turn_seconds / auto_choose apply to both games.
   Status lines show on screen while online. */
static NetSettings s_battle_settings;

static int setting_int(const char *key, int fallback) {
  char value[32];
  if (!api || api->abi_version < 2 || !api->get_setting ||
      !api->get_setting(api->user_data, "OnlineBattle", key, value, sizeof(value)))
    return fallback;
  if (!strcmp(value, "true") || !strcmp(value, "True"))
    return 1;
  if (!strcmp(value, "false") || !strcmp(value, "False"))
    return 0;
  return atoi(value);
}

static void show_status(const char *text) {
  static char shown[256];
  if (!api || api->abi_version < 2 || !api->set_status)
    return;
  if (!strcmp(shown, text ? text : ""))
    return;
  snprintf(shown, sizeof(shown), "%s", text ? text : "");
  api->set_status(api->user_data, MOD_ID, text);
}

static NetSettings local_settings(void) {
  NetSettings settings;
  const int seconds = setting_int("turn_seconds", 0);
  settings.turn_seconds = seconds > 0 && seconds <= 3600 ? (uint32_t)seconds : 0;
  settings.auto_choose = setting_int("auto_choose", 1) != 0;
  return settings;
}

/* The rules both games must agree on: the connect command, singles or
   doubles (toolentryTaisenGetBattleType) and how many Pokemon each brings. */
static uint32_t local_rules(CPUState *s, uint32_t command) {
  int ok = 0;
  const uint32_t type = call(s, 0x8025DA88u, 0, 0, &ok) & 0xFFu;
  const uint32_t count = call(s, kRulePokemonCount, 0, 0, &ok) & 0xFFu;
  return (command & 0xFFu) << 16 | count << 8 | type;
}

static char s_problem[160]; /* why the last connect failed, for the status */

/* The other trainer, the seed, player 1's settings and both sides' rules,
   waited for as the GBA connection would wait for a link (up to a minute;
   the other player may still be in the menus). */
static int online_handshake(CPUState *s, uint32_t command, uint8_t *opponent) {
  const int player1 = net_role() == NET_HOST;
  const uint32_t rules = local_rules(s, command);
  s_problem[0] = 0;
  if (player1) {
    s_battle_settings = local_settings();
    net_send_settings(&s_battle_settings);
  }
  net_send_rules(rules);
  uint32_t seed = 0, remote_rules = 0;
  NetSettings settings;
  for (int i = 0; i < 1200; ++i) {
    const int ready = net_remote_hero(NULL) && net_seed(&seed) &&
                      net_remote_rules(&remote_rules) && (player1 || net_settings(&settings));
    if (ready)
      break;
    if (!net_connected())
      break;
    show_status("Online battle: waiting for the other player to choose the same battle...");
    wait_ms(50);
  }
  if (!net_connected()) {
    snprintf(s_problem, sizeof(s_problem), "Online battle: the other player is not connected.");
    return 0;
  }
  if (!net_remote_hero(opponent) || !net_seed(&seed) || !net_remote_rules(&remote_rules) ||
      (!player1 && !net_settings(&settings))) {
    snprintf(s_problem, sizeof(s_problem),
             "Online battle: the other player did not start the battle in time.");
    return 0;
  }
  if (remote_rules != rules) {
    snprintf(s_problem, sizeof(s_problem),
             "Online battle: the other player chose different rules (%s, %u Pokemon here; "
             "%s, %u there).",
             (rules & 0xFF) == 1 ? "doubles" : "singles", (rules >> 8) & 0xFF,
             (remote_rules & 0xFF) == 1 ? "doubles" : "singles", (remote_rules >> 8) & 0xFF);
    fprintf(stderr, "[online-battle] rules differ: ours %06X, theirs %06X\n", rules, remote_rules);
    return 0;
  }
  if (!player1)
    s_battle_settings = settings;
  fprintf(stderr, "[online-battle] turn timer %u s, on timeout %s\n", s_battle_settings.turn_seconds,
          s_battle_settings.auto_choose ? "choose" : "give up");
  return 1;
}

static void connect_patch(CPUState *s) {
  const uint32_t mode = s->gpr[3], command = s->gpr[4], heroes = s->gpr[5];
  int ok = 0;
  const uint32_t own = call(s, kSavedataGetStatus, 0, 2, &ok);
  const uint8_t *own_bytes = ok && own ? memory(s, own, kHeroSize) : NULL;
  fprintf(stderr, "[online-battle] connect mode %u command 0x%X heroes %08X own %08X%s\n", mode,
          command, heroes, own, ok ? "" : " (guest call failed)");

  /* Keep this player's trainer for the other side. */
  if (own_bytes) {
    FILE *f = open_file("me.hero", "wb");
    if (f) {
      fwrite(own_bytes, 1, kHeroSize, f);
      fclose(f);
    }
  }

  s_virtual_p2 = 0;
  if (command != kGbaVsGcCommand || mode != 0 || !heroes || !own_bytes)
    return; /* not a Colosseum Battle connect: the game's own code runs */
  uint8_t opponent[kHeroSize];
  if (online()) {
    if (!online_handshake(s, command, opponent))
      return;
  } else {
    FILE *f = open_file("opponent.hero", "rb");
    if (!f)
      return; /* no opponent: a real GBA connects as usual */
    const size_t got = fread(opponent, 1, sizeof(opponent), f);
    fclose(f);
    if (got != sizeof(opponent)) {
      fprintf(stderr, "[online-battle] opponent.hero is %zu bytes, need %u\n", got, kHeroSize);
      return;
    }
  }

  const uint32_t p1 = read32(s, heroes), p2 = read32(s, heroes + 4);
  if (!p1 || !p2 || !memory(s, p1, kHeroSize) || !memory(s, p2, kHeroSize))
    return;
  /* Host (and one-PC) layout: own trainer first. The guest's own trainer is
     player 2, so both games set up the same battle. */
  const uint32_t mine = net_role() == NET_GUEST ? p2 : p1;
  const uint32_t theirs = net_role() == NET_GUEST ? p1 : p2;
  call(s, kHeroBiosCopy, mine, own, &ok); /* what the game does for player 1 */
  memcpy(memory(s, theirs, kHeroSize), opponent, kHeroSize);
  call(s, kHeroBiosSetHomePlace, theirs, 0, &ok);
  call(s, kHeroBiosSetSexDataId, theirs, 0, &ok);

  char a[16], b[16];
  hero_name(s, mine, a, sizeof(a));
  hero_name(s, theirs, b, sizeof(b));
  fprintf(stderr, "[online-battle] %s (controller 1) vs %s (controller 2)%s\n", a, b,
          net_role() == NET_GUEST ? ", as guest" : net_role() == NET_HOST ? ", as host" : "");
  s_virtual_p2 = 1;
  new_battle();
  seed_battle_rng();
  moderngekko_mod_return_u32(s, 0); /* connected */
}

/* fn_8006A81C(entry, controller): Colosseum Battle assigns controllers to
   the players (state 0xE4, 0x8005BF48-0x8005C0BC), player 1 controller 1,
   player 2 controller 2. On the guest the other way round: its own trainer,
   player 2, is on controller 1. */
static int s_in_set_controller;

static void set_controller_patch(CPUState *s) {
  const uint32_t controller = s->gpr[4];
  if (s_in_set_controller || net_role() != NET_GUEST || s->lr < 0x8005BF00u ||
      s->lr >= 0x8005C100u || (controller != 1 && controller != 2))
    return;
  int ok = 0;
  s_in_set_controller = 1; /* the call below runs the original */
  call(s, 0x8006A81Cu, s->gpr[3], 3 - controller, &ok);
  s_in_set_controller = 0;
  if (ok)
    moderngekko_mod_return_u32(s, s->gpr[3]);
}

/* Team selection (fn_800679C0, once a frame). The game shows its "Is this
   okay?" confirmation, and marks a player ready, only for the player on
   socket 1 with a Colosseum save; every other player was a GBA that reports
   "done" itself (lbl_803A9F08[4 + player] = 1). A GameCube player 2 would pick
   and then wait forever, so mark it ready once its picks are complete and the
   last ball has settled (half a second). */
/* Online, the other player picks on their own machine: once they confirm,
   their order arrives and is written into their entry (fn_8006B09C(player):
   +0x08 six orders, +0x20 count), and they are marked ready. Our own picks
   go out when the game marks us ready. */
static uint8_t s_remote_picks[6];
static int s_remote_pick_count = -1;
static int s_sent_picks;

/* Marks player 2 (index 1) ready once its picks are complete and settled;
   the game itself only confirms player 1's. */
static void settle_player2(CPUState *s, uint8_t *ready) {
  static unsigned settled;
  if (!ready || *ready) {
    settled = 0;
    return;
  }
  int ok1 = 0, ok2 = 0, ok3 = 0;
  const uint32_t picked = call(s, kEntryOrderCount, 1, 0, &ok1);
  const uint32_t owned = call(s, kEntryPokemonCount, 1, 0, &ok2) & 0xFFFFu;
  const uint32_t rule = call(s, kRulePokemonCount, 0, 0, &ok3) & 0xFFFFu;
  const uint32_t needed = owned < rule ? owned : rule;
  if (!ok1 || !ok2 || !ok3 || needed == 0 || picked != needed) {
    settled = 0;
    return;
  }
  if (++settled >= 30) {
    *ready = 1;
    settled = 0;
    fprintf(stderr, "[online-battle] player 2 picked %u, ready\n", picked);
  }
}

static void team_select_online(CPUState *s) {
  const uint32_t theirs = remote_player(), mine = 1 - theirs;
  uint8_t *my_ready = memory(s, kTeamSelectState + 4 + mine, 1);
  uint8_t *their_ready = memory(s, kTeamSelectState + 4 + theirs, 1);
  int ok = 0;
  if (mine == 1)
    settle_player2(s, my_ready); /* the guest: no confirmation for player 2 */
  if (my_ready && *my_ready && !s_sent_picks) {
    const uint32_t entry = call(s, 0x8006B09Cu, mine, 0, &ok);
    const uint32_t count = ok ? read32(s, entry + 0x20) : 0;
    if (ok && count >= 1 && count <= 6) {
      uint8_t picks[6];
      for (uint32_t i = 0; i < count; ++i)
        picks[i] = (uint8_t)read32(s, entry + 8 + i * 4);
      net_send_picks(picks, (int)count);
      s_sent_picks = 1;
      fprintf(stderr, "[online-battle] sent our %u picks\n", count);
    }
  }
  uint8_t picks[6];
  const int arrived = net_take_remote_picks(picks);
  if (arrived >= 1) {
    memcpy(s_remote_picks, picks, (size_t)arrived);
    s_remote_pick_count = arrived;
  }
  if (s_remote_pick_count >= 1 && their_ready && !*their_ready) {
    const uint32_t entry = call(s, 0x8006B09Cu, theirs, 0, &ok);
    if (!ok || !memory(s, entry, 0x24))
      return;
    for (uint32_t i = 0; i < 6; ++i)
      write32(s, entry + 8 + i * 4,
              i < (uint32_t)s_remote_pick_count ? s_remote_picks[i] : 0xFFFFFFFFu);
    write32(s, entry + 0x20, (uint32_t)s_remote_pick_count);
    *their_ready = 1;
    fprintf(stderr, "[online-battle] the other player picked %d, ready\n", s_remote_pick_count);
  }
}

static void team_select_return(CPUState *s) {
  if (!s_virtual_p2)
    return;
  s_start_frames = 0; /* team selection is up: the START prompt is past */
  if (online()) {
    team_select_online(s);
    return;
  }
  settle_player2(s, memory(s, kTeamSelectState + 4 + 1, 1));
}


/* GameCube controller icons for player 2. The Gang Battle list (row "Battle
   with 2", right-hand icon, layout item 0x989) and the "Is this okay?"
   screen (item 0x99D) are layout records in game memory (lbl_802EF0A8, 7
   words each; word 3 = height << 16 | sprite id). Sprite 0x27D is the
   GameCube controller, 0x27C the GBA. While an opponent trainer is set up,
   point both at the controller; otherwise restore them. Checked from the pad poll,
   about once a second. Item 0x99D is shared with the GBA-vs-GBA screen,
   which then shows a controller on the right too while online. */
typedef struct IconSwap {
  uint32_t word3_address, gba_word3, gc_word3;
} IconSwap;

static const IconSwap kIconSwaps[] = {
    {0x802FFBACu, 0x002E027Cu, 0x0031027Du}, /* Gang Battle list, row 0, item 0x989 */
    {0x802FFDDCu, 0x002E027Cu, 0x0031027Du}, /* "Is this okay?" item 0x99D */
};

static void write32(CPUState *s, uint32_t a, uint32_t v) {
  uint8_t *p = memory(s, a, 4);
  if (p) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
  }
}

static int opponent_ready(void) {
  if (online()) {
    static uint8_t scratch[kHeroSize];
    return net_remote_hero(scratch);
  }
  FILE *f = open_file("opponent.hero", "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

/* Online: our trainer goes to the other side whenever it changes (the save
   loads, the party changes), so it is there before Gang Battle connects. */
static void send_own_hero(CPUState *s) {
  static uint32_t sent_hash;
  static int sent_connected;
  const int connected = net_connected();
  if (!connected) {
    sent_connected = 0;
    return;
  }
  int ok = 0;
  const uint32_t own = call(s, kSavedataGetStatus, 0, 2, &ok);
  const uint8_t *bytes = ok && own ? memory(s, own, kHeroSize) : NULL;
  if (!bytes)
    return;
  /* Before the save loads the name is garbage: wait for 1-10 characters of
     plain text (UTF-16BE, Latin-1 range). */
  uint32_t length = 0;
  while (length < 10 && (bytes[length * 2] || bytes[length * 2 + 1]))
    ++length;
  for (uint32_t i = 0; i < length; ++i)
    if (bytes[i * 2] || bytes[i * 2 + 1] < 0x20)
      return;
  if (!length)
    return;
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < kHeroSize; ++i)
    hash = (hash ^ bytes[i]) * 16777619u;
  if (sent_connected && hash == sent_hash)
    return;
  net_send_hero(bytes);
  sent_hash = hash;
  sent_connected = 1;
  char name[16];
  hero_name(s, own, name, sizeof(name));
  fprintf(stderr, "[online-battle] sent our trainer %s\n", name);
}

/* "Are these Players okay?" (menu 0xB6) waits for A on every controller.
   Controller 2 is the other player, who confirms on their own machine:
   press A for it (a few frames on, a few off). */
static uint32_t s_pad_status;

static void pad_poll_entry(CPUState *s) { s_pad_status = s->gpr[3]; }

/* Then "To start the battle, press START on Player 1's Controller": on the
   guest, player 1 is the host, who starts on their side. That prompt follows
   the players screen, so START is pressed for controller 2 from when the
   players screen closes until team selection runs (at most 15 s). */

static void confirm_players_for_remote(CPUState *s) {
  static unsigned frame;
  static int players_open;
  int ok = 0;
  uint8_t button = 0;
  if (!s_pad_status)
    return;
  const int open = (call(s, 0x80102620u, 0xB6, 0, &ok) & 0xFF) && ok; /* menuIsCheck */
  if (open) {
    /* A (0x0100) for "Okay", alternating with START (0x1000) on the guest:
       the START prompt opens over the still-open players screen. */
    button = net_role() == NET_GUEST && (frame / 12) % 2 ? 0x10 : 0x01;
    s_start_frames = 0;
  } else if (players_open && net_role() == NET_GUEST) {
    s_start_frames = 15 * 60;
  }
  players_open = open;
  if (!button && s_start_frames) {
    --s_start_frames;
    button = 0x10; /* START */
  }
  if (!button) {
    frame = 0;
    return;
  }
  uint8_t *port2 = memory(s, s_pad_status + 12, 12); /* PADStatus[1] */
  if (!port2 || ++frame % 12 >= 3)
    return;
  port2[0] |= button;
  port2[10] = 0; /* err: PAD_ERR_NONE */
}

/* While the game waits for the other player's choice (link_poll_patch counts
   its polls; s_polls is the pad poll count when it last asked). */
static unsigned s_polls;
static unsigned s_wait_polls, s_last_wait_poll;
static unsigned s_switch_menu_poll; /* when our replacement menu opened */

static void remote_name(char *out, size_t size) {
  static uint8_t hero[kHeroSize];
  size_t n = 0;
  if (net_remote_hero(hero)) {
    for (uint32_t i = 0; i < 10 && n + 1 < size; ++i) {
      const uint16_t c = (uint16_t)((hero[i * 2] << 8) | hero[i * 2 + 1]);
      if (!c)
        break;
      out[n++] = c < 0x80 ? (char)c : '?';
    }
  }
  if (!n)
    n = (size_t)snprintf(out, size, "the other player");
  out[n < size ? n : size - 1] = 0;
}

static void update_status(void) {
  char text[256], name[24];
  remote_name(name, sizeof(name));
  const uint32_t desync = net_desync_turn();
  const int state = net_state();
  if (state != NET_STATE_CONNECTED)
    s_problem[0] = 0; /* a new connection starts over */
  switch (state) {
  case NET_STATE_OPENING_PORT:
    snprintf(text, sizeof(text), "Online battle: opening TCP port %u on the router...", net_port());
    break;
  case NET_STATE_LISTENING: {
    char ip[64];
    const int upnp = net_upnp(ip, sizeof(ip));
    if (upnp == UPNP_MAPPED && ip[0] && upnp_address_is_private(ip))
      snprintf(text, sizeof(text),
               "Online battle: waiting on port %u. Port opened, but your router is behind another "
               "one (%s): players outside your network may not reach you",
               net_port(), ip);
    else if (upnp == UPNP_MAPPED)
      snprintf(text, sizeof(text),
               "Online battle: waiting for the other player. Give them %s%s%u",
               ip[0] ? ip : "your public address", ip[0] ? ":" : ", port ", net_port());
    else if (upnp == UPNP_NO_ROUTER || upnp == UPNP_REFUSED)
      snprintf(text, sizeof(text),
               "Online battle: waiting on TCP port %u. The router did not open it: forward it by "
               "hand for players outside your network",
               net_port());
    else
      snprintf(text, sizeof(text), "Online battle: waiting for the other player to connect (TCP port %u)",
               net_port());
    break;
  }
  case NET_STATE_LISTEN_FAILED:
    snprintf(text, sizeof(text), "Online battle: cannot use TCP port %u. Is it in use?", net_port());
    break;
  case NET_STATE_CONNECTING:
    snprintf(text, sizeof(text), "Online battle: connecting to %s...", net_target());
    break;
  case NET_STATE_HANDSHAKE:
    snprintf(text, sizeof(text), "Online battle: connecting...");
    break;
  case NET_STATE_VERSION_MISMATCH:
    snprintf(text, sizeof(text), "Online battle: the other player has a different version of the game");
    break;
  case NET_STATE_DISCONNECTED:
    snprintf(text, sizeof(text), "Online battle: the connection to %s was lost", name);
    break;
  case NET_STATE_CONNECTED:
    if (desync)
      snprintf(text, sizeof(text), "Online battle: the games went out of sync at turn %u. The battle was stopped.",
               desync);
    else if (s_problem[0])
      snprintf(text, sizeof(text), "%s", s_problem);
    else if (s_virtual_p2 && s_polls - s_last_wait_poll < 10 && s_wait_polls > 90)
      snprintf(text, sizeof(text), "Waiting for %s...", name);
    else if (s_virtual_p2)
      text[0] = 0;
    else
      snprintf(text, sizeof(text), "Online battle: connected to %s. Colosseum Battle > Gang Battle > two controllers",
               name);
    break;
  default:
    text[0] = 0;
    break;
  }
  show_status(text);
}

/* The replacement menu after a faint has no timer of its own (the game
   checks the clock only before opening it). With a turn timer, once the
   turn's time has passed, pick for the player: up to the first Pokemon that
   can come out, A to choose it, A to send it out, again until it closes. */
static void replacement_timeout(CPUState *s) {
  const int seconds = (int)s_battle_settings.turn_seconds;
  if (!s_local_switch_menu || !seconds || !s_pad_status)
    return;
  const unsigned waited = s_polls - s_switch_menu_poll;
  if (waited < (unsigned)seconds * 60u)
    return;
  if (waited == (unsigned)seconds * 60u)
    fprintf(stderr, "[online-battle] turn %u: time ran out on the replacement, choosing\n", s_turn);
  uint8_t *port1 = memory(s, s_pad_status, 12); /* PADStatus[0]: our player */
  const unsigned step = (waited / 12u) % 6u;
  if (!port1 || waited % 12u >= 4u)
    return;
  port1[10] = 0;
  if (step == 0)
    port1[1] |= 0x08; /* up (0x0008) */
  else if (step == 2 || step == 4)
    port1[0] |= 0x01; /* A (0x0100) */
}

/* Test helper: <COLO_ONLINE_DIR>/warp.txt holding "floor [entry]" (hex or
   decimal) sends the player there with the game's own door change
   (floorLink, 0x80113828), e.g. 0x390 for the Phenac Pokemon Center
   basement, where the trade terminal is. The file is removed once used. */
static void warp_from_file(CPUState *s) {
  FILE *f = open_file("warp.txt", "rb");
  if (!f)
    return;
  char line[64] = {0};
  fread(line, 1, sizeof(line) - 1, f);
  fclose(f);
  char path[1024];
  snprintf(path, sizeof(path), "%s/warp.txt", data_dir());
  remove(path);
  char *end = NULL;
  const uint32_t floor = (uint32_t)strtoul(line, &end, 0);
  const uint32_t entry = end ? (uint32_t)strtoul(end, NULL, 0) : 0;
  int ok = 0;
  if (floor)
    call(s, 0x80113828u, floor, entry, &ok);
  fprintf(stderr, "[online-battle] warp to floor %#x entry %u%s\n", floor, entry, ok ? "" : " failed");
}

static void pad_poll_return(CPUState *s) {
  static unsigned polls;
  static int shown = -1;
  ++s_polls;
  if (!online() && data_dir() && polls % 30 == 0)
    warp_from_file(s);
  if (online())
    replacement_timeout(s);
  if (online() && s_virtual_p2)
    confirm_players_for_remote(s);
  if (online() && polls % 30 == 0)
    update_status();
  if (polls++ % 60 != 0)
    return;
  if (online())
    send_own_hero(s);
  const int want = opponent_ready();
  if (want == shown)
    return;
  for (size_t i = 0; i < sizeof(kIconSwaps) / sizeof(kIconSwaps[0]); ++i) {
    const IconSwap *swap = &kIconSwaps[i];
    const uint32_t now = read32(s, swap->word3_address);
    /* Only touch records that still hold one of the two expected values. */
    if (now != swap->gba_word3 && now != swap->gc_word3)
      continue;
    write32(s, swap->word3_address, want ? swap->gc_word3 : swap->gba_word3);
  }
  shown = want;
}
/* ---- Random-number callers (survey) -------------------------------------
   COLO_ONLINE_RNG_SURVEY=1 counts the callers (return address) of the two
   game-level random functions, fn_800E0C54 (u16) and _fadeEffectGetRandom
   (0x800E0C04, value % n), and prints them at each turn's command phase. */
typedef struct CallerCount {
  uint32_t lr, caller2;
  uint32_t which;
  uint32_t count;
} CallerCount;
static CallerCount s_callers[1024];
static unsigned s_caller_count;

static int rng_survey(void) {
  const char *v = getenv("COLO_ONLINE_RNG_SURVEY");
  return v && v[0] == '1';
}

/* The caller's own return address: the LR it saved in its caller's frame
   (back chain at 0(r1), LR save word at +4). */
static uint32_t caller2_of(CPUState *s) {
  const uint32_t back = read32(s, s->gpr[1]);
  return back ? read32(s, back + 4) : 0;
}

static void count_caller(CPUState *s, uint32_t which) {
  const uint32_t lr = s->lr, caller2 = caller2_of(s);
  for (unsigned i = 0; i < s_caller_count; ++i)
    if (s_callers[i].lr == lr && s_callers[i].caller2 == caller2 && s_callers[i].which == which) {
      ++s_callers[i].count;
      return;
    }
  if (s_caller_count < sizeof(s_callers) / sizeof(s_callers[0]))
    s_callers[s_caller_count++] = (CallerCount){lr, caller2, which, 1};
}

static void dump_callers(const char *when) {
  if (!s_caller_count)
    return;
  fprintf(stderr, "[online-battle] rng callers %s:", when);
  for (unsigned i = 0; i < s_caller_count; ++i)
    fprintf(stderr, " %c%08X<%08X*%u", s_callers[i].which ? 'm' : 'u', s_callers[i].lr,
            s_callers[i].caller2, s_callers[i].count);
  fprintf(stderr, "\n");
  s_caller_count = 0;
}

/* ---- The battle's own random numbers -------------------------------------
   Colosseum has one random generator (lbl_80478C90, LCG * 0x343FD +
   0x269EC3, output state >> 16) for everything: damage rolls, hits and
   critical hits, but also camera moves, effects and menus, whose draws vary
   with timing. Two games running one battle would part ways within a turn.
   So while a battle with player 2 runs, draws made by the battle code
   (0x801E0000-0x8026FFFF) while the turn plays out come from a private
   generator with the same formula, seeded identically on both sides.
   Draws during the command phase (menus pick default targets and the like,
   as often as the player moves the cursor) and draws from the camera and
   effects (0x801C0000-0x801DFFFF) stay on the game's generator: each side's
   view may differ, the battle may not. */
static uint32_t s_battle_rng;
static uint32_t s_battle_draws;
static int s_command_phase;

static int battle_draw(CPUState *s) {
  return s_virtual_p2 && !s_command_phase && s->lr >= 0x801E0000u && s->lr < 0x80270000u;
}

static uint32_t battle_next(void) {
  s_battle_rng = s_battle_rng * 0x343FDu + 0x269EC3u;
  ++s_battle_draws;
  return s_battle_rng >> 16;
}

static void seed_battle_rng(void) {
  const char *v = getenv("COLO_ONLINE_SEED");
  s_battle_rng = v && v[0] ? (uint32_t)strtoul(v, NULL, 0) : 0x434F4C4Fu; /* "COLO" */
  if (online())
    net_seed(&s_battle_rng); /* the host's draw */
  s_battle_draws = 0;
  s_turn = 0;
  s_command_phase = 0;
  fprintf(stderr, "[online-battle] battle seed %08X\n", s_battle_rng);
}

static void rng_u16_patch(CPUState *s) {
  if (rng_survey())
    count_caller(s, 0);
  if (battle_draw(s))
    moderngekko_mod_return_u32(s, battle_next());
}

static void rng_mod_patch(CPUState *s) {
  if (rng_survey())
    count_caller(s, 1);
  if (!battle_draw(s))
    return;
  const uint32_t n = s->gpr[3];
  const uint32_t a = battle_next(), b = battle_next();
  moderngekko_mod_return_u32(s, n ? ((b << 16) | a) % n : 0);
}

/* Command phase: fightMenuOpenInfoMenu .. fightMenuCloseInfoMenu around
   every turn's choices. The state at its start is the turn's checksum. */
static void command_phase_start(CPUState *s) {
  (void)s;
  if (!s_virtual_p2)
    return;
  s_command_phase = 1;
}

/* A new turn: the first action selection after the battle drew numbers
   (both trainers choose with no battle draws in between). The info menu
   above also reopens around switch menus, so it cannot count turns. */
static void count_turn(void) {
  static uint32_t counted_draws = 0xFFFFFFFFu;
  if (!s_virtual_p2 || s_battle_draws == counted_draws)
    return;
  counted_draws = s_battle_draws;
  ++s_turn;
  fprintf(stderr, "[online-battle] turn %u: battle rng %08X after %u draws\n", s_turn, s_battle_rng,
          s_battle_draws);
  /* Safety net: both games must reach every turn with the same generator. */
  if (online())
    net_send_check(s_turn, s_battle_rng ^ (s_battle_draws * 0x9E3779B1u));
}

static void command_phase_end(CPUState *s) {
  (void)s;
  s_command_phase = 0;
}
/* ---- Player 2's turns ----------------------------------------------------
   The battle asks every trainer for its action at once, and only one
   GameCube command menu can be on screen: the game was built for one
   GameCube player, the others choosing on their GBAs. So player 2 goes the
   GBA way: fightMenuFightTrainerAgbHeroOpenMenu sends the move data down the
   link and waits (animating "Waiting...", honouring the turn timer and an
   abort) for a 32-bit reply. Here the link calls for player 2's channel are
   answered locally, and the reply is player 2's choice:
     kind << 16 | target << 8 | move slot  (kind 1: move)
     kind 2: switch to party index (low 16 bits)
     kind 3: give up   kind 4: let the game choose (its timeout pick)
   The choice comes from <dir>/p2.choice ("move N", "switch N", "giveup",
   "auto"), read once; COLO_ONLINE_P2_AUTO=1 answers "auto" at once. Later
   it comes from the network. */
enum {
  kRemoteController = 2,  /* player 2: GSInputDevice 2, link channel 1 */
  kRemoteChannel = kRemoteController - 1,
  kSelectAction = 0x801F923Cu,       /* fightTrainerSelectFightAction(trainer, param) */
  kSelectSwitch = 0x801F9600u,       /* fightTrainerSelectIrekaeFightPokemon(trainer, ...) */
  kGbaSelectAction = 0x80262508u,    /* fightMenuFightTrainerAgbHeroOpenMenu */
  kGbaSelectSwitch = 0x80262D3Cu,    /* fightMenuFightTrainerAgbHeroSelectIrekaeFightPokemon */
  kFightActionInit = 0x801F1460u,
  kOutPokemonInitAction = 0x80207760u,
  kBattleAborted = 0x801EF634u,
};

static uint32_t call4(CPUState *s, uint32_t address, uint32_t a, uint32_t b, uint32_t c,
                      uint32_t d, int *ok) {
  CPUState copy = *s;
  copy.gpr[3] = a;
  copy.gpr[4] = b;
  copy.gpr[5] = c;
  copy.gpr[6] = d;
  *ok = api && api->call_guest && api->call_guest(api->user_data, &copy, address);
  return *ok ? copy.gpr[3] : 0;
}

static int is_remote_trainer(CPUState *s, uint32_t trainer) {
  int ok = 0;
  if (!s_virtual_p2 || !trainer)
    return 0;
  const uint32_t controller = call4(s, kTrainerGetStatus, trainer, 0, 0x4B, 0, &ok);
  return ok && controller == kRemoteController;
}

/* Our own trainer in this battle (controller 1), for sending its choices. */
static uint32_t s_local_trainer;
static int s_local_switch_pending;
static int s_local_choices_left; /* out Pokemon still to choose for this turn */
static void send_choice(uint32_t word, const char *what);
static int party_index_of(CPUState *s, uint32_t id);

static int is_local_trainer(CPUState *s, uint32_t trainer) {
  int ok = 0;
  if (!s_virtual_p2 || !trainer)
    return 0;
  const uint32_t controller = call4(s, kTrainerGetStatus, trainer, 0, 0x4B, 0, &ok);
  return ok && controller == 1;
}

static void select_action_patch(CPUState *s) {
  const uint32_t trainer = s->gpr[3];
  if (rng_survey())
    dump_callers("before command phase");
  count_turn();
  s_local_switch_menu = 0;
  if (is_local_trainer(s, trainer)) {
    int ok = 0;
    if (s_local_switch_pending && online()) {
      /* The replacement was not seen being stored: send the Pokemon now out
         (fightOutPokemonGetPokemonPtr, battle id 0xCE). The other game is
         still waiting for it, before its own next turn. */
      const uint32_t out = call4(s, kTrainerGetStatus, trainer, 0, 0x46, 0, &ok);
      const uint32_t pokemon = call4(s, 0x80205B8Cu, out, 0, 0, 0, &ok);
      const int i = party_index_of(s, call4(s, 0x8012640Cu, pokemon, 0, 0xCE, 0, &ok));
      if (i >= 0)
        send_choice((2u << 16) | (uint32_t)i, "the replacement now out");
    }
    s_local_switch_pending = 0;
    s_local_trainer = trainer;
    s_local_choices_left = 0;
    for (uint32_t i = 0; i < 2; ++i) /* fightTrainerGetValidFightOutPokemonPtr */
      s_local_choices_left += call4(s, 0x801F981Cu, trainer, i, 0, 0, &ok) && ok;
  }
  if (!is_remote_trainer(s, trainer))
    return; /* player 1: the game's own GameCube menu */
  int ok = 0;
  /* What fightTrainerSelectFightAction does before choosing a menu. */
  const uint32_t action = call4(s, kTrainerGetStatus, trainer, 0, 0x4D, 0, &ok);
  if (ok && action)
    call4(s, kFightActionInit, action, 0, 0, 0, &ok);
  for (uint32_t i = 0; i < 2; ++i)
    call4(s, kOutPokemonInitAction, call4(s, kTrainerGetStatus, trainer, 0, 0x46, i, &ok), 0, 0,
          0, &ok);
  call4(s, kOutPokemonInitAction, call4(s, kTrainerGetStatus, trainer, 0, 0x47, 0, &ok), 0, 0, 0,
        &ok);
  if ((call4(s, kBattleAborted, 0, 0, 0, 0, &ok) & 0xFFFFu) == 1) {
    moderngekko_mod_return_u32(s, 0);
    return;
  }
  /* Continue in the GBA player's menu, with the same arguments and return. */
  s->pc = kGbaSelectAction;
}

static void select_switch_patch(CPUState *s) {
  if (s_virtual_p2) {
    int ok = 0;
    fprintf(stderr, "[online-battle] turn %u: replacement for trainer %08X (controller %u, data %u)\n",
            s_turn, s->gpr[3], call4(s, kTrainerGetStatus, s->gpr[3], 0, 0x4B, 0, &ok),
            call4(s, kTrainerGetStatus, s->gpr[3], 0, 0x43, 0, &ok) & 0xFFFFu);
  }
  if (is_local_trainer(s, s->gpr[3])) {
    s_local_trainer = s->gpr[3];
    s_local_switch_pending = 1; /* sent when the game stores the pick */
    s_local_switch_menu = 1;
    s_switch_menu_poll = s_polls;
  }
  if (is_remote_trainer(s, s->gpr[3]))
    s->pc = kGbaSelectSwitch; /* same arguments (trainer, p1, p2, p3) */
}

/* ---- Our choices, to the other side --------------------------------------
   The GameCube menu turns a choice into an action on the out Pokemon; the
   same reply words as the GBA's go to the other game, whose GBA-player menu
   turns them back into the same action. */
static int is_local_out_pokemon(CPUState *s, uint32_t slot) {
  int ok = 0;
  if (!s_local_trainer || !slot)
    return 0;
  for (uint32_t i = 0; i < 2; ++i)
    if (call4(s, kTrainerGetStatus, s_local_trainer, 0, 0x46, i, &ok) == slot && ok)
      return 1;
  return 0;
}

/* Party index (0-5) of the Pokemon whose battle id (status 0xCE) is id. */
static int party_index_of(CPUState *s, uint32_t id) {
  int ok = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t pokemon = call4(s, kTrainerGetStatus, s_local_trainer, 0, 0x45, i, &ok);
    if (!ok || !pokemon)
      continue;
    const uint32_t v = call4(s, 0x8012640Cu, pokemon, 0, 0xCE, 0, &ok); /* pokemonGetStatus */
    if (ok && (uint16_t)v == (uint16_t)id)
      return (int)i;
  }
  return -1;
}

static void send_choice(uint32_t word, const char *what) {
  if (s_local_choices_left > 0)
    --s_local_choices_left;
  net_send_choice(word);
  fprintf(stderr, "[online-battle] turn %u: sent %s (%06X)\n", s_turn, what, word);
}

/* fightOutPokemonCreateFightActionAttackWaza(slot, 0, 0x13, 0, name, move id,
   target, move slot, 0) */
static void attack_action_entry(CPUState *s) {
  if (!online() || s_local_choices_left <= 0 || !is_local_out_pokemon(s, s->gpr[3]))
    return;
  send_choice((1u << 16) | (s->gpr[10] & 0xFFu), "a move");
}

/* fightOutPokemonCreateFightAction(slot, 0, kind, 0, name, index): kind 8
   run (give up), kind 9 switch to the Pokemon with battle id index. */
static void action_entry(CPUState *s) {
  if (!online() || s_local_choices_left <= 0 || !is_local_out_pokemon(s, s->gpr[3]))
    return;
  if (s->gpr[5] == 8) {
    send_choice(3u << 16, "give up");
  } else if (s->gpr[5] == 9) {
    const int i = party_index_of(s, s->gpr[8]);
    if (i >= 0)
      send_choice((2u << 16) | (uint32_t)i, "a switch");
  }
}

/* After a faint, fn_8021F24C stores the replacement the menu returned with
   pokemonSetStatus(pokemon, 0, 0x121, 0, battle id). */
static void set_status_entry(CPUState *s) {
  if (s_local_switch_pending && s->gpr[5] == 0x121)
    fprintf(stderr, "[online-battle] (0x121 = %d from %08X)\n", (int)s->gpr[7], s->lr);
  if (!s_local_switch_pending || s->gpr[5] != 0x121 || s->lr < 0x8021F24Cu ||
      s->lr >= 0x8021F39Cu)
    return;
  s_local_switch_pending = 0;
  s_local_switch_menu = 0;
  const int i = party_index_of(s, s->gpr[7]);
  if (online() && i >= 0)
    send_choice((2u << 16) | (uint32_t)i, "a replacement");
}

/* fn_801F18DC: "is this a link battle" (floor status 0x1A == 0x10). In a
   link battle the GameCube player's replacement prompt ("1P: Switch
   POKeMON  R OK") never takes input once a GBA is not involved, so while
   our player picks a replacement the game shows its own single-player party
   screen instead. Only this menu's look depends on it. */
static void link_battle_patch(CPUState *s) {
  if (s_virtual_p2 && s_local_switch_menu)
    moderngekko_mod_return_u32(s, 0);
}

/* fightTimerCommandIsOver: the other player's turn timer runs on their
   machine (their timeout pick arrives as a choice), so the waits for them
   never time out here. */
static void timer_over_patch(CPUState *s) {
  const uint32_t lr = s->lr;
  if (!online() || !s_virtual_p2)
    return;
  if ((lr >= 0x80262508u && lr < 0x80262508u + 0x82Cu) ||
      (lr >= 0x80262D3Cu && lr < 0x80262D3Cu + 0x430u) ||
      (lr >= 0x80089F78u && lr < 0x80089F78u + 0xA24u))
    moderngekko_mod_return_u32(s, 0);
}

static int parse_choice(const char *line, uint32_t *word) {
  int n = 0;
  if (sscanf(line, " move %d", &n) == 1 && n >= 1 && n <= 4) {
    int target = 0;
    sscanf(line, " move %*d %d", &target);
    *word = (1u << 16) | ((uint32_t)(target & 0xFF) << 8) | (uint32_t)(n - 1);
    return 1;
  }
  if (sscanf(line, " switch %d", &n) == 1 && n >= 1 && n <= 6) {
    *word = (2u << 16) | (uint32_t)(n - 1);
    return 1;
  }
  if (strncmp(line, "giveup", 6) == 0) {
    *word = 3u << 16;
    return 1;
  }
  if (strncmp(line, "auto", 4) == 0) {
    *word = 4u << 16;
    return 1;
  }
  return 0;
}

/* Player 2's next choice, if one has been made. */
static int take_choice(uint32_t *word) {
  if (online()) {
    if (net_take_choice(word)) {
      fprintf(stderr, "[online-battle] turn %u: the other player chose %06X\n", s_turn, *word);
      return 1;
    }
    if (!net_connected()) {
      *word = 5u << 16; /* the other player left: end the battle */
      return 1;
    }
    return 0;
  }
  const char *autoplay = getenv("COLO_ONLINE_P2_AUTO");
  if (autoplay && autoplay[0] == '1') {
    *word = 4u << 16;
    return 1;
  }
  const char *dir = data_dir();
  if (!dir)
    return 0;
  char path[1024], line[128] = {0};
  snprintf(path, sizeof(path), "%s/p2.choice", dir);
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  const size_t got = fread(line, 1, sizeof(line) - 1, f);
  fclose(f);
  remove(path);
  line[got] = 0;
  if (!parse_choice(line, word)) {
    fprintf(stderr, "[online-battle] p2.choice not understood: %s\n", line);
    return 0;
  }
  fprintf(stderr, "[online-battle] player 2 chose %s", line);
  if (!got || line[got - 1] != '\n')
    fprintf(stderr, "\n");
  return 1;
}

/* fn_80072D58(channel, data, size): send the move data; nothing to send. */
static void link_send_patch(CPUState *s) {
  if (s_virtual_p2 && s->gpr[3] == kRemoteChannel)
    moderngekko_mod_return_u32(s, 0);
}

/* fn_80072A00(channel): cancel (timer ran out, battle aborted). */
static void link_cancel_patch(CPUState *s) {
  if (s_virtual_p2 && s->gpr[3] == kRemoteChannel)
    moderngekko_mod_return_u32(s, 0);
}

/* fn_80072C74(channel, &reply): < 0 nothing yet, 0 reply stored (as the GBA
   sends it, little-endian), > 0 link error. */
static void link_poll_patch(CPUState *s) {
  if (!s_virtual_p2 || s->gpr[3] != kRemoteChannel)
    return;
  static int waiting;
  uint32_t word = 0;
  if (online() && net_desync_turn()) {
    word = 5u << 16; /* out of sync: end the battle (shown as a link error) */
  } else if (!take_choice(&word)) {
    if (!waiting)
      fprintf(stderr, "[online-battle] turn %u: waiting for the other player\n", s_turn);
    waiting = 1;
    ++s_wait_polls;
    s_last_wait_poll = s_polls;
    moderngekko_mod_return_u32(s, 0xFFFFFFFFu);
    return;
  }
  waiting = 0;
  s_wait_polls = 0;
  const uint32_t swapped = (word >> 24) | ((word >> 8) & 0xFF00u) | ((word << 8) & 0xFF0000u) |
                           (word << 24);
  write32(s, s->gpr[4], swapped);
  moderngekko_mod_return_u32(s, 0);
}
/* Turn timer: the game's own command timer, countdown and timeout choice
   included. Online, player 1's [OnlineBattle] turn_seconds (0: off); off
   line, COLO_ONLINE_TURN_SECONDS (e.g. 30, 60, 120) in any battle. Off
   leaves the game's rules alone (no timer in the built-in rules). */
static int turn_seconds(void) {
  if (online())
    return s_virtual_p2 ? (int)s_battle_settings.turn_seconds : 0;
  const char *v = getenv("COLO_ONLINE_TURN_SECONDS");
  const int seconds = v ? atoi(v) : 0;
  return seconds > 0 && seconds <= 3600 ? seconds : 0;
}

/* fightTrainerTimeOutSelectFightAction(trainer, out Pokemon, param): the
   game's choice when the turn timer runs out (the first usable move). With
   "When time runs out: Give up", our player gives up instead, as if they had
   chosen it (fightOutPokemonCreateFightAction kind 8, sent to the other
   side by action_entry). The other player's timeouts happen on their
   machine. */
static void timeout_choice_patch(CPUState *s) {
  if (!online() || !s_virtual_p2 || s_battle_settings.auto_choose ||
      !is_local_trainer(s, s->gpr[3]) || !s->gpr[4])
    return;
  CPUState copy = *s;
  copy.gpr[3] = s->gpr[4];
  copy.gpr[4] = 0;
  copy.gpr[5] = 8;
  copy.gpr[6] = 0;
  copy.gpr[7] = 0x80375D30u; /* lbl_80375D30, as the game passes */
  copy.gpr[8] = 0;
  if (api && api->call_guest && api->call_guest(api->user_data, &copy, 0x8020505Cu)) {
    fprintf(stderr, "[online-battle] turn %u: time ran out, giving up\n", s_turn);
    moderngekko_mod_return_u32(s, 0);
  }
}

static void command_seconds_patch(CPUState *s) {
  if (turn_seconds())
    moderngekko_mod_return_u32(s, (uint32_t)turn_seconds());
}

static void command_timer_enabled_patch(CPUState *s) {
  if (turn_seconds())
    moderngekko_mod_return_u32(s, 1);
}

static void new_battle(void) {
  s_remote_pick_count = -1;
  s_sent_picks = 0;
  s_local_trainer = 0;
  s_local_switch_pending = 0;
  s_local_switch_menu = 0;
  s_local_choices_left = 0;
  if (online())
    net_new_battle();
}

static void on_unload(void) {
  if (online())
    net_stop(); /* close the port the router opened for us */
}

static void on_load(const ModernGekkoModHostApi *host) {
  api = host;
  net_start();
  if (online()) {
    fprintf(stderr, "[online-battle] loaded, online (host ABI %u)\n", host ? host->abi_version : 0);
    update_status();
  } else
    fprintf(stderr, "[online-battle] loaded, trainer files in %s\n",
            data_dir() ? data_dir() : "(COLO_ONLINE_DIR not set)");
}

static const ModernGekkoModPatch patches[] = {
    /* Literal addresses: scripts/make-hook-aware-module.py scans for them. */
    RECOMP_PATCH(0x800849B4u, connect_patch), /* kConnect */
    RECOMP_PATCH(0x80077B84u, command_seconds_patch),       /* rule command seconds */
    RECOMP_PATCH(0x801F1700u, command_timer_enabled_patch), /* fightFloorIsUseFightTimerCommand */
    RECOMP_PATCH(0x801F923Cu, select_action_patch),  /* fightTrainerSelectFightAction */
    RECOMP_PATCH(0x801F9600u, select_switch_patch),  /* fightTrainerSelectIrekaeFightPokemon */
    RECOMP_PATCH(0x80072D58u, link_send_patch),
    RECOMP_PATCH(0x80072A00u, link_cancel_patch),
    RECOMP_PATCH(0x80072C74u, link_poll_patch),
    RECOMP_PATCH(0x800E0C54u, rng_u16_patch),  /* fn_800E0C54: u16 */
    RECOMP_PATCH(0x800E0C04u, rng_mod_patch),  /* _fadeEffectGetRandom: value % n */
    RECOMP_PATCH(0x8006A81Cu, set_controller_patch), /* fn_8006A81C(entry, controller) */
    RECOMP_PATCH(0x80265924u, timer_over_patch),     /* fightTimerCommandIsOver */
    RECOMP_PATCH(0x801F18DCu, link_battle_patch),    /* fn_801F18DC: link battle? */
    RECOMP_PATCH(0x801F9130u, timeout_choice_patch), /* fightTrainerTimeOutSelectFightAction */
};

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK_RETURN(0x800679C0u, team_select_return), /* team selection, per frame */
    RECOMP_HOOK(0x800AB150u, pad_poll_entry),            /* PADRead(status) */
    RECOMP_HOOK_RETURN(0x800AB150u, pad_poll_return),    /* PADRead, per frame */
    RECOMP_HOOK(0x80204F6Cu, attack_action_entry), /* fightOutPokemonCreateFightActionAttackWaza */
    RECOMP_HOOK(0x8020505Cu, action_entry),        /* fightOutPokemonCreateFightAction */
    RECOMP_HOOK(0x801254B4u, set_status_entry),    /* pokemonSetStatus */
    RECOMP_HOOK(0x80261AD0u, command_phase_start),       /* fightMenuOpenInfoMenu */
    RECOMP_HOOK(0x80261954u, command_phase_end),         /* fightMenuCloseInfoMenu */
};

static const ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    NULL, 0,
    patches, sizeof(patches) / sizeof(patches[0]),
    hooks, sizeof(hooks) / sizeof(hooks[0]),
    NULL, 0,
    NULL, 0,
    NULL, 0,
    NULL, 0,
    on_load,
    on_unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
  return &descriptor;
}
