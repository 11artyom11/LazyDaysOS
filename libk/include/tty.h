#ifndef __TTYO_LIB__
#define __TTYO_LIB__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vgadef.h"
 
/* Check if the compiler is targeting the wrong operating system. */
#if defined(__linux__)
#error "Cross-compiler is not set up, ELF image is not fair"
#endif
 
/* This will only work for the 32-bit ix86 targets. */
#if !defined(__i386__)
#error "This project needs to be compiled with a ix86-elf compiler"
#endif

#ifdef __CRLF
	#define __NEWLINE '\r\n'
#else /* define \n as line feed by default */
	#define __NEWLINE '\n'
#endif

#define K_PRINT_MAX_BUF (sizeof (long int) * 8)

enum INT_BASE 
{
	BINARY = 2,
	OCTAL = 8,
	DECIMAL = 10,
	HEXADECIMAL = 16
};

size_t strlen(const char* str);

void terminal_initialize(void);
 
void terminal_setcolor(uint8_t color);
 
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);

void terminal_putchar(char c);

void terminal_put_uint(uint32_t d, enum INT_BASE base, void (*putc) (char));

void terminal_put_int(int32_t d);
 
void terminal_write(const char* data, size_t size) ;
 
void terminal_writestring(const char* data);

 
#endif //__TTYO_LIB__