#ifndef __CPULOAD_H__
#define __CPULOAD_H__

#include <stddef.h>
#include <stdint.h>

/*
    CPU load accounting. The kernel has a single idle point: the network loop
    sleeping until the next interrupt (cpu_idle_wait). Time spent there is idle;
    everything else (packet processing, protocol timers, interrupt handlers that
    run while the loop is working) is busy. One-second windows give the busy
    share that /api/stats reports.

    Generic interface; the implementation is architecture-specific (i386: the
    time-stamp counter around "sti; hlt", kernel/compat/i386/cpuload.c), so code
    under kernel/net/ can sleep and disable interrupts without inline assembly.
*/

/* Disable / enable interrupts, e.g. around a check-then-sleep */
void cpu_irq_disable(void);
void cpu_irq_enable(void);

/* Call with interrupts disabled: enables them and sleeps until the next interrupt,
   atomically, so an interrupt arriving right after the caller's check still wakes it.
   The time asleep is counted as idle. */
void cpu_idle_wait(void);

/* Every network tick (100 ms): closes the one-second measurement windows */
void cpu_load_tick(uint32_t now_ms);

/* Busy share of the last complete one-second window, 0-1000 (0 until the first window ends) */
uint32_t cpu_load_busy_permille(void);

/* Time spent idle since the first tick, in milliseconds */
uint32_t cpu_load_idle_ms(void);

size_t cpu_load_self_test(void);

#endif /* __CPULOAD_H__ */
