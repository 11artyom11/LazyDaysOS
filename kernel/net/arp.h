#ifndef __ARP_H__
#define __ARP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define ARP_TABLE_SIZE      64
#define ARP_REACHABLE_MS    (5 * 60 * 1000)     /* forget a mapping after 5 minutes */
#define ARP_RETRY_MS        1000
#define ARP_MAX_REQUESTS    3                   /* then drop the held packet, ICMP host unreachable */
#define ARP_MAX_INCOMPLETE  16                  /* unresolved entries; the rest is kept for confirmed neighbours */

/* Ethernet-layer handler for EtherType 0x0806 */
void arp_receive(struct netdev* dev, const uint8_t* payload, size_t len);

/*
    Send an IPv4 packet to next_hop (host order) on dev. If next_hop's MAC is not
    known yet, a request goes out and the packet is held until the reply arrives;
    one packet is held per destination and a newer one replaces it. Returns false
    only if the packet had to be dropped right away.
*/
bool arp_output(struct netdev* dev, uint32_t next_hop, const void* packet, size_t len);

/* Retransmit pending requests and expire old entries; call periodically */
void arp_tick(uint32_t now_ms);

/* MAC for ip on dev if it is resolved */
bool arp_lookup(const struct netdev* dev, uint32_t ip, uint8_t mac[ETH_ALEN]);

void arp_print_table(void);

struct arp_stats
{
    uint32_t rx_requests;
    uint32_t rx_replies;
    uint32_t rx_invalid;
    uint32_t tx_requests;
    uint32_t tx_replies;
    uint32_t held_replaced;             /* held packet replaced by a newer one */
    uint32_t held_dropped;              /* resolution failed */
    uint32_t table_full;
    uint32_t incomplete_evicted;        /* unresolved entry replaced by a newer resolution */
};

extern struct arp_stats arp_stats;

#endif /* __ARP_H__ */
