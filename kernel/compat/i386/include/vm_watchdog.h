#ifndef __VM_WATCHDOG__
#define __VM_WATCHDOG__

#include <stddef.h>
#include <stdint.h>
#include <kio.h>

#define MEM$PAGING_ENABLED 1
#define MEM$PAGING_DISABLED 0

/**
 * @brief This procedure does operations necessary to enable paging mode.
 * 
 * @warning This procedure assumes that protected mode is active.
 * 
 */
uint16_t mem$setup_paging(void);

void pagefault_hander (void);

#endif /* __VM_WATCHDOG__ */