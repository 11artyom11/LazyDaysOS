#include "kio.h"
#include "idt.h"
#include "util.h"

static bool boot_successful = false;
void kernel_main(void)
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();
	k_print("[K_INFO] powering up kernel... Done\n");

	asm("push %eax \n\
		 mov %cr0, %eax");

	register int cr0 asm("eax");
	if ((cr0) & 1) {
		k_print("[K_INFO] kernel now in protected mode cr0: %d \n", cr0);
	} else {
		k_print("[K_INFO] kernel now in real mode cr0: %d \n", cr0);
	}

	boot_successful = true;
	if (boot_successful){
		k_print("[K_SUCC] getting kernel ready... Done\n");
	} else {
		k_print("[K_FAIL] kernel failed to set up\n");
	}

	k_print("[K_INFO] running utility self-test");
	size_t status = util$self_test();
	if (status != UTIL$SUCCESS) {
        k_print("[K_FAIL] utility self test failed, status:%d\n", status);
		goto kern_exit;
	}
	k_print("... Done\n");
	k_print("[K_SUCC] utility self test passed\n");



kern_exit:
	asm("int $55"); return;
}
