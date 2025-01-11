#include "kio.h"
#include "idt.h"
#include "util.h"
#include <kern_powerup.h>
#include <vm_watchdog.h>

static bool boot_successful = false;
static int mem$paging_state = MEM$PAGING_DISABLED;
extern bool input_allowed = false;

void kernel_main(void)
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();

	/* Powerup main kernel modules */
	/* Important note: After completion of this procedure protected mode is activated */
	uint16_t res = powerup_kernel_i386();
	if (KERN$PWRUP == res){
		k_print("[K_SUCC] getting kernel ready... Done\n");
	} else {
		k_print("[K_FAIL] kernel failed to set up\n");
	}

kern_exit:
	asm("int $55"); return;
}