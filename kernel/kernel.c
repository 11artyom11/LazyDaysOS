#include "kio.h"
#include "idt.h"

static bool boot_successful = false;
void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	__init_idt__();
	
	k_printf("Setting up kernel...\n");	

	boot_successful = true;
	if (boot_successful){
		k_printf("Kernel is ready");
	} else {
		k_printf("Kernel failed to set up");	
	}
	
}
