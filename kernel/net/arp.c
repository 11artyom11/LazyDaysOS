#include "arp.h"
#include "eth.h"
#include "ipv4.h"
#include "net.h"
#include "icmp.h"
#include <kio.h>
#include <pit.h>
#include "../heap.h"

#define ARP_HTYPE_ETHERNET  1
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

struct arp_packet
{
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];              /* sender hardware address */
    uint32_t spa;                       /* sender protocol address */
    uint8_t tha[ETH_ALEN];              /* target hardware address */
    uint32_t tpa;                       /* target protocol address */
} __attribute__((packed));

enum arp$state { ARP$FREE = 0, ARP$INCOMPLETE, ARP$REACHABLE };

struct arp$entry
{
    enum arp$state state;
    uint32_t ip;
    struct netdev* dev;
    uint8_t mac[ETH_ALEN];
    uint32_t updated_ms;                /* last confirmation, or last request while incomplete */
    uint8_t requests;                   /* requests sent while incomplete */
    uint8_t* held;                      /* IPv4 packet waiting for resolution (kmalloc'd) */
    size_t held_len;
};

static struct arp$entry arp$table[ARP_TABLE_SIZE];

struct arp_stats arp_stats;

static struct arp$entry* arp$$find(const struct netdev* dev, uint32_t ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp$table[i].state != ARP$FREE && arp$table[i].ip == ip && arp$table[i].dev == dev) {
            return &arp$table[i];
        }
    }
    return NULL;
}

static void arp$$release(struct arp$entry* entry)
{
    kfree(entry->held);
    entry->held = NULL;
    entry->held_len = 0;
    entry->state = ARP$FREE;
}

/*
    A slot for a new entry; incomplete = for a resolution that is about to start.
    Unresolved entries are capped at ARP_MAX_INCOMPLETE and then replace each other,
    oldest first. Otherwise traffic towards many nonexistent neighbours (replies to
    a flood of spoofed sources) fills the table with unresolved entries, pushes out
    confirmed neighbours such as the default gateway, and leaves no room to resolve
    them again. A confirmed neighbour takes a free slot, else the oldest unresolved
    entry, else the least recently confirmed one.
*/
static struct arp$entry* arp$$allocate(bool incomplete)
{
    struct arp$entry* free_slot = NULL;
    struct arp$entry* oldest_incomplete = NULL;
    struct arp$entry* oldest_reachable = NULL;
    int incomplete_count = 0;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp$entry* e = &arp$table[i];
        if (e->state == ARP$FREE) {
            free_slot = free_slot ? free_slot : e;
        } else if (e->state == ARP$INCOMPLETE) {
            incomplete_count++;
            if (!oldest_incomplete || (int32_t)(e->updated_ms - oldest_incomplete->updated_ms) < 0) {
                oldest_incomplete = e;
            }
        } else if (!oldest_reachable || (int32_t)(e->updated_ms - oldest_reachable->updated_ms) < 0) {
            oldest_reachable = e;
        }
    }

    struct arp$entry* victim;
    if (incomplete && incomplete_count >= ARP_MAX_INCOMPLETE) {
        victim = oldest_incomplete;
    } else if (free_slot) {
        return free_slot;
    } else {
        victim = oldest_incomplete ? oldest_incomplete : oldest_reachable;
    }
    if (victim) {
        if (victim->state == ARP$INCOMPLETE) {
            arp_stats.incomplete_evicted++;
        }
        arp$$release(victim);
    }
    return victim;
}

static void arp$$send(struct netdev* dev, uint16_t op, const uint8_t dst_mac[ETH_ALEN],
                      const uint8_t target_mac[ETH_ALEN], uint32_t target_ip)
{
    struct arp_packet arp;
    arp.htype = net_htons(ARP_HTYPE_ETHERNET);
    arp.ptype = net_htons(ETHERTYPE_IPV4);
    arp.hlen = ETH_ALEN;
    arp.plen = 4;
    arp.op = net_htons(op);
    for (int i = 0; i < ETH_ALEN; i++) {
        arp.sha[i] = dev->mac[i];
        arp.tha[i] = target_mac[i];
    }
    arp.spa = net_htonl(ipv4_iface_address(dev));
    arp.tpa = net_htonl(target_ip);
    eth_send(dev, dst_mac, ETHERTYPE_ARP, &arp, sizeof(arp));
}

static void arp$$send_request(struct netdev* dev, uint32_t ip)
{
    static const uint8_t unknown[ETH_ALEN] = {0};
    arp$$send(dev, ARP_OP_REQUEST, eth_broadcast, unknown, ip);
    arp_stats.tx_requests++;
}

/* Record a confirmed mapping and release a held packet, if any */
static void arp$$resolved(struct arp$entry* entry, const uint8_t mac[ETH_ALEN])
{
    for (int i = 0; i < ETH_ALEN; i++) {
        entry->mac[i] = mac[i];
    }
    entry->state = ARP$REACHABLE;
    entry->updated_ms = pit_uptime_ms();
    if (entry->held) {
        eth_send(entry->dev, entry->mac, ETHERTYPE_IPV4, entry->held, entry->held_len);
        kfree(entry->held);
        entry->held = NULL;
        entry->held_len = 0;
    }
}

