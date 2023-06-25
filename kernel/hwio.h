#ifndef __HARDWARE_INPUT_OUTPUT__
#define __HARDWARE_INPUT_OUTPUT__
#include <stdint.h>

/* Write a byte length value to specified HW I/O port */
void outb(uint16_t _port, uint8_t _value);

/* Read a byte and return from specified HW I/O port */
uint8_t inb(uint16_t _port);

#endif /*  __HARDWARE_INPUT_OUTPUT__ */
