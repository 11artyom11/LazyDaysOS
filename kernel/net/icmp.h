#ifndef __ICMP_H__
#define __ICMP_H__

#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define ICMP_ECHO_REPLY     0
#define ICMP_DEST_UNREACH   3
#define ICMP_ECHO_REQUEST   8
#define ICMP_TIME_EXCEEDED  11

/* ICMP_DEST_UNREACH codes */
#define ICMP_NET_UNREACH    0
#define ICMP_HOST_UNREACH   1
#define ICMP_PROTO_UNREACH  2
#define ICMP_PORT_UNREACH   3

struct icmp_echo
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* IPv4 handler for protocol 1; src and dst in host order */
void icmp_receive(struct netdev* dev, uint32_t src, uint32_t dst, const uint8_t* payload, size_t len);

/*
    Report a problem with `packet` (a whole IPv4 packet we received or were
    forwarding) to its sender, quoting its header and first 8 payload bytes.
    Silently does nothing where RFC 1122 forbids an error: about ICMP errors,
    non-first fragments, broadcast/multicast destinations, or senders that
    aren't a single host. The error comes from ingress's address, or from the
    outgoing interface's address if ingress is NULL or unconfigured.
*/
void icmp_send_error(struct netdev* ingress, uint8_t type, uint8_t code, const uint8_t* packet, size_t len);

/*
    Send one echo request and run the network loop until the reply arrives.
    Returns the round trip time in ms (timer resolution is 10 ms), or -1 on timeout.
*/
int32_t icmp_ping(uint32_t dst, uint32_t timeout_ms);

struct icmp_stats
{
    uint32_t rx_echo_requests;
    uint32_t rx_echo_replies;
    uint32_t rx_invalid;                /* too short or bad checksum */
    uint32_t rx_other;
    uint32_t tx_echo_replies;
    uint32_t tx_echo_requests;
    uint32_t tx_errors;
};

extern struct icmp_stats icmp_stats;

#endif /* __ICMP_H__ */
