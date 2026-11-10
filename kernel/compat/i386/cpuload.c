#include "../../cpuload.h"
#include <stdbool.h>
#include <kio.h>
#include <pit.h>
#include <util.h>

/*
    i386 implementation: the time-stamp counter (TSC) is read around "sti; hlt".
    Only ratios of TSC deltas are used (busy share), plus the TSC rate measured
    over the windows themselves for idle_ms, so no calibration step is needed.
    An interrupt that wakes the hlt runs its handler before the second rdtsc, so
    handler time during idle counts as idle; handlers only acknowledge devices.
*/

#define CPU$WINDOW_MS   1000

static uint64_t cpu$idle_cycles = 0;        /* only touched from the main loop */
static uint64_t cpu$window_tsc = 0;
static uint64_t cpu$window_idle = 0;
static uint32_t cpu$window_ms = 0;
static bool cpu$window_open = false;
static uint32_t cpu$busy_permille = 0;
static uint64_t cpu$cycles_per_ms = 0;      /* measured over the last window */

static inline uint64_t cpu$$rdtsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cpu_irq_disable(void)
{
    asm volatile("cli" ::: "memory");
}

void cpu_irq_enable(void)
{
    asm volatile("sti" ::: "memory");
}

void cpu_idle_wait(void)
{
    uint64_t start = cpu$$rdtsc();
    /* sti takes effect after the next instruction, so no interrupt can slip in before hlt */
    asm volatile("sti; hlt" ::: "memory");
    cpu$idle_cycles += cpu$$rdtsc() - start;
}

void cpu_load_tick(uint32_t now_ms)
{
    uint64_t tsc = cpu$$rdtsc();
    if (!cpu$window_open) {
        cpu$window_tsc = tsc;
        cpu$window_idle = cpu$idle_cycles;
        cpu$window_ms = now_ms;
        cpu$window_open = true;
        return;
    }
    uint32_t elapsed_ms = now_ms - cpu$window_ms;
    if (elapsed_ms < CPU$WINDOW_MS) {
        return;
    }
    uint64_t total = tsc - cpu$window_tsc;
    uint64_t idle = cpu$idle_cycles - cpu$window_idle;
    if (total > 0) {
        uint64_t idle_permille = idle * 1000 / total;
        cpu$busy_permille = idle_permille >= 1000 ? 0 : (uint32_t)(1000 - idle_permille);
        cpu$cycles_per_ms = total / elapsed_ms;
    }
    cpu$window_tsc = tsc;
    cpu$window_idle = cpu$idle_cycles;
    cpu$window_ms = now_ms;
}

uint32_t cpu_load_busy_permille(void)
{
    return cpu$busy_permille;
}

uint32_t cpu_load_idle_ms(void)
{
    return cpu$cycles_per_ms ? (uint32_t)(cpu$idle_cycles / cpu$cycles_per_ms) : 0;
}

/**
 * @brief Sleeps through cpu_idle_wait() for 20 timer ticks and checks that
 *        nearly all of that time was counted as idle, then spins for the same
 *        time and checks that none of it was. Needs interrupts enabled.
 *
 * @return UTIL$SUCCESS on success
 * @return UTIL$FAIL otherwise
 */
size_t cpu_load_self_test(void)
{
    uint64_t a = cpu$$rdtsc(), b = cpu$$rdtsc();
    if (b <= a) {
        k_print("[UTIL_FAIL] cpu: the time-stamp counter does not advance\n");
        return UTIL$FAIL;
    }

    uint64_t idle0 = cpu$idle_cycles, t0 = cpu$$rdtsc();
    uint32_t start = pit_get_ticks();
    while (pit_get_ticks() - start < 20) {
        cpu_irq_disable();
        cpu_idle_wait();
    }
    uint64_t total = cpu$$rdtsc() - t0, idle = cpu$idle_cycles - idle0;
    if (idle * 10 < total * 9) {
        k_print("[UTIL_FAIL] cpu: only %u%% of an idle wait was counted as idle\n", (uint32_t)(idle * 100 / total));
        return UTIL$FAIL;
    }

    idle0 = cpu$idle_cycles;
    start = pit_get_ticks();
    while (pit_get_ticks() - start < 5) {
        asm volatile("pause");
    }
    if (cpu$idle_cycles != idle0) {
        k_print("[UTIL_FAIL] cpu: busy time was counted as idle\n");
        return UTIL$FAIL;
    }
    return UTIL$SUCCESS;
}
