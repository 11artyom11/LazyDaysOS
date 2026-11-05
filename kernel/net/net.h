#ifndef __NET_H__
#define __NET_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
    Network stack entry points and shared helpers.

    Conventions:
    - IPv4 addresses are uint32_t in HOST byte order everywhere in the stack
      (10.0.2.15 == 0x0A00020F); conversion happens only when reading or
      writing packet headers.
    - Header structs are packed and hold fields in NETWORK byte order.
    - Nothing here or above may depend on x86 or a particular NIC driver.
*/

static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t net_htonl(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

#define net_ntohs net_htons
#define net_ntohl net_htonl

#define NET_IP(a, b, c, d)  (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define NET_IP_BROADCAST    0xFFFFFFFF

/* RFC 1071 Internet checksum over len bytes, ready to store as-is in a header field.
   Computing it over a header that already contains a valid checksum yields 0. */
uint16_t net_checksum(const void* data, size_t len);

/* The same in pieces (e.g. pseudo-header + segment): start with sum 0, add each
   chunk, then finish. Only the last chunk may have an odd length. */
uint32_t net_checksum_add(uint32_t sum, const void* data, size_t len);
uint16_t net_checksum_finish(uint32_t sum);

/* Update a stored checksum (as found in the header) after a 16/32-bit field
   changed from old to new (host order values), without touching the data */
uint16_t net_checksum_replace16(uint16_t check, uint16_t old_word, uint16_t new_word);
uint16_t net_checksum_replace32(uint16_t check, uint32_t old_value, uint32_t new_value);

/* Parse "a.b.c.d" (host order result); false on malformed input */
bool net_parse_ip(const char* text, uint32_t* ip);

/* Parse "a.b.c.d/len"; false on malformed input or len > 32 */
bool net_parse_cidr(const char* text, uint32_t* ip, uint8_t* prefix_len);

static inline uint32_t net_prefix_to_mask(uint8_t prefix_len)
{
    return prefix_len == 0 ? 0 : 0xFFFFFFFF << (32 - prefix_len);
}

void net_print_ip(uint32_t ip);

/* Non-cryptographic pseudo random numbers (transaction ids) */
uint32_t net_random32(void);

/* Register the stack with netdev and apply ethN=/gw= options from the command line */
void net_initialize(void);

/* Idle-loop step: run protocol timers, then netdev_run_once() */
void net_run_once(void);

/* Checksum and address parsing unit tests; no network needed */
size_t net_util_self_test(void);

/* Ping every ping=a.b.c.d target from the command line; test mode only */
size_t net_self_test(void);

#endif /* __NET_H__ */