void arp_receive(struct netdev* dev, const uint8_t* payload, size_t len)
{
    const struct arp_packet* arp = (const struct arp_packet*)payload;
    if (len < sizeof(struct arp_packet) || net_ntohs(arp->htype) != ARP_HTYPE_ETHERNET ||
        net_ntohs(arp->ptype) != ETHERTYPE_IPV4 || arp->hlen != ETH_ALEN || arp->plen != 4) {
        arp_stats.rx_invalid++;
        return;
    }

    uint16_t op = net_ntohs(arp->op);
    uint32_t sender = net_ntohl(arp->spa);
    uint32_t target = net_ntohl(arp->tpa);
    uint32_t ours = ipv4_iface_address(dev);

    if (op == ARP_OP_REQUEST) {
        arp_stats.rx_requests++;
    } else if (op == ARP_OP_REPLY) {
        arp_stats.rx_replies++;
    } else {
        arp_stats.rx_invalid++;
        return;
    }

    /* RFC 826: update an existing mapping from any ARP packet; sender 0.0.0.0 is a probe */
    struct arp$entry* entry = sender ? arp$$find(dev, sender) : NULL;
    bool merged = entry != NULL;
    if (merged) {
        arp$$resolved(entry, arp->sha);
    }

    if (ours == 0 || target != ours) {
        return;
    }

    /* Addressed to us: whoever asks will most likely talk to us next, so remember them */
    if (!merged && sender) {
        entry = arp$$allocate(false);
        if (entry) {
            entry->ip = sender;
            entry->dev = dev;
            entry->held = NULL;
            arp$$resolved(entry, arp->sha);
        } else {
            arp_stats.table_full++;
        }
    }

    if (op == ARP_OP_REQUEST) {
        arp$$send(dev, ARP_OP_REPLY, arp->sha, arp->sha, sender);
        arp_stats.tx_replies++;
    }
}

bool arp_output(struct netdev* dev, uint32_t next_hop, const void* packet, size_t len)
{
    if (next_hop == NET_IP_BROADCAST) {
        return eth_send(dev, eth_broadcast, ETHERTYPE_IPV4, packet, len);
    }

    struct arp$entry* entry = arp$$find(dev, next_hop);
    if (entry && entry->state == ARP$REACHABLE) {
        return eth_send(dev, entry->mac, ETHERTYPE_IPV4, packet, len);
    }

    uint8_t* copy = kmalloc(len);
    if (!copy) {
        return false;
    }
    const uint8_t* src = packet;
    for (size_t i = 0; i < len; i++) {
        copy[i] = src[i];
    }

    if (entry) {
        /* Already resolving: keep only the newest packet */
        if (entry->held) {
            kfree(entry->held);
            arp_stats.held_replaced++;
        }
        entry->held = copy;
        entry->held_len = len;
        return true;
    }

    entry = arp$$allocate(true);
    if (!entry) {
        arp_stats.table_full++;
        kfree(copy);
        return false;
    }
    entry->state = ARP$INCOMPLETE;
    entry->ip = next_hop;
    entry->dev = dev;
    entry->held = copy;
    entry->held_len = len;
    entry->requests = 1;
    entry->updated_ms = pit_uptime_ms();
    arp$$send_request(dev, next_hop);
    return true;
}

void arp_tick(uint32_t now_ms)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp$entry* e = &arp$table[i];
        if (e->state == ARP$INCOMPLETE && now_ms - e->updated_ms >= ARP_RETRY_MS) {
            if (e->requests >= ARP_MAX_REQUESTS) {
                /* Free the slot first: the ICMP error below may need an ARP entry itself */
                uint8_t* held = e->held;
                size_t held_len = e->held_len;
                e->held = NULL;
                arp$$release(e);
                if (held) {
                    arp_stats.held_dropped++;
                    icmp_send_error(NULL, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH, held, held_len);
                    kfree(held);
                }
                continue;
            }
            e->requests++;
            e->updated_ms = now_ms;
            arp$$send_request(e->dev, e->ip);
        } else if (e->state == ARP$REACHABLE && now_ms - e->updated_ms >= ARP_REACHABLE_MS) {
            arp$$release(e);
        }
    }
}

bool arp_lookup(const struct netdev* dev, uint32_t ip, uint8_t mac[ETH_ALEN])
{
    struct arp$entry* entry = arp$$find(dev, ip);
    if (!entry || entry->state != ARP$REACHABLE) {
        return false;
    }
    for (int i = 0; i < ETH_ALEN; i++) {
        mac[i] = entry->mac[i];
    }
    return true;
}

void arp_print_table(void)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp$entry* e = &arp$table[i];
        if (e->state == ARP$FREE) {
            continue;
        }
        k_print("  ");
        net_print_ip(e->ip);
        k_print(" %s ", e->dev->name);
        if (e->state == ARP$REACHABLE) {
            netdev_print_mac(e->mac);
        } else {
            k_print("(incomplete)");
        }
        k_print("\n");
    }
}
