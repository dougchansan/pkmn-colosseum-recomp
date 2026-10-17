/* Network link for the online battle mod: one TCP connection between the
 * two players, framed messages, a background thread, and an inbox the
 * game-side hooks read without blocking.
 *
 *   COLO_ONLINE_HOST=<port>         wait for the other player on this TCP port
 *   COLO_ONLINE_JOIN=<host>:<port>  connect to the other player
 *   COLO_ONLINE_PLAYER=1|2          on the waiting side: which player it is
 *                                   (default 1); the joining side takes the
 *                                   other, so a side that cannot open
 *                                   connections can still be either player
 *
 * Turn choices are a few bytes once a turn, so latency only shows as how
 * long "Waiting..." stays up; nothing runs in lockstep. When the connection
 * drops, the waiting side listens again and the joining side reconnects. */
#include "online_battle_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
typedef CRITICAL_SECTION mutex_t;
static void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
static void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
static void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
static int last_error(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define BAD_SOCKET (-1)
#define close_socket close
typedef pthread_mutex_t mutex_t;
static void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static int last_error(void) { return errno; }
#endif

enum {
  kProtocolVersion = 2,
  kMsgHello = 1,    /* version, the sender's player number */
  kMsgHero = 2,     /* the sender's trainer */
  kMsgSeed = 3,     /* player 1: the battle's seed */
  kMsgPicks = 4,    /* count, team order */
  kMsgChoice = 5,   /* one reply word */
  kMsgCheck = 6,    /* turn, checksum */
  kMsgSettings = 7, /* player 1: turn seconds, auto-choose */
  kMsgRules = 8,    /* the rules the sender picked for this battle */
  kMaxPayload = 0x4000,
  kChoiceQueue = 64,
  kCheckHistory = 512,
};

static mutex_t s_lock;
static socket_t s_socket = BAD_SOCKET;
static int s_role = NET_OFF; /* player 1 (NET_HOST) or 2 (NET_GUEST) */
static int s_listen;          /* this side waits for the connection */
static int s_state = NET_STATE_OFF;
static unsigned s_port;
static char s_target[256];

/* Inbox, under s_lock; cleared for each new connection. */
static uint8_t s_remote_hero[NET_HERO_SIZE];
static int s_have_remote_hero;
static uint32_t s_seed;
static int s_have_seed;
static NetSettings s_settings;
static int s_have_settings;
static uint32_t s_remote_rules;
static int s_have_remote_rules;
static uint8_t s_remote_picks[6];
static int s_remote_pick_count = -1;
static uint32_t s_choices[kChoiceQueue];
static unsigned s_choice_head, s_choice_tail;
static uint32_t s_local_checks[kCheckHistory], s_remote_checks[kCheckHistory];
static uint8_t s_local_check_set[kCheckHistory], s_remote_check_set[kCheckHistory];
static uint32_t s_desync_turn;
static uint32_t s_battle_index; /* battles on this connection, for the seed */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int send_all(socket_t s, const uint8_t *data, size_t size) {
  while (size) {
    const int sent = send(s, (const char *)data, (int)size, 0);
    if (sent <= 0)
      return 0;
    data += sent;
    size -= (size_t)sent;
  }
  return 1;
}

static int recv_all(socket_t s, uint8_t *data, size_t size) {
  while (size) {
    const int got = recv(s, (char *)data, (int)size, 0);
    if (got <= 0)
      return 0;
    data += got;
    size -= (size_t)got;
  }
  return 1;
}

static int send_message(uint8_t type, const uint8_t *payload, uint32_t size) {
  uint8_t header[5];
  header[0] = type;
  put32(header + 1, size);
  mutex_lock(&s_lock);
  const socket_t s = s_socket;
  int ok = s != BAD_SOCKET && send_all(s, header, sizeof(header)) &&
           (!size || send_all(s, payload, size));
  mutex_unlock(&s_lock);
  return ok;
}

static void compare_check(uint32_t turn) {
  const uint32_t i = turn % kCheckHistory;
  if (s_local_check_set[i] && s_remote_check_set[i] && s_local_checks[i] != s_remote_checks[i] &&
      !s_desync_turn) {
    s_desync_turn = turn;
    fprintf(stderr, "[online-battle] DESYNC at turn %u: ours %08X, theirs %08X\n", turn,
            s_local_checks[i], s_remote_checks[i]);
  }
}

static void clear_inbox(void) {
  s_have_remote_hero = 0;
  s_have_seed = 0;
  s_have_settings = 0;
  s_have_remote_rules = 0;
  s_remote_pick_count = -1;
  s_choice_head = s_choice_tail = 0;
  memset(s_local_check_set, 0, sizeof(s_local_check_set));
  memset(s_remote_check_set, 0, sizeof(s_remote_check_set));
  s_desync_turn = 0;
  s_battle_index = 0;
}

/* The battle's seed: player 1 draws it, both sides use it. (The C library's
   generator state is per thread on Windows: seed it here.) */
static void send_seed(void) {
  srand((unsigned)time(NULL) ^ (unsigned)clock() * 2654435761u);
  uint32_t seed = (uint32_t)rand() ^ ((uint32_t)rand() << 15) ^ ((uint32_t)rand() << 30);
  const char *fixed = getenv("COLO_ONLINE_SEED");
  if (fixed && fixed[0])
    seed = (uint32_t)strtoul(fixed, NULL, 0);
  mutex_lock(&s_lock);
  s_seed = seed;
  s_have_seed = 1;
  mutex_unlock(&s_lock);
  uint8_t buf[4];
  put32(buf, seed);
  send_message(kMsgSeed, buf, sizeof(buf));
}

/* Returns 0 to drop the connection. */
static int handle(uint8_t type, const uint8_t *p, uint32_t size) {
  int seed = 0, ok = 1;
  mutex_lock(&s_lock);
  switch (type) {
  case kMsgHello:
    if (size < 5 || get32(p) != kProtocolVersion) {
      fprintf(stderr, "[online-battle] the other game has a different online battle version\n");
      s_state = NET_STATE_VERSION_MISMATCH;
      ok = 0;
      break;
    }
    if (!s_listen) {
      /* The waiting side decides who is who; we are the other player. */
      s_role = p[4] == 2 ? NET_HOST : NET_GUEST;
      seed = s_role == NET_HOST;
    }
    s_state = NET_STATE_CONNECTED;
    fprintf(stderr, "[online-battle] connected as player %d\n", s_role == NET_HOST ? 1 : 2);
    break;
  case kMsgHero:
    if (size == NET_HERO_SIZE) {
      memcpy(s_remote_hero, p, NET_HERO_SIZE);
      s_have_remote_hero = 1;
      fprintf(stderr, "[online-battle] received the other trainer\n");
    }
    break;
  case kMsgSeed:
    if (size == 4) {
      s_seed = get32(p);
      s_have_seed = 1;
    }
    break;
  case kMsgSettings:
    if (size == 8) {
      s_settings.turn_seconds = get32(p);
      s_settings.auto_choose = get32(p + 4) != 0;
      s_have_settings = 1;
    }
    break;
  case kMsgRules:
    if (size == 4) {
      s_remote_rules = get32(p);
      s_have_remote_rules = 1;
    }
    break;
  case kMsgPicks:
    if (size >= 1 && p[0] <= 6 && size == 1u + p[0]) {
      memcpy(s_remote_picks, p + 1, p[0]);
      s_remote_pick_count = p[0];
      fprintf(stderr, "[online-battle] received the other team's picks (%u)\n", p[0]);
    }
    break;
  case kMsgChoice:
    if (size == 4 && s_choice_tail - s_choice_head < kChoiceQueue)
      s_choices[s_choice_tail++ % kChoiceQueue] = get32(p);
    break;
  case kMsgCheck:
    if (size == 8) {
      const uint32_t turn = get32(p), i = turn % kCheckHistory;
      s_remote_checks[i] = get32(p + 4);
      s_remote_check_set[i] = 1;
      compare_check(turn);
    }
    break;
  default:
    break;
  }
  mutex_unlock(&s_lock);
  if (seed)
    send_seed();
  return ok;
}

static socket_t open_host(unsigned port) {
  socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == BAD_SOCKET)
    return BAD_SOCKET;
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((unsigned short)port);
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
    fprintf(stderr, "[online-battle] cannot listen on port %u (error %d)\n", port, last_error());
    close_socket(listener);
    return BAD_SOCKET;
  }
  fprintf(stderr, "[online-battle] waiting on TCP port %u for the other player\n", port);
  socket_t peer = accept(listener, NULL, NULL);
  close_socket(listener);
  return peer;
}

