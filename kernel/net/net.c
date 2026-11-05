#include "net.h"
#include "netdev.h"
#include "eth.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "../cmdline.h"
#include <kio.h>
#include <pit.h>
#include <util.h>

#define NET$TICK_MS             100     /* protocol timer granularity */
#define NET$PING_TIMEOUT_MS     3000    /* covers QEMU's 1 s e1000 receive hold after bring-up */

static uint32_t net$last_tick = 0;

uint32_t net_checksum_add(uint32_t sum, const void* data, size_t len)
{
    const uint8_t* p = data;

    /* Sum big-endian 16-bit words, so the result is correct on any CPU */
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    /* Fold now so long inputs can't overflow the 32-bit accumulator */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

uint16_t net_checksum_finish(uint32_t sum)
{
    return net_htons((uint16_t)~sum);
}

uint16_t net_checksum(const void* data, size_t len)
{
    return net_checksum_finish(net_checksum_add(0, data, len));
}

/* Parse a decimal number up to max; advances *text */
static bool net$$parse_number(const char** text, uint32_t max, uint32_t* value)
{
    const char* p = *text;
    uint32_t v = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > max) {
            return false;
        }
        p++;
    }
    *text = p;
    *value = v;
    return true;
}

static bool net$$parse_ip_prefix(const char** text, uint32_t* ip)
{
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t octet;
        if (!net$$parse_number(text, 255, &octet)) {
            return false;
        }
        result = (result << 8) | octet;
        if (i < 3) {
            if (**text != '.') {
                return false;
            }
            (*text)++;
        }
    }
    *ip = result;
    return true;
}

bool net_parse_ip(const char* text, uint32_t* ip)
{
    return net$$parse_ip_prefix(&text, ip) && *text == '\0';
}

bool net_parse_cidr(const char* text, uint32_t* ip, uint8_t* prefix_len)
{
    uint32_t prefix;
    if (!net$$parse_ip_prefix(&text, ip) || *text != '/') {
        return false;
    }
    text++;
    if (!net$$parse_number(&text, 32, &prefix) || *text != '\0') {
        return false;
    }
    *prefix_len = (uint8_t)prefix;
    return true;
}

