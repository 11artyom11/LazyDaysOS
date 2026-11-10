#include "dhcp.h"
#include "net.h"
#include "ipv4.h"
#include "udp.h"
#include <kio.h>
#include <pit.h>
#include <strbuf.h>
#include "events.h"
#include "clients.h"

/*
    RFC 2131 server for directly attached clients (no relay agents). Each pool
    belongs to one interface; the interface's address is the server identifier
    and the router handed to clients.

    Replies are broadcast unless the client already has an address (ciaddr):
    the RFC allows that, every client accepts it, and it avoids sending to a
    MAC whose IP isn't configured yet. "Has an address" means the request also
    came from ciaddr: some clients (GNS3's VPCS) fill in ciaddr while still
    sending from 0.0.0.0, and would never answer the ARP for a unicast ACK.
*/

#define DHCPS_MAX_POOLS         4
#define DHCPS_MAX_LEASES        64
#define DHCPS_OFFER_HOLD_MS     30000   /* an unanswered offer is released after this */
#define DHCPS_DECLINE_HOLD_MS   (10 * 60 * 1000)
#define DHCPS_DEFAULT_LEASE_S   3600

enum dhcps$state { DHCPS$FREE = 0, DHCPS$OFFERED, DHCPS$BOUND, DHCPS$DECLINED };

struct dhcps$pool
{
    struct netdev* dev;                 /* NULL = unused */
    uint32_t first;
    uint32_t last;
};

struct dhcps$lease
{
    enum dhcps$state state;
    struct dhcps$pool* pool;
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    uint32_t expires_ms;
    char hostname[DHCP_HOSTNAME_MAX + 1];
};

static struct dhcps$pool dhcps$pools[DHCPS_MAX_POOLS];
static struct dhcps$lease dhcps$leases[DHCPS_MAX_LEASES];
static uint32_t dhcps$dns = 0;
static uint32_t dhcps$lease_s = DHCPS_DEFAULT_LEASE_S;
static bool dhcps$bound_port = false;

