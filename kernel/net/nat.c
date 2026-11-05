#include "nat.h"
#include "net.h"
#include "ipv4.h"
#include "icmp.h"
#include <kio.h>
#include <pit.h>

#define NAT$NONE            (-1)
#define NAT$PORT_FIRST      32768       /* range for ports that can't be preserved */
#define NAT$PORT_COUNT      28232       /* 32768 - 60999, the Linux ephemeral range */
#define NAT$MAX_OUTSIDE     2

#define TCP_FLAG_FIN        0x01
#define TCP_FLAG_SYN        0x02
#define TCP_FLAG_RST        0x04
#define TCP_FLAG_ACK        0x10

/* Offsets inside the transport header */
#define UDP$CHECKSUM        6
#define TCP$FLAGS           13
#define TCP$CHECKSUM        16
#define ICMP$CHECKSUM       2
#define ICMP$ID             4

enum nat$tcp_state { NAT$TCP_SYN, NAT$TCP_ESTABLISHED, NAT$TCP_FIN, NAT$TCP_RST };

struct nat$entry
{
    bool used;
    uint8_t protocol;
    uint8_t tcp_state;
    bool replied;
    const struct netdev* outside;
    uint32_t int_ip;                    /* inside host */
    uint16_t int_port;                  /* or ICMP echo identifier */
    uint32_t rem_ip;                    /* remote host */
    uint16_t rem_port;                  /* 0 for ICMP */
    uint32_t ext_ip;                    /* outside port's address */
    uint16_t ext_port;                  /* or translated ICMP identifier */
    uint32_t expires_ms;
    int16_t next_out;                   /* hash chains */
    int16_t next_in;
};

static struct nat$entry nat$table[NAT_TABLE_SIZE];
static int16_t nat$out_hash[NAT_HASH_SIZE];
static int16_t nat$in_hash[NAT_HASH_SIZE];
static const struct netdev* nat$outside[NAT$MAX_OUTSIDE];
static bool nat$initialized = false;

struct nat_stats nat_stats;

static void nat$$init(void)
{
    for (int i = 0; i < NAT_HASH_SIZE; i++) {
        nat$out_hash[i] = NAT$NONE;
        nat$in_hash[i] = NAT$NONE;
    }
    nat$initialized = true;
}

static uint32_t nat$$hash(uint8_t protocol, uint32_t a, uint16_t b, uint32_t c, uint16_t d)
{
    /* Mix in all fields so connections spread over the buckets */
    uint32_t h = protocol * 0x9E3779B1u;
    h = (h ^ a) * 0x85EBCA6Bu;
    h = (h ^ ((uint32_t)b << 16 | d)) * 0xC2B2AE35u;
    h = (h ^ c) * 0x27D4EB2Fu;
    h ^= h >> 15;
    return h & (NAT_HASH_SIZE - 1);
}

static uint32_t nat$$out_key(const struct nat$entry* e)
{
    return nat$$hash(e->protocol, e->int_ip, e->int_port, e->rem_ip, e->rem_port);
}

static uint32_t nat$$in_key(const struct nat$entry* e)
{
    return nat$$hash(e->protocol, e->rem_ip, e->rem_port, e->ext_ip, e->ext_port);
}

bool nat_enable(struct netdev* dev)
{
    if (!nat$initialized) {
        nat$$init();
    }
    for (int i = 0; i < NAT$MAX_OUTSIDE; i++) {
        if (nat$outside[i] == dev) {
            return true;
        }
        if (nat$outside[i] == NULL) {
            nat$outside[i] = dev;
            return true;
        }
    }
    return false;
}

bool nat_is_outside(const struct netdev* dev)
{
    for (int i = 0; i < NAT$MAX_OUTSIDE; i++) {
        if (nat$outside[i] && nat$outside[i] == dev) {
            return true;
        }
    }
    return false;
}

/* ---- table maintenance ---------------------------------------------------- */

static void nat$$unlink(int16_t* head, int16_t index, bool outward)
{
    int16_t* link = head;
    while (*link != NAT$NONE) {
        if (*link == index) {
            *link = outward ? nat$table[index].next_out : nat$table[index].next_in;
            return;
        }
        link = outward ? &nat$table[*link].next_out : &nat$table[*link].next_in;
    }
}

