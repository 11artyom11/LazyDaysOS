#ifndef __E1000_H__
#define __E1000_H__

#include <stddef.h>

/* Intel 8254x (e1000) Ethernet driver: 82540EM (QEMU's default "e1000") and 82545EM */

/* Bring up every supported PCI NIC and register one netdev per port; returns the count */
size_t e1000_initialize(void);

#endif /* __E1000_H__ */
