#ifndef __RIP_H__
#define __RIP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strbuf.h>

/*
    RIPv2 (RFC 2453): distance-vector routing between routers.

    Command line:
      rip=eth1,eth2       speak RIP on these ports (repeatable fields, comma separated);
                          "rip" alone = every port that isn't a NAT outside port
      rip_timers=U,T,G    update, timeout and garbage-collection timers in seconds
                          (default 30,180,120 as in RFC 2453; tests use short ones)

    - Advertises the connected networks of every port except NAT outside ports
      (so stub LANs are reachable), plus the routes it learned. Metric = hops,
      16 = unreachable.
    - Updates go to 255.255.255.255:520 out of each RIP port every U seconds (plus
      a random 0-5 s), with split horizon and poisoned reverse (routes learned on
      a port go back out of it with metric 16). Changes trigger a partial update
      within 1-5 s.
    - A route not refreshed for T seconds becomes unreachable, is advertised as
      such for G seconds, then deleted. When a port's link goes down, routes
      through it become unreachable at once and its network is advertised as
      unreachable; when it comes back up, the router asks its neighbours for
      their tables.
    - Learned routes go into the routing table with origin RIP; connected and
      static routes for the same prefix win.

    Accepted only on RIP ports, never on NAT outside ports, only from a neighbour
    on the port's subnet (not our own address), source port 520, version 2.
    Entries are checked: address family 2, metric 1-16, contiguous mask, no host
    bits, no 0/8 (except the default route), 127/8 or class D/E. There is no
    authentication yet: anyone on a RIP link can announce routes (RFC 4822 keyed
    authentication is future work), so only enable RIP on links between routers.
*/

#define RIP_PORT                520
#define RIP_INFINITY            16
#define RIP_MAX_ROUTES          256
#define RIP_ENTRIES_PER_PACKET  25

/* Read rip= / rip_timers= from the command line and start; does nothing without rip */
void rip_start(void);
void rip_tick(uint32_t now_ms);
bool rip_running(void);

/* {"ports":[...],"routes":[...],<counters>} */
void rip_json(struct strbuf* sb);

struct rip_stats
{
    uint32_t rx_responses;
    uint32_t rx_requests;
    uint32_t rx_bad;                    /* malformed message, version, unusable entry */
    uint32_t rx_refused;                /* not a RIP port, WAN port, not a neighbour, our own */
    uint32_t tx_updates;                /* periodic, one per port per round */
    uint32_t tx_triggered;
    uint32_t route_changes;
    uint32_t table_full;
};

extern struct rip_stats rip_stats;

#endif /* __RIP_H__ */