static void nat$$remove(int16_t index)
{
    struct nat$entry* e = &nat$table[index];
    nat$$unlink(&nat$out_hash[nat$$out_key(e)], index, true);
    nat$$unlink(&nat$in_hash[nat$$in_key(e)], index, false);
    e->used = false;
}

static int16_t nat$$find_out(uint8_t protocol, uint32_t int_ip, uint16_t int_port,
                             uint32_t rem_ip, uint16_t rem_port, const struct netdev* outside)
{
    for (int16_t i = nat$out_hash[nat$$hash(protocol, int_ip, int_port, rem_ip, rem_port)];
         i != NAT$NONE; i = nat$table[i].next_out) {
        struct nat$entry* e = &nat$table[i];
        if (e->protocol == protocol && e->int_ip == int_ip && e->int_port == int_port &&
            e->rem_ip == rem_ip && e->rem_port == rem_port && e->outside == outside) {
            return i;
        }
    }
    return NAT$NONE;
}

static int16_t nat$$find_in(uint8_t protocol, uint32_t rem_ip, uint16_t rem_port,
                            uint32_t ext_ip, uint16_t ext_port)
{
    for (int16_t i = nat$in_hash[nat$$hash(protocol, rem_ip, rem_port, ext_ip, ext_port)];
         i != NAT$NONE; i = nat$table[i].next_in) {
        struct nat$entry* e = &nat$table[i];
        if (e->protocol == protocol && e->rem_ip == rem_ip && e->rem_port == rem_port &&
            e->ext_ip == ext_ip && e->ext_port == ext_port) {
            return i;
        }
    }
    return NAT$NONE;
}

/* External port already used by a different inside endpoint? (only on new connections) */
static bool nat$$ext_port_taken(uint8_t protocol, uint32_t ext_ip, uint16_t port,
                                uint32_t int_ip, uint16_t int_port)
{
    for (int i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat$entry* e = &nat$table[i];
        if (e->used && e->protocol == protocol && e->ext_ip == ext_ip && e->ext_port == port &&
            (e->int_ip != int_ip || e->int_port != int_port)) {
            return true;
        }
    }
    return false;
}

static uint16_t nat$$choose_port(uint8_t protocol, const struct netdev* outside, uint32_t ext_ip,
                                 uint32_t int_ip, uint16_t int_port)
{
    /* Endpoint-independent mapping: reuse this endpoint's existing external port */
    for (int i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat$entry* e = &nat$table[i];
        if (e->used && e->protocol == protocol && e->outside == outside &&
            e->int_ip == int_ip && e->int_port == int_port) {
            return e->ext_port;
        }
    }
    /* Port preservation */
    if (int_port != 0 && !nat$$ext_port_taken(protocol, ext_ip, int_port, int_ip, int_port)) {
        return int_port;
    }
    uint32_t start = net_random32() % NAT$PORT_COUNT;
    for (uint32_t n = 0; n < NAT$PORT_COUNT; n++) {
        uint16_t port = NAT$PORT_FIRST + (start + n) % NAT$PORT_COUNT;
        if (!nat$$ext_port_taken(protocol, ext_ip, port, int_ip, int_port)) {
            return port;
        }
    }
    return 0;
}

static int16_t nat$$create(uint8_t protocol, const struct netdev* outside, uint32_t int_ip, uint16_t int_port,
                           uint32_t rem_ip, uint16_t rem_port, uint32_t ext_ip)
{
    int16_t slot = NAT$NONE;
    for (int16_t i = 0; i < NAT_TABLE_SIZE; i++) {
        if (!nat$table[i].used) {
            slot = i;
            break;
        }
    }
    uint16_t ext_port = nat$$choose_port(protocol, outside, ext_ip, int_ip, int_port);
    if (slot == NAT$NONE || ext_port == 0) {
        nat_stats.table_full++;
        return NAT$NONE;
    }

    struct nat$entry* e = &nat$table[slot];
    e->used = true;
    e->protocol = protocol;
    e->replied = false;
    e->tcp_state = NAT$TCP_SYN;
    e->outside = outside;
    e->int_ip = int_ip;
    e->int_port = int_port;
    e->rem_ip = rem_ip;
    e->rem_port = rem_port;
    e->ext_ip = ext_ip;
    e->ext_port = ext_port;

    uint32_t out_key = nat$$out_key(e), in_key = nat$$in_key(e);
    e->next_out = nat$out_hash[out_key];
    nat$out_hash[out_key] = slot;
    e->next_in = nat$in_hash[in_key];
    nat$in_hash[in_key] = slot;
    nat_stats.created++;
    return slot;
}

