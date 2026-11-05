#ifndef __DHCP_H__
#define __DHCP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/* DHCP (RFC 2131) messages and options (RFC 2132), shared by client and server */

#define DHCP_MAGIC_COOKIE       0x63825363
#define DHCP_OP_REQUEST         1       /* BOOTREQUEST, client -> server */
#define DHCP_OP_REPLY           2       /* BOOTREPLY, server -> client */
#define DHCP_FLAG_BROADCAST     0x8000
#define DHCP_OPTIONS_LEN        308     /* makes the whole message 548 bytes */

/* Message types (option 53) */
#define DHCPDISCOVER            1
#define DHCPOFFER               2
#define DHCPREQUEST             3
#define DHCPDECLINE             4
#define DHCPACK                 5
#define DHCPNAK                 6
#define DHCPRELEASE             7
#define DHCPINFORM              8

/* Options */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MESSAGE_TYPE   53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_RENEWAL_T1     58
#define DHCP_OPT_REBINDING_T2   59
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

struct dhcp_message
{
    uint8_t op;
    uint8_t htype;                      /* 1 = Ethernet */
    uint8_t hlen;                       /* 6 */
    uint8_t hops;
    uint32_t xid;                       /* transaction id chosen by the client */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;                    /* client's current address (renewing) */
    uint32_t yiaddr;                    /* "your" address, set by the server */
    uint32_t siaddr;
    uint32_t giaddr;                    /* relay agent */
    uint8_t chaddr[16];                 /* client MAC in the first 6 bytes */
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[DHCP_OPTIONS_LEN];
} __attribute__((packed));

#define DHCP_FIXED_LEN          (sizeof(struct dhcp_message) - DHCP_OPTIONS_LEN)

/* Zero msg and fill the fixed fields common to every message */
void dhcp_init_message(struct dhcp_message* msg, uint8_t op, uint32_t xid, const uint8_t mac[ETH_ALEN]);

/* Append an option at *cursor; returns false (and appends nothing) if it doesn't fit */
bool dhcp_put_option(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint8_t len, const void* data);
bool dhcp_put_u8(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint8_t value);
bool dhcp_put_u32(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint32_t value);  /* value in host order */
/* Terminate the options; returns the total message length to send */
size_t dhcp_finish(struct dhcp_message* msg, size_t cursor);

/* Validate the fixed part of a received message (length, magic, Ethernet addressing) */
bool dhcp_valid(const struct dhcp_message* msg, size_t len, uint8_t expected_op);

/* Find an option in a received message of len bytes; returns its data or NULL */
const uint8_t* dhcp_find_option(const struct dhcp_message* msg, size_t len, uint8_t code, uint8_t* opt_len);

/* Convenience lookups; false if the option is absent or malformed. Values in host order. */
bool dhcp_get_u8(const struct dhcp_message* msg, size_t len, uint8_t code, uint8_t* value);
bool dhcp_get_u32(const struct dhcp_message* msg, size_t len, uint8_t code, uint32_t* value);

/* ---- client: ethN=dhcp ---- */

void dhcp_client_start(struct netdev* dev);
void dhcp_client_tick(uint32_t now_ms);
/* True once every started client holds a lease */
bool dhcp_client_all_bound(void);
/* First DNS server learned from a lease, 0 if none */
uint32_t dhcp_client_dns(void);

/* ---- server: dhcpd=ethN,first,last ---- */

/* Serve addresses first..last (host order) to clients on dev; dev must have an address */
bool dhcp_server_add_pool(struct netdev* dev, uint32_t first, uint32_t last);
/* DNS server to hand out; 0 = pass on the one the DHCP client learned, if any */
void dhcp_server_set_dns(uint32_t dns);
void dhcp_server_set_lease_time(uint32_t seconds);
void dhcp_server_tick(uint32_t now_ms);

struct dhcp_stats
{
    uint32_t client_leases;             /* ACKs accepted by the client */
    uint32_t client_naks;
    uint32_t server_offers;
    uint32_t server_acks;
    uint32_t server_naks;
    uint32_t server_releases;
    uint32_t server_declines;
    uint32_t server_pool_exhausted;
    uint32_t invalid;
};

extern struct dhcp_stats dhcp_stats;

#endif /* __DHCP_H__ */
