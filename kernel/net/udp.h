#ifndef __UDP_H__
#define __UDP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define UDP_HEADER_LEN      8
#define UDP_PAYLOAD_MAX     (1500 - 20 - UDP_HEADER_LEN)
#define UDP_MAX_BINDINGS    16

#define UDP_PORT_ECHO       7
#define UDP_PORT_DHCP_SERVER 67
#define UDP_PORT_DHCP_CLIENT 68

struct udp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;                    /* header + data */
    uint16_t checksum;                  /* 0 = not computed (allowed in IPv4) */
} __attribute__((packed));

/* Addresses in host order; dev is the interface the datagram arrived on */
typedef void (*udp_handler)(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len);

/* Deliver datagrams for port to handler; false if the port is taken or the table is full */
bool udp_bind(uint16_t port, udp_handler handler);
void udp_unbind(uint16_t port);

/* IPv4 handler for protocol 17; gets the whole IP packet for ICMP errors */
void udp_receive(struct netdev* dev, const uint8_t* packet, size_t len, size_t header_len);

/* src_ip 0 = the outgoing interface's address */
bool udp_send(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
              const void* data, size_t len);

/* Out of dev with exactly src_ip (may be 0.0.0.0), no route lookup; see ipv4_send_dev */
bool udp_send_dev(struct netdev* dev, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                  uint16_t dst_port, const void* data, size_t len);

/* RFC 862 echo service: send every datagram back unchanged */
void udp_echo_handler(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                      uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len);

struct udp_stats
{
    uint32_t rx_datagrams;
    uint32_t rx_invalid;                /* bad length or checksum */
    uint32_t rx_no_port;                /* answered with ICMP port unreachable */
    uint32_t tx_datagrams;
};

extern struct udp_stats udp_stats;

#endif /* __UDP_H__ */
