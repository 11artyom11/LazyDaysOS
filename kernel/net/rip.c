#include "rip.h"
#include "ipv4.h"
#include "udp.h"
#include "nat.h"
#include "net.h"
#include "netdev.h"
#include "events.h"
#include "../cmdline.h"
#include "../random.h"
#include <kio.h>
#include <pit.h>

#define RIP$CMD_REQUEST     1
#define RIP$CMD_RESPONSE    2
#define RIP$VERSION         2
#define RIP$AFI_INET        2
#define RIP$AFI_AUTH        0xFFFF
#define RIP$HEADER_LEN      4
#define RIP$ENTRY_LEN       20
#define RIP$PACKET_MAX      (RIP$HEADER_LEN + RIP_ENTRIES_PER_PACKET * RIP$ENTRY_LEN)

struct rip$entry
{
    uint16_t afi;
    uint16_t tag;
    uint32_t addr;
    uint32_t mask;
    uint32_t next_hop;
    uint32_t metric;
} __attribute__((packed));

struct rip$route
{
    bool used;
    uint32_t network, mask;
    uint8_t prefix_len;
    uint32_t gateway;                   /* the neighbour that announced it */
    struct netdev* dev;
    uint8_t metric;
    uint32_t updated_ms;                /* last announcement: the timeout counts from here */
    bool deleting;                      /* unreachable, announced as such until delete_at_ms */
    uint32_t delete_at_ms;
    bool changed;                       /* not yet in a triggered update */
};

static bool rip$running = false;
static bool rip$enabled[NETDEV_MAX];    /* RIP is spoken on port i */
static bool rip$was_up[NETDEV_MAX];
static bool rip$iface_changed[NETDEV_MAX];
static uint32_t rip$update_ms = 30000, rip$timeout_ms = 180000, rip$garbage_ms = 120000;
static struct rip$route rip$routes[RIP_MAX_ROUTES];
static uint32_t rip$next_update_ms;
static bool rip$trigger_pending = false;
static uint32_t rip$trigger_at_ms;

struct rip_stats rip_stats;

static bool rip$$reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static int rip$$index(const struct netdev* dev)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        if (netdev_get(i) == dev) {
            return (int)i;
        }
    }
    return -1;
}

/* RIP is spoken here: configured, never a NAT outside port, has an address */
static bool rip$$port(const struct netdev* dev)
{
    int i = rip$$index(dev);
    return i >= 0 && rip$enabled[i] && !nat_is_outside(dev) && ipv4_iface_address(dev) != 0;
}

/* The connected network of dev is announced (on every RIP port) */
static bool rip$$announced_iface(const struct netdev* dev)
{
    return !nat_is_outside(dev) && ipv4_iface_address(dev) != 0;
}

static bool rip$$is_connected(uint32_t network, uint32_t mask)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        uint32_t addr = ipv4_iface_address(dev), m = ipv4_iface_netmask(dev);
        if (addr && m == mask && (addr & m) == network) {
            return true;
        }
    }
    return false;
}

/* A destination worth a route: contiguous mask, no host bits, not 0/8 (except
   the default route), loopback or class D/E */
static bool rip$$usable(uint32_t addr, uint32_t mask)
{
    if (!ipv4_mask_valid(mask) || (addr & ~mask) != 0) {
        return false;
    }
    if (addr == 0) {
        return mask == 0;
    }
    uint32_t top = addr >> 24;
    return top != 0 && top != 127 && top < 224;
}

static struct rip$route* rip$$find(uint32_t network, uint8_t prefix_len)
{
    for (int i = 0; i < RIP_MAX_ROUTES; i++) {
        if (rip$routes[i].used && rip$routes[i].network == network && rip$routes[i].prefix_len == prefix_len) {
            return &rip$routes[i];
        }
    }
    return NULL;
}

static void rip$$schedule_trigger(uint32_t now)
{
    if (!rip$trigger_pending) {
        rip$trigger_pending = true;
        rip$trigger_at_ms = now + 1000 + random32() % 4000;     /* RFC 2453 3.10.1: 1-5 s */
    }
}

static void rip$$changed(struct rip$route* r, uint32_t now)
{
    r->changed = true;
    rip_stats.route_changes++;
    rip$$schedule_trigger(now);
}

static void rip$$install(struct rip$route* r)
{
    ipv4_add_route_from(r->network, r->prefix_len, r->gateway, r->dev, IPV4_ORIGIN_RIP, r->metric);
}

