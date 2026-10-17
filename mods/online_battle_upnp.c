/* UPnP port mapping for the online battle host: asks the home router (an
 * Internet Gateway Device) to forward the battle's TCP port to this machine,
 * so the other player can connect without the host setting up port
 * forwarding by hand. Plain SSDP discovery, one HTTP GET for the router's
 * description and SOAP calls (AddPortMapping, GetExternalIPAddress,
 * DeletePortMapping); no library.
 *
 * Runs on the network thread: every call may block for a few seconds. */
#include "online_battle_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int socket_t;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

typedef struct Gateway {
  char host[64];
  unsigned port;
  char control_path[256];
  char service[128];
  char local_ip[64];
} Gateway;

static Gateway s_gateway;
static int s_have_gateway;
static unsigned s_mapped_port;

static void set_timeout(socket_t s, int ms) {
#ifdef _WIN32
  DWORD t = (DWORD)ms;
#else
  struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
}

/* Case-insensitive search. */
static const char *find_ci(const char *haystack, const char *needle) {
  const size_t n = strlen(needle);
  for (; *haystack; ++haystack) {
    size_t i = 0;
    while (i < n && haystack[i] &&
           (haystack[i] | 0x20) == (needle[i] | 0x20))
      ++i;
    if (i == n)
      return haystack;
  }
  return NULL;
}

/* "http://host:port/path" into its parts. */
static int parse_url(const char *url, char *host, size_t host_size, unsigned *port, char *path,
                     size_t path_size) {
  if (strncmp(url, "http://", 7) != 0)
    return 0;
  url += 7;
  const char *slash = strchr(url, '/');
  const char *colon = strchr(url, ':');
  const char *host_end = colon && (!slash || colon < slash) ? colon : slash ? slash : url + strlen(url);
  const size_t len = (size_t)(host_end - url);
  if (!len || len >= host_size)
    return 0;
  memcpy(host, url, len);
  host[len] = 0;
  *port = colon && (!slash || colon < slash) ? (unsigned)atoi(colon + 1) : 80;
  snprintf(path, path_size, "%s", slash ? slash : "/");
  return 1;
}

/* One HTTP request to host:port; the response (headers and body) in out.
   Returns the status code, 0 on failure. local_ip (optional) receives the
   address this machine used to reach the router. */
static int http(const char *host, unsigned port, const char *request, char *out, size_t out_size,
                char *local_ip, size_t local_ip_size) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    return 0;
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == BAD_SOCKET)
    return 0;
  set_timeout(s, 3000);
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close_socket(s);
    return 0;
  }
  if (local_ip) {
    struct sockaddr_in me;
    socklen_t me_len = sizeof(me);
    if (getsockname(s, (struct sockaddr *)&me, &me_len) == 0)
      inet_ntop(AF_INET, &me.sin_addr, local_ip, (socklen_t)local_ip_size);
  }
  const size_t request_len = strlen(request);
  if (send(s, request, (int)request_len, 0) != (int)request_len) {
    close_socket(s);
    return 0;
  }
  size_t used = 0;
  for (;;) {
    if (used + 1 >= out_size)
      break;
    const int got = recv(s, out + used, (int)(out_size - 1 - used), 0);
    if (got <= 0)
      break;
    used += (size_t)got;
  }
  out[used] = 0;
  close_socket(s);
  int status = 0;
  if (sscanf(out, "HTTP/%*s %d", &status) != 1)
    return 0;
  return status;
}

/* The local address of the interface that leads to the internet (a UDP
   "connect" sends nothing; it only picks the route). Multicast would
   otherwise leave by whatever interface the system prefers, such as a
   virtual machine's host-only adapter. */
static int internet_interface(struct in_addr *out) {
  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == BAD_SOCKET)
    return 0;
  struct sockaddr_in remote;
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  int ok = 0;
  if (connect(s, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    if (getsockname(s, (struct sockaddr *)&me, &len) == 0) {
      *out = me.sin_addr;
      ok = 1;
    }
  }
  close_socket(s);
  return ok;
}

/* SSDP: the first Internet Gateway Device's description URL. */
static int discover(char *location, size_t size) {
  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == BAD_SOCKET)
    return 0;
  set_timeout(s, 2500);
  struct in_addr local;
  if (internet_interface(&local)) {
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr = local;
    bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&local, sizeof(local));
  }
  struct sockaddr_in group;
  memset(&group, 0, sizeof(group));
  group.sin_family = AF_INET;
  group.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
  static const char *const kTargets[] = {
      "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
      "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1",
  };
  for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
    char search[256];
    const int n = snprintf(search, sizeof(search),
                           "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                           "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n",
                           kTargets[i]);
    sendto(s, search, n, 0, (struct sockaddr *)&group, sizeof(group));
  }
  int found = 0;
  for (int tries = 0; tries < 8 && !found; ++tries) {
    char reply[1536];
    const int got = recv(s, reply, sizeof(reply) - 1, 0);
    if (got <= 0)
      break;
    reply[got] = 0;
    const char *loc = find_ci(reply, "\nLOCATION:");
    if (!loc)
      continue;
    loc += 10;
    while (*loc == ' ')
      ++loc;
    size_t len = strcspn(loc, "\r\n");
    if (len >= size)
      continue;
    memcpy(location, loc, len);
    location[len] = 0;
    found = 1;
  }
  close_socket(s);
  return found;
}

