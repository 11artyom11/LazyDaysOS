#include "ipv4.h"
#include "net.h"
#include "arp.h"
#include "icmp.h"
#include <kio.h>

#define IPV4_FLAG_MF        0x2000      /* more fragments */
#define IPV4_FRAGMENT_MASK  0x1FFF

struct ipv4$iface
{
    uint32_t addr;                      /* 0 = unconfigured */
    uint32_t mask;
};

struct ipv4$route
{
    bool used;
    uint32_t network;
    uint32_t mask;
    uint8_t prefix_len;
    uint32_t gateway;                   /* 0 = directly connected */
    struct netdev* dev;
};

static struct ipv4$iface ipv4$ifaces[NETDEV_MAX];
static struct ipv4$route ipv4$routes[IPV4_ROUTES_MAX];
static uint16_t ipv4$next_id = 1;

struct ipv4_stats ipv4_stats;

static struct ipv4$iface* ipv4$$iface(const struct netdev* dev)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        if (netdev_get(i) == dev) {
            return &ipv4$ifaces[i];
        }
    }
    return NULL;
}

uint32_t ipv4_iface_address(const struct netdev* dev)
{
    struct ipv4$iface* iface = ipv4$$iface(dev);
    return iface ? iface->addr : 0;
}

bool ipv4_route_lookup(uint32_t dst, struct netdev** dev, uint32_t* next_hop)
{
    struct ipv4$route* best = NULL;
    for (int i = 0; i < IPV4_ROUTES_MAX; i++) {
        struct ipv4$route* r = &ipv4$routes[i];
        if (r->used && (dst & r->mask) == r->network && (!best || r->prefix_len > best->prefix_len)) {
            best = r;
        }
    }
    if (!best) {
        return false;
    }
    *dev = best->dev;
    *next_hop = best->gateway ? best->gateway : dst;
    return true;
}

bool ipv4_add_route(uint32_t network, uint8_t prefix_len, uint32_t gateway, struct netdev* dev)
{
    if (dev == NULL) {
        uint32_t unused;
        if (!ipv4_route_lookup(gateway, &dev, &unused)) {
            return false;
        }
    }

    uint32_t mask = net_prefix_to_mask(prefix_len);
    struct ipv4$route* slot = NULL;
    for (int i = 0; i < IPV4_ROUTES_MAX; i++) {
        struct ipv4$route* r = &ipv4$routes[i];
        if (r->used && r->network == (network & mask) && r->prefix_len == prefix_len) {
            slot = r;                   /* replace the existing route to this prefix */
            break;
        }
        if (!r->used && !slot) {
            slot = r;
        }
    }
    if (!slot) {
        return false;
    }
    slot->used = true;
    slot->network = network & mask;
    slot->mask = mask;
    slot->prefix_len = prefix_len;
    slot->gateway = gateway;
    slot->dev = dev;
    return true;
}

void ipv4_set_address(struct netdev* dev, uint32_t addr, uint8_t prefix_len)
{
    struct ipv4$iface* iface = ipv4$$iface(dev);
    if (!iface) {
        return;
    }
    iface->addr = addr;
    iface->mask = net_prefix_to_mask(prefix_len);
    ipv4_add_route(addr, prefix_len, 0, dev);
}

bool ipv4_is_own_address(uint32_t ip)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        if (ipv4$ifaces[i].addr && ipv4$ifaces[i].addr == ip) {
            return true;
        }
    }
    return false;
}

/* Addressed to this host: one of our addresses, or a broadcast on the receiving interface */
static bool ipv4$$is_local(const struct netdev* dev, uint32_t dst)
{
    if (dst == NET_IP_BROADCAST || ipv4_is_own_address(dst)) {
        return true;
    }
    struct ipv4$iface* iface = ipv4$$iface(dev);
    return iface && iface->addr && dst == (iface->addr | ~iface->mask);
}

void ipv4_receive(struct netdev* dev, const uint8_t* packet, size_t len)
{
    const struct ipv4_header* ip = (const struct ipv4_header*)packet;
    ipv4_stats.rx_packets++;

    if (len < IPV4_HEADER_LEN || (ip->version_ihl >> 4) != 4) {
        ipv4_stats.rx_bad_header++;
        return;
    }
    size_t header_len = (ip->version_ihl & 0x0F) * 4;
    size_t total_len = net_ntohs(ip->total_len);
    /* Frames may carry Ethernet padding after the packet, so total_len <= len */
    if (header_len < IPV4_HEADER_LEN || total_len < header_len || total_len > len ||
        net_checksum(packet, header_len) != 0) {
        ipv4_stats.rx_bad_header++;
        return;
    }
    if (net_ntohs(ip->flags_fragment) & (IPV4_FLAG_MF | IPV4_FRAGMENT_MASK)) {
        ipv4_stats.rx_fragments++;
        return;
    }

    uint32_t dst = net_ntohl(ip->dst);
    if (!ipv4$$is_local(dev, dst)) {
        ipv4_stats.rx_not_for_us++;
        return;
    }

    const uint8_t* payload = packet + header_len;
    size_t payload_len = total_len - header_len;
    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        ipv4_stats.rx_delivered++;
        icmp_receive(dev, net_ntohl(ip->src), dst, payload, payload_len);
        break;
    default:
        ipv4_stats.rx_unknown_protocol++;
        break;
    }
}

bool ipv4_send(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, size_t len)
{
    struct netdev* dev;
    uint32_t next_hop;
    uint8_t packet[IPV4_HEADER_LEN + IPV4_PAYLOAD_MAX];
    struct ipv4_header* ip = (struct ipv4_header*)packet;

    if (len > IPV4_PAYLOAD_MAX) {
        return false;
    }
    if (!ipv4_route_lookup(dst, &dev, &next_hop)) {
        ipv4_stats.tx_no_route++;
        return false;
    }
    if (src == 0) {
        src = ipv4_iface_address(dev);
    }

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = net_htons(IPV4_HEADER_LEN + len);
    ip->id = net_htons(ipv4$next_id++);
    ip->flags_fragment = 0;
    ip->ttl = IPV4_DEFAULT_TTL;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src = net_htonl(src);
    ip->dst = net_htonl(dst);
    ip->checksum = net_checksum(ip, IPV4_HEADER_LEN);

    const uint8_t* data = payload;
    for (size_t i = 0; i < len; i++) {
        packet[IPV4_HEADER_LEN + i] = data[i];
    }

    ipv4_stats.tx_packets++;
    return arp_output(dev, next_hop, packet, IPV4_HEADER_LEN + len);
}

void ipv4_print_config(void)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        k_print("[K_INFO] %s: ", dev->name);
        if (ipv4$ifaces[i].addr) {
            net_print_ip(ipv4$ifaces[i].addr);
            uint32_t mask = ipv4$ifaces[i].mask;
            uint32_t prefix = 0;
            while (mask & 0x80000000) {
                prefix++;
                mask <<= 1;
            }
            k_print("/%u\n", prefix);
        } else {
            k_print("no IPv4 address\n");
        }
    }
    for (int i = 0; i < IPV4_ROUTES_MAX; i++) {
        struct ipv4$route* r = &ipv4$routes[i];
        if (!r->used) {
            continue;
        }
        k_print("[K_INFO] route ");
        net_print_ip(r->network);
        k_print("/%u ", r->prefix_len);
        if (r->gateway) {
            k_print("via ");
            net_print_ip(r->gateway);
            k_print(" ");
        }
        k_print("dev %s\n", r->dev->name);
    }
}