/* Unreachable now: out of the routing table, announced with metric 16 until deleted */
static void rip$$unreachable(struct rip$route* r, uint32_t now)
{
    if (r->deleting) {
        return;
    }
    r->metric = RIP_INFINITY;
    r->deleting = true;
    r->delete_at_ms = now + rip$garbage_ms;
    ipv4_remove_route_from(r->network, r->prefix_len, IPV4_ORIGIN_RIP);
    rip$$changed(r, now);
}

/* ---- sending ---------------------------------------------------------------- */

struct rip$packet
{
    struct netdev* dev;
    uint32_t dst;
    uint16_t dport;
    uint8_t data[RIP$PACKET_MAX];
    size_t entries;
};

static void rip$$flush(struct rip$packet* p)
{
    if (p->entries == 0) {
        return;
    }
    p->data[0] = RIP$CMD_RESPONSE;
    p->data[1] = RIP$VERSION;
    p->data[2] = p->data[3] = 0;
    udp_send_dev(p->dev, ipv4_iface_address(p->dev), RIP_PORT, p->dst, p->dport,
                 p->data, RIP$HEADER_LEN + p->entries * RIP$ENTRY_LEN);
    p->entries = 0;
}

static void rip$$append(struct rip$packet* p, uint32_t network, uint32_t mask, uint32_t metric)
{
    struct rip$entry* e = (struct rip$entry*)(p->data + RIP$HEADER_LEN + p->entries * RIP$ENTRY_LEN);
    e->afi = net_htons(RIP$AFI_INET);
    e->tag = 0;
    e->addr = net_htonl(network);
    e->mask = net_htonl(mask);
    e->next_hop = 0;
    e->metric = net_htonl(metric);
    if (++p->entries == RIP_ENTRIES_PER_PACKET) {
        rip$$flush(p);
    }
}

/* Our table as seen from dev: connected networks, then learned routes with split
   horizon and poisoned reverse. only_changed: just what changed since the last
   triggered update. */
static void rip$$send_table(struct netdev* dev, uint32_t dst, uint16_t dport, bool only_changed)
{
    struct rip$packet p = { .dev = dev, .dst = dst, .dport = dport, .entries = 0 };
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* d = netdev_get(i);
        if (!rip$$announced_iface(d) || (only_changed && !rip$iface_changed[i])) {
            continue;
        }
        uint32_t mask = ipv4_iface_netmask(d);
        rip$$append(&p, ipv4_iface_address(d) & mask, mask, d->link_up ? 1 : RIP_INFINITY);
    }
    for (int i = 0; i < RIP_MAX_ROUTES; i++) {
        struct rip$route* r = &rip$routes[i];
        if (!r->used || (only_changed && !r->changed)) {
            continue;
        }
        rip$$append(&p, r->network, r->mask, r->dev == dev ? RIP_INFINITY : r->metric);
    }
    rip$$flush(&p);
}

/* Ask the neighbours on dev for their whole table */
static void rip$$send_request(struct netdev* dev)
{
    uint8_t data[RIP$HEADER_LEN + RIP$ENTRY_LEN] = { RIP$CMD_REQUEST, RIP$VERSION, 0, 0 };
    struct rip$entry* e = (struct rip$entry*)(data + RIP$HEADER_LEN);
    e->afi = 0;
    e->tag = 0;
    e->addr = e->mask = e->next_hop = 0;
    e->metric = net_htonl(RIP_INFINITY);
    udp_send_dev(dev, ipv4_iface_address(dev), RIP_PORT, NET_IP_BROADCAST, RIP_PORT, data, sizeof(data));
}

static void rip$$send_all(bool only_changed)
{
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        if (rip$$port(dev) && dev->link_up) {
            rip$$send_table(dev, NET_IP_BROADCAST, RIP_PORT, only_changed);
            if (only_changed) {
                rip_stats.tx_triggered++;
            } else {
                rip_stats.tx_updates++;
            }
        }
    }
    for (int i = 0; i < RIP_MAX_ROUTES; i++) {
        rip$routes[i].changed = false;
    }
    for (int i = 0; i < NETDEV_MAX; i++) {
        rip$iface_changed[i] = false;
    }
    rip$trigger_pending = false;
}

/* ---- receiving -------------------------------------------------------------- */