/* One attempt to reach host:port. */
static socket_t open_join(const char *target) {
  char host[256];
  snprintf(host, sizeof(host), "%s", target);
  char *colon = strrchr(host, ':');
  if (!colon)
    return BAD_SOCKET;
  *colon = 0;
  const char *port = colon + 1;
  struct addrinfo hints, *found = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &found) != 0)
    return BAD_SOCKET;
  socket_t result = BAD_SOCKET;
  for (struct addrinfo *a = found; a && result == BAD_SOCKET; a = a->ai_next) {
    socket_t s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == BAD_SOCKET)
      continue;
    if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0)
      result = s;
    else
      close_socket(s);
  }
  freeaddrinfo(found);
  return result;
}

static void set_state(int state) {
  mutex_lock(&s_lock);
  s_state = state;
  mutex_unlock(&s_lock);
}

/* One connection, from the handshake until it drops. */
static void run_connection(socket_t s) {
  int yes = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
  mutex_lock(&s_lock);
  clear_inbox();
  s_socket = s;
  s_state = NET_STATE_HANDSHAKE;
  mutex_unlock(&s_lock);

  uint8_t hello[5];
  put32(hello, kProtocolVersion);
  hello[4] = (uint8_t)(s_role == NET_HOST ? 1 : 2);
  send_message(kMsgHello, hello, sizeof(hello));
  if (s_listen && s_role == NET_HOST)
    send_seed();

  static uint8_t payload[kMaxPayload];
  for (;;) {
    uint8_t header[5];
    if (!recv_all(s, header, sizeof(header)))
      break;
    const uint32_t size = get32(header + 1);
    if (size > kMaxPayload || (size && !recv_all(s, payload, size)))
      break;
    if (!handle(header[0], payload, size))
      break;
  }
  mutex_lock(&s_lock);
  if (s_state != NET_STATE_VERSION_MISMATCH)
    s_state = NET_STATE_DISCONNECTED;
  s_socket = BAD_SOCKET;
  mutex_unlock(&s_lock);
  close_socket(s);
  fprintf(stderr, "[online-battle] the connection to the other player closed\n");
}

