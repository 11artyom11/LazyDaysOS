#ifndef __ETH_H__
#define __ETH_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define ETHERTYPE_IPV4      0x0800
#define ETHERTYPE_ARP       0x0806

#define ETH_PAYLOAD_MAX     (ETH_FRAME_MAX - ETH_HEADER_LEN)    /* 1500, the IPv4 MTU */

struct eth_header
{
    uint8_t dst[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t ethertype;                 /* network byte order */
} __attribute__((packed));

extern const uint8_t eth_broadcast[ETH_ALEN];

/* netdev rx handler: validates the frame and dispatches by EtherType */
void eth_receive(struct netdev* dev, const uint8_t* frame, size_t len);

/* Prepend an Ethernet header and transmit; payload may be at most ETH_PAYLOAD_MAX */
bool eth_send(struct netdev* dev, const uint8_t dst[ETH_ALEN], uint16_t ethertype,
              const void* payload, size_t len);

struct eth_stats
{
    uint32_t rx_not_for_us;             /* unicast to another MAC (promiscuous NICs) */
    uint32_t rx_unknown_type;
};

extern struct eth_stats eth_stats;

#endif /* __ETH_H__ */