static void rip$$response(struct netdev* dev, uint32_t src, const uint8_t* data, size_t len)
{
    uint32_t now = pit_uptime_ms();
    uint32_t iaddr = ipv4_iface_address(dev), imask = ipv4_iface_netmask(dev);
    /* Only from a neighbour on this port's network (RFC 2453 3.9.2) */
    if ((src & imask) != (iaddr & imask) || ipv4_is_own_address(src) || src == (iaddr | ~imask) || src == (iaddr & imask)) {
        rip_stats.rx_refused++;
        return;
    }
    rip_stats.rx_responses++;

    for (size_t off = RIP$HEADER_LEN; off + RIP$ENTRY_LEN <= len; off += RIP$ENTRY_LEN) {
        const struct rip$entry* e = (const struct rip$entry*)(data + off);
        uint16_t afi = net_ntohs(e->afi);
        uint32_t addr = net_ntohl(e->addr), mask = net_ntohl(e->mask);
        uint32_t next_hop = net_ntohl(e->next_hop), metric = net_ntohl(e->metric);
        if (afi != RIP$AFI_INET || metric < 1 || metric > RIP_INFINITY || !rip$$usable(addr, mask)) {
            rip_stats.rx_bad++;         /* includes authentication entries (AFI 0xFFFF): not supported */
            continue;
        }
        if (rip$$is_connected(addr, mask)) {
            continue;                   /* we know our own networks best */
        }
        /* RFC 2453 4.4: a next hop on this port's network may be used directly */
        uint32_t gateway = (next_hop && (next_hop & imask) == (iaddr & imask) && !ipv4_is_own_address(next_hop))
                           ? next_hop : src;
        metric = metric + 1 > RIP_INFINITY ? RIP_INFINITY : metric + 1;
        uint8_t prefix_len = ipv4_prefix_len(mask);

        struct rip$route* r = rip$$find(addr, prefix_len);
        if (!r) {
            if (metric == RIP_INFINITY) {
                continue;
            }
            for (int i = 0; i < RIP_MAX_ROUTES && !r; i++) {
                if (!rip$routes[i].used) {
                    r = &rip$routes[i];
                }
            }
            if (!r) {
                rip_stats.table_full++;
                continue;
            }
            *r = (struct rip$route){ .used = true, .network = addr, .mask = mask, .prefix_len = prefix_len,
                                     .gateway = gateway, .dev = dev, .metric = (uint8_t)metric, .updated_ms = now };
            rip$$install(r);
            rip$$changed(r, now);
            continue;
        }
        if (r->gateway == gateway && r->dev == dev) {
            /* The neighbour we route through: believe it, better or worse */
            if (metric == RIP_INFINITY) {
                rip$$unreachable(r, now);
                continue;
            }
            r->updated_ms = now;
            if (r->deleting || metric != r->metric) {
                r->deleting = false;
                r->metric = (uint8_t)metric;
                rip$$install(r);
                rip$$changed(r, now);
            }
        } else if (metric < r->metric) {
            r->gateway = gateway;
            r->dev = dev;
            r->metric = (uint8_t)metric;
            r->updated_ms = now;
            r->deleting = false;
            rip$$install(r);
            rip$$changed(r, now);
        }
    }
}

static void rip$$request(struct netdev* dev, uint32_t src, uint16_t sport, const uint8_t* data, size_t len)
{
    rip_stats.rx_requests++;
    const struct rip$entry* first = (const struct rip$entry*)(data + RIP$HEADER_LEN);
    if (len == RIP$HEADER_LEN + RIP$ENTRY_LEN && first->afi == 0 && net_ntohl(first->metric) == RIP_INFINITY) {
        rip$$send_table(dev, src, sport, false);        /* the whole table */
        return;
    }
    /* Specific entries: answer each with our metric (no split horizon, RFC 2453 3.9.1) */
    struct rip$packet p = { .dev = dev, .dst = src, .dport = sport, .entries = 0 };
    for (size_t off = RIP$HEADER_LEN; off + RIP$ENTRY_LEN <= len; off += RIP$ENTRY_LEN) {
        const struct rip$entry* e = (const struct rip$entry*)(data + off);
        uint32_t addr = net_ntohl(e->addr), mask = net_ntohl(e->mask);
        if (net_ntohs(e->afi) != RIP$AFI_INET || !ipv4_mask_valid(mask)) {
            rip_stats.rx_bad++;
            continue;
        }
        const struct rip$route* r = rip$$find(addr & mask, ipv4_prefix_len(mask));
        uint32_t metric = rip$$is_connected(addr & mask, mask) ? 1 : r ? r->metric : RIP_INFINITY;
        rip$$append(&p, addr & mask, mask, metric);
    }
    rip$$flush(&p);
}

