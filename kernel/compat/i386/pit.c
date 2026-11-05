#include <stddef.h>
#include <pit.h>
#include <util.h>
#include <kio.h>
#include "../../hwio.h"
#include "../../panic.h"

#define PIT_CHANNEL0_PORT   0x40
#define PIT_COMMAND_PORT    0x43
#define PIT_BASE_HZ         1193182

/* channel 0, lobyte/hibyte access, mode 3 (square wave), binary */
#define PIT_CMD_CH0_RATE    0x36

#define EFLAGS_IF           0x200

static volatile uint32_t pit_ticks = 0;
static uint32_t pit_hz = PIT_DEFAULT_HZ;

void pit_initialize(uint32_t hz)
{
	uint32_t divisor = PIT_BASE_HZ / hz;

	pit_hz = hz;
	outb(PIT_COMMAND_PORT, PIT_CMD_CH0_RATE);
	outb(PIT_CHANNEL0_PORT, divisor & 0xFF);
	outb(PIT_CHANNEL0_PORT, (divisor >> 8) & 0xFF);
	k_print("[K_INFO] PIT timer running at %u Hz\n", hz);
}

void pit_tick(void)
{
	pit_ticks++;
}

uint32_t pit_get_ticks(void)
{
	return pit_ticks;
}

uint32_t pit_get_hz(void)
{
	return pit_hz;
}

uint32_t pit_uptime_ms(void)
{
	return pit_ticks * (1000 / pit_hz);
}

void pit_sleep_ms(uint32_t ms)
{
	uint32_t eflags;
	asm volatile("pushf; pop %0" : "=r"(eflags));
	if (!(eflags & EFLAGS_IF)) {
		k_panic("pit_sleep_ms called with interrupts disabled");
	}

	/* Round up to whole ticks, plus one so a partially elapsed tick doesn't count */
	uint32_t wait_ticks = (ms * pit_hz + 999) / 1000 + 1;
	uint32_t start = pit_ticks;
	while (pit_ticks - start < wait_ticks) {
		asm volatile("hlt");
	}
}

/**
 * @brief Checks that IRQ0 is actually delivered and the tick counter advances.
 *
 * @return UTIL$SUCCESS if ticks advanced while sleeping
 * @return UTIL$FAIL otherwise
 */
size_t pit_self_test(void)
{
	uint32_t before = pit_get_ticks();
	pit_sleep_ms(50);
	uint32_t elapsed = pit_get_ticks() - before;

	if (elapsed < (50 * pit_hz) / 1000) {
		k_print("[UTIL_FAIL] timer advanced only %u ticks in 50 ms\n", elapsed);
		return UTIL$FAIL;
	}
	return UTIL$SUCCESS;
}
