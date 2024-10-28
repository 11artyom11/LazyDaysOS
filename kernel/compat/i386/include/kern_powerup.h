#ifndef __KERN_PWRUP_H__
#define __KERN_PWRUP_H__

#include <stdint.h>


#define KERN$PWRUP      1
#define KERN$PWRDOWN    0

uint16_t powerup_kernel_i386();

#endif /* __KERN_PWRUP_H__ */