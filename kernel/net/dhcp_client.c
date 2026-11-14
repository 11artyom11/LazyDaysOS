#include "dhcp.h"
#include "net.h"
#include "ipv4.h"
#include "udp.h"
#include <kio.h>
#include "../random.h"
#include <pit.h>
#include <strbuf.h>
#include "events.h"

/*
    RFC 2131 client state machine, one instance per interface:

    INIT --DISCOVER--> SELECTING --OFFER/REQUEST--> REQUESTING --ACK--> BOUND
      ^                                               |                   | T1
      +----------------------- NAK / give up ---------+          RENEWING (unicast)
      +------------------------------ lease expired ---------- REBINDING (broadcast) <- T2

    Requests ask for broadcast replies: until the lease is applied the stack would
    treat a unicast to the offered address as someone else's packet.
*/

#define DHCPC_MAX               NETDEV_MAX
#define DHCPC_FIRST_RETRY_MS    2000
#define DHCPC_MAX_RETRY_MS      16000
#define DHCPC_REQUEST_TRIES     4       /* REQUESTING gives up and restarts after this */
#define DHCPC_MIN_RENEW_RETRY_MS 2000
#define DHCPC_DEFAULT_LEASE_S   3600

enum dhcpc$state { DHCPC$INIT, DHCPC$SELECTING, DHCPC$REQUESTING, DHCPC$BOUND, DHCPC$RENEWING, DHCPC$REBINDING };

struct dhcpc$client
{
    struct netdev* dev;                 /* NULL = slot unused */
    enum dhcpc$state state;
    uint32_t xid;
    uint32_t offered_ip;
    uint32_t server_id;
    uint32_t next_send_ms;
    uint32_t retry_ms;
    uint8_t tries;

    /* Current lease */
    uint32_t ip;
    uint32_t mask;
    uint32_t router;
    uint32_t dns;
    uint32_t lease_start_ms;
    uint32_t t1_s, t2_s, lease_s;
};

static struct dhcpc$client dhcpc$clients[DHCPC_MAX];
static bool dhcpc$bound_port = false;

static bool dhcpc$$reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint8_t dhcpc$$prefix_len(uint32_t mask)
{
    uint8_t len = 0;
    while (mask & 0x80000000) {
        len++;
        mask <<= 1;
    }
    return len;
}

/* Common options of DISCOVER and REQUEST */
static void dhcpc$$identify(struct dhcpc$client* c, struct dhcp_message* msg, size_t* cursor)
{
    static const uint8_t params[] = {
        DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_LEASE_TIME,
        DHCP_OPT_SERVER_ID, DHCP_OPT_RENEWAL_T1, DHCP_OPT_REBINDING_T2
    };
    uint8_t client_id[1 + ETH_ALEN] = {1};
    for (int i = 0; i < ETH_ALEN; i++) {
        client_id[1 + i] = c->dev->mac[i];
    }
    dhcp_put_option(msg, cursor, DHCP_OPT_CLIENT_ID, sizeof(client_id), client_id);
    dhcp_put_option(msg, cursor, DHCP_OPT_PARAM_LIST, sizeof(params), params);
    dhcp_put_option(msg, cursor, DHCP_OPT_HOSTNAME, 10, "lazydaysos");
}

static void dhcpc$$send_discover(struct dhcpc$client* c)
{
    struct dhcp_message msg;
    size_t cursor = 0;
    dhcp_init_message(&msg, DHCP_OP_REQUEST, c->xid, c->dev->mac);
    msg.flags = net_htons(DHCP_FLAG_BROADCAST);
    dhcp_put_u8(&msg, &cursor, DHCP_OPT_MESSAGE_TYPE, DHCPDISCOVER);
    dhcpc$$identify(c, &msg, &cursor);
    size_t len = dhcp_finish(&msg, cursor);
    udp_send_dev(c->dev, 0, UDP_PORT_DHCP_CLIENT, NET_IP_BROADCAST, UDP_PORT_DHCP_SERVER, &msg, len);
}

static void dhcpc$$send_request(struct dhcpc$client* c)
{
    struct dhcp_message msg;
    size_t cursor = 0;
    dhcp_init_message(&msg, DHCP_OP_REQUEST, c->xid, c->dev->mac);
    dhcp_put_u8(&msg, &cursor, DHCP_OPT_MESSAGE_TYPE, DHCPREQUEST);

    if (c->state == DHCPC$REQUESTING) {
        /* Answering an offer: name the address and the server we picked */
        msg.flags = net_htons(DHCP_FLAG_BROADCAST);
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_REQUESTED_IP, c->offered_ip);
        dhcp_put_u32(&msg, &cursor, DHCP_OPT_SERVER_ID, c->server_id);
    } else {
        /* Renewing or rebinding an existing lease: it is identified by ciaddr */
        msg.ciaddr = net_htonl(c->ip);
    }
    dhcpc$$identify(c, &msg, &cursor);
    size_t len = dhcp_finish(&msg, cursor);

    if (c->state == DHCPC$RENEWING) {
        udp_send(c->ip, UDP_PORT_DHCP_CLIENT, c->server_id, UDP_PORT_DHCP_SERVER, &msg, len);
    } else {
        uint32_t src = c->state == DHCPC$REBINDING ? c->ip : 0;
        udp_send_dev(c->dev, src, UDP_PORT_DHCP_CLIENT, NET_IP_BROADCAST, UDP_PORT_DHCP_SERVER, &msg, len);
    }
}