/* The router's WAN connection service and its control URL. */
static int find_gateway(void) {
  if (s_have_gateway)
    return 1;
  char location[512];
  if (!discover(location, sizeof(location))) {
    fprintf(stderr, "[online-battle] UPnP: no router answered\n");
    return 0;
  }
  Gateway g;
  memset(&g, 0, sizeof(g));
  char path[256];
  if (!parse_url(location, g.host, sizeof(g.host), &g.port, path, sizeof(path)))
    return 0;
  char request[512];
  snprintf(request, sizeof(request),
           "GET %s HTTP/1.1\r\nHOST: %s:%u\r\nConnection: close\r\n\r\n", path, g.host, g.port);
  static char description[65536];
  if (http(g.host, g.port, request, description, sizeof(description), g.local_ip,
           sizeof(g.local_ip)) != 200) {
    fprintf(stderr, "[online-battle] UPnP: cannot read the router's description at %s\n", location);
    return 0;
  }
  for (const char *type = find_ci(description, "<serviceType>"); type;
       type = find_ci(type + 1, "<serviceType>")) {
    const char *start = type + 13;
    const char *end = strchr(start, '<');
    if (!end || (size_t)(end - start) >= sizeof(g.service))
      continue;
    const char *wan = strstr(start, ":WAN");
    if (!wan || wan > end ||
        (strncmp(wan, ":WANIPConnection", 16) && strncmp(wan, ":WANPPPConnection", 17)))
      continue;
    memcpy(g.service, start, (size_t)(end - start));
    g.service[end - start] = 0;
    const char *control = find_ci(end, "<controlURL>");
    if (!control)
      continue;
    control += 12;
    const char *control_end = strchr(control, '<');
    if (!control_end)
      continue;
    char url[256];
    const size_t len = (size_t)(control_end - control);
    if (len >= sizeof(url))
      continue;
    memcpy(url, control, len);
    url[len] = 0;
    if (strncmp(url, "http://", 7) == 0) {
      char host[64];
      parse_url(url, host, sizeof(host), &g.port, g.control_path, sizeof(g.control_path));
    } else {
      snprintf(g.control_path, sizeof(g.control_path), "%s%s", url[0] == '/' ? "" : "/", url);
    }
    s_gateway = g;
    s_have_gateway = 1;
    fprintf(stderr, "[online-battle] UPnP: router %s:%u (%s), we are %s\n", g.host, g.port,
            g.service, g.local_ip);
    return 1;
  }
  fprintf(stderr, "[online-battle] UPnP: the router offers no WAN connection service\n");
  return 0;
}

static int soap(const char *action, const char *arguments, char *out, size_t out_size) {
  char body[2048];
  const int body_len =
      snprintf(body, sizeof(body),
               "<?xml version=\"1.0\"?>\r\n"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
               "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
               action, s_gateway.service, arguments, action);
  char request[3072];
  snprintf(request, sizeof(request),
           "POST %s HTTP/1.1\r\nHOST: %s:%u\r\n"
           "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
           "SOAPACTION: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
           s_gateway.control_path, s_gateway.host, s_gateway.port, s_gateway.service, action,
           body_len, body);
  return http(s_gateway.host, s_gateway.port, request, out, out_size, NULL, 0);
}

static int add_mapping(unsigned port, unsigned lease) {
  char arguments[640], reply[4096];
  snprintf(arguments, sizeof(arguments),
           "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
           "<NewProtocol>TCP</NewProtocol><NewInternalPort>%u</NewInternalPort>"
           "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
           "<NewPortMappingDescription>Pokemon Colosseum online battle</NewPortMappingDescription>"
           "<NewLeaseDuration>%u</NewLeaseDuration>",
           port, port, s_gateway.local_ip, lease);
  return soap("AddPortMapping", arguments, reply, sizeof(reply)) == 200;
}

int upnp_map_tcp(unsigned port, char *external_ip, size_t external_ip_size) {
  if (external_ip && external_ip_size)
    external_ip[0] = 0;
  if (!find_gateway())
    return UPNP_NO_ROUTER;
  /* A two-hour lease (renewed whenever the host waits for a connection), so
     a crash does not leave the port open; some routers only take permanent
     mappings. */
  if (!add_mapping(port, 7200) && !add_mapping(port, 0)) {
    fprintf(stderr, "[online-battle] UPnP: the router refused to open TCP port %u\n", port);
    return UPNP_REFUSED;
  }
  s_mapped_port = port;
  char reply[4096];
  if (external_ip && soap("GetExternalIPAddress", "", reply, sizeof(reply)) == 200) {
    const char *ip = find_ci(reply, "<NewExternalIPAddress>");
    if (ip) {
      ip += 22;
      const size_t len = strcspn(ip, "<");
      if (len < external_ip_size) {
        memcpy(external_ip, ip, len);
        external_ip[len] = 0;
      }
    }
  }
  fprintf(stderr, "[online-battle] UPnP: TCP port %u forwarded to %s, public address %s\n", port,
          s_gateway.local_ip, external_ip && external_ip[0] ? external_ip : "unknown");
  return UPNP_MAPPED;
}

void upnp_unmap(void) {
  if (!s_mapped_port)
    return;
  char arguments[256], reply[2048];
  snprintf(arguments, sizeof(arguments),
           "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
           "<NewProtocol>TCP</NewProtocol>",
           s_mapped_port);
  soap("DeletePortMapping", arguments, reply, sizeof(reply));
  fprintf(stderr, "[online-battle] UPnP: closed TCP port %u on the router\n", s_mapped_port);
  s_mapped_port = 0;
}

int upnp_address_is_private(const char *ip) {
  unsigned a = 0, b = 0;
  if (sscanf(ip, "%u.%u", &a, &b) != 2)
    return 0;
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
         (a == 100 && b >= 64 && b <= 127) || a == 0;
}
