#include <pci.h>
#include <kio.h>
#include "../../hwio.h"

#define PCI_CONFIG_ADDRESS      0xCF8
#define PCI_CONFIG_DATA         0xCFC
#define PCI_ENABLE_BIT          0x80000000

#define PCI_VENDOR_NONE         0xFFFF
#define PCI_HEADER_MULTIFUNC    0x80
#define PCI_BAR_IO              0x1

static struct pci_device pci_devices[PCI_MAX_DEVICES];
static size_t pci_count = 0;

static uint32_t pci$$read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = PCI_ENABLE_BIT | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci$$write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = PCI_ENABLE_BIT | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_config_read(const struct pci_device* dev, uint8_t offset)
{
    return pci$$read(dev->bus, dev->slot, dev->func, offset);
}

void pci_config_write(const struct pci_device* dev, uint8_t offset, uint32_t value)
{
    pci$$write(dev->bus, dev->slot, dev->func, offset, value);
}

static void pci$$probe(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci$$read(bus, slot, func, PCI_CFG_VENDOR_ID);
    if ((id & 0xFFFF) == PCI_VENDOR_NONE) {
        return;
    }
    if (pci_count == PCI_MAX_DEVICES) {
        k_print("[K_FAIL] pci: more than %u devices, ignoring the rest\n", PCI_MAX_DEVICES);
        return;
    }

    struct pci_device* dev = &pci_devices[pci_count++];
    uint32_t class_rev = pci$$read(bus, slot, func, PCI_CFG_CLASS_REV);

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = id >> 16;
    dev->class_code = class_rev >> 24;
    dev->subclass = (class_rev >> 16) & 0xFF;
    dev->prog_if = (class_rev >> 8) & 0xFF;
    dev->irq_line = pci$$read(bus, slot, func, PCI_CFG_INTERRUPT_LINE) & 0xFF;
    for (int i = 0; i < 6; i++) {
        dev->bar[i] = pci$$read(bus, slot, func, PCI_CFG_BAR0 + i * 4);
    }

    k_print("[K_INFO] pci %u:%u.%u %x:%x class %x.%x irq %u\n",
            bus, slot, func, dev->vendor_id, dev->device_id,
            dev->class_code, dev->subclass, dev->irq_line);
}

void pci_initialize(void)
{
    pci_count = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci$$read(bus, slot, 0, PCI_CFG_VENDOR_ID);
            if ((id & 0xFFFF) == PCI_VENDOR_NONE) {
                continue;
            }
            uint8_t header = (pci$$read(bus, slot, 0, PCI_CFG_HEADER_TYPE & 0xFC) >> 16) & 0xFF;
            uint8_t functions = (header & PCI_HEADER_MULTIFUNC) ? 8 : 1;
            for (uint8_t func = 0; func < functions; func++) {
                pci$$probe(bus, slot, func);
            }
        }
    }
    k_print("[K_INFO] pci: %u devices\n", (uint32_t)pci_count);
}

size_t pci_device_count(void)
{
    return pci_count;
}

struct pci_device* pci_get_device(size_t index)
{
    return index < pci_count ? &pci_devices[index] : NULL;
}

struct pci_device* pci_find_device(uint16_t vendor_id, uint16_t device_id, size_t index)
{
    for (size_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            if (index-- == 0) {
                return &pci_devices[i];
            }
        }
    }
    return NULL;
}

uint32_t pci_bar_address(const struct pci_device* dev, int bar)
{
    if (dev->bar[bar] & PCI_BAR_IO) {
        return dev->bar[bar] & ~0x3u;
    }
    return dev->bar[bar] & ~0xFu;
}

bool pci_bar_is_io(const struct pci_device* dev, int bar)
{
    return dev->bar[bar] & PCI_BAR_IO;
}

void pci_enable_device(const struct pci_device* dev)
{
    uint32_t command = pci_config_read(dev, PCI_CFG_COMMAND);
    command |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    command &= ~PCI_CMD_INTX_DISABLE;
    /* Upper 16 bits are the status register; writing 1s there clears error bits, so write 0s */
    pci_config_write(dev, PCI_CFG_COMMAND, command & 0xFFFF);
}
