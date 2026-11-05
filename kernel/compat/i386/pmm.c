#include <pmm.h>
#include <util.h>
#include <kio.h>
#include "../../panic.h"

#define LOW_MEMORY_END      0x100000ULL         /* BIOS data, VGA memory, option ROMs */
#define PHYS_SPACE_END      0x100000000ULL      /* 4 GiB */
#define FRAME_SHIFT         12
#define BITS_PER_WORD       32

/* Provided by linker.ld */
extern char _kernel_start[];
extern char _kernel_end[];

static uint32_t mem$bitmap[MEM$MAX_FRAMES / BITS_PER_WORD];
static size_t mem$frame_limit = 0;      /* one past the highest frame that can ever be free */
static size_t mem$total = 0;
static size_t mem$free = 0;
static size_t mem$search_hint = 0;

static inline int mem$$is_used(size_t frame)
{
    return (mem$bitmap[frame / BITS_PER_WORD] >> (frame % BITS_PER_WORD)) & 1;
}

static inline void mem$$set_used(size_t frame)
{
    if (!mem$$is_used(frame)) {
        mem$bitmap[frame / BITS_PER_WORD] |= 1u << (frame % BITS_PER_WORD);
        mem$free--;
    }
}

static inline void mem$$set_free(size_t frame)
{
    if (mem$$is_used(frame)) {
        mem$bitmap[frame / BITS_PER_WORD] &= ~(1u << (frame % BITS_PER_WORD));
        mem$free++;
    }
}

/* Free only whole frames inside [start, end) */
static void mem$$release_region(uint64_t start, uint64_t end)
{
    if (end > PHYS_SPACE_END) {
        end = PHYS_SPACE_END;
    }
    uint64_t first = (start + MEM$FRAME_SIZE - 1) >> FRAME_SHIFT;
    uint64_t last = end >> FRAME_SHIFT;
    for (uint64_t f = first; f < last; f++) {
        mem$$set_free((size_t)f);
    }
    if (last > first && last > mem$frame_limit) {
        mem$frame_limit = (size_t)last;
    }
}

/* Reserve every frame touched by [start, end) */
static void mem$$reserve_region(uint64_t start, uint64_t end)
{
    if (end > PHYS_SPACE_END) {
        end = PHYS_SPACE_END;
    }
    uint64_t first = start >> FRAME_SHIFT;
    uint64_t last = (end + MEM$FRAME_SIZE - 1) >> FRAME_SHIFT;
    for (uint64_t f = first; f < last; f++) {
        mem$$set_used((size_t)f);
    }
}

uint16_t mem$pmm_init(const multiboot_info_t* mbi)
{
    /* Everything is used until the bootloader tells us otherwise */
    for (size_t i = 0; i < MEM$MAX_FRAMES / BITS_PER_WORD; i++) {
        mem$bitmap[i] = 0xFFFFFFFF;
    }
    mem$free = 0;
    mem$frame_limit = 0;

    if (mbi->flags & MULTIBOOT_INFO_MEM_MAP) {
        uint32_t addr = mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;
        while (addr < end) {
            const multiboot_mmap_entry_t* entry = (const multiboot_mmap_entry_t*)addr;
            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                mem$$release_region(entry->addr, entry->addr + entry->len);
            }
            addr += entry->size + sizeof(entry->size);
        }
    } else if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
        mem$$release_region(LOW_MEMORY_END, LOW_MEMORY_END + (uint64_t)mbi->mem_upper * 1024);
    } else {
        k_print("[K_FAIL] bootloader provided no memory information\n");
        return UTIL$FAIL;
    }
    mem$total = mem$free;

    /* Keep away from what is already in use */
    mem$$reserve_region(0, LOW_MEMORY_END);
    mem$$reserve_region((uint32_t)_kernel_start, (uint32_t)_kernel_end);
    mem$$reserve_region((uint32_t)mbi, (uint32_t)mbi + sizeof(*mbi));
    if (mbi->flags & MULTIBOOT_INFO_MEM_MAP) {
        mem$$reserve_region(mbi->mmap_addr, (uint64_t)mbi->mmap_addr + mbi->mmap_length);
    }
    if (mbi->flags & MULTIBOOT_INFO_CMDLINE) {
        mem$$reserve_region(mbi->cmdline, (uint64_t)mbi->cmdline + 1);
    }

    k_print("[K_INFO] physical memory: %u MiB usable, %u frames free, top 0x%x\n",
            (uint32_t)(mem$total / 256), (uint32_t)mem$free, mem$phys_top());
    return UTIL$SUCCESS;
}

