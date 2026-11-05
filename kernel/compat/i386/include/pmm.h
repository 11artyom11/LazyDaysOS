#ifndef __PMM_H__
#define __PMM_H__

#include <stddef.h>
#include <stdint.h>
#include <multiboot.h>

/* Physical memory manager: one bit per 4 KiB frame, 1 = used */

#define MEM$FRAME_SIZE      4096
#define MEM$MAX_FRAMES      (1024 * 1024)       /* covers the full 32-bit physical space */
#define MEM$NO_FRAME        0                   /* frame 0 is always reserved, so 0 means failure */

/**
 * @brief Build the frame bitmap from the multiboot memory map. Low memory (< 1 MiB),
 *        the kernel image and the multiboot structures are marked used.
 *
 * @warning Must run before paging is enabled; it reads multiboot data by physical address.
 */
uint16_t mem$pmm_init(const multiboot_info_t* mbi);

/* Allocate one frame; returns its physical address or MEM$NO_FRAME */
uint32_t mem$frame_alloc(void);

/* Allocate count physically contiguous frames; returns the first address or MEM$NO_FRAME */
uint32_t mem$frame_alloc_contig(size_t count);

void mem$frame_free(uint32_t phys_addr);

size_t mem$frames_total(void);              /* usable frames reported by the bootloader */
size_t mem$frames_free(void);
uint32_t mem$phys_top(void);                /* end of the highest usable RAM region */

size_t mem$pmm_self_test(void);

#endif /* __PMM_H__ */
