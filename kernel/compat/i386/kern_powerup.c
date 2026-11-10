#include <util.h>
#include <kern_powerup.h>
#include <vm_watchdog.h>
#include <pmm.h>
#include <pit.h>
#include <kio.h>
#include "../../gdt.h"
#include "../../cpuload.h"
#include "../../heap.h"
#include "../../hwio.h"
#include "../../isr.h"
#include "../../net/netdev.h"
#include "../../net/net.h"
#include "../../cmdline.h"
#include <pci.h>
#include <e1000.h>

#define QEMU_DEBUG_EXIT_PORT 0xF4

bool kern$test_mode = false;

uint16_t powerup_kernel_i386(const multiboot_info_t* mbi)
{
	k_print("[K_INFO] powering up kernel... Done\n");

	uint32_t cr0;
	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	if (cr0 & 1) {
		k_print("[K_INFO] kernel now in protected mode cr0: 0x%x \n", cr0);
	} else {
		k_print("[K_INFO] kernel now in real mode cr0: 0x%x \n", cr0);
	}

	cmdline_init((mbi->flags & MULTIBOOT_INFO_CMDLINE) ? (const char*)mbi->cmdline : "");
	k_print("[K_INFO] command line: %s\n", cmdline_raw());
	if (cmdline_has("test")) {
		kern$test_mode = true;
		k_print("[K_INFO] test mode: self tests decide the QEMU exit status\n");
	}

	/* The frame allocator reads multiboot data, so it must run before paging */
	if (mem$pmm_init(mbi) != UTIL$SUCCESS) {
		return KERN$PWRDOWN;
	}

	/* Try to setup memory paging */
	if (UTIL$SUCCESS == mem$setup_paging()) {
		k_print("[K_SUCC] switching processor to paging mode... Done\n");
	} else {
		k_print("[K_FAIL] processor failed to switch to paging mode\n");
		return KERN$PWRDOWN;
	}

	if (mem$heap_init(MEM$HEAP_DEFAULT_SIZE) != UTIL$SUCCESS) {
		return KERN$PWRDOWN;
	}

	pit_initialize(PIT_DEFAULT_HZ);

	pci_initialize();
	k_print("[K_INFO] network: %u e1000 port(s)\n", (uint32_t)e1000_initialize());
	net_initialize();
	return KERN$PWRUP;
}

size_t kern$self_test(void)
{
	size_t status = 0;

	k_print("[K_INFO] running self tests\n");

	if ((status = util$self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test gdt_self_test()\n");
	if ((status = gdt_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test mem$pmm_self_test()\n");
	if ((status = mem$pmm_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test mem$heap_self_test()\n");
	if ((status = mem$heap_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test isr_self_test()\n");
	if ((status = isr_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test pit_self_test()\n");
	if ((status = pit_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test cpu_load_self_test()\n");
	if ((status = cpu_load_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	k_print("[K_INFO] .........  Test net_util_self_test()\n");
	if ((status = net_util_self_test()) != UTIL$SUCCESS) {
		return status;
	}

	/* Needs QEMU user networking, which only the test setup guarantees */
	if (kern$test_mode) {
		k_print("[K_INFO] .........  Test net_self_test()\n");
		if ((status = net_self_test()) != UTIL$SUCCESS) {
			return status;
		}
	}

	return UTIL$SUCCESS;
}

void kern$test_exit(uint8_t status)
{
	outb(QEMU_DEBUG_EXIT_PORT, status);

	/* QEMU stops asynchronously; don't run anything else meanwhile */
	for (;;) {
		asm volatile("cli; hlt");
	}
}
