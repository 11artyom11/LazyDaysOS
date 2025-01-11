#include <util.h>
#include <kern_powerup.h>
#include <vm_watchdog.h>
#include <kio.h>

extern bool input_allowed;

uint16_t powerup_kernel_i386()
{
	k_print("[K_INFO] powering up kernel... Done\n");

	asm("push %eax \n\
		 mov %cr0, %eax");

	register int cr0 asm("eax");
	if ((cr0) & 1) {
		k_print("[K_INFO] kernel now in protected mode cr0: %d \n", cr0);
	} else {
		k_print("[K_INFO] kernel now in real mode cr0: %d \n", cr0);
	}

	/* Try to setup memory paging */
	size_t status = 0;
	
	status = mem$setup_paging();
	if (UTIL$SUCCESS == status) {
		k_print("[K_SUCC] switching processor to paging mode... Done\n");
	} else {
		k_print("[K_SUCC] processor failed to switch to paging mode\n");
	}

	k_print("[K_INFO] running utility self-test\n");
	status = util$self_test();
	if (status != UTIL$SUCCESS) {
        k_print("[K_FAIL] utility self test failed, status:%d\n", status);
		goto kern_exit;
	}
	k_print("[K_SUCC] utility self test passed ... Done\n");
	input_allowed = true;
	return KERN$PWRUP;
kern_exit:
	return KERN$PWRDOWN; 
}