#pragma once
#include "kio.h"

#define i686_GDT_CODE_SEGMENT 0x08
#define i686_GDT_DATA_SEGMENT 0x10

void i686_GDT_Initialize();

/* Checks that the kernel code and data segments span all 4 GiB */
size_t gdt_self_test(void);