#include "kio.h"
#include "idt.h"

static bool boot_successful = false;
void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();
	k_print("Setting up kernel...\n");
	asm("int $4");	
	boot_successful = true;
	if (boot_successful){
		k_print("Kernel is ready\n");
	} else {
		k_print("Kernel failed to set up\n");	
	}
	return;
}
