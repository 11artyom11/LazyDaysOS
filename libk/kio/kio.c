#include "kio.h"
#include <stdarg.h>

size_t k_printf(char* format, ...)
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
				terminal_putchar('%');
			} else if (*t_format == 'c') {
				char char_to_print = (char)va_arg(argp, int);
				terminal_putchar(char_to_print);
			} else if (*t_format == 'u') {
				uint32_t uint_to_print = va_arg(argp, uint32_t);
				terminal_put_uint(uint_to_print, DECIMAL, terminal_putchar);
			} else if (*t_format == 'd') {
				int32_t  int_to_print = va_arg(argp, int32_t);
				terminal_put_int(int_to_print);
			}
		} else {
			terminal_putchar(*t_format);
		}
		t_format++;
	}
	return t_format - format;
	va_end(argp);
}