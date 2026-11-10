#include "netdev.h"
#include "../cpuload.h"
#include <kio.h>

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

struct netdev* netdev_find(const char* name)
{
    for (size_t i = 0; i < netdev$count; i++) {
        const char* a = netdev$devices[i].name;
        const char* b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return &netdev$devices[i];
        }
    }
    return NULL;
}

bool netdev_transmit(struct netdev* dev, const void* frame, size_t len)
{
    if (len < ETH_HEADER_LEN || len > ETH_FRAME_MAX || !dev->ops->transmit(dev, frame, len)) {
        dev->stats.tx_dropped++;
        return false;
    }
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += len;
    dev->tx_unflushed = true;
    return true;
}

void netdev_flush_all(void)
{
    for (size_t i = 0; i < netdev$count; i++) {
        struct netdev* dev = &netdev$devices[i];
        if (dev->tx_unflushed) {
            dev->tx_unflushed = false;
            if (dev->ops->flush) {
                dev->ops->flush(dev);
            }
        }
    }
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
    /* Everything sent since the last step (the previous poll round, protocol timers) goes out now,
       as one batch per device, before polling again or sleeping */
    netdev_flush_all();

    /* Check and sleep atomically: cpu_idle_wait() re-enables interrupts and sleeps
       in one step, so an interrupt arriving after the check still wakes it instead
       of being missed until the next tick. The sleep is what counts as CPU idle. */
    cpu_irq_disable();
    if (netdev$work_pending) {
        cpu_irq_enable();
        netdev$$poll_all();
    } else {
        cpu_idle_wait();
    }
}

void netdev_print_mac(const uint8_t mac[ETH_ALEN])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < ETH_ALEN; i++) {
        k_print("%c%c%s", digits[mac[i] >> 4], digits[mac[i] & 0xF], i < ETH_ALEN - 1 ? ":" : "");
    }
}