/* Track TCP flags and refresh the idle timeout */
static void nat$$touch(struct nat$entry* e, const uint8_t* transport, bool inbound)
{
    uint32_t timeout;
    if (inbound) {
        e->replied = true;
    }
    switch (e->protocol) {
    case IP_PROTO_TCP: {
        uint8_t flags = transport[TCP$FLAGS];
        if (flags & TCP_FLAG_RST) {
            e->tcp_state = NAT$TCP_RST;
        } else if (flags & TCP_FLAG_FIN) {
            e->tcp_state = NAT$TCP_FIN;
        } else if (e->tcp_state == NAT$TCP_SYN && (inbound || !(flags & TCP_FLAG_SYN))) {
            /* A reply, or a mid-stream packet we hadn't seen: treat as established */
            e->tcp_state = NAT$TCP_ESTABLISHED;
        }
        timeout = e->tcp_state == NAT$TCP_SYN ? NAT_TIMEOUT_TCP_SYN_MS :
                  e->tcp_state == NAT$TCP_ESTABLISHED ? NAT_TIMEOUT_TCP_EST_MS :
                  e->tcp_state == NAT$TCP_FIN ? NAT_TIMEOUT_TCP_FIN_MS : NAT_TIMEOUT_TCP_RST_MS;
        break;
    }
    case IP_PROTO_UDP:
        timeout = e->replied ? NAT_TIMEOUT_UDP_ASSURED_MS : NAT_TIMEOUT_UDP_MS;
        break;
    default:
        timeout = NAT_TIMEOUT_ICMP_MS;
        break;
    }
    e->expires_ms = pit_uptime_ms() + timeout;
}

void nat_flush(const struct netdev* dev)
{
    for (int16_t i = 0; i < NAT_TABLE_SIZE; i++) {
        if (nat$table[i].used && nat$table[i].outside == dev) {
            nat$$remove(i);
        }
    }
}

void nat_tick(uint32_t now_ms)
{
    for (int16_t i = 0; i < NAT_TABLE_SIZE; i++) {
        if (nat$table[i].used && (int32_t)(now_ms - nat$table[i].expires_ms) >= 0) {
            nat$$remove(i);
            nat_stats.expired++;
        }
    }
}

/* ---- packet rewriting ----------------------------------------------------- */

