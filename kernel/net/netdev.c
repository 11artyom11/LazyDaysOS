#include "netdev.h"
#include <kio.h>
#include <util.h>
#include <pit.h>

static struct netdev netdev$devices[NETDEV_MAX];
static size_t netdev$count = 0;
static netdev_rx_handler netdev$rx_handler = NULL;
static volatile bool netdev$work_pending = false;

struct netdev* netdev_register(const struct netdev_ops* ops, void* priv, const uint8_t mac[ETH_ALEN])
{
    if (netdev$count == NETDEV_MAX) {
        k_print("[K_FAIL] netdev: more than %u devices\n", NETDEV_MAX);
        return NULL;
    }

    struct netdev* dev = &netdev$devices[netdev$count];
    dev->name[0] = 'e';
    dev->name[1] = 't';
    dev->name[2] = 'h';
    dev->name[3] = '0' + netdev$count;
    dev->name[4] = '\0';
    for (int i = 0; i < ETH_ALEN; i++) {
        dev->mac[i] = mac[i];
    }
    dev->ops = ops;
    dev->priv = priv;
    dev->link_up = false;
    dev->rx_pending = false;

    netdev$count++;
    return dev;
}

size_t netdev_count(void)
{
    return netdev$count;
}

struct netdev* netdev_get(size_t index)
{
    return index < netdev$count ? &netdev$devices[index] : NULL;
}

bool netdev_transmit(struct netdev* dev, const void* frame, size_t len)
{
    if (len < ETH_HEADER_LEN || len > ETH_FRAME_MAX || !dev->ops->transmit(dev, frame, len)) {
        dev->stats.tx_dropped++;
        return false;
    }
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += len;
    return true;
}

netdev_rx_handler netdev_set_rx_handler(netdev_rx_handler handler)
{
    netdev_rx_handler previous = netdev$rx_handler;
    netdev$rx_handler = handler;
    return previous;
}

void netdev_receive(struct netdev* dev, const uint8_t* frame, size_t len)
{
    if (len < ETH_HEADER_LEN || netdev$rx_handler == NULL) {
        dev->stats.rx_dropped++;
        return;
    }
    dev->stats.rx_packets++;
    dev->stats.rx_bytes += len;
    netdev$rx_handler(dev, frame, len);
}

void netdev_mark_pending(struct netdev* dev)
{
    dev->rx_pending = true;
    netdev$work_pending = true;
}

static void netdev$$poll_all(void)
{
    netdev$work_pending = false;
    for (size_t i = 0; i < netdev$count; i++) {
        struct netdev* dev = &netdev$devices[i];
        if (!dev->rx_pending) {
            continue;
        }
        dev->rx_pending = false;
        if (dev->ops->poll(dev, NETDEV_POLL_BUDGET) == NETDEV_POLL_BUDGET) {
            /* Budget exhausted: more frames may be waiting, come back next round */
            netdev_mark_pending(dev);
        }
    }
}

void netdev_run_once(void)
{
    /* Check and sleep atomically: "sti; hlt" lets an interrupt arriving after
       the check still wake the hlt instead of being missed until the next tick */
    asm volatile("cli");
    if (netdev$work_pending) {
        asm volatile("sti");
        netdev$$poll_all();
    } else {
        asm volatile("sti; hlt");
    }
}

void netdev_print_mac(const uint8_t mac[ETH_ALEN])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < ETH_ALEN; i++) {
        k_print("%c%c%s", digits[mac[i] >> 4], digits[mac[i] & 0xF], i < ETH_ALEN - 1 ? ":" : "");
    }
}

/* ------------------------------------------------------------------------- */
/* Self test: a hand-built ARP request to QEMU's user-network (slirp) gateway */

/* QEMU's e1000 model holds received frames for 1 s after every RCTL write, so
   the first reply after bring-up takes up to ~1 s; real hardware answers in microseconds */
#define NETDEV$PROBE_TIMEOUT_MS 3000

#define ETHERTYPE_ARP       0x0806
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2
#define ARP_FRAME_LEN       42

static const uint8_t netdev$$probe_src_ip[4] = {10, 0, 2, 15};     /* QEMU's default guest address */
static const uint8_t netdev$$probe_dst_ip[4] = {10, 0, 2, 2};      /* QEMU's gateway */

