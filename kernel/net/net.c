#include "net.h"
#include "netdev.h"
#include "eth.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "dhcp.h"
#include "nat.h"
#include "tcp.h"
#include "http.h"
#include "api.h"
#include "../cmdline.h"
#include <kio.h>
#include <pit.h>
#include <util.h>

#define NET$TICK_MS             100     /* protocol timer granularity */
#define NET$PING_TIMEOUT_MS     3000    /* covers QEMU's 1 s e1000 receive hold after bring-up */
#define NET$DHCP_WAIT_MS        10000   /* self test: time for all DHCP clients to get a lease */
#define NET$MAX_FIELDS          4

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

uint16_t net_transport_checksum(uint32_t src, uint32_t dst, uint8_t protocol, const void* segment, size_t len)
{
    struct
    {
        uint32_t src;
        uint32_t dst;
        uint8_t zero;
        uint8_t protocol;
        uint16_t length;
    } __attribute__((packed)) pseudo = {
        net_htonl(src), net_htonl(dst), 0, protocol, net_htons((uint16_t)len)
    };
    uint32_t sum = net_checksum_add(0, &pseudo, sizeof(pseudo));
    return net_checksum_finish(net_checksum_add(sum, segment, len));
}

uint16_t net_checksum_replace16(uint16_t check, uint16_t old_word, uint16_t new_word)
{
    /* RFC 1624 eqn. 3: HC' = ~(~HC + ~m + m') */
    uint32_t sum = (~net_ntohs(check) & 0xFFFF) + (~old_word & 0xFFFF) + new_word;
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = (sum & 0xFFFF) + (sum >> 16);
    return net_htons((uint16_t)~sum);
}

