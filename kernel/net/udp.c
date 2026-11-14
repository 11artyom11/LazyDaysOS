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
    if (udp->checksum != 0 && net_transport_checksum(src, dst, IP_PROTO_UDP, udp, segment_len) != 0) {
        udp_stats.rx_invalid++;
        return;
    }
    udp_stats.rx_datagrams++;

    uint16_t port = net_ntohs(udp->dst_port);
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        /* Free slots have port 0: a datagram to port 0 must not match one (it used to
           call the free slot's NULL handler, a remote crash; found by fuzzing) */
        if (udp$bindings[i].handler && udp$bindings[i].port == port) {
            udp$bindings[i].handler(dev, src, net_ntohs(udp->src_port), dst, port,
                                    (const uint8_t*)udp + UDP_HEADER_LEN, segment_len - UDP_HEADER_LEN);
            return;
        }
    }
    udp_stats.rx_no_port++;
    icmp_send_error(dev, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, packet, len);
}

/* Fill segment with header + data; returns its length */
static size_t udp$$build(uint8_t* segment, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                         uint16_t dst_port, const void* data, size_t len)
{
    struct udp_header* udp = (struct udp_header*)segment;
    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length = net_htons(UDP_HEADER_LEN + len);
    udp->checksum = 0;
    const uint8_t* bytes = data;
    for (size_t i = 0; i < len; i++) {
        segment[UDP_HEADER_LEN + i] = bytes[i];
    }
    udp->checksum = net_transport_checksum(src_ip, dst_ip, IP_PROTO_UDP, segment, UDP_HEADER_LEN + len);
    if (udp->checksum == 0) {
        udp->checksum = 0xFFFF;         /* 0 would mean "no checksum" */
    }
    return UDP_HEADER_LEN + len;
}

bool udp_send(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
              const void* data, size_t len)
{
    uint8_t segment[UDP_HEADER_LEN + UDP_PAYLOAD_MAX];
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
    size_t seg_len = udp$$build(segment, src_ip, src_port, dst_ip, dst_port, data, len);
    if (!ipv4_send(src_ip, dst_ip, IP_PROTO_UDP, segment, seg_len)) {
        return false;
    }
    udp_stats.tx_datagrams++;
    return true;
}

bool udp_send_dev(struct netdev* dev, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                  uint16_t dst_port, const void* data, size_t len)
{
    uint8_t segment[UDP_HEADER_LEN + UDP_PAYLOAD_MAX];
    if (len > UDP_PAYLOAD_MAX) {
        return false;
    }
    size_t seg_len = udp$$build(segment, src_ip, src_port, dst_ip, dst_port, data, len);
    if (!ipv4_send_dev(dev, src_ip, dst_ip, IP_PROTO_UDP, segment, seg_len)) {
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
