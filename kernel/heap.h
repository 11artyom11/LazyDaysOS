#ifndef __HEAP_H__
#define __HEAP_H__

#include <stddef.h>
#include <stdint.h>

#define MEM$HEAP_DEFAULT_SIZE   (16 * 1024 * 1024)  /* room for 8 ports with 256-slot receive rings */
#define MEM$HEAP_ALIGN          16

struct mem$heap_stats
{
    size_t hs$size;             /* total bytes managed, headers included */
    size_t hs$used;             /* bytes handed out to callers */
    size_t hs$free;             /* bytes available in free blocks */
    size_t hs$blocks;           /* number of blocks, used and free */
};

/* Take `bytes` of contiguous physical memory from the frame allocator for the heap */
uint16_t mem$heap_init(size_t bytes);

/* Returns MEM$HEAP_ALIGN-aligned memory, or NULL if size is 0 or nothing fits */
void* kmalloc(size_t size);

/* Panics on pointers that kmalloc did not return and on double frees */
void kfree(void* ptr);

void mem$heap_get_stats(struct mem$heap_stats* stats);

size_t mem$heap_self_test(void);

#endif /* __HEAP_H__ */