#ifdef _WIN32
static DWORD WINAPI net_thread(LPVOID unused)
#else
static void *net_thread(void *unused)
#endif
{
  (void)unused;
  unsigned attempts = 0;
  for (;;) {
    socket_t s = BAD_SOCKET;
    if (s_listen) {
      set_state(NET_STATE_LISTENING);
      s = open_host(s_port);
      if (s == BAD_SOCKET) {
        set_state(NET_STATE_LISTEN_FAILED);
        sleep_ms(5000);
        continue;
      }
    } else {
      set_state(NET_STATE_CONNECTING);
      s = open_join(s_target);
      if (s == BAD_SOCKET) {
        if (!(attempts++ % 15))
          fprintf(stderr, "[online-battle] cannot reach %s yet (error %d), retrying\n", s_target,
                  last_error());
        sleep_ms(2000); /* the other player may not be up yet */
        continue;
      }
      fprintf(stderr, "[online-battle] connected to %s\n", s_target);
    }
    attempts = 0;
    run_connection(s);
    /* Show what happened for a moment before waiting again. */
    sleep_ms(4000);
  }
}

void net_start(void) {
  const char *host = getenv("COLO_ONLINE_HOST");
  const char *join = getenv("COLO_ONLINE_JOIN");
  if (host && host[0]) {
    s_listen = 1;
    s_port = (unsigned)atoi(host);
  } else if (join && join[0]) {
    snprintf(s_target, sizeof(s_target), "%s", join);
  } else {
    return;
  }
  const char *player = getenv("COLO_ONLINE_PLAYER");
  s_role = s_listen ? (player && atoi(player) == 2 ? NET_GUEST : NET_HOST) : NET_GUEST;
  mutex_init(&s_lock);
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  HANDLE thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
  if (thread)
    CloseHandle(thread);
#else
  pthread_t thread;
  if (pthread_create(&thread, NULL, net_thread, NULL) == 0)
    pthread_detach(thread);
#endif
}