static void rip$$receive(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len)
{
    (void)dst_ip;
    (void)dst_port;
    if (!rip$$port(dev) || !dev->link_up) {
        rip_stats.rx_refused++;         /* not a RIP port, or a NAT outside port */
        return;
    }
    if (len < RIP$HEADER_LEN + RIP$ENTRY_LEN || (len - RIP$HEADER_LEN) % RIP$ENTRY_LEN != 0 ||
        len > RIP$PACKET_MAX || data[1] != RIP$VERSION) {
        rip_stats.rx_bad++;
        return;
    }
    if (data[0] == RIP$CMD_REQUEST) {
        rip$$request(dev, src_ip, src_port, data, len);
    } else if (data[0] == RIP$CMD_RESPONSE && src_port == RIP_PORT) {
        rip$$response(dev, src_ip, data, len);
    } else {
        rip_stats.rx_bad++;
    }
}

/* ---- timers ----------------------------------------------------------------- */

void rip_tick(uint32_t now_ms)
{
    if (!rip$running) {
        return;
    }
    /* Links that went down or came back up */
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        if (dev->link_up == rip$was_up[i]) {
            continue;
        }
        rip$was_up[i] = dev->link_up;
        rip$iface_changed[i] = true;
        rip$$schedule_trigger(now_ms);
        char text[EVENTS_TEXT_LEN];
        struct strbuf sb;
        sb_init(&sb, text, sizeof(text));
        sb_str(&sb, dev->name);
        sb_str(&sb, dev->link_up ? " link up" : " link down");
        events_add("rip", text);
        if (!dev->link_up) {
            for (int r = 0; r < RIP_MAX_ROUTES; r++) {
                if (rip$routes[r].used && rip$routes[r].dev == dev) {
                    rip$$unreachable(&rip$routes[r], now_ms);
                }
            }
        } else if (rip$$port(dev)) {
            rip$$send_request(dev);
        }
    }
    /* Timeouts and garbage collection (RFC 2453 3.8) */
    for (int i = 0; i < RIP_MAX_ROUTES; i++) {
        struct rip$route* r = &rip$routes[i];
        if (!r->used) {
            continue;
        }
        if (r->deleting) {
            if (rip$$reached(now_ms, r->delete_at_ms)) {
                r->used = false;
            }
        } else if (now_ms - r->updated_ms >= rip$timeout_ms) {
            rip$$unreachable(r, now_ms);
        }
    }
    if (rip$$reached(now_ms, rip$next_update_ms)) {
        uint32_t jitter = rip$update_ms / 6 < 5000 ? rip$update_ms / 6 : 5000;
        rip$next_update_ms = now_ms + rip$update_ms + (jitter ? random32() % jitter : 0);
        rip$$send_all(false);
    } else if (rip$trigger_pending && rip$$reached(now_ms, rip$trigger_at_ms)) {
        rip$$send_all(true);
    }
}

/* ---- start and status ------------------------------------------------------- */

static bool rip$$parse_seconds(const char** p, uint32_t* ms)
{
    uint32_t v = 0;
    const char* s = *p;
    if (*s < '0' || *s > '9') {
        return false;
    }
    while (*s >= '0' && *s <= '9' && v <= 3600) {
        v = v * 10 + (uint32_t)(*s++ - '0');
    }
    if (v < 1 || v > 3600) {
        return false;
    }
    *ms = v * 1000;
    *p = s;
    return true;
}