void net_print_ip(uint32_t ip)
{
    k_print("%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

void net_initialize(void)
{
    char value[32];
    char key[NETDEV_NAME_LEN];

    netdev_set_rx_handler(eth_receive);

    /* ethN=a.b.c.d/len */
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        for (int c = 0; c < NETDEV_NAME_LEN; c++) {
            key[c] = dev->name[c];
        }
        if (!cmdline_get(key, 0, value, sizeof(value))) {
            continue;
        }
        uint32_t addr;
        uint8_t prefix_len;
        if (!net_parse_cidr(value, &addr, &prefix_len)) {
            k_print("[K_FAIL] %s=%s: expected address/prefix, e.g. 10.0.2.15/24\n", key, value);
            continue;
        }
        ipv4_set_address(dev, addr, prefix_len);
    }

    /* route=net/len,gateway (repeatable): static routes */
    for (size_t n = 0; cmdline_get("route", n, value, sizeof(value)); n++) {
        char* comma = value;
        while (*comma && *comma != ',') {
            comma++;
        }
        uint32_t network, gateway;
        uint8_t prefix_len;
        bool ok = *comma == ',';
        if (ok) {
            *comma = '\0';
            ok = net_parse_cidr(value, &network, &prefix_len) && net_parse_ip(comma + 1, &gateway);
        }
        if (!ok) {
            k_print("[K_FAIL] route=%s: expected network/len,gateway e.g. 10.1.0.0/16,10.0.2.1\n", value);
        } else if (!ipv4_add_route(network, prefix_len, gateway, NULL)) {
            k_print("[K_FAIL] route to %s: gateway is not on a connected network\n", value);
        }
    }

    /* gw=a.b.c.d: default route via a directly connected gateway */
    if (cmdline_get("gw", 0, value, sizeof(value))) {
        uint32_t gateway;
        if (!net_parse_ip(value, &gateway)) {
            k_print("[K_FAIL] gw=%s: not an IPv4 address\n", value);
        } else if (!ipv4_add_route(0, 0, gateway, NULL)) {
            k_print("[K_FAIL] gw=%s: no interface is on the gateway's network\n", value);
        }
    }

    if (cmdline_has("udp_echo") && udp_bind(UDP_PORT_ECHO, udp_echo_handler)) {
        k_print("[K_INFO] UDP echo service on port %u\n", UDP_PORT_ECHO);
    }

    ipv4_print_config();
}

void net_run_once(void)
{
    uint32_t now = pit_uptime_ms();
    if (now - net$last_tick >= NET$TICK_MS) {
        net$last_tick = now;
        arp_tick(now);
    }
    netdev_run_once();
}

/**
 * @brief Checks the Internet checksum against RFC 1071's worked example and
 *        the address parsers against valid and malformed input. No network needed.
 *
 * @return UTIL$SUCCESS on success
 * @return UTIL$FAIL otherwise
 */
size_t net_util_self_test(void)
{
    /* RFC 1071 section 3: these words sum to 0xDDF2, so the checksum is 0x220D */
    uint8_t data[10] = {0x00, 0x01, 0xF2, 0x03, 0xF4, 0xF5, 0xF6, 0xF7, 0, 0};
    uint16_t sum = net_checksum(data, 8);
    const uint8_t* bytes = (const uint8_t*)&sum;
    if (bytes[0] != 0x22 || bytes[1] != 0x0D) {
        k_print("[UTIL_FAIL] checksum is 0x%x, expected 0x220d\n", net_ntohs(sum));
        return UTIL$FAIL;
    }
    /* Appending the checksum must make the data verify to 0 */
    data[8] = bytes[0];
    data[9] = bytes[1];
    if (net_checksum(data, 10) != 0) {
        k_print("[UTIL_FAIL] checksum does not verify\n");
        return UTIL$FAIL;
    }

    uint32_t ip;
    uint8_t prefix;
    if (!net_parse_cidr("10.0.2.15/24", &ip, &prefix) || ip != NET_IP(10, 0, 2, 15) || prefix != 24) {
        k_print("[UTIL_FAIL] net_parse_cidr rejected or misread 10.0.2.15/24\n");
        return UTIL$FAIL;
    }
    if (!net_parse_ip("255.255.255.255", &ip) || ip != NET_IP_BROADCAST) {
        k_print("[UTIL_FAIL] net_parse_ip misread 255.255.255.255\n");
        return UTIL$FAIL;
    }
    static const char* invalid[] = {"256.1.1.1", "1.2.3", "1.2.3.4.5", "1.2.3.4/33", "1..2.3", "", "1.2.3.4x"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (net_parse_ip(invalid[i], &ip) || net_parse_cidr(invalid[i], &ip, &prefix)) {
            k_print("[UTIL_FAIL] accepted malformed address '%s'\n", invalid[i]);
            return UTIL$FAIL;
        }
    }
    return UTIL$SUCCESS;
}

/**
 * @brief Pings every ping=a.b.c.d target given on the command line. Exercises
 *        routing, ARP resolution with a held packet, IPv4 and ICMP both ways.
 *
 * @return UTIL$SUCCESS if every target answered (or none was given)
 * @return UTIL$FAIL otherwise
 */
size_t net_self_test(void)
{
    char value[32];
    for (size_t n = 0; cmdline_get("ping", n, value, sizeof(value)); n++) {
        uint32_t target;
        if (!net_parse_ip(value, &target)) {
            k_print("[UTIL_FAIL] ping=%s: not an IPv4 address\n", value);
            return UTIL$FAIL;
        }
        int32_t rtt = icmp_ping(target, NET$PING_TIMEOUT_MS);
        if (rtt < 0) {
            k_print("[UTIL_FAIL] no echo reply from %s within %u ms\n", value, NET$PING_TIMEOUT_MS);
            return UTIL$FAIL;
        }
        k_print("[K_INFO] ping %s: reply in %d ms\n", value, rtt);
    }
    return UTIL$SUCCESS;
}