int net_role(void) { return s_role; }

int net_state(void) {
  if (s_role == NET_OFF)
    return NET_STATE_OFF;
  mutex_lock(&s_lock);
  const int state = s_state;
  mutex_unlock(&s_lock);
  return state;
}

int net_connected(void) { return net_state() == NET_STATE_CONNECTED; }

const char *net_target(void) { return s_listen ? NULL : s_target; }
unsigned net_port(void) { return s_port; }

void net_send_hero(const uint8_t *hero) { send_message(kMsgHero, hero, NET_HERO_SIZE); }

int net_remote_hero(uint8_t *out) {
  mutex_lock(&s_lock);
  const int have = s_have_remote_hero;
  if (have && out)
    memcpy(out, s_remote_hero, NET_HERO_SIZE);
  mutex_unlock(&s_lock);
  return have;
}

int net_seed(uint32_t *out) {
  mutex_lock(&s_lock);
  const int have = s_have_seed;
  /* A different seed for each battle on one connection (both sides count
     the battles they start). */
  *out = s_seed + s_battle_index * 0x9E3779B9u;
  mutex_unlock(&s_lock);
  return have;
}

void net_send_settings(const NetSettings *settings) {
  uint8_t buf[8];
  put32(buf, settings->turn_seconds);
  put32(buf + 4, settings->auto_choose ? 1u : 0u);
  send_message(kMsgSettings, buf, sizeof(buf));
}

int net_settings(NetSettings *out) {
  mutex_lock(&s_lock);
  const int have = s_have_settings;
  *out = s_settings;
  mutex_unlock(&s_lock);
  return have;
}

void net_send_rules(uint32_t rules) {
  uint8_t buf[4];
  put32(buf, rules);
  send_message(kMsgRules, buf, sizeof(buf));
}

int net_remote_rules(uint32_t *out) {
  mutex_lock(&s_lock);
  const int have = s_have_remote_rules;
  *out = s_remote_rules;
  mutex_unlock(&s_lock);
  return have;
}

void net_send_picks(const uint8_t *picks, int count) {
  uint8_t buf[7];
  buf[0] = (uint8_t)count;
  memcpy(buf + 1, picks, (size_t)count);
  send_message(kMsgPicks, buf, 1u + (uint32_t)count);
}

int net_take_remote_picks(uint8_t *out) {
  mutex_lock(&s_lock);
  const int count = s_remote_pick_count;
  if (count >= 0)
    memcpy(out, s_remote_picks, (size_t)count);
  s_remote_pick_count = -1;
  mutex_unlock(&s_lock);
  return count;
}

void net_send_choice(uint32_t word) {
  uint8_t buf[4];
  put32(buf, word);
  send_message(kMsgChoice, buf, sizeof(buf));
}

int net_take_choice(uint32_t *word) {
  mutex_lock(&s_lock);
  const int have = s_choice_head != s_choice_tail;
  if (have)
    *word = s_choices[s_choice_head++ % kChoiceQueue];
  mutex_unlock(&s_lock);
  return have;
}

void net_send_check(uint32_t turn, uint32_t value) {
  mutex_lock(&s_lock);
  s_local_checks[turn % kCheckHistory] = value;
  s_local_check_set[turn % kCheckHistory] = 1;
  compare_check(turn);
  mutex_unlock(&s_lock);
  uint8_t buf[8];
  put32(buf, turn);
  put32(buf + 4, value);
  send_message(kMsgCheck, buf, sizeof(buf));
}

void net_new_battle(void) {
  mutex_lock(&s_lock);
  s_choice_head = s_choice_tail = 0;
  memset(s_local_check_set, 0, sizeof(s_local_check_set));
  memset(s_remote_check_set, 0, sizeof(s_remote_check_set));
  s_desync_turn = 0;
  s_have_remote_rules = 0;
  ++s_battle_index;
  mutex_unlock(&s_lock);
}

uint32_t net_desync_turn(void) {
  mutex_lock(&s_lock);
  const uint32_t turn = s_desync_turn;
  mutex_unlock(&s_lock);
  return turn;
}
