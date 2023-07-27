#include "kio.h"
#include "idt.h"

static bool boot_successful = false;
void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	__init_idt__();
	asm("int $30");
	asm("int $31");
	asm("int $32");
	k_printf("Setting up kernel...\n");	

	boot_successful = true;
	asm("int $33");
	if (boot_successful){
		k_printf("Kernel is ready");
	} else {
		k_printf("Kernel failed to set up");	
	}
	return;
}