uint16_t net_checksum_replace32(uint16_t check, uint32_t old_value, uint32_t new_value)
{
    check = net_checksum_replace16(check, old_value >> 16, new_value >> 16);
    return net_checksum_replace16(check, old_value & 0xFFFF, new_value & 0xFFFF);
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

uint32_t net_random32(void)
{
    static uint32_t state = 0;
    if (state == 0) {
        /* Seed from boot time and the first MAC so ports on different boxes differ */
        state = pit_get_ticks() * 2654435761u + 0x9E3779B9u;
        struct netdev* dev = netdev_get(0);
        for (int i = 0; dev && i < ETH_ALEN; i++) {
            state = (state ^ dev->mac[i]) * 16777619u;
        }
        if (state == 0) {
            state = 1;
        }
    }
    /* xorshift32: fine for transaction ids, NOT for anything security relevant */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void net_print_ip(uint32_t ip)
{
    k_print("%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

/* Split value at commas in place; returns the number of fields */
static size_t net$$split(char* value, char** fields, size_t max)
{
    size_t n = 0;
    fields[n++] = value;
    for (char* p = value; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

void net_initialize(void)
{
    char value[48];
    char key[NETDEV_NAME_LEN];
    char* f[NET$MAX_FIELDS];

    netdev_set_rx_handler(eth_receive);

    /* ethN=a.b.c.d/len or ethN=dhcp */
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
        if (value[0] == 'd' && value[1] == 'h' && value[2] == 'c' && value[3] == 'p' && value[4] == '\0') {
            dhcp_client_start(dev);
        } else if (net_parse_cidr(value, &addr, &prefix_len)) {
            ipv4_set_address(dev, addr, prefix_len);
        } else {
            k_print("[K_FAIL] %s=%s: expected address/prefix (e.g. 10.0.2.15/24) or dhcp\n", key, value);
        }
    }

    /* route=net/len,gateway[,ethN] (repeatable): static routes. Name the port when
       the gateway's network isn't known yet, e.g. it comes from DHCP. */
    for (size_t n = 0; cmdline_get("route", n, value, sizeof(value)); n++) {
        size_t count = net$$split(value, f, NET$MAX_FIELDS);
        uint32_t network, gateway;
        uint8_t prefix_len;
        struct netdev* dev = count == 3 ? netdev_find(f[2]) : NULL;
        if (count < 2 || count > 3 || !net_parse_cidr(f[0], &network, &prefix_len) ||
            !net_parse_ip(f[1], &gateway) || (count == 3 && !dev)) {
            k_print("[K_FAIL] route=%s: expected network/len,gateway[,ethN]\n", f[0]);
        } else if (!ipv4_add_route(network, prefix_len, gateway, dev)) {
            k_print("[K_FAIL] route to %s: gateway is not on a connected network\n", f[0]);
        }
    }

    /* dhcpd=ethN,first,last (repeatable): DHCP server pools */
    for (size_t n = 0; cmdline_get("dhcpd", n, value, sizeof(value)); n++) {
        uint32_t first, last;
        struct netdev* dev;
        if (net$$split(value, f, NET$MAX_FIELDS) != 3 || !(dev = netdev_find(f[0])) ||
            !net_parse_ip(f[1], &first) || !net_parse_ip(f[2], &last)) {
            k_print("[K_FAIL] dhcpd=%s: expected ethN,first,last\n", f[0]);
        } else if (!dhcp_server_add_pool(dev, first, last)) {
            k_print("[K_FAIL] dhcpd on %s: range must be inside the port's (static) subnet\n", f[0]);
        } else {
            k_print("[K_INFO] dhcp: serving %s-%s on %s\n", f[1], f[2], f[0]);
        }
    }
    if (cmdline_get("dns", 0, value, sizeof(value))) {
        uint32_t dns;
        if (net_parse_ip(value, &dns)) {
            dhcp_server_set_dns(dns);
        } else {
            k_print("[K_FAIL] dns=%s: not an IPv4 address\n", value);
        }
    }
    if (cmdline_get("dhcp_lease", 0, value, sizeof(value))) {
        const char* p = value;
        uint32_t seconds;
        if (net$$parse_number(&p, 7 * 24 * 3600, &seconds) && *p == '\0' && seconds >= 60) {
            dhcp_server_set_lease_time(seconds);
        } else {
            k_print("[K_FAIL] dhcp_lease=%s: expected seconds, 60 to 604800\n", value);
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

    /* nat=ethN (repeatable): ethN is an outside (WAN) port */
    for (size_t n = 0; cmdline_get("nat", n, value, sizeof(value)); n++) {
        struct netdev* dev = netdev_find(value);
        if (dev && nat_enable(dev)) {
            k_print("[K_INFO] nat: %s is the outside port, unsolicited inbound traffic is dropped\n", value);
        } else {
            k_print("[K_FAIL] nat=%s: no such port (or too many outside ports)\n", value);
        }
    }

    if (cmdline_has("tcp_test")) {
        tcp_enable_test_services();
        k_print("[K_INFO] TCP test services: echo on port 7, 1 MiB chargen on port 19\n");
    }

    if (cmdline_has("http")) {
        if (api_start(80)) {
            k_print("[K_INFO] HTTP management interface on port 80 (inside ports only)\n");
        } else {
            k_print("[K_FAIL] HTTP: could not start the server\n");
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
        dhcp_client_tick(now);
        dhcp_server_tick(now);
        nat_tick(now);
        tcp_tick(now);
        http_tick(now);
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

    /* Incremental update must match a full recompute after changing a 32-bit field */
    uint8_t header[20] = {0x45, 0, 0, 84, 0x12, 0x34, 0x40, 0, 64, 1, 0, 0, 10, 0, 9, 1, 8, 8, 8, 8};
    uint16_t check = net_checksum(header, sizeof(header));
    header[10] = ((uint8_t*)&check)[0];
    header[11] = ((uint8_t*)&check)[1];
    uint32_t old_src = NET_IP(10, 0, 9, 1), new_src = NET_IP(203, 0, 113, 77);
    uint16_t updated = net_checksum_replace32(check, old_src, new_src);
    header[12] = 203; header[13] = 0; header[14] = 113; header[15] = 77;
    header[10] = 0;
    header[11] = 0;
    uint16_t recomputed = net_checksum(header, sizeof(header));
    if (updated != recomputed) {
        k_print("[UTIL_FAIL] incremental checksum 0x%x, recomputed 0x%x\n", net_ntohs(updated), net_ntohs(recomputed));
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

    uint32_t start = pit_uptime_ms();
    while (!dhcp_client_all_bound()) {
        if (pit_uptime_ms() - start >= NET$DHCP_WAIT_MS) {
            k_print("[UTIL_FAIL] DHCP: no lease within %u ms\n", NET$DHCP_WAIT_MS);
            return UTIL$FAIL;
        }
        net_run_once();
    }

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
