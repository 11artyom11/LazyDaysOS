#include "kio.h"
#include "serial.h"
#include "idt.h"
#include "util.h"
#include "panic.h"
#include "net/netdev.h"
#include <kern_powerup.h>
#include <multiboot.h>

bool input_allowed = false;

void kernel_main(uint32_t magic, const multiboot_info_t* mbi)
{
	/* Initialize terminal interface */
	terminal_initialize();
	serial_initialize();

	if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		k_panic("not loaded by a multiboot bootloader");
	}

	init_idt();

	/* Powerup main kernel modules */
	if (KERN$PWRUP != powerup_kernel_i386(mbi)) {
		k_panic("kernel failed to set up");
	}

	/* IDT, PIC and timer are ready: start taking interrupts */
	asm volatile("sti");

	size_t status = kern$self_test();
	if (status != UTIL$SUCCESS) {
		k_print("[K_FAIL] self tests failed, status:%d\n", status);
		if (kern$test_mode) {
			kern$test_exit(KERN$TEST_FAIL);
		}
		k_panic("self tests failed");
	}
	k_print("[K_SUCC] self tests passed ... Done\n");

	if (kern$test_mode) {
		kern$test_exit(KERN$TEST_PASS);
	}

	k_print("[K_SUCC] getting kernel ready... Done\n");
	input_allowed = true;

	/* Idle: interrupt handlers do the urgent part, received frames are processed here */
	for (;;) {
		netdev_run_once();
	}
}
