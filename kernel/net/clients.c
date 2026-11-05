#include "clients.h"
#include "ipv4.h"
#include "arp.h"
#include "dhcp.h"
#include "events.h"
#include <pit.h>

struct clients$entry
{
    bool used;
    bool blocked;
    uint32_t ip;
    struct netdev* dev;                 /* NULL until traffic was seen */
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t up_bytes, down_bytes;
    uint32_t up_packets, down_packets;
};

static struct clients$entry clients$table[CLIENTS_MAX];

struct clients_stats clients_stats;

static struct clients$entry* clients$$find(uint32_t ip)
{
    for (int i = 0; i < CLIENTS_MAX; i++) {
        if (clients$table[i].used && clients$table[i].ip == ip) {
            return &clients$table[i];
        }
    }
    return NULL;
}

/* A free entry, else the least recently seen unblocked one */
static struct clients$entry* clients$$allocate(uint32_t ip)
{
    struct clients$entry* victim = NULL;
    for (int i = 0; i < CLIENTS_MAX; i++) {
        struct clients$entry* e = &clients$table[i];
        if (!e->used) {
            victim = e;
            break;
        }
        if (!e->blocked && (!victim || (int32_t)(e->last_seen_ms - victim->last_seen_ms) < 0)) {
            victim = e;
        }
    }
    if (!victim) {
        return NULL;
    }
    uint8_t* zero = (uint8_t*)victim;
    for (size_t i = 0; i < sizeof(*victim); i++) {
        zero[i] = 0;
    }
    victim->used = true;
    victim->ip = ip;
    victim->first_seen_ms = victim->last_seen_ms = pit_uptime_ms();
    return victim;
}

void clients_account(struct netdev* dev, uint32_t ip, size_t up_bytes, size_t down_bytes)
{
    /* Only hosts directly on the port's subnet, not networks routed behind it */
    uint32_t mask = ipv4_iface_netmask(dev);
    uint32_t addr = ipv4_iface_address(dev);
    if (addr == 0 || (ip & mask) != (addr & mask)) {
        return;
    }

    struct clients$entry* e = clients$$find(ip);
    if (!e) {
        e = clients$$allocate(ip);
        if (!e) {
            return;
        }
    }
    if (e->dev == NULL) {
        e->dev = dev;
        char text[EVENTS_TEXT_LEN];
        struct strbuf sb;
        sb_init(&sb, text, sizeof(text));
        sb_str(&sb, "new device ");
        sb_ip(&sb, ip);
        sb_str(&sb, " on ");
        sb_str(&sb, dev->name);
        events_add("client", text);
    }
    e->last_seen_ms = pit_uptime_ms();
    if (up_bytes) {
        e->up_bytes += up_bytes;
        e->up_packets++;
    }
    if (down_bytes) {
        e->down_bytes += down_bytes;
        e->down_packets++;
    }
}

bool clients_is_blocked(uint32_t ip)
{
    struct clients$entry* e = clients$$find(ip);
    return e && e->blocked;
}

bool clients_set_blocked(uint32_t ip, bool blocked)
{
    struct clients$entry* e = clients$$find(ip);
    if (!e) {
        if (!blocked) {
            return true;                /* nothing to unblock */
        }
        e = clients$$allocate(ip);
        if (!e) {
            return false;
        }
    }
    if (e->blocked != blocked) {
        e->blocked = blocked;
        char text[EVENTS_TEXT_LEN];
        struct strbuf sb;
        sb_init(&sb, text, sizeof(text));
        sb_str(&sb, blocked ? "blocked " : "unblocked ");
        sb_ip(&sb, ip);
        events_add("firewall", text);
    }
    return true;
}

void clients_json(struct strbuf* sb)
{
    uint32_t now = pit_uptime_ms();
    bool first = true;
    sb_char(sb, '[');
    for (int i = 0; i < CLIENTS_MAX; i++) {
        struct clients$entry* e = &clients$table[i];
        if (!e->used) {
            continue;
        }
        uint8_t mac[ETH_ALEN];
        char hostname[DHCP_HOSTNAME_MAX + 1] = "";
        uint32_t lease_s = 0;
        bool has_mac = e->dev && arp_lookup(e->dev, e->ip, mac);
        bool leased = dhcp_server_lease_info(e->ip, has_mac ? NULL : mac, hostname, sizeof(hostname), &lease_s);

        if (!first) {
            sb_char(sb, ',');
        }
        first = false;
        sb_char(sb, '{');
        sb_json_key(sb, "ip", true);
        sb_char(sb, '"');
        sb_ip(sb, e->ip);
        sb_char(sb, '"');
        sb_json_key(sb, "mac", false);
        if (has_mac || leased) {
            sb_char(sb, '"');
            sb_mac(sb, mac);
            sb_char(sb, '"');
        } else {
            sb_str(sb, "null");
        }
        sb_json_key(sb, "hostname", false);
        sb_json_str(sb, hostname);
        sb_json_key(sb, "port", false);
        sb_json_str(sb, e->dev ? e->dev->name : "");
        sb_json_key(sb, "blocked", false);
        sb_str(sb, e->blocked ? "true" : "false");
        sb_json_key(sb, "up_bytes", false);
        sb_uint(sb, e->up_bytes);
        sb_json_key(sb, "down_bytes", false);
        sb_uint(sb, e->down_bytes);
        sb_json_key(sb, "up_packets", false);
        sb_uint(sb, e->up_packets);
        sb_json_key(sb, "down_packets", false);
        sb_uint(sb, e->down_packets);
        sb_json_key(sb, "last_seen_s", false);
        sb_uint(sb, (now - e->last_seen_ms) / 1000);
        sb_json_key(sb, "lease_s", false);
        if (leased) {
            sb_uint(sb, lease_s);
        } else {
            sb_str(sb, "null");
        }
        sb_char(sb, '}');
    }
    sb_char(sb, ']');
}
