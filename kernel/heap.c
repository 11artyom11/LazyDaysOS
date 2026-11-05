#include "heap.h"
#include "panic.h"
#include <pmm.h>
#include <util.h>
#include <kio.h>

/*
    The heap is one contiguous region split into blocks laid out back to back:

    +--------+-----------+--------+-----------+--------+-----
    | header | payload   | header | payload   | header | ...
    +--------+-----------+--------+-----------+--------+-----

    The next block always starts right after the current payload, so walking the
    heap needs no explicit list. Allocation is first fit; freeing merges the block
    with free neighbours.
*/

#define HEAP$MAGIC_USED     0x4C415A59      /* "LAZY" */
#define HEAP$MAGIC_FREE     0x46524545      /* "FREE" */

struct heap$block
{
    size_t hb$size;                 /* payload bytes, multiple of MEM$HEAP_ALIGN */
    struct heap$block* hb$prev;     /* previous block in memory, NULL for the first */
    uint32_t hb$magic;
    uint32_t hb$reserved;           /* pads the header to MEM$HEAP_ALIGN bytes */
};

#define HEAP$HEADER_SIZE    sizeof(struct heap$block)
#define HEAP$MIN_SPLIT      (HEAP$HEADER_SIZE + MEM$HEAP_ALIGN)

static char* heap$start = NULL;
static char* heap$end = NULL;

static inline struct heap$block* heap$$next(struct heap$block* block)
{
    char* next = (char*)block + HEAP$HEADER_SIZE + block->hb$size;
    return next < heap$end ? (struct heap$block*)next : NULL;
}

static inline size_t heap$$round_up(size_t size)
{
    return (size + MEM$HEAP_ALIGN - 1) & ~(size_t)(MEM$HEAP_ALIGN - 1);
}

uint16_t mem$heap_init(size_t bytes)
{
    size_t frames = (bytes + MEM$FRAME_SIZE - 1) / MEM$FRAME_SIZE;
    uint32_t base = mem$frame_alloc_contig(frames);
    if (base == MEM$NO_FRAME) {
        k_print("[K_FAIL] no %u contiguous frames for the kernel heap\n", (uint32_t)frames);
        return UTIL$FAIL;
    }

    heap$start = (char*)base;
    heap$end = heap$start + frames * MEM$FRAME_SIZE;

    struct heap$block* first = (struct heap$block*)heap$start;
    first->hb$size = (heap$end - heap$start) - HEAP$HEADER_SIZE;
    first->hb$prev = NULL;
    first->hb$magic = HEAP$MAGIC_FREE;

    k_print("[K_INFO] kernel heap: %u KiB at 0x%x\n", (uint32_t)(frames * 4), base);
    return UTIL$SUCCESS;
}

void* kmalloc(size_t size)
{
    if (size == 0 || heap$start == NULL) {
        return NULL;
    }
    size = heap$$round_up(size);

    for (struct heap$block* block = (struct heap$block*)heap$start; block; block = heap$$next(block)) {
        if (block->hb$magic != HEAP$MAGIC_FREE || block->hb$size < size) {
            continue;
        }

        /* Split off the tail if it can hold another block */
        if (block->hb$size - size >= HEAP$MIN_SPLIT) {
            struct heap$block* rest = (struct heap$block*)((char*)block + HEAP$HEADER_SIZE + size);
            rest->hb$size = block->hb$size - size - HEAP$HEADER_SIZE;
            rest->hb$prev = block;
            rest->hb$magic = HEAP$MAGIC_FREE;

            struct heap$block* after = heap$$next(rest);
            if (after) {
                after->hb$prev = rest;
            }
            block->hb$size = size;
        }

        block->hb$magic = HEAP$MAGIC_USED;
        return (char*)block + HEAP$HEADER_SIZE;
    }
    return NULL;
}

/* Absorb `next` into `block`; both must be free and adjacent */
static void heap$$merge(struct heap$block* block, struct heap$block* next)
{
    block->hb$size += HEAP$HEADER_SIZE + next->hb$size;
    next->hb$magic = 0;
    struct heap$block* after = heap$$next(block);
    if (after) {
        after->hb$prev = block;
    }
}

