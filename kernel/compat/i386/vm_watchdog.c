#include "include/vm_watchdog.h"
#include <util.h>

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t page_table[1024] __attribute__((aligned(4096)));

/**
 * @brief This procedure does operations necessary to enable paging mode.
 * 
 * @warning This procedure assumes that protected mode is active.
 * 
 */
uint16_t mem$setup_paging(void)
{
    for (int i = 0; i < 1024; i++) {
        // Identity map (virtual address = physical address)
        page_table[i] = (i * 0x1000) | 3; // Present (P) | Read/Write (RW)
    }

    page_directory[0] = ((uint32_t)page_table) | 3; // Present | RW

    // Load the page directory into CR3
    asm volatile("mov %0, %%cr3" :: "r"(&page_directory));

    // Enable paging by setting the PG bit in CR0
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000; // Set PG bit
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
    return UTIL$SUCCESS;
}


void pagefault_hander (void)
{
    k_print("Pagefault occured backing up\n");
    return;
}