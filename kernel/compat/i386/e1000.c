#include <e1000.h>
#include <pci.h>
#include <kio.h>
#include "../../heap.h"
#include "../../isr.h"
#include "../../idt.h"
#include "../../net/netdev.h"

/*
    Register and descriptor layouts follow the Intel "PCI/PCI-X Family of Gigabit
    Ethernet Controllers Software Developer's Manual" (8254x), chapters 3 and 13.
*/

#define INTEL_VENDOR_ID     0x8086

static const uint16_t e1000$supported[] = {
    0x100E,                             /* 82540EM, QEMU "e1000" */
    0x100F,                             /* 82545EM, QEMU "e1000-82545em" */
};

/* Registers (byte offsets into BAR0) */
#define REG_CTRL            0x0000
#define REG_STATUS          0x0008
#define REG_EERD            0x0014
#define REG_ICR             0x00C0
#define REG_IMS             0x00D0
#define REG_IMC             0x00D8
#define REG_RCTL            0x0100
#define REG_TCTL            0x0400
#define REG_TIPG            0x0410
#define REG_RDBAL           0x2800
#define REG_RDBAH           0x2804
#define REG_RDLEN           0x2808
#define REG_RDH             0x2810
#define REG_RDT             0x2818
#define REG_TDBAL           0x3800
#define REG_TDBAH           0x3804
#define REG_TDLEN           0x3808
#define REG_TDH             0x3810
#define REG_TDT             0x3818
#define REG_MPC             0x4010      /* missed packets count, clears on read */
#define REG_MTA             0x5200      /* multicast table, 128 entries */
#define REG_RAL0            0x5400
#define REG_RAH0            0x5404

#define CTRL_ASDE           (1u << 5)   /* auto speed detection */
#define CTRL_SLU            (1u << 6)   /* set link up */
#define CTRL_RST            (1u << 26)

#define STATUS_LU           (1u << 1)   /* link up */

#define EERD_START          (1u << 0)
#define EERD_DONE           (1u << 4)
#define EERD_ADDR_SHIFT     8
#define EERD_DATA_SHIFT     16

#define ICR_TXDW            (1u << 0)
#define ICR_LSC             (1u << 2)   /* link status change */
#define ICR_RXDMT0          (1u << 4)   /* receive descriptors running low */
#define ICR_RXO             (1u << 6)   /* receiver overrun */
#define ICR_RXT0            (1u << 7)   /* receive timer: frames arrived */

#define RCTL_EN             (1u << 1)
#define RCTL_BAM            (1u << 15)  /* accept broadcast */
#define RCTL_BSIZE_2048     (0u << 16)
#define RCTL_SECRC          (1u << 26)  /* strip Ethernet CRC */

#define TCTL_EN             (1u << 1)
#define TCTL_PSP            (1u << 3)   /* pad short packets to 64 bytes */
#define TCTL_CT_SHIFT       4
#define TCTL_COLD_SHIFT     12
#define TIPG_DEFAULT        0x0060200A  /* IPGT 10, IPGR1 8, IPGR2 6 for copper */

#define TXD_CMD_EOP         (1u << 0)   /* end of packet */
#define TXD_CMD_IFCS        (1u << 1)   /* insert FCS */
#define TXD_CMD_RS          (1u << 3)   /* report status (sets DD) */
#define DESC_STATUS_DD      (1u << 0)   /* descriptor done */
#define RXD_STATUS_EOP      (1u << 1)

#define RX_RING_SIZE        256         /* ring byte size must be a multiple of 128; ~25 ms of
                                           buffering at 10k packets/s (32 slots: ~3 ms) */
#define TX_RING_SIZE        32
#define RX_BUFFER_SIZE      2048
#define TX_BUFFER_SIZE      2048
#define E1000_MAX_PORTS     NETDEV_MAX

