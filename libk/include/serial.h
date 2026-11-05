/* Polled output to the first 16550 UART (COM1) */
#ifndef __SERIAL_LIB__
#define __SERIAL_LIB__

#include <stdbool.h>

#define COM1_PORT 0x3F8

/* Program COM1 for 115200 8N1; returns false if no UART answers */
bool serial_initialize(void);

/* Write one character, translating '\n' to "\r\n". No-op until initialized */
void serial_putchar(char c);

#endif /* __SERIAL_LIB__ */
