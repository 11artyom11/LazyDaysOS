#include "tty.h"
#include "ttyin.h"

uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
	return fg | bg << 4;
}

uint16_t vga_entry(unsigned char uc, uint8_t color)
{
	return (uint16_t) uc | (uint16_t) color << 8;
}

size_t strlen(const char* str)
{
	size_t len = 0;
	while (str[len])
		len++;
	return len;
}

void terminal_initialize(void)
{
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
	/* 0xB8000 is the address where resides video memory for coloured monitors */
	terminal_buffer = (uint16_t*) 0xB8000;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}
}

void terminal_setcolor(uint8_t color)
{
	terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y)
{
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

void terminal_putchar(char c)
{
	if (__NEWLINE == c) {
		terminal_row ++;
		terminal_column = 0;
		return;
	}

	if (BACKSPACE_PRESSED == c) {
		if (terminal_column != 0) {
			terminal_column--;
			terminal_putentryat(BLANK_CHAR, terminal_color, terminal_column, terminal_row);
		}
		return;
	}

	terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
	if (++terminal_column == VGA_WIDTH) {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT)
			terminal_row = 0;
	}
}

void terminal_put_uint(uint32_t d,
						enum INT_BASE base,
							void (*putc)(char))
{
	static const char digits[] = "0123456789abcdef";
	char buf[K_PRINT_MAX_BUF] ;
	char* p = &buf[K_PRINT_MAX_BUF-1];

	do
	{
		*p-- = digits[d % base];
		d/= base;
	} while (d != 0);

	while (++p != &buf[K_PRINT_MAX_BUF])
	{
		(*putc)(*p);
	}
}

void terminal_put_int(int32_t d)
{
	/* Need to check sign bit */
	if (d < 0){
		terminal_putchar('-');
		d = (~d) + 1;
	}
	return terminal_put_uint(d, DECIMAL, terminal_putchar);
}

void terminal_write(const char* data, size_t size)
{
	for (size_t i = 0; i < size; i++)
		terminal_putchar(data[i]);
}

void terminal_writestring(const char* data)
{
	terminal_write(data, strlen(data));
}
