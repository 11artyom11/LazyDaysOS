#ifndef __PIT_H__
#define __PIT_H__

#include <stddef.h>
#include <stdint.h>

#define PIT_DEFAULT_HZ  100

/* Program PIT channel 0 to fire IRQ0 at the given rate */
void pit_initialize(uint32_t hz);

/* IRQ0 routine, registered in isr_map */
void pit_tick(void);

uint32_t pit_get_ticks(void);
uint32_t pit_get_hz(void);
uint32_t pit_uptime_ms(void);

/* Sleep with hlt until at least ms have passed. Interrupts must be enabled */
void pit_sleep_ms(uint32_t ms);

size_t pit_self_test(void);

#endif /* __PIT_H__ */
