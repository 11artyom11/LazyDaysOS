#ifndef __PCI_H__
#define __PCI_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_MAX_DEVICES         32
#define PCI_IRQ_NONE            0xFF

/* Configuration space offsets */
#define PCI_CFG_VENDOR_ID       0x00
#define PCI_CFG_COMMAND         0x04
#define PCI_CFG_CLASS_REV       0x08
#define PCI_CFG_HEADER_TYPE     0x0E
#define PCI_CFG_BAR0            0x10
#define PCI_CFG_INTERRUPT_LINE  0x3C

#define PCI_CMD_IO_SPACE        0x0001
#define PCI_CMD_MEM_SPACE       0x0002
#define PCI_CMD_BUS_MASTER      0x0004
#define PCI_CMD_INTX_DISABLE    0x0400

#define PCI_CLASS_NETWORK       0x02

struct pci_device
{
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t irq_line;                   /* legacy PIC IRQ assigned by the BIOS, PCI_IRQ_NONE if unset */
    uint32_t bar[6];                    /* raw BAR values; use pci_bar_address() */
};

/* Brute-force scan of all buses via configuration mechanism #1 */
void pci_initialize(void);

size_t pci_device_count(void);
struct pci_device* pci_get_device(size_t index);

/* index-th device (0 = first) matching vendor:device, or NULL */
struct pci_device* pci_find_device(uint16_t vendor_id, uint16_t device_id, size_t index);

uint32_t pci_config_read(const struct pci_device* dev, uint8_t offset);
void pci_config_write(const struct pci_device* dev, uint8_t offset, uint32_t value);

/* Base address of a BAR with the type bits masked off */
uint32_t pci_bar_address(const struct pci_device* dev, int bar);
bool pci_bar_is_io(const struct pci_device* dev, int bar);

/* Turn on memory decoding, bus mastering (DMA) and legacy INTx */
void pci_enable_device(const struct pci_device* dev);

#endif /* __PCI_H__ */
