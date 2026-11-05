#include "kio.h"
#include "serial.h"
#include <stdarg.h>

/* Kernel messages go to the VGA console and are mirrored to COM1 */
static void kio_putchar(char c)
{
	terminal_putchar(c);
	serial_putchar(c);
}

size_t k_print(char* format, ...)
{
	va_list argp;
	va_start(argp, format);
	char* t_format = format;
	while (*t_format != '\0')
	{
		if (*t_format == '%')
		{
			t_format++;
			if (*t_format == '%')
			{
				kio_putchar('%');
			} else if (*t_format == 'c') {
				char char_to_print = (char)va_arg(argp, int);
				kio_putchar(char_to_print);
			} else if (*t_format == 'u') {
				uint32_t uint_to_print = va_arg(argp, uint32_t);
				terminal_put_uint(uint_to_print, DECIMAL, kio_putchar);
			} else if (*t_format == 'x') {
				uint32_t hex_to_print = va_arg(argp, uint32_t);
				terminal_put_uint(hex_to_print, HEXADECIMAL, kio_putchar);
			} else if (*t_format == 'd') {
				int32_t  int_to_print = va_arg(argp, int32_t);
				if (int_to_print < 0) {
					kio_putchar('-');
					terminal_put_uint((uint32_t)(~int_to_print) + 1, DECIMAL, kio_putchar);
				} else {
					terminal_put_uint((uint32_t)int_to_print, DECIMAL, kio_putchar);
				}
			} else if (*t_format == 's') {
				char* string_p = (char*)va_arg(argp, char*);
				while (*string_p) {
					kio_putchar(*string_p++);
				}
			}
		} else {
			kio_putchar(*t_format);
		}
		t_format++;
	}
	va_end(argp);
	return t_format - format;
}
