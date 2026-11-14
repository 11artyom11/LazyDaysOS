/*
    What the network stack needs from the rest of the kernel, for a Linux process:
    a fake clock, a heap on malloc that knows which blocks belong to the boot
    state, console output, deterministic "entropy" and the CPU-load hooks.
*/

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/heap.h"
#include "../kernel/cpuload.h"
#include "../kernel/random.h"
#include <pit.h>

#if defined(__SANITIZE_ADDRESS__)
#define FUZZ_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FUZZ_ASAN 1
#endif
#endif
#ifdef FUZZ_ASAN
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(p, n) ((void)(p), (void)(n))
#define ASAN_UNPOISON_MEMORY_REGION(p, n) ((void)(p), (void)(n))
#endif

uint32_t system_ready_ms = 0;

/* ---- console ---- */

size_t k_print(char* format, ...)
{
    static int verbose = -1;
    if (verbose < 0) {
        verbose = getenv("FUZZ_LOG") != NULL;
    }
    if (verbose) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);       /* the kernel's formats (%u %d %x %s %c) are printf's */
        va_end(ap);
    }
    return 0;
}

/* ---- clock ---- */

uint32_t fuzz_now_ms = 0;

void pit_initialize(uint32_t hz) { (void)hz; }
void pit_tick(void) {}
uint32_t pit_get_ticks(void) { return fuzz_now_ms / 10; }
uint32_t pit_get_hz(void) { return 100; }
uint32_t pit_uptime_ms(void) { return fuzz_now_ms; }
void pit_sleep_ms(uint32_t ms) { fuzz_now_ms += ms; }
size_t pit_self_test(void) { return 1; }

/* ---- CPU load: no idle time to measure here ---- */

void cpu_irq_disable(void) {}
void cpu_irq_enable(void) {}
void cpu_idle_wait(void) {}
void cpu_load_tick(uint32_t now_ms) { (void)now_ms; }
uint32_t cpu_load_busy_permille(void) { return 0; }
uint32_t cpu_load_idle_ms(void) { return 0; }
size_t cpu_load_self_test(void) { return 1; }

/* ---- entropy: deterministic, so every run of an input behaves the same ---- */

static uint32_t fuzz$cycles = 0x12345678;
static uint32_t fuzz$cycles_saved;

uint32_t random_arch_cycles(void)
{
    return fuzz$cycles += 7919;
}

size_t random_arch_seed(uint32_t* out, size_t words)
{
    for (size_t i = 0; i < words; i++) {
        out[i] = 0x9E3779B9u * (uint32_t)(i + 1);
    }
    return words;
}

/* ---- heap ---- */

struct fuzz$block
{
    void* ptr;
    size_t size;
    void* caller;
    uint32_t born_ms;                   /* fuzz clock at allocation */
    bool boot;                          /* allocated before the snapshot: part of the boot state */
    bool freed;                         /* a boot block the input freed (kept, poisoned) */
    void* saved;                        /* boot blocks: contents at snapshot time */
};

static struct fuzz$block* fuzz$blocks = NULL;
static size_t fuzz$count = 0, fuzz$cap = 0;

static struct fuzz$block* fuzz$$find(void* ptr)
{
    for (size_t i = 0; i < fuzz$count; i++) {
        if (fuzz$blocks[i].ptr == ptr) {
            return &fuzz$blocks[i];
        }
    }
    return NULL;
}

void* kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;                    /* like the kernel's heap */
    }
    void* p = malloc(size);
    if (!p) {
        return NULL;
    }
    if (fuzz$count == fuzz$cap) {
        fuzz$cap = fuzz$cap ? fuzz$cap * 2 : 64;
        fuzz$blocks = realloc(fuzz$blocks, fuzz$cap * sizeof(*fuzz$blocks));
    }
    fuzz$blocks[fuzz$count++] = (struct fuzz$block){ .ptr = p, .size = size, .caller = __builtin_return_address(0),
                                                     .born_ms = fuzz_now_ms };
    return p;
}

void kfree(void* ptr)
{
    if (!ptr) {
        return;
    }
    struct fuzz$block* b = fuzz$$find(ptr);
    if (!b || b->freed) {
        fprintf(stderr, "FUZZ: kfree(%p) of a pointer that is not allocated (double free?)\n", ptr);
        abort();
    }
    if (b->boot) {
        b->freed = true;                /* keep the address for the restore; catch later use */
        ASAN_POISON_MEMORY_REGION(ptr, b->size);
        return;
    }
    free(ptr);
    *b = fuzz$blocks[--fuzz$count];
}

uint16_t mem$heap_init(size_t bytes) { (void)bytes; return 1; }
size_t mem$heap_self_test(void) { return 1; }

void mem$heap_get_stats(struct mem$heap_stats* stats)
{
    size_t used = 0;
    for (size_t i = 0; i < fuzz$count; i++) {
        used += fuzz$blocks[i].freed ? 0 : fuzz$blocks[i].size;
    }
    stats->hs$size = MEM$HEAP_DEFAULT_SIZE;
    stats->hs$used = used;
    stats->hs$free = MEM$HEAP_DEFAULT_SIZE - used;
    stats->hs$blocks = fuzz$count;
}

/* Everything allocated so far belongs to the boot state (and so does the cycle counter) */
void fuzz_heap_snapshot(void)
{
    fuzz$cycles_saved = fuzz$cycles;
    for (size_t i = 0; i < fuzz$count; i++) {
        struct fuzz$block* b = &fuzz$blocks[i];
        b->boot = true;
        b->saved = malloc(b->size);
        memcpy(b->saved, b->ptr, b->size);
    }
}

/* Blocks allocated after boot that are at least min_age_ms old and still held; prints them */
size_t fuzz_heap_outstanding(uint32_t min_age_ms)
{
    size_t n = 0;
    for (size_t i = 0; i < fuzz$count; i++) {
        struct fuzz$block* b = &fuzz$blocks[i];
        if (!b->boot && fuzz_now_ms - b->born_ms >= min_age_ms) {
            fprintf(stderr, "FUZZ:   %zu bytes allocated by %p %u s ago still held\n", b->size, b->caller,
                    (fuzz_now_ms - b->born_ms) / 1000);
            n++;
        }
    }
    return n;
}

/* Free what the input allocated, give boot blocks back their boot contents */
void fuzz_heap_restore(void)
{
    fuzz$cycles = fuzz$cycles_saved;
    for (size_t i = 0; i < fuzz$count;) {
        struct fuzz$block* b = &fuzz$blocks[i];
        if (!b->boot) {
            free(b->ptr);
            *b = fuzz$blocks[--fuzz$count];
            continue;
        }
        if (b->freed) {
            ASAN_UNPOISON_MEMORY_REGION(b->ptr, b->size);
            b->freed = false;
        }
        memcpy(b->ptr, b->saved, b->size);
        i++;
    }
}
