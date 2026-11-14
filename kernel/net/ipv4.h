#ifndef __IPV4_H__
#define __IPV4_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
#include <strbuf.h>

#define IPV4_HEADER_LEN     20                  /* without options */
#define IPV4_PAYLOAD_MAX    (1500 - IPV4_HEADER_LEN)
#define IPV4_DEFAULT_TTL    64
#define IPV4_ROUTES_MAX     256                 /* a 100-router RIP mesh needs ~200 */

/* Where a route came from; a lower value wins for the same prefix */
#define IPV4_ORIGIN_CONNECTED   0
#define IPV4_ORIGIN_STATIC      1               /* route=, gw=, the DHCP client's default route */
#define IPV4_ORIGIN_RIP         2

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

/* Remove the address and the connected route; routes via gateways stay */
void ipv4_clear_address(struct netdev* dev);

/* Interface address in host order, 0 if unconfigured */
uint32_t ipv4_iface_address(const struct netdev* dev);
uint32_t ipv4_iface_netmask(const struct netdev* dev);

/* True if ip is assigned to any of our interfaces */
bool ipv4_is_own_address(uint32_t ip);

/* gateway 0 = directly connected on dev; dev NULL = the interface that reaches gateway */
bool ipv4_add_route(uint32_t network, uint8_t prefix_len, uint32_t gateway, struct netdev* dev);

bool ipv4_remove_route(uint32_t network, uint8_t prefix_len);

/* A route from a routing protocol: refused (false) if the prefix already has a route
   from a better origin; removing only removes a route of that origin */
bool ipv4_add_route_from(uint32_t network, uint8_t prefix_len, uint32_t gateway, struct netdev* dev,
                         uint8_t origin, uint8_t metric);
bool ipv4_remove_route_from(uint32_t network, uint8_t prefix_len, uint8_t origin);

/* Prefix lengths: true if mask is contiguous ones */
bool ipv4_mask_valid(uint32_t mask);

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

/* Send straight out of dev to dst (on-link or 255.255.255.255) with exactly this
   source address, without a route lookup; used by DHCP before addresses exist */
bool ipv4_send_dev(struct netdev* dev, uint32_t src, uint32_t dst, uint8_t protocol,
                   const void* payload, size_t len);

void ipv4_print_config(void);

/* Our port that owns address ip, or NULL */
struct netdev* ipv4_iface_with_address(uint32_t ip);
uint8_t ipv4_prefix_len(uint32_t mask);
void ipv4_routes_json(struct strbuf* sb);

struct ipv4_stats
{
    uint32_t rx_packets;
    uint32_t rx_delivered;
    uint32_t rx_bad_header;             /* version, length or header checksum */
    uint32_t rx_fragments;              /* fragments are not reassembled yet */
    uint32_t rx_unknown_protocol;
    uint32_t tx_packets;
    uint32_t tx_no_route;
    uint32_t fwd_packets;
    uint32_t fwd_ttl_exceeded;
    uint32_t fwd_no_route;
    uint32_t fwd_martian;               /* bad source/destination or directed broadcast */
    uint32_t rx_wrong_port;             /* arrived on a NAT outside port for an inside port's address */
};

extern struct ipv4_stats ipv4_stats;

#endif /* __IPV4_H__ */
