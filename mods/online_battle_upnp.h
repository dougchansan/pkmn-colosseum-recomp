#pragma once

#include <stddef.h>

enum { UPNP_MAPPED = 0, UPNP_NO_ROUTER = 1, UPNP_REFUSED = 2 };

/* Asks the router to forward TCP port to this machine (blocks a few
   seconds). external_ip receives the router's public address when known. */
int upnp_map_tcp(unsigned port, char *external_ip, size_t external_ip_size);
/* Removes the mapping made above, if any. */
void upnp_unmap(void);
/* 10/8, 172.16/12, 192.168/16 or 100.64/10: another router (or the ISP's
   carrier-grade NAT) sits between this one and the internet. */
int upnp_address_is_private(const char *ip);