static bool dhcps$$mac_equal(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < ETH_ALEN; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static struct dhcps$lease* dhcps$$by_mac(const struct dhcps$pool* pool, const uint8_t* mac)
{
    for (int i = 0; i < DHCPS_MAX_LEASES; i++) {
        struct dhcps$lease* l = &dhcps$leases[i];
        if (l->state != DHCPS$FREE && l->state != DHCPS$DECLINED && l->pool == pool && dhcps$$mac_equal(l->mac, mac)) {
            return l;
        }
    }
    return NULL;
}

static struct dhcps$lease* dhcps$$by_ip(uint32_t ip)
{
    for (int i = 0; i < DHCPS_MAX_LEASES; i++) {
        if (dhcps$leases[i].state != DHCPS$FREE && dhcps$leases[i].ip == ip) {
            return &dhcps$leases[i];
        }
    }
    return NULL;
}

static bool dhcps$$in_pool(const struct dhcps$pool* pool, uint32_t ip)
{
    return ip >= pool->first && ip <= pool->last && ip != ipv4_iface_address(pool->dev);
}

static struct dhcps$lease* dhcps$$new_lease(struct dhcps$pool* pool, uint32_t ip, const uint8_t* mac)
{
    for (int i = 0; i < DHCPS_MAX_LEASES; i++) {
        struct dhcps$lease* l = &dhcps$leases[i];
        if (l->state == DHCPS$FREE) {
            l->hostname[0] = '\0';
            l->pool = pool;
            l->ip = ip;
            for (int b = 0; b < ETH_ALEN; b++) {
                l->mac[b] = mac[b];
            }
            return l;
        }
    }
    return NULL;
}

/* Lease to offer for a DISCOVER: the client's existing one, else the address it asks for, else the first free */
static struct dhcps$lease* dhcps$$choose(struct dhcps$pool* pool, const uint8_t* mac, uint32_t requested)
{
    struct dhcps$lease* lease = dhcps$$by_mac(pool, mac);
    if (lease) {
        return lease;
    }
    if (requested && dhcps$$in_pool(pool, requested) && !dhcps$$by_ip(requested)) {
        return dhcps$$new_lease(pool, requested, mac);
    }
    for (uint32_t ip = pool->first; ip <= pool->last && ip != 0; ip++) {
        if (dhcps$$in_pool(pool, ip) && !dhcps$$by_ip(ip)) {
            return dhcps$$new_lease(pool, ip, mac);
        }
    }
    return NULL;
}

/* src_ip is the IPv4 source of the request: unicast only to a client that already uses ciaddr */
static void dhcps$$reply(struct dhcps$pool* pool, const struct dhcp_message* req, uint32_t src_ip,
                         uint8_t type, uint32_t yiaddr)
{
    struct dhcp_message msg;
    size_t cursor = 0;
    uint32_t server = ipv4_iface_address(pool->dev);

    dhcp_init_message(&msg, DHCP_OP_REPLY, req->xid, req->chaddr);
    msg.flags = req->flags;
    msg.yiaddr = net_htonl(yiaddr);
    dhcp_put_u8(&msg, &cursor, DHCP_OPT_MESSAGE_TYPE, type);
    dhcp_put_u32(&msg, &cursor, DHCP_OPT_SERVER_ID, server);

    if (type != DHCPNAK) {
        uint32_t dns = dhcps$dns ? dhcps$dns : dhcp_client_dns();
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_LEASE_TIME, dhcps$lease_s);
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_RENEWAL_T1, dhcps$lease_s / 2);
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_REBINDING_T2, dhcps$lease_s / 8 * 7);
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_SUBNET_MASK, ipv4_iface_netmask(pool->dev));
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_ROUTER, server);
        if (dns) {
            dhcp_put_u32(&msg, &cursor, DHCP_OPT_DNS, dns);
        }
    }
    size_t len = dhcp_finish(&msg, cursor);

    uint32_t ciaddr = net_ntohl(req->ciaddr);
    if (ciaddr && ciaddr == src_ip && type != DHCPNAK) {
        udp_send(server, UDP_PORT_DHCP_SERVER, ciaddr, UDP_PORT_DHCP_CLIENT, &msg, len);
    } else {
        udp_send_dev(pool->dev, server, UDP_PORT_DHCP_SERVER, NET_IP_BROADCAST, UDP_PORT_DHCP_CLIENT, &msg, len);
    }
}

/* Remember the client's hostname (option 12), keeping only printable ASCII */
static void dhcps$$save_hostname(struct dhcps$lease* lease, const struct dhcp_message* msg, size_t len)
{
    uint8_t name_len;
    const uint8_t* name = dhcp_find_option(msg, len, DHCP_OPT_HOSTNAME, &name_len);
    if (!name) {
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < name_len && n < DHCP_HOSTNAME_MAX; i++) {
        if (name[i] >= 0x20 && name[i] < 0x7F) {
            lease->hostname[n++] = (char)name[i];
        }
    }
    lease->hostname[n] = '\0';
}

static void dhcps$$bind_lease(struct dhcps$pool* pool, struct dhcps$lease* lease,
                              const struct dhcp_message* msg, size_t len, uint32_t now)
{
    bool renewal = lease->state == DHCPS$BOUND;
    lease->state = DHCPS$BOUND;
    lease->expires_ms = now + dhcps$lease_s * 1000;
    dhcps$$save_hostname(lease, msg, len);
    if (!renewal) {
        char text[EVENTS_TEXT_LEN];
        struct strbuf sb;
        sb_init(&sb, text, sizeof(text));
        sb_ip(&sb, lease->ip);
        sb_str(&sb, " leased to ");
        sb_mac(&sb, lease->mac);
        if (lease->hostname[0]) {
            sb_str(&sb, " (");
            sb_str(&sb, lease->hostname);
            sb_char(&sb, ')');
        }
        events_add("dhcp", text);
        clients_account(pool->dev, lease->ip, 0, 0);
    }
}