static void dhcpc$$enter(struct dhcpc$client* c, enum dhcpc$state state, uint32_t now)
{
    c->state = state;
    c->tries = 0;
    c->retry_ms = DHCPC_FIRST_RETRY_MS;
    c->next_send_ms = now;              /* act on the next tick */
    if (state == DHCPC$INIT || state == DHCPC$SELECTING) {
        c->xid = random32();
    }
}

static void dhcpc$$drop_lease(struct dhcpc$client* c)
{
    if (c->ip == 0) {
        return;
    }
    k_print("[K_INFO] dhcp: %s lost lease on ", c->dev->name);
    net_print_ip(c->ip);
    k_print("\n");
    if (c->router) {
        ipv4_remove_route(0, 0);
    }
    ipv4_clear_address(c->dev);
    c->ip = 0;
    c->router = 0;
    c->dns = 0;
}

static void dhcpc$$apply_ack(struct dhcpc$client* c, const struct dhcp_message* msg, size_t len, uint32_t now)
{
    uint32_t ip = net_ntohl(msg->yiaddr);
    uint32_t mask = 0, router = 0, dns = 0, lease = DHCPC_DEFAULT_LEASE_S, t1, t2, server;

    dhcp_get_u32(msg, len, DHCP_OPT_SUBNET_MASK, &mask);
    dhcp_get_u32(msg, len, DHCP_OPT_ROUTER, &router);
    dhcp_get_u32(msg, len, DHCP_OPT_DNS, &dns);
    dhcp_get_u32(msg, len, DHCP_OPT_LEASE_TIME, &lease);
    if (!dhcp_get_u32(msg, len, DHCP_OPT_RENEWAL_T1, &t1)) {
        t1 = lease / 2;
    }
    if (!dhcp_get_u32(msg, len, DHCP_OPT_REBINDING_T2, &t2)) {
        t2 = lease / 8 * 7;
    }
    if (dhcp_get_u32(msg, len, DHCP_OPT_SERVER_ID, &server)) {
        c->server_id = server;
    }
    if (mask == 0) {
        mask = 0xFFFFFF00;              /* no mask option: assume /24 */
    }

    bool changed = ip != c->ip || mask != c->mask || router != c->router;
    if (changed) {
        if (c->router) {
            ipv4_remove_route(0, 0);
        }
        ipv4_set_address(c->dev, ip, dhcpc$$prefix_len(mask));
        if (router && !ipv4_add_route(0, 0, router, c->dev)) {
            router = 0;
        }
    }

    c->ip = ip;
    c->mask = mask;
    c->router = router;
    c->dns = dns;
    c->lease_s = lease;
    c->t1_s = t1;
    c->t2_s = t2;
    c->lease_start_ms = now;
    dhcp_stats.client_leases++;
    dhcpc$$enter(c, DHCPC$BOUND, now);

    if (changed) {
        char text[EVENTS_TEXT_LEN];
        struct strbuf sb;
        sb_init(&sb, text, sizeof(text));
        sb_str(&sb, c->dev->name);
        sb_str(&sb, " got ");
        sb_ip(&sb, ip);
        sb_str(&sb, " from DHCP server ");
        sb_ip(&sb, c->server_id);
        events_add("wan", text);
        k_print("[K_INFO] dhcp: %s bound ", c->dev->name);
        net_print_ip(ip);
        k_print("/%u", dhcpc$$prefix_len(mask));
        if (router) {
            k_print(" router ");
            net_print_ip(router);
        }
        if (dns) {
            k_print(" dns ");
            net_print_ip(dns);
        }
        k_print(" lease %u s\n", lease);
    }
}

static void dhcpc$$receive(struct netdev* dev, uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port, const uint8_t* data, size_t len)
{
    (void)src_ip; (void)src_port; (void)dst_ip; (void)dst_port;
    const struct dhcp_message* msg = (const struct dhcp_message*)data;
    uint8_t type;

    if (!dhcp_valid(msg, len, DHCP_OP_REPLY) || !dhcp_get_u8(msg, len, DHCP_OPT_MESSAGE_TYPE, &type)) {
        return;
    }
    for (int i = 0; i < DHCPC_MAX; i++) {
        struct dhcpc$client* c = &dhcpc$clients[i];
        if (c->dev != dev || msg->xid != c->xid) {
            continue;
        }
        for (int b = 0; b < ETH_ALEN; b++) {
            if (msg->chaddr[b] != dev->mac[b]) {
                return;
            }
        }

        uint32_t now = pit_uptime_ms();
        if (type == DHCPOFFER && c->state == DHCPC$SELECTING) {
            uint32_t server;
            if (msg->yiaddr == 0 || !dhcp_get_u32(msg, len, DHCP_OPT_SERVER_ID, &server)) {
                dhcp_stats.invalid++;
                return;
            }
            /* Take the first offer */
            c->offered_ip = net_ntohl(msg->yiaddr);
            c->server_id = server;
            dhcpc$$enter(c, DHCPC$REQUESTING, now);
        } else if (type == DHCPACK && (c->state == DHCPC$REQUESTING || c->state == DHCPC$RENEWING ||
                                       c->state == DHCPC$REBINDING)) {
            dhcpc$$apply_ack(c, msg, len, now);
        } else if (type == DHCPNAK && (c->state == DHCPC$REQUESTING || c->state == DHCPC$RENEWING ||
                                       c->state == DHCPC$REBINDING)) {
            k_print("[K_INFO] dhcp: %s got NAK, starting over\n", dev->name);
            dhcp_stats.client_naks++;
            dhcpc$$drop_lease(c);
            dhcpc$$enter(c, DHCPC$INIT, now);
        }
        return;
    }
}

