#ifndef __NAT_H__
#define __NAT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/*
    NAPT ("masquerading") with connection tracking, and the firewall that comes
    with it. A port marked outside (nat=ethN) is the WAN side:

    - packets forwarded out of it get its address as source and a mapped port
      (TCP/UDP) or echo identifier (ICMP); the same internal endpoint keeps the
      same external port whatever it talks to (RFC 4787 endpoint-independent
      mapping), and the internal port is kept when it is free
    - packets arriving on it are translated back only if they belong to a
      tracked connection AND come from the exact remote address and port that
      connection was opened to (address- and port-dependent filtering); every
      other packet that would be forwarded inward is dropped
    - ICMP errors about a translated packet are translated too, including the
      packet quoted inside them (RFC 5508), so traceroute and "port unreachable"
      work through the NAT

    Only TCP, UDP and ICMP echo are translated; other protocols are not forwarded
    out of an outside port.
*/

#define NAT_TABLE_SIZE          512
#define NAT_HASH_SIZE           256     /* power of two */

/* Idle timeouts (RFC 4787 REQ-5 asks for >= 2 min for UDP, RFC 5382 for >= 2 h 4 min for TCP) */
#define NAT_TIMEOUT_UDP_MS      (30 * 1000)         /* nothing came back yet */
#define NAT_TIMEOUT_UDP_ASSURED_MS (180 * 1000)     /* replies were seen */
#define NAT_TIMEOUT_ICMP_MS     (30 * 1000)
#define NAT_TIMEOUT_TCP_SYN_MS  (120 * 1000)
#define NAT_TIMEOUT_TCP_EST_MS  (7440 * 1000)
#define NAT_TIMEOUT_TCP_FIN_MS  (120 * 1000)
#define NAT_TIMEOUT_TCP_RST_MS  (10 * 1000)

/* Mark dev as the outside (WAN) port */
bool nat_enable(struct netdev* dev);
bool nat_is_outside(const struct netdev* dev);

/*
    Outbound: packet (a writable IPv4 packet about to leave via outside port dev)
    gets its source rewritten. Returns false if it must be dropped (protocol not
    translatable, table full, outside port has no address).
*/
bool nat_outbound(struct netdev* dev, uint8_t* packet, size_t len, size_t header_len);

/*
    Inbound: packet arrived on outside port dev, addressed to dev's address.
    Returns true if it belonged to a tracked connection and was rewritten
    toward the inside host (forward it); false = not ours, deliver locally.
*/
bool nat_inbound(struct netdev* dev, uint8_t* packet, size_t len, size_t header_len);

/* Forget every mapping on dev, e.g. after its address changed */
void nat_flush(const struct netdev* dev);

void nat_tick(uint32_t now_ms);

size_t nat_active_entries(void);
void nat_print_table(void);

struct nat_stats
{
    uint32_t created;
    uint32_t expired;
    uint32_t outbound;                  /* packets translated LAN -> WAN */
    uint32_t inbound;                   /* packets translated WAN -> LAN */
    uint32_t icmp_errors;               /* ICMP errors translated inbound */
    uint32_t table_full;
    uint32_t untranslatable;            /* protocol or packet NAT can't handle */
    uint32_t fw_dropped;                /* unsolicited inbound packets dropped */
};

extern struct nat_stats nat_stats;

#endif /* __NAT_H__ */