static void dhcps$$handle_request(struct dhcps$pool* pool, const struct dhcp_message* msg, size_t len,
                                  uint32_t src_ip, uint32_t now)
{
    uint32_t requested = 0, server_id = 0;
    uint32_t ciaddr = net_ntohl(msg->ciaddr);
    bool has_server_id = dhcp_get_u32(msg, len, DHCP_OPT_SERVER_ID, &server_id);
    dhcp_get_u32(msg, len, DHCP_OPT_REQUESTED_IP, &requested);
    struct dhcps$lease* lease = dhcps$$by_mac(pool, msg->chaddr);

    if (has_server_id) {
        /* SELECTING: the client picked a server; if not us, our offer is moot */
        if (server_id != ipv4_iface_address(pool->dev)) {
            if (lease && lease->state == DHCPS$OFFERED) {
                lease->state = DHCPS$FREE;
            }
            return;
        }
        if (lease && lease->ip == requested) {
            dhcps$$bind_lease(pool, lease, msg, len, now);
            dhcp_stats.server_acks++;
            dhcps$$reply(pool, msg, src_ip, DHCPACK, lease->ip);
        } else {
            dhcp_stats.server_naks++;
            dhcps$$reply(pool, msg, src_ip, DHCPNAK, 0);
        }
        return;
    }

    /* INIT-REBOOT (requested, no ciaddr) or RENEWING/REBINDING (ciaddr) */
    uint32_t ip = ciaddr ? ciaddr : requested;
    if (ip == 0) {
        dhcp_stats.invalid++;
        return;
    }
    uint32_t mask = ipv4_iface_netmask(pool->dev);
    bool on_our_net = (ip & mask) == (ipv4_iface_address(pool->dev) & mask);

    if (lease && lease->ip == ip) {
        dhcps$$bind_lease(pool, lease, msg, len, now);
        dhcp_stats.server_acks++;
        dhcps$$reply(pool, msg, src_ip, DHCPACK, ip);
        return;
    }
    if (!lease && dhcps$$in_pool(pool, ip) && !dhcps$$by_ip(ip)) {
        /* A client we forgot (e.g. after a reboot): give it back its address */
        lease = dhcps$$new_lease(pool, ip, msg->chaddr);
        if (lease) {
            dhcps$$bind_lease(pool, lease, msg, len, now);
            dhcp_stats.server_acks++;
            dhcps$$reply(pool, msg, src_ip, DHCPACK, ip);
            return;
        }
    }
    /* Wrong network, someone else's address, or not ours to give: NAK so the
       client restarts. When ciaddr is set (renewing) stay silent instead if
       the address isn't from our network - another server owns it. */
    if (on_our_net || !ciaddr) {
        dhcp_stats.server_naks++;
        dhcps$$reply(pool, msg, src_ip, DHCPNAK, 0);
    }
}

static void dhcps$$receive(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len)
{
    (void)src_port; (void)dst_ip; (void)dst_port;
    const struct dhcp_message* msg = (const struct dhcp_message*)data;
    struct dhcps$pool* pool = NULL;
    uint8_t type;

    for (int i = 0; i < DHCPS_MAX_POOLS; i++) {
        if (dhcps$pools[i].dev == dev) {
            pool = &dhcps$pools[i];
        }
    }
    if (!pool || !dhcp_valid(msg, len, DHCP_OP_REQUEST) || msg->giaddr != 0 ||
        !dhcp_get_u8(msg, len, DHCP_OPT_MESSAGE_TYPE, &type)) {
        return;
    }

    uint32_t now = pit_uptime_ms();
    struct dhcps$lease* lease;
    uint32_t requested = 0;

    switch (type) {
    case DHCPDISCOVER:
        dhcp_get_u32(msg, len, DHCP_OPT_REQUESTED_IP, &requested);
        lease = dhcps$$choose(pool, msg->chaddr, requested);
        if (!lease) {
            dhcp_stats.server_pool_exhausted++;
            return;
        }
        if (lease->state != DHCPS$BOUND) {
            lease->state = DHCPS$OFFERED;
            lease->expires_ms = now + DHCPS_OFFER_HOLD_MS;
        }
        dhcp_stats.server_offers++;
        dhcps$$reply(pool, msg, src_ip, DHCPOFFER, lease->ip);
        break;
    case DHCPREQUEST:
        dhcps$$handle_request(pool, msg, len, src_ip, now);
        break;
    case DHCPRELEASE:
        lease = dhcps$$by_mac(pool, msg->chaddr);
        if (lease && lease->ip == net_ntohl(msg->ciaddr)) {
            lease->state = DHCPS$FREE;
            dhcp_stats.server_releases++;
        }
        break;
    case DHCPDECLINE:
        /* The client found the address in use: keep it out of circulation for a while */
        if (dhcp_get_u32(msg, len, DHCP_OPT_REQUESTED_IP, &requested) &&
            (lease = dhcps$$by_ip(requested)) != NULL && dhcps$$mac_equal(lease->mac, msg->chaddr)) {
            lease->state = DHCPS$DECLINED;
            lease->expires_ms = now + DHCPS_DECLINE_HOLD_MS;
            dhcp_stats.server_declines++;
        }
        break;
    default:
        break;                          /* INFORM and others: not supported */
    }
}

