#include "include/vm_watchdog.h"
#include <pmm.h>
#include <util.h>

#define PG_PRESENT          0x001
#define PG_WRITABLE         0x002
#define PG_WRITE_THROUGH    0x008
#define PG_CACHE_DISABLE    0x010
#define PG_LARGE            0x080       /* 4 MiB page (needs CR4.PSE) */

#define CR0_PG              0x80000000
#define CR4_PSE             0x00000010

#define LARGE_PAGE_SIZE     0x400000

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t page_table[1024] __attribute__((aligned(4096)));

/**
 * @brief This procedure does operations necessary to enable paging mode.
 *
 *        Layout (identity mapped, virtual address = physical address):
 *          0 - 4 KiB      not present, so NULL dereferences fault
 *          4 KiB - 4 MiB  4 KiB pages
 *          4 MiB - 4 GiB  4 MiB pages; uncached above the end of RAM (device memory)
 *
 * @warning This procedure assumes that protected mode is active and that
 *          mem$pmm_init() has already run.
 */
uint16_t mem$setup_paging(void)
{
    uint32_t ram_top = mem$phys_top();

    page_table[0] = 0;
    for (int i = 1; i < 1024; i++) {
        page_table[i] = (i * MEM$FRAME_SIZE) | PG_PRESENT | PG_WRITABLE;
    }
    page_directory[0] = ((uint32_t)page_table) | PG_PRESENT | PG_WRITABLE;

    for (uint32_t i = 1; i < 1024; i++) {
        uint32_t base = i * LARGE_PAGE_SIZE;
        uint32_t flags = PG_PRESENT | PG_WRITABLE | PG_LARGE;
        if (base >= ram_top) {
            flags |= PG_CACHE_DISABLE | PG_WRITE_THROUGH;
        }
        page_directory[i] = base | flags;
    }

    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_PSE;
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // Load the page directory into CR3
    asm volatile("mov %0, %%cr3" :: "r"(&page_directory));

    // Enable paging by setting the PG bit in CR0
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG;
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    return UTIL$SUCCESS;
}
