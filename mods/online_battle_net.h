#pragma once

#include <stddef.h>
#include <stdint.h>

#define NET_HERO_SIZE 0xB18u

/* Which player this side is (known once connected, for the joining side). */
enum { NET_OFF = 0, NET_HOST = 1, NET_GUEST = 2 };

enum {
  NET_STATE_OFF,
  NET_STATE_OPENING_PORT,  /* asking the router to forward the port (UPnP) */
  NET_STATE_LISTENING,     /* waiting for the other player to connect */
  NET_STATE_LISTEN_FAILED, /* the port is in use or not allowed */
  NET_STATE_CONNECTING,    /* trying to reach the other player */
  NET_STATE_HANDSHAKE,
  NET_STATE_CONNECTED,
  NET_STATE_DISCONNECTED,  /* the connection dropped; trying again soon */
  NET_STATE_VERSION_MISMATCH,
};

/* Player 1's battle settings, which both games use. */
typedef struct NetSettings {
  uint32_t turn_seconds; /* 0: no timer */
  int auto_choose;       /* on timeout: 1 the game chooses, 0 give up */
} NetSettings;

/* Starts the link from COLO_ONLINE_HOST / COLO_ONLINE_JOIN (no-op without). */
void net_start(void);
int net_role(void);
int net_state(void);
int net_connected(void);
const char *net_target(void); /* host:port when joining, else NULL */
unsigned net_port(void);       /* the port when waiting */
/* UPnP on the waiting side: -1 not tried (or off), else UPNP_MAPPED (and the
   router's public address), UPNP_NO_ROUTER or UPNP_REFUSED. */
int net_upnp(char *public_ip, size_t size);
/* Closes what net_start opened on the router. */
void net_stop(void);

void net_send_hero(const uint8_t *hero);
int net_remote_hero(uint8_t *out); /* 1 once the other trainer arrived */
int net_seed(uint32_t *out);       /* 1 once the battle seed is known */

void net_send_settings(const NetSettings *settings);
int net_settings(NetSettings *out); /* 1 once player 1's arrived */

void net_send_rules(uint32_t rules);
int net_remote_rules(uint32_t *out); /* 1 once the other side's arrived */

void net_send_picks(const uint8_t *picks, int count);
int net_take_remote_picks(uint8_t *out); /* count, or -1 if none yet */

void net_send_choice(uint32_t word);
int net_take_choice(uint32_t *word);

/* Per-turn checksums; a mismatch is reported once per battle. */
void net_send_check(uint32_t turn, uint32_t value);
uint32_t net_desync_turn(void);
void net_new_battle(void);