bool dhcp_server_add_pool(struct netdev* dev, uint32_t first, uint32_t last)
{
    uint32_t addr = ipv4_iface_address(dev);
    uint32_t mask = ipv4_iface_netmask(dev);
    if (addr == 0 || first > last || (first & mask) != (addr & mask) || (last & mask) != (addr & mask) ||
        first == (addr & mask) || last == (addr | ~mask)) {
        return false;                   /* must lie inside the interface's subnet, not network/broadcast */
    }
    if (!dhcps$bound_port) {
        dhcps$bound_port = udp_bind(UDP_PORT_DHCP_SERVER, dhcps$$receive);
        if (!dhcps$bound_port) {
            return false;
        }
    }
    for (int i = 0; i < DHCPS_MAX_POOLS; i++) {
        if (dhcps$pools[i].dev == NULL || dhcps$pools[i].dev == dev) {
            dhcps$pools[i].dev = dev;
            dhcps$pools[i].first = first;
            dhcps$pools[i].last = last;
            return true;
        }
    }
    return false;
}

void dhcp_server_set_dns(uint32_t dns)
{
    dhcps$dns = dns;
}

void dhcp_server_set_lease_time(uint32_t seconds)
{
    dhcps$lease_s = seconds;
}

bool dhcp_server_lease_info(uint32_t ip, uint8_t mac[ETH_ALEN], char* hostname, size_t hostname_cap,
                            uint32_t* remaining_s)
{
    struct dhcps$lease* l = dhcps$$by_ip(ip);
    if (!l || l->state != DHCPS$BOUND) {
        return false;
    }
    if (mac) {
        for (int i = 0; i < ETH_ALEN; i++) {
            mac[i] = l->mac[i];
        }
    }
    size_t n = 0;
    for (; l->hostname[n] && n + 1 < hostname_cap; n++) {
        hostname[n] = l->hostname[n];
    }
    if (hostname_cap) {
        hostname[n] = '\0';
    }
    *remaining_s = (l->expires_ms - pit_uptime_ms()) / 1000;
    return true;
}

bool dhcp_server_has_pool(const struct netdev* dev)
{
    for (int i = 0; i < DHCPS_MAX_POOLS; i++) {
        if (dhcps$pools[i].dev == dev) {
            return true;
        }
    }
    return false;
}

size_t dhcp_server_lease_count(void)
{
    size_t n = 0;
    for (int i = 0; i < DHCPS_MAX_LEASES; i++) {
        n += dhcps$leases[i].state == DHCPS$BOUND;
    }
    return n;
}

void dhcp_server_tick(uint32_t now_ms)
{
    for (int i = 0; i < DHCPS_MAX_LEASES; i++) {
        struct dhcps$lease* l = &dhcps$leases[i];
        if (l->state != DHCPS$FREE && (int32_t)(now_ms - l->expires_ms) >= 0) {
            l->state = DHCPS$FREE;
        }
    }
}