uint32_t mem$frame_alloc(void)
{
    size_t words = (mem$frame_limit + BITS_PER_WORD - 1) / BITS_PER_WORD;
    for (size_t n = 0; n < words; n++) {
        size_t w = (mem$search_hint + n) % words;
        if (mem$bitmap[w] == 0xFFFFFFFF) {
            continue;
        }
        for (size_t bit = 0; bit < BITS_PER_WORD; bit++) {
            size_t frame = w * BITS_PER_WORD + bit;
            if (frame < mem$frame_limit && !mem$$is_used(frame)) {
                mem$$set_used(frame);
                mem$search_hint = w;
                return (uint32_t)frame << FRAME_SHIFT;
            }
        }
    }
    return MEM$NO_FRAME;
}

uint32_t mem$frame_alloc_contig(size_t count)
{
    size_t run = 0;
    if (count == 0) {
        return MEM$NO_FRAME;
    }
    for (size_t frame = 0; frame < mem$frame_limit; frame++) {
        run = mem$$is_used(frame) ? 0 : run + 1;
        if (run == count) {
            size_t first = frame + 1 - count;
            for (size_t f = first; f <= frame; f++) {
                mem$$set_used(f);
            }
            return (uint32_t)first << FRAME_SHIFT;
        }
    }
    return MEM$NO_FRAME;
}

void mem$frame_free(uint32_t phys_addr)
{
    size_t frame = phys_addr >> FRAME_SHIFT;
    if ((phys_addr & (MEM$FRAME_SIZE - 1)) || frame >= mem$frame_limit) {
        k_panic("mem$frame_free: invalid frame address");
    }
    if (!mem$$is_used(frame)) {
        k_panic("mem$frame_free: frame is already free");
    }
    mem$$set_free(frame);
}

size_t mem$frames_total(void)
{
    return mem$total;
}

size_t mem$frames_free(void)
{
    return mem$free;
}

uint32_t mem$phys_top(void)
{
    /* RAM reaching the 4 GiB boundary is reported as the last addressable byte */
    if (mem$frame_limit >= MEM$MAX_FRAMES) {
        return 0xFFFFFFFF;
    }
    return (uint32_t)mem$frame_limit << FRAME_SHIFT;
}

/**
 * @brief Allocates and frees single and contiguous frames, checking the free counter
 *        and that allocations are distinct, aligned and marked used.
 *
 * @return UTIL$SUCCESS on success
 * @return UTIL$FAIL otherwise
 */
size_t mem$pmm_self_test(void)
{
    size_t free_before = mem$frames_free();

    uint32_t a = mem$frame_alloc();
    uint32_t b = mem$frame_alloc();
    if (a == MEM$NO_FRAME || b == MEM$NO_FRAME || a == b) {
        k_print("[UTIL_FAIL] frame allocation returned 0x%x and 0x%x\n", a, b);
        return UTIL$FAIL;
    }
    if ((a | b) & (MEM$FRAME_SIZE - 1)) {
        k_print("[UTIL_FAIL] frames are not page aligned\n");
        return UTIL$FAIL;
    }
    if (mem$frames_free() != free_before - 2) {
        k_print("[UTIL_FAIL] free frame counter did not drop by 2\n");
        return UTIL$FAIL;
    }

    /* Frames must be usable memory */
    *(volatile uint32_t*)a = 0xCAFEBABE;
    if (*(volatile uint32_t*)a != 0xCAFEBABE) {
        k_print("[UTIL_FAIL] frame 0x%x is not writable\n", a);
        return UTIL$FAIL;
    }

    uint32_t run = mem$frame_alloc_contig(4);
    if (run == MEM$NO_FRAME) {
        k_print("[UTIL_FAIL] contiguous allocation of 4 frames failed\n");
        return UTIL$FAIL;
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (!mem$$is_used((run >> FRAME_SHIFT) + i)) {
            k_print("[UTIL_FAIL] contiguous frame %u not marked used\n", i);
            return UTIL$FAIL;
        }
    }

    for (uint32_t i = 0; i < 4; i++) {
        mem$frame_free(run + i * MEM$FRAME_SIZE);
    }
    mem$frame_free(a);
    mem$frame_free(b);

    if (mem$frames_free() != free_before) {
        k_print("[UTIL_FAIL] free frame counter not restored\n");
        return UTIL$FAIL;
    }
    return UTIL$SUCCESS;
}
