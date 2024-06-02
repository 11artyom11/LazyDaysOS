#include "kio.h"
#include "idt.h"
#include "util.h"

static bool boot_successful = false;
void kernel_main(void)
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();
	k_print("[INFO] Setting up kernel...\n");

	asm("push %eax \n\
		 mov %cr0, %eax");

	register int cr0 asm("eax");
	if ((cr0) & 1) {
		k_print("[INFO] We're now in protected mode cr0: %d \n", cr0);
	} else {
		k_print("[INFO] We're now in real mode cr0: %d \n", cr0);
	}

	boot_successful = true;
	if (boot_successful){
		k_print("[SUCC] Kernel is ready\n");
	} else {
		k_print("[FAIL] Kernel failed to set up\n");
	}

	k_print("Running utility self-test\n");
	size_t status = self_test();
	if (status != 1) {
        k_print("[KFAIL] SELF TEST FAILED\n");
		goto kern_exit;
	}
	k_print("[KSUCC] Utility self test passed\n");



kern_exit:
	asm("int $55"); return;
}
