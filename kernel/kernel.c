#include "kio.h"
void kernel_main(void) 
{
	const char a = 'c';
	/* Initialize terminal interface */
	terminal_initialize();
	
	k_printf("artyom says hallpo %c::%u::%d", '@', 987, -197);	
}