void dhcp_client_start(struct netdev* dev)
{
    if (!dhcpc$bound_port) {
        dhcpc$bound_port = udp_bind(UDP_PORT_DHCP_CLIENT, dhcpc$$receive);
        if (!dhcpc$bound_port) {
            k_print("[K_FAIL] dhcp: UDP port %u is taken\n", UDP_PORT_DHCP_CLIENT);
            return;
        }
    }
    for (int i = 0; i < DHCPC_MAX; i++) {
        if (dhcpc$clients[i].dev == NULL) {
            dhcpc$clients[i].dev = dev;
            dhcpc$$enter(&dhcpc$clients[i], DHCPC$INIT, pit_uptime_ms());
            k_print("[K_INFO] dhcp: client started on %s\n", dev->name);
            return;
        }
    }
}

/* Retransmit with exponential backoff while waiting in SELECTING/REQUESTING */
static void dhcpc$$backoff(struct dhcpc$client* c, uint32_t now)
{
    c->tries++;
    c->next_send_ms = now + c->retry_ms;
    if (c->retry_ms < DHCPC_MAX_RETRY_MS) {
        c->retry_ms *= 2;
    }
}

void dhcp_client_tick(uint32_t now_ms)
{
    for (int i = 0; i < DHCPC_MAX; i++) {
        struct dhcpc$client* c = &dhcpc$clients[i];
        if (c->dev == NULL) {
            continue;
        }
        uint32_t elapsed_s = (now_ms - c->lease_start_ms) / 1000;

        switch (c->state) {
        case DHCPC$INIT:
            dhcpc$$enter(c, DHCPC$SELECTING, now_ms);
            /* fall through: send the first DISCOVER right away */
        case DHCPC$SELECTING:
            if (dhcpc$$reached(now_ms, c->next_send_ms)) {
                dhcpc$$send_discover(c);
                dhcpc$$backoff(c, now_ms);
            }
            break;
        case DHCPC$REQUESTING:
            if (c->tries >= DHCPC_REQUEST_TRIES) {
                dhcpc$$enter(c, DHCPC$INIT, now_ms);
            } else if (dhcpc$$reached(now_ms, c->next_send_ms)) {
                dhcpc$$send_request(c);
                dhcpc$$backoff(c, now_ms);
            }
            break;
        case DHCPC$BOUND:
            if (elapsed_s >= c->t1_s) {
                dhcpc$$enter(c, DHCPC$RENEWING, now_ms);
            }
            break;
        case DHCPC$RENEWING:
        case DHCPC$REBINDING:
            if (elapsed_s >= c->lease_s) {
                dhcpc$$drop_lease(c);
                dhcpc$$enter(c, DHCPC$INIT, now_ms);
            } else if (c->state == DHCPC$RENEWING && elapsed_s >= c->t2_s) {
                dhcpc$$enter(c, DHCPC$REBINDING, now_ms);
            } else if (dhcpc$$reached(now_ms, c->next_send_ms)) {
                dhcpc$$send_request(c);
                /* RFC 2131 4.4.5: wait half the time left until T2 (or expiry), at least a bit */
                uint32_t limit_s = c->state == DHCPC$RENEWING ? c->t2_s : c->lease_s;
                uint32_t wait_ms = (limit_s - elapsed_s) * 500;
                c->next_send_ms = now_ms + (wait_ms < DHCPC_MIN_RENEW_RETRY_MS ? DHCPC_MIN_RENEW_RETRY_MS : wait_ms);
            }
            break;
        }
    }
}

bool dhcp_client_running(const struct netdev* dev)
{
    for (int i = 0; i < DHCPC_MAX; i++) {
        if (dhcpc$clients[i].dev == dev) {
            return true;
        }
    }
    return false;
}

bool dhcp_client_all_bound(void)
{
    for (int i = 0; i < DHCPC_MAX; i++) {
        if (dhcpc$clients[i].dev && dhcpc$clients[i].ip == 0) {
            return false;
        }
    }
    return true;
}

uint32_t dhcp_client_dns(void)
{
    for (int i = 0; i < DHCPC_MAX; i++) {
        if (dhcpc$clients[i].dev && dhcpc$clients[i].dns) {
            return dhcpc$clients[i].dns;
        }
    }
    return 0;
}