void rip_start(void)
{
    char value[96];
    bool all = cmdline_has("rip");
    if (!all && !cmdline_get("rip", 0, value, sizeof(value))) {
        return;
    }
    for (size_t i = 0; i < netdev_count(); i++) {
        rip$enabled[i] = all && !nat_is_outside(netdev_get(i));
    }
    for (size_t n = 0; !all && cmdline_get("rip", n, value, sizeof(value)); n++) {
        char* name = value;
        while (*name) {
            char* end = name;
            while (*end && *end != ',') {
                end++;
            }
            char saved = *end;
            *end = '\0';
            int i = rip$$index(netdev_find(name));
            if (i < 0 || nat_is_outside(netdev_get((size_t)i))) {
                k_print("[K_FAIL] rip: %s is not a port that can speak RIP\n", name);
            } else {
                rip$enabled[i] = true;
            }
            name = saved ? end + 1 : end;
        }
    }
    if (cmdline_get("rip_timers", 0, value, sizeof(value))) {
        const char* p = value;
        uint32_t u, t, g;
        if (rip$$parse_seconds(&p, &u) && *p++ == ',' && rip$$parse_seconds(&p, &t) && *p++ == ',' &&
            rip$$parse_seconds(&p, &g) && *p == '\0' && t > u) {
            rip$update_ms = u;
            rip$timeout_ms = t;
            rip$garbage_ms = g;
        } else {
            k_print("[K_FAIL] rip_timers=%s: expected update,timeout,garbage seconds (timeout > update)\n", value);
        }
    }
    if (!udp_bind(RIP_PORT, rip$$receive)) {
        k_print("[K_FAIL] rip: UDP port 520 is taken\n");
        return;
    }
    rip$running = true;
    uint32_t now = pit_uptime_ms();
    k_print("[K_INFO] rip: speaking RIPv2 on");
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        rip$was_up[i] = dev->link_up;
        if (rip$enabled[i]) {
            k_print(" %s", dev->name);
            if (rip$$port(dev) && dev->link_up) {
                rip$$send_request(dev);         /* RFC 2453 3.9.1: ask the neighbours first */
            }
        }
    }
    k_print(" (timers %u/%u/%u s)\n", rip$update_ms / 1000, rip$timeout_ms / 1000, rip$garbage_ms / 1000);
    rip$next_update_ms = now + 1000;
}

bool rip_running(void)
{
    return rip$running;
}

void rip_json(struct strbuf* sb)
{
    uint32_t now = pit_uptime_ms();
    sb_char(sb, '{');
    sb_json_key(sb, "ports", true);
    sb_char(sb, '[');
    bool first = true;
    for (size_t i = 0; i < netdev_count(); i++) {
        if (rip$enabled[i]) {
            if (!first) {
                sb_char(sb, ',');
            }
            first = false;
            sb_json_str(sb, netdev_get(i)->name);
        }
    }
    sb_char(sb, ']');
    const struct { const char* name; uint32_t value; } counters[] = {
        { "update_s", rip$update_ms / 1000 }, { "timeout_s", rip$timeout_ms / 1000 },
        { "garbage_s", rip$garbage_ms / 1000 },
        { "rx_responses", rip_stats.rx_responses }, { "rx_requests", rip_stats.rx_requests },
        { "rx_bad", rip_stats.rx_bad }, { "rx_refused", rip_stats.rx_refused },
        { "tx_updates", rip_stats.tx_updates }, { "tx_triggered", rip_stats.tx_triggered },
        { "route_changes", rip_stats.route_changes }, { "table_full", rip_stats.table_full },
    };
    for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
        sb_json_key(sb, counters[i].name, false);
        sb_uint(sb, counters[i].value);
    }
    sb_json_key(sb, "routes", false);
    sb_char(sb, '[');
    first = true;
    for (int i = 0; i < RIP_MAX_ROUTES; i++) {
        const struct rip$route* r = &rip$routes[i];
        if (!r->used) {
            continue;
        }
        if (!first) {
            sb_char(sb, ',');
        }
        first = false;
        sb_char(sb, '{');
        sb_json_key(sb, "network", true);
        sb_char(sb, '"');
        sb_ip(sb, r->network);
        sb_char(sb, '/');
        sb_uint(sb, r->prefix_len);
        sb_char(sb, '"');
        sb_json_key(sb, "gateway", false);
        sb_char(sb, '"');
        sb_ip(sb, r->gateway);
        sb_char(sb, '"');
        sb_json_key(sb, "port", false);
        sb_json_str(sb, r->dev->name);
        sb_json_key(sb, "metric", false);
        sb_uint(sb, r->metric);
        sb_json_key(sb, "age_s", false);
        sb_uint(sb, (now - r->updated_ms) / 1000);
        sb_json_key(sb, "state", false);
        sb_json_str(sb, r->deleting ? "unreachable" : "up");
        sb_char(sb, '}');
    }
    sb_str(sb, "]}");
}