struct e1000$rx_desc
{
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000$tx_desc
{
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

struct e1000$port
{
    volatile uint8_t* mmio;
    const struct pci_device* pci;
    struct netdev* netdev;
    volatile struct e1000$rx_desc* rx_ring;
    volatile struct e1000$tx_desc* tx_ring;
    uint8_t* rx_buffers[RX_RING_SIZE];
    uint8_t* tx_buffers[TX_RING_SIZE];
    uint32_t rx_next;                   /* next descriptor the hardware will fill */
    uint32_t tx_next;                   /* next descriptor we will hand to the hardware */
};

static struct e1000$port e1000$ports[E1000_MAX_PORTS];
static size_t e1000$port_count = 0;

static inline uint32_t e1000$$read(struct e1000$port* port, uint32_t reg)
{
    return *(volatile uint32_t*)(port->mmio + reg);
}

static inline void e1000$$write(struct e1000$port* port, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t*)(port->mmio + reg) = value;
}

static bool e1000$$eeprom_read(struct e1000$port* port, uint8_t word, uint16_t* value)
{
    e1000$$write(port, REG_EERD, EERD_START | ((uint32_t)word << EERD_ADDR_SHIFT));
    for (int spin = 0; spin < 100000; spin++) {
        uint32_t eerd = e1000$$read(port, REG_EERD);
        if (eerd & EERD_DONE) {
            *value = eerd >> EERD_DATA_SHIFT;
            return true;
        }
    }
    return false;
}

static void e1000$$read_mac(struct e1000$port* port, uint8_t mac[ETH_ALEN])
{
    uint16_t word;
    if (e1000$$eeprom_read(port, 0, &word)) {
        for (uint8_t i = 0; i < 3; i++) {
            e1000$$eeprom_read(port, i, &word);
            mac[i * 2] = word & 0xFF;
            mac[i * 2 + 1] = word >> 8;
        }
        return;
    }
    /* No EEPROM access: the receive address register was loaded at reset */
    uint32_t ral = e1000$$read(port, REG_RAL0);
    uint32_t rah = e1000$$read(port, REG_RAH0);
    for (int i = 0; i < 4; i++) {
        mac[i] = (ral >> (i * 8)) & 0xFF;
    }
    mac[4] = rah & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
}

static bool e1000$$init_rx(struct e1000$port* port)
{
    port->rx_ring = kmalloc(sizeof(struct e1000$rx_desc) * RX_RING_SIZE);
    if (!port->rx_ring) {
        return false;
    }
    for (int i = 0; i < RX_RING_SIZE; i++) {
        port->rx_buffers[i] = kmalloc(RX_BUFFER_SIZE);
        if (!port->rx_buffers[i]) {
            return false;
        }
        /* Identity mapped: the virtual address is the physical DMA address */
        port->rx_ring[i].addr = (uint32_t)port->rx_buffers[i];
        port->rx_ring[i].status = 0;
    }

    e1000$$write(port, REG_RDBAL, (uint32_t)port->rx_ring);
    e1000$$write(port, REG_RDBAH, 0);
    e1000$$write(port, REG_RDLEN, sizeof(struct e1000$rx_desc) * RX_RING_SIZE);
    e1000$$write(port, REG_RDH, 0);
    e1000$$write(port, REG_RDT, RX_RING_SIZE - 1);  /* all but one descriptor owned by hardware */
    (void)e1000$$read(port, REG_MPC);                /* start the missed-packet count from zero */
    port->rx_next = 0;

    e1000$$write(port, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
    return true;
}

static bool e1000$$init_tx(struct e1000$port* port)
{
    port->tx_ring = kmalloc(sizeof(struct e1000$tx_desc) * TX_RING_SIZE);
    if (!port->tx_ring) {
        return false;
    }
    for (int i = 0; i < TX_RING_SIZE; i++) {
        port->tx_buffers[i] = kmalloc(TX_BUFFER_SIZE);
        if (!port->tx_buffers[i]) {
            return false;
        }
        port->tx_ring[i].addr = (uint32_t)port->tx_buffers[i];
        port->tx_ring[i].cmd = 0;
        port->tx_ring[i].status = DESC_STATUS_DD;   /* free for software */
    }

    e1000$$write(port, REG_TDBAL, (uint32_t)port->tx_ring);
    e1000$$write(port, REG_TDBAH, 0);
    e1000$$write(port, REG_TDLEN, sizeof(struct e1000$tx_desc) * TX_RING_SIZE);
    e1000$$write(port, REG_TDH, 0);
    e1000$$write(port, REG_TDT, 0);
    port->tx_next = 0;

    e1000$$write(port, REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));
    e1000$$write(port, REG_TIPG, TIPG_DEFAULT);
    return true;
}

static bool e1000$$transmit(struct netdev* dev, const void* frame, size_t len)
{
    struct e1000$port* port = dev->priv;
    volatile struct e1000$tx_desc* desc = &port->tx_ring[port->tx_next];

    /* The hardware sets DD once it has sent the descriptor; until then the ring is full */
    if (!(desc->status & DESC_STATUS_DD)) {
        return false;
    }

    const uint8_t* src = frame;
    uint8_t* dst = port->tx_buffers[port->tx_next];
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }

    desc->length = len;
    desc->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;

    port->tx_next = (port->tx_next + 1) % TX_RING_SIZE;
    asm volatile("" ::: "memory");      /* descriptor writes must land before the tail bump */
    e1000$$write(port, REG_TDT, port->tx_next);
    return true;
}

