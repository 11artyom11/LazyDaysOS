#include "../include/ttyin.h"
#include "../../kernel/hwio.h"
#include "../include/kio.h"

uint8_t keyboard_to_ascii(uint8_t key)
{
	if(key == 0x1C) return '\n';
	if(key == 0x39) return ' ';
	if(key == POINT_RELEASED) return '.';
	if(key == SLASH_RELEASED) return '/';
	if(key == ZERO_PRESSED)  return '0';
	if(key == BACKSPACE_PRESSED) return BACKSPACE_PRESSED;
    if(key >= ONE_PRESSED && key <= NINE_PRESSED)
		return _num[key - ONE_PRESSED];
	if(key >= 0x10 && key <= 0x1C)
	{
		return _qwertzuiop[key - 0x10];
	} else if(key >= 0x1E && key <= 0x26)
	{
		return _asdfghjkl[key - 0x1E];
	} else if(key >= 0x2C && key <= 0x32)
	{
		return _yxcvbnm[key - 0x2C];
	}
	return NULL_KEY;
}

void keyboard_read()
{
	uint8_t lastkey = 0;
	lastkey = inb(0x60);
    lastkey = keyboard_to_ascii(lastkey);
    if (lastkey != NULL_KEY) {
        terminal_putchar((char)lastkey);
    }
}
