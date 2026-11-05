#ifndef __IPV4_H__
#define __IPV4_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define IPV4_HEADER_LEN     20                  /* without options */
#define IPV4_PAYLOAD_MAX    (1500 - IPV4_HEADER_LEN)
#define IPV4_DEFAULT_TTL    64
#define IPV4_ROUTES_MAX     16

#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

struct ipv4_header
{
    uint8_t version_ihl;                /* version 4, header length in 32-bit words */
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

/* Assign an address to an interface (replaces any previous one) and add its connected route */
void ipv4_set_address(struct netdev* dev, uint32_t addr, uint8_t prefix_len);

/* Interface address in host order, 0 if unconfigured */
uint32_t ipv4_iface_address(const struct netdev* dev);

/* True if ip is assigned to any of our interfaces */
bool ipv4_is_own_address(uint32_t ip);

/* gateway 0 = directly connected on dev; dev NULL = the interface that reaches gateway */
bool ipv4_add_route(uint32_t network, uint8_t prefix_len, uint32_t gateway, struct netdev* dev);

/* Longest-prefix match; returns false if dst is unreachable */
bool ipv4_route_lookup(uint32_t dst, struct netdev** dev, uint32_t* next_hop);

/* Ethernet-layer handler for EtherType 0x0800 */
void ipv4_receive(struct netdev* dev, const uint8_t* packet, size_t len);

/*
    Build a header around payload and send it to dst, choosing the interface by
    route. src 0 = the outgoing interface's address. Returns false if there is no
    route or the packet was dropped right away.
*/
bool ipv4_send(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, size_t len);

void ipv4_print_config(void);

struct ipv4_stats
{
    uint32_t rx_packets;
    uint32_t rx_delivered;
    uint32_t rx_bad_header;             /* version, length or header checksum */
    uint32_t rx_fragments;              /* fragments are not reassembled yet */
    uint32_t rx_not_for_us;             /* would be forwarded by a router */
    uint32_t rx_unknown_protocol;
    uint32_t tx_packets;
    uint32_t tx_no_route;
};

extern struct ipv4_stats ipv4_stats;

#endif /* __IPV4_H__ */
