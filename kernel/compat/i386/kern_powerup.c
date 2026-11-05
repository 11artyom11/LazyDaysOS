#include <util.h>
#include <kern_powerup.h>
#include <vm_watchdog.h>
#include <pmm.h>
#include <pit.h>
#include <kio.h>
#include "../../heap.h"
#include "../../hwio.h"
#include "../../isr.h"

#define QEMU_DEBUG_EXIT_PORT 0xF4

bool kern$test_mode = false;

/* True if `word` appears as a whitespace separated token in `cmdline` */
static bool kern$$cmdline_has(const char* cmdline, const char* word)
{
	const char* p = cmdline;
	while (*p) {
		while (*p == ' ') {
			p++;
		}
		const char* w = word;
		const char* t = p;
		while (*w && *t == *w) {
			t++;
			w++;
		}
		if (*w == '\0' && (*t == ' ' || *t == '\0')) {
			return true;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return false;
}

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

	if ((mbi->flags & MULTIBOOT_INFO_CMDLINE) && kern$$cmdline_has((const char*)mbi->cmdline, "test")) {
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
	return KERN$PWRUP;
}

size_t kern$self_test(void)
{
	size_t status = 0;

	k_print("[K_INFO] running self tests\n");

	if ((status = util$self_test()) != UTIL$SUCCESS) {
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
