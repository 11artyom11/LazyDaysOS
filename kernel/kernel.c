#include "kio.h"
#include "idt.h"

static bool boot_successful = false;
void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	init_idt();
	k_printf("Setting up kernel...\n");
	asm("int $4");	
	boot_successful = true;
	if (boot_successful){
		k_printf("Kernel is ready\n");
	} else {
		k_printf("Kernel failed to set up\n");	
	}
	return;
}
