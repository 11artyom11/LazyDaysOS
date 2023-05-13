#include "tty.h"

void kernel_main(void) 
{
	/* Initialize terminal interface */
	terminal_initialize();
	
	/* Newline support is left as an exercise. */
	terminal_writestring("Hello, kernel World!\n");
	terminal_writestring("Hello, Second line!\n barxatnie tyagi");
}