static struct netdev* netdev$$probe_dev;
static volatile bool netdev$$probe_answered;
static uint8_t netdev$$probe_gateway_mac[ETH_ALEN];

static void netdev$$probe_rx(struct netdev* dev, const uint8_t* frame, size_t len)
{
    if (dev != netdev$$probe_dev || len < ARP_FRAME_LEN) {
        return;
    }
    uint16_t ethertype = (frame[12] << 8) | frame[13];
    uint16_t opcode = (frame[20] << 8) | frame[21];
    if (ethertype != ETHERTYPE_ARP || opcode != ARP_OP_REPLY) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (frame[28 + i] != netdev$$probe_dst_ip[i]) {
            return;
        }
    }
    for (int i = 0; i < ETH_ALEN; i++) {
        netdev$$probe_gateway_mac[i] = frame[22 + i];
    }
    netdev$$probe_answered = true;
}

static void netdev$$build_arp_request(const struct netdev* dev, uint8_t* f)
{
    int p = 0;
    for (int i = 0; i < ETH_ALEN; i++) f[p++] = 0xFF;          /* broadcast */
    for (int i = 0; i < ETH_ALEN; i++) f[p++] = dev->mac[i];
    f[p++] = ETHERTYPE_ARP >> 8;   f[p++] = ETHERTYPE_ARP & 0xFF;
    f[p++] = 0x00; f[p++] = 0x01;                               /* hardware: Ethernet */
    f[p++] = 0x08; f[p++] = 0x00;                               /* protocol: IPv4 */
    f[p++] = ETH_ALEN; f[p++] = 4;
    f[p++] = 0x00; f[p++] = ARP_OP_REQUEST;
    for (int i = 0; i < ETH_ALEN; i++) f[p++] = dev->mac[i];
    for (int i = 0; i < 4; i++) f[p++] = netdev$$probe_src_ip[i];
    for (int i = 0; i < ETH_ALEN; i++) f[p++] = 0x00;
    for (int i = 0; i < 4; i++) f[p++] = netdev$$probe_dst_ip[i];
}

/**
 * @brief Sends an ARP request for 10.0.2.2 on every device and waits for the reply.
 *        Exercises PCI setup, DMA rings, transmit, receive interrupts and polling.
 *        Only meaningful with QEMU user networking (-nic user / -netdev user).
 *
 * @return UTIL$SUCCESS if every device got an answer
 * @return UTIL$FAIL otherwise, or if there are no devices
 */
size_t netdev_self_test(void)
{
    if (netdev$count == 0) {
        k_print("[UTIL_FAIL] no network devices found\n");
        return UTIL$FAIL;
    }

    netdev_rx_handler previous = netdev_set_rx_handler(netdev$$probe_rx);
    size_t status = UTIL$SUCCESS;

    for (size_t i = 0; i < netdev$count; i++) {
        struct netdev* dev = &netdev$devices[i];
        uint8_t frame[ARP_FRAME_LEN];

        netdev$$probe_dev = dev;
        netdev$$probe_answered = false;
        netdev$$build_arp_request(dev, frame);
        if (!netdev_transmit(dev, frame, sizeof(frame))) {
            k_print("[UTIL_FAIL] %s: transmit failed\n", dev->name);
            status = UTIL$FAIL;
            break;
        }

        uint32_t start = pit_uptime_ms();
        while (!netdev$$probe_answered && pit_uptime_ms() - start < NETDEV$PROBE_TIMEOUT_MS) {
            netdev_run_once();
        }
        if (!netdev$$probe_answered) {
            k_print("[UTIL_FAIL] %s: no ARP reply from 10.0.2.2 within %u ms\n",
                    dev->name, NETDEV$PROBE_TIMEOUT_MS);
            status = UTIL$FAIL;
            break;
        }
        k_print("[K_INFO] %s: gateway 10.0.2.2 is at ", dev->name);
        netdev_print_mac(netdev$$probe_gateway_mac);
        k_print(" (%u ms)\n", pit_uptime_ms() - start);
    }

    netdev_set_rx_handler(previous);
    return status;
}
