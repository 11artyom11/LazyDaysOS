#include "kio.h"
#include "idt.h"

static bool boot_successful = false;
void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();
	k_print("[INFO] Setting up kernel...\n");
	k_print("[INFO] We're now in protected mode\n");
	asm("int $4");	
	boot_successful = true;
	if (boot_successful){
		k_print("[SUCC] Kernel is ready\n");
	} else {
		k_print("[FAIL] Kernel failed to set up\n");	
	}
	return;
}