static size_t e1000$$poll(struct netdev* dev, size_t budget)
{
    struct e1000$port* port = dev->priv;
    size_t done = 0;

    /* Frames the NIC dropped because every receive buffer was full (the ring overflowed) */
    dev->stats.rx_missed += e1000$$read(port, REG_MPC);

    while (done < budget) {
        volatile struct e1000$rx_desc* desc = &port->rx_ring[port->rx_next];
        if (!(desc->status & DESC_STATUS_DD)) {
            break;
        }

        /* Frames never span buffers (2048 > max frame), so anything without EOP or with errors is dropped */
        if ((desc->status & RXD_STATUS_EOP) && desc->errors == 0) {
            netdev_receive(dev, port->rx_buffers[port->rx_next], desc->length);
        } else {
            dev->stats.rx_dropped++;
        }

        /* Give the descriptor back: it becomes the new tail */
        desc->status = 0;
        uint32_t returned = port->rx_next;
        port->rx_next = (port->rx_next + 1) % RX_RING_SIZE;
        asm volatile("" ::: "memory");
        e1000$$write(port, REG_RDT, returned);
        done++;
    }
    return done;
}

static const struct netdev_ops e1000$ops = {
    .transmit = e1000$$transmit,
    .poll = e1000$$poll,
};

/* Shared by all ports; reading ICR acknowledges and deasserts the interrupt */
static void e1000$$irq(void)
{
    for (size_t i = 0; i < e1000$port_count; i++) {
        struct e1000$port* port = &e1000$ports[i];
        uint32_t icr = e1000$$read(port, REG_ICR);
        if (icr & ICR_LSC) {
            port->netdev->link_up = (e1000$$read(port, REG_STATUS) & STATUS_LU) != 0;
        }
        if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO)) {
            netdev_mark_pending(port->netdev);
        }
    }
}

static bool e1000$$irq_registered(uint8_t irq)
{
    for (size_t i = 0; i < e1000$port_count; i++) {
        if (e1000$ports[i].pci->irq_line == irq) {
            return true;
        }
    }
    return false;
}

static bool e1000$$bring_up(struct e1000$port* port, const struct pci_device* pci)
{
    if (pci_bar_is_io(pci, 0)) {
        k_print("[K_FAIL] e1000: BAR0 is not memory mapped\n");
        return false;
    }
    port->pci = pci;
    port->mmio = (volatile uint8_t*)pci_bar_address(pci, 0);
    pci_enable_device(pci);

    /* Reset, then mask and clear every interrupt cause */
    e1000$$write(port, REG_IMC, 0xFFFFFFFF);
    e1000$$write(port, REG_CTRL, e1000$$read(port, REG_CTRL) | CTRL_RST);
    for (int spin = 0; spin < 100000 && (e1000$$read(port, REG_CTRL) & CTRL_RST); spin++)
        ;
    e1000$$write(port, REG_IMC, 0xFFFFFFFF);
    e1000$$read(port, REG_ICR);

    e1000$$write(port, REG_CTRL, e1000$$read(port, REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (int i = 0; i < 128; i++) {
        e1000$$write(port, REG_MTA + i * 4, 0);
    }

    uint8_t mac[ETH_ALEN];
    e1000$$read_mac(port, mac);

    if (!e1000$$init_rx(port) || !e1000$$init_tx(port)) {
        k_print("[K_FAIL] e1000: out of memory for descriptor rings\n");
        return false;
    }

    port->netdev = netdev_register(&e1000$ops, port, mac);
    if (!port->netdev) {
        return false;
    }
    port->netdev->link_up = (e1000$$read(port, REG_STATUS) & STATUS_LU) != 0;

    k_print("[K_INFO] %s: e1000 %x at 0x%x irq %u mac ",
            port->netdev->name, pci->device_id, (uint32_t)port->mmio, pci->irq_line);
    netdev_print_mac(mac);
    k_print(" link %s\n", port->netdev->link_up ? "up" : "down");
    return true;
}

size_t e1000_initialize(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device* pci = pci_get_device(i);
        bool supported = false;
        for (size_t m = 0; m < sizeof(e1000$supported) / sizeof(e1000$supported[0]); m++) {
            supported |= pci->vendor_id == INTEL_VENDOR_ID && pci->device_id == e1000$supported[m];
        }
        if (!supported || e1000$port_count == E1000_MAX_PORTS) {
            continue;
        }

        struct e1000$port* port = &e1000$ports[e1000$port_count];
        if (!e1000$$bring_up(port, pci)) {
            continue;
        }

        /* One shared handler per IRQ line; it checks every port */
        if (pci->irq_line != PCI_IRQ_NONE && pci->irq_line < 16 && !e1000$$irq_registered(pci->irq_line)) {
            isr_register(MAP_IRQ(pci->irq_line), e1000$$irq);
        }
        e1000$port_count++;

        /* Unmask only after the port is counted, so the shared handler can see it */
        e1000$$write(port, REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC);
    }
    return e1000$port_count;
}
