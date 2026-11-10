#ifndef __NETDEV_H__
#define __NETDEV_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
    netdev is the only interface between the network stack and Ethernet drivers.
    A driver registers one netdev per port and supplies transmit/poll callbacks.

    Receive path: the driver's IRQ handler only acknowledges the device and calls
    netdev_mark_pending(). The idle loop (netdev_run_once) then calls the driver's
    poll(), which hands each frame to netdev_receive() outside interrupt context.
*/

#define NETDEV_MAX          8
#define NETDEV_NAME_LEN     8
#define NETDEV_POLL_BUDGET  64          /* frames per device per poll round */

#define ETH_ALEN            6
#define ETH_HEADER_LEN      14
#define ETH_FRAME_MAX       1514        /* without FCS; the NIC appends/strips it */

struct netdev;

struct netdev_ops
{
    /* Queue one frame (ETH_HEADER_LEN..ETH_FRAME_MAX bytes); false if it couldn't */
    bool (*transmit)(struct netdev* dev, const void* frame, size_t len);

    /* Deliver up to budget received frames via netdev_receive(); returns how many */
    size_t (*poll)(struct netdev* dev, size_t budget);
    /* Optional: hand queued transmits to the hardware. Drivers may batch them (one doorbell
       write per batch instead of per frame); the stack flushes before polling again or sleeping */
    void (*flush)(struct netdev* dev);
};

struct netdev_stats
{
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_dropped;                /* no handler, bad length, or driver errors */
    uint32_t rx_missed;                 /* dropped by the NIC itself: no free receive buffer (ring full) */
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_dropped;                /* ring full or bad length */
};

struct netdev
{
    char name[NETDEV_NAME_LEN];         /* "eth0", "eth1", ... in registration order */
    uint8_t mac[ETH_ALEN];
    bool link_up;
    const struct netdev_ops* ops;
    void* priv;                         /* driver state */
    struct netdev_stats stats;
    volatile bool rx_pending;
    bool tx_unflushed;                  /* transmits since the last flush */
};

typedef void (*netdev_rx_handler)(struct netdev* dev, const uint8_t* frame, size_t len);

/* Returns the new device, or NULL if NETDEV_MAX is reached */
struct netdev* netdev_register(const struct netdev_ops* ops, void* priv, const uint8_t mac[ETH_ALEN]);

size_t netdev_count(void);
struct netdev* netdev_get(size_t index);
struct netdev* netdev_find(const char* name);       /* by name, e.g. "eth1"; NULL if none */

bool netdev_transmit(struct netdev* dev, const void* frame, size_t len);

/* The single consumer of received frames (the stack); returns the previous one */
netdev_rx_handler netdev_set_rx_handler(netdev_rx_handler handler);

/* Driver side: called from poll() for every received frame */
void netdev_receive(struct netdev* dev, const uint8_t* frame, size_t len);

/* Driver side: called from the IRQ handler when frames are waiting */
void netdev_mark_pending(struct netdev* dev);

/* Make every queued transmit visible to the hardware (called by netdev_run_once) */
void netdev_flush_all(void);

/* One idle-loop step: flush transmits, then poll devices with pending work, otherwise hlt until the next interrupt */
void netdev_run_once(void);

void netdev_print_mac(const uint8_t mac[ETH_ALEN]);

#endif /* __NETDEV_H__ */
