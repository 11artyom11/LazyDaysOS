#ifndef __CLIENTS_H__
#define __CLIENTS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strbuf.h>
#include "netdev.h"

/*
    Hosts on the router's inside networks (any port that isn't a NAT outside
    port), learned from forwarded traffic, with per-host byte counters and a
    "blocked" switch that stops their forwarded traffic. Talking to the router
    itself (dashboard, DHCP, ping) is never blocked, so a blocked device can
    still be unblocked.
*/

#define CLIENTS_MAX         32

/* Forwarding path: count a packet from (up) or to (down) ip on inside port dev */
void clients_account(struct netdev* dev, uint32_t ip, size_t up_bytes, size_t down_bytes);

bool clients_is_blocked(uint32_t ip);

/* Block or unblock ip (creates the entry if the host wasn't seen yet); false if the table is full */
bool clients_set_blocked(uint32_t ip, bool blocked);

/* JSON array: ip, mac, hostname, port, blocked, byte/packet counters, last seen, lease */
void clients_json(struct strbuf* sb);

struct clients_stats
{
    uint32_t blocked_packets;
};

extern struct clients_stats clients_stats;

#endif /* __CLIENTS_H__ */