void kfree(void* ptr)
{
    if (ptr == NULL) {
        return;
    }
    if ((char*)ptr < heap$start + HEAP$HEADER_SIZE || (char*)ptr >= heap$end) {
        k_panic("kfree: pointer outside the kernel heap");
    }

    struct heap$block* block = (struct heap$block*)((char*)ptr - HEAP$HEADER_SIZE);
    if (block->hb$magic == HEAP$MAGIC_FREE) {
        k_panic("kfree: double free");
    }
    if (block->hb$magic != HEAP$MAGIC_USED) {
        k_panic("kfree: corrupted block header or foreign pointer");
    }
    block->hb$magic = HEAP$MAGIC_FREE;

    struct heap$block* next = heap$$next(block);
    if (next && next->hb$magic == HEAP$MAGIC_FREE) {
        heap$$merge(block, next);
    }
    if (block->hb$prev && block->hb$prev->hb$magic == HEAP$MAGIC_FREE) {
        heap$$merge(block->hb$prev, block);
    }
}

void mem$heap_get_stats(struct mem$heap_stats* stats)
{
    stats->hs$size = heap$end - heap$start;
    stats->hs$used = 0;
    stats->hs$free = 0;
    stats->hs$blocks = 0;

    if (heap$start == NULL) {
        return;
    }
    for (struct heap$block* block = (struct heap$block*)heap$start; block; block = heap$$next(block)) {
        if (block->hb$magic == HEAP$MAGIC_USED) {
            stats->hs$used += block->hb$size;
        } else {
            stats->hs$free += block->hb$size;
        }
        stats->hs$blocks++;
    }
}

/**
 * @brief Exercises allocation, alignment, splitting, freeing in mixed order and
 *        coalescing back to a single free block.
 *
 * @return UTIL$SUCCESS on success
 * @return UTIL$FAIL otherwise
 */
size_t mem$heap_self_test(void)
{
    struct mem$heap_stats before, after;
    mem$heap_get_stats(&before);

    if (kmalloc(0) != NULL) {
        k_print("[UTIL_FAIL] kmalloc(0) did not return NULL\n");
        return UTIL$FAIL;
    }
    if (kmalloc(before.hs$size) != NULL) {
        k_print("[UTIL_FAIL] oversized kmalloc did not return NULL\n");
        return UTIL$FAIL;
    }

    char* a = kmalloc(10);
    char* b = kmalloc(1000);
    char* c = kmalloc(64);
    if (!a || !b || !c) {
        k_print("[UTIL_FAIL] kmalloc returned NULL\n");
        return UTIL$FAIL;
    }
    if (((uint32_t)a | (uint32_t)b | (uint32_t)c) & (MEM$HEAP_ALIGN - 1)) {
        k_print("[UTIL_FAIL] kmalloc result not %u-byte aligned\n", MEM$HEAP_ALIGN);
        return UTIL$FAIL;
    }

    /* Blocks must not overlap: fill each and verify none was overwritten */
    for (int i = 0; i < 10; i++)   a[i] = 'a';
    for (int i = 0; i < 1000; i++) b[i] = 'b';
    for (int i = 0; i < 64; i++)   c[i] = 'c';
    for (int i = 0; i < 10; i++) {
        if (a[i] != 'a') {
            k_print("[UTIL_FAIL] heap blocks overlap\n");
            return UTIL$FAIL;
        }
    }
    for (int i = 0; i < 1000; i++) {
        if (b[i] != 'b') {
            k_print("[UTIL_FAIL] heap blocks overlap\n");
            return UTIL$FAIL;
        }
    }

    /* Free the middle block first, then the neighbours: everything must merge again */
    kfree(b);
    char* reuse = kmalloc(512);
    if (reuse != b) {
        k_print("[UTIL_FAIL] freed block was not reused by first fit\n");
        return UTIL$FAIL;
    }
    kfree(reuse);
    kfree(a);
    kfree(c);

    mem$heap_get_stats(&after);
    if (after.hs$blocks != before.hs$blocks || after.hs$used != before.hs$used) {
        k_print("[UTIL_FAIL] heap did not coalesce: %u blocks, %u bytes used\n",
                (uint32_t)after.hs$blocks, (uint32_t)after.hs$used);
        return UTIL$FAIL;
    }
    return UTIL$SUCCESS;
}