static uint16_t nat$$get16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void nat$$put16(uint8_t* p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

/* Where a translatable packet's "port" lives: TCP/UDP source/destination port, ICMP echo id */
static bool nat$$ports(uint8_t protocol, const uint8_t* transport, size_t len, bool is_reply,
                       uint16_t* src_port, uint16_t* dst_port)
{
    if (protocol == IP_PROTO_TCP || protocol == IP_PROTO_UDP) {
        if (len < 8) {
            return false;
        }
        *src_port = nat$$get16(transport);
        *dst_port = nat$$get16(transport + 2);
        return true;
    }
    if (protocol == IP_PROTO_ICMP && len >= 8) {
        uint8_t type = transport[0];
        if (type == (is_reply ? ICMP_ECHO_REPLY : ICMP_ECHO_REQUEST)) {
            *src_port = nat$$get16(transport + ICMP$ID);
            *dst_port = 0;
            return true;
        }
    }
    return false;
}

/*
    Rewrite one address and one port/id of a packet and fix its checksums.
    addr_field/port_field point into the packet; the IP header checksum is
    left to the caller (forwarding recomputes it anyway).
*/
static void nat$$rewrite(uint8_t protocol, uint8_t* transport, size_t transport_len,
                         uint8_t* addr_field, uint32_t new_addr, uint8_t* port_field, uint16_t new_port)
{
    uint32_t old_addr = ((uint32_t)nat$$get16(addr_field) << 16) | nat$$get16(addr_field + 2);
    uint16_t old_port = nat$$get16(port_field);
    nat$$put16(addr_field, new_addr >> 16);
    nat$$put16(addr_field + 2, new_addr & 0xFFFF);
    nat$$put16(port_field, new_port);

    uint8_t* check = NULL;
    bool covers_addr = true;            /* TCP/UDP checksums include the IP pseudo-header */
    if (protocol == IP_PROTO_TCP && transport_len >= TCP$CHECKSUM + 2) {
        check = transport + TCP$CHECKSUM;
    } else if (protocol == IP_PROTO_UDP && transport_len >= UDP$CHECKSUM + 2) {
        check = transport + UDP$CHECKSUM;
        if (nat$$get16(check) == 0) {
            return;                     /* sender didn't use a checksum */
        }
    } else if (protocol == IP_PROTO_ICMP && transport_len >= ICMP$CHECKSUM + 2) {
        check = transport + ICMP$CHECKSUM;
        covers_addr = false;
    }
    if (!check) {
        return;
    }

    /* Load and store the field byte by byte in native order ("as stored"),
       which is what the checksum helpers expect, on any CPU */
    union { uint16_t value; uint8_t bytes[2]; } field;
    field.bytes[0] = check[0];
    field.bytes[1] = check[1];
    if (covers_addr) {
        field.value = net_checksum_replace32(field.value, old_addr, new_addr);
    }
    field.value = net_checksum_replace16(field.value, old_port, new_port);
    if (protocol == IP_PROTO_UDP && field.value == 0) {
        field.value = 0xFFFF;
    }
    check[0] = field.bytes[0];
    check[1] = field.bytes[1];
}

bool nat_outbound(struct netdev* dev, uint8_t* packet, size_t len, size_t header_len)
{
    struct ipv4_header* ip = (struct ipv4_header*)packet;
    uint8_t* transport = packet + header_len;
    size_t transport_len = len - header_len;
    uint32_t ext_ip = ipv4_iface_address(dev);
    uint16_t src_port, dst_port;

    if (ext_ip == 0 || !nat$$ports(ip->protocol, transport, transport_len, false, &src_port, &dst_port)) {
        nat_stats.untranslatable++;
        return false;
    }
    uint32_t src = net_ntohl(ip->src);
    uint32_t dst = net_ntohl(ip->dst);

    int16_t index = nat$$find_out(ip->protocol, src, src_port, dst, dst_port, dev);
    if (index == NAT$NONE) {
        index = nat$$create(ip->protocol, dev, src, src_port, dst, dst_port, ext_ip);
        if (index == NAT$NONE) {
            return false;
        }
    }
    struct nat$entry* e = &nat$table[index];
    nat$$touch(e, transport, false);

    uint8_t* port_field = ip->protocol == IP_PROTO_ICMP ? transport + ICMP$ID : transport;
    nat$$rewrite(ip->protocol, transport, transport_len, (uint8_t*)&ip->src, e->ext_ip, port_field, e->ext_port);
    nat_stats.outbound++;
    return true;
}

/*
    An ICMP error from the outside quoting a packet we translated: the quoted
    packet went out as ext_ip:ext_port -> rem_ip:rem_port. Restore it to what the
    inside host sent and send the error to that host.
*/
static bool nat$$inbound_icmp_error(struct ipv4_header* ip, uint8_t* icmp, size_t icmp_len)
{
    if (icmp_len < 8 + IPV4_HEADER_LEN + 8) {
        return false;
    }
    uint8_t* quoted = icmp + 8;
    struct ipv4_header* qip = (struct ipv4_header*)quoted;
    size_t qheader_len = (qip->version_ihl & 0x0F) * 4;
    if (qheader_len < IPV4_HEADER_LEN || 8 + qheader_len + 8 > icmp_len) {
        return false;
    }
    uint8_t* qtransport = quoted + qheader_len;
    size_t qtransport_len = icmp_len - 8 - qheader_len;
    uint16_t qsrc_port, qdst_port;
    if (!nat$$ports(qip->protocol, qtransport, qtransport_len, false, &qsrc_port, &qdst_port)) {
        return false;
    }

    int16_t index = nat$$find_in(qip->protocol, net_ntohl(qip->dst), qdst_port, net_ntohl(qip->src), qsrc_port);
    if (index == NAT$NONE) {
        return false;
    }
    struct nat$entry* e = &nat$table[index];

    /* Quoted packet: source back to the inside host (only 8 transport bytes are
       quoted, so a TCP checksum isn't there to fix; ICMP/UDP ones are) */
    uint8_t* qport = qip->protocol == IP_PROTO_ICMP ? qtransport + ICMP$ID : qtransport;
    nat$$rewrite(qip->protocol, qtransport, qtransport_len, (uint8_t*)&qip->src, e->int_ip, qport, e->int_port);
    qip->checksum = 0;
    qip->checksum = net_checksum(quoted, qheader_len);

    /* Outer packet: deliver to the inside host; recompute the ICMP checksum over everything */
    ip->dst = net_htonl(e->int_ip);
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t c = net_checksum(icmp, icmp_len);
    icmp[2] = ((uint8_t*)&c)[0];
    icmp[3] = ((uint8_t*)&c)[1];
    nat_stats.icmp_errors++;
    return true;
}

bool nat_inbound(struct netdev* dev, uint8_t* packet, size_t len, size_t header_len)
{
    struct ipv4_header* ip = (struct ipv4_header*)packet;
    uint8_t* transport = packet + header_len;
    size_t transport_len = len - header_len;
    uint16_t src_port, dst_port;

    if (!nat_is_outside(dev)) {
        return false;
    }
    if (ip->protocol == IP_PROTO_ICMP && transport_len >= 1 &&
        (transport[0] == ICMP_DEST_UNREACH || transport[0] == ICMP_TIME_EXCEEDED)) {
        return nat$$inbound_icmp_error(ip, transport, transport_len);
    }
    if (!nat$$ports(ip->protocol, transport, transport_len, true, &src_port, &dst_port)) {
        return false;
    }

    /* For ICMP echo replies the identifier is the "destination port" we mapped */
    uint16_t rem_port = ip->protocol == IP_PROTO_ICMP ? 0 : src_port;
    uint16_t ext_port = ip->protocol == IP_PROTO_ICMP ? src_port : dst_port;
    int16_t index = nat$$find_in(ip->protocol, net_ntohl(ip->src), rem_port, net_ntohl(ip->dst), ext_port);
    if (index == NAT$NONE) {
        return false;
    }
    struct nat$entry* e = &nat$table[index];
    nat$$touch(e, transport, true);

    uint8_t* port_field = ip->protocol == IP_PROTO_ICMP ? transport + ICMP$ID : transport + 2;
    nat$$rewrite(ip->protocol, transport, transport_len, (uint8_t*)&ip->dst, e->int_ip, port_field, e->int_port);
    nat_stats.inbound++;
    return true;
}

size_t nat_active_entries(void)
{
    size_t n = 0;
    for (int i = 0; i < NAT_TABLE_SIZE; i++) {
        n += nat$table[i].used;
    }
    return n;
}

void nat_print_table(void)
{
    static const char* names[] = {"syn", "est", "fin", "rst"};
    for (int i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat$entry* e = &nat$table[i];
        if (!e->used) {
            continue;
        }
        k_print("  %s ", e->protocol == IP_PROTO_TCP ? "tcp" : e->protocol == IP_PROTO_UDP ? "udp" : "icmp");
        net_print_ip(e->int_ip);
        k_print(":%u -> ", e->int_port);
        net_print_ip(e->ext_ip);
        k_print(":%u -> ", e->ext_port);
        net_print_ip(e->rem_ip);
        k_print(":%u %s\n", e->rem_port, e->protocol == IP_PROTO_TCP ? names[e->tcp_state] : (e->replied ? "replied" : "new"));
    }
}
