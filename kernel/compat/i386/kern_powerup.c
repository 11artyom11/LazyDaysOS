#include <util.h>
#include <kern_powerup.h>
#include <kio.h>

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


	k_print("[K_INFO] running utility self-test");
	size_t status = util$self_test();
	if (status != UTIL$SUCCESS) {
        k_print("[K_FAIL] utility self test failed, status:%d\n", status);
		goto kern_exit;
	}
	k_print("... Done\n");
	k_print("[K_SUCC] utility self test passed\n");

	return KERN$PWRUP;
kern_exit:
	return KERN$PWRDOWN; 
}