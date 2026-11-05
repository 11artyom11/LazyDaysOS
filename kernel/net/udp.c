#include "udp.h"
#include "ipv4.h"
#include "icmp.h"
#include "net.h"

struct udp$binding
{
    uint16_t port;                      /* 0 = free */
    udp_handler handler;
};

static struct udp$binding udp$bindings[UDP_MAX_BINDINGS];

struct udp_stats udp_stats;

bool udp_bind(uint16_t port, udp_handler handler)
{
    struct udp$binding* slot = NULL;
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp$bindings[i].port == port) {
            return false;
        }
        if (udp$bindings[i].port == 0 && !slot) {
            slot = &udp$bindings[i];
        }
    }
    if (!slot || port == 0) {
        return false;
    }
    slot->port = port;
    slot->handler = handler;
    return true;
}

void udp_unbind(uint16_t port)
{
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp$bindings[i].port == port) {
            udp$bindings[i].port = 0;
        }
    }
}

/* Checksum over the IPv4 pseudo-header and the UDP header + data */
static uint16_t udp$$checksum(uint32_t src, uint32_t dst, const void* segment, size_t len)
{
    struct
    {
        uint32_t src;
        uint32_t dst;
        uint8_t zero;
        uint8_t protocol;
        uint16_t length;
    } __attribute__((packed)) pseudo = {
        net_htonl(src), net_htonl(dst), 0, IP_PROTO_UDP, net_htons((uint16_t)len)
    };
    uint32_t sum = net_checksum_add(0, &pseudo, sizeof(pseudo));
    return net_checksum_finish(net_checksum_add(sum, segment, len));
}

void udp_receive(struct netdev* dev, const uint8_t* packet, size_t len, size_t header_len)
{
    const struct ipv4_header* ip = (const struct ipv4_header*)packet;
    const struct udp_header* udp = (const struct udp_header*)(packet + header_len);
    size_t segment_len = len - header_len;
    uint32_t src = net_ntohl(ip->src);
    uint32_t dst = net_ntohl(ip->dst);

    if (segment_len < UDP_HEADER_LEN || net_ntohs(udp->length) < UDP_HEADER_LEN ||
        net_ntohs(udp->length) > segment_len) {
        udp_stats.rx_invalid++;
        return;
    }
    segment_len = net_ntohs(udp->length);
    if (udp->checksum != 0 && udp$$checksum(src, dst, udp, segment_len) != 0) {
        udp_stats.rx_invalid++;
        return;
    }
    udp_stats.rx_datagrams++;

    uint16_t port = net_ntohs(udp->dst_port);
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (udp$bindings[i].port == port) {
            udp$bindings[i].handler(dev, src, net_ntohs(udp->src_port), dst, port,
                                    (const uint8_t*)udp + UDP_HEADER_LEN, segment_len - UDP_HEADER_LEN);
            return;
        }
    }
    udp_stats.rx_no_port++;
    icmp_send_error(dev, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, packet, len);
}

bool udp_send(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
              const void* data, size_t len)
{
    uint8_t segment[UDP_HEADER_LEN + UDP_PAYLOAD_MAX];
    struct udp_header* udp = (struct udp_header*)segment;
    struct netdev* dev;
    uint32_t next_hop;

    if (len > UDP_PAYLOAD_MAX) {
        return false;
    }
    /* The checksum covers the source address, so pick it the way ipv4_send would */
    if (src_ip == 0) {
        if (!ipv4_route_lookup(dst_ip, &dev, &next_hop)) {
            return false;
        }
        src_ip = ipv4_iface_address(dev);
    }

    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length = net_htons(UDP_HEADER_LEN + len);
    udp->checksum = 0;
    const uint8_t* bytes = data;
    for (size_t i = 0; i < len; i++) {
        segment[UDP_HEADER_LEN + i] = bytes[i];
    }
    udp->checksum = udp$$checksum(src_ip, dst_ip, segment, UDP_HEADER_LEN + len);
    if (udp->checksum == 0) {
        udp->checksum = 0xFFFF;         /* 0 would mean "no checksum" */
    }

    if (!ipv4_send(src_ip, dst_ip, IP_PROTO_UDP, segment, UDP_HEADER_LEN + len)) {
        return false;
    }
    udp_stats.tx_datagrams++;
    return true;
}

void udp_echo_handler(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                      uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len)
{
    (void)dev;
    /* Never echo to or from broadcast: two echo services could loop forever */
    if (dst_ip == NET_IP_BROADCAST || !ipv4_is_own_address(dst_ip) || src_port == UDP_PORT_ECHO) {
        return;
    }
    udp_send(dst_ip, dst_port, src_ip, src_port, data, len);
}
