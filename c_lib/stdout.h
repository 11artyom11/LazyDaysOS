#ifndef __C_LIB__
#define __C_LIB__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
 
/* Check if the compiler thinks you are targeting the wrong operating system. */
#if defined(__linux__)
#error "Cross-compiler is not set up, ELF image is not fair"
#endif
 
/* This tutorial will only work for the 32-bit ix86 targets. */
#if !defined(__i386__)
#error "This project needs to be compiled with a ix86-elf compiler"
#endif

#ifdef __CRLF
	#define __NEWLINE '\r\n'
#else /* define \n as line feed by default */
	#define __NEWLINE '\n'
#endif
 
/* Hardware text mode color constants. */
enum vga_color {
	VGA_COLOR_BLACK = 0,
	VGA_COLOR_BLUE = 1,
	VGA_COLOR_GREEN = 2,
	VGA_COLOR_CYAN = 3,
	VGA_COLOR_RED = 4,
	VGA_COLOR_MAGENTA = 5,
	VGA_COLOR_BROWN = 6,
	VGA_COLOR_LIGHT_GREY = 7,
	VGA_COLOR_DARK_GREY = 8,
	VGA_COLOR_LIGHT_BLUE = 9,
	VGA_COLOR_LIGHT_GREEN = 10,
	VGA_COLOR_LIGHT_CYAN = 11,
	VGA_COLOR_LIGHT_RED = 12,
	VGA_COLOR_LIGHT_MAGENTA = 13,
	VGA_COLOR_LIGHT_BROWN = 14,
	VGA_COLOR_WHITE = 15,
};

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;
 
uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg);
 
uint16_t vga_entry(unsigned char uc, uint8_t color);
 
size_t strlen(const char* str);

void terminal_initialize(void);
 
void terminal_setcolor(uint8_t color);
 
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);

void terminal_putchar(char c);
 
void terminal_write(const char* data, size_t size) ;
 
void terminal_writestring(const char* data);
 
#endif //__C_LIB__