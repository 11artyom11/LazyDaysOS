#include "../include/serial.h"
#include "../../kernel/hwio.h"

#define UART_DATA          0    /* data register (divisor low byte while DLAB=1) */
#define UART_INT_ENABLE    1    /* interrupt enable (divisor high byte while DLAB=1) */
#define UART_FIFO_CTRL     2
#define UART_LINE_CTRL     3
#define UART_MODEM_CTRL    4
#define UART_LINE_STATUS   5

#define UART_LCR_DLAB      0x80
#define UART_LCR_8N1       0x03
#define UART_LSR_THR_EMPTY 0x20

static bool serial_ready = false;

bool serial_initialize(void)
{
	outb(COM1_PORT + UART_INT_ENABLE, 0x00);           /* polled mode, no UART interrupts */
	outb(COM1_PORT + UART_LINE_CTRL, UART_LCR_DLAB);
	outb(COM1_PORT + UART_DATA, 0x01);                 /* divisor 1 -> 115200 baud */
	outb(COM1_PORT + UART_INT_ENABLE, 0x00);
	outb(COM1_PORT + UART_LINE_CTRL, UART_LCR_8N1);
	outb(COM1_PORT + UART_FIFO_CTRL, 0xC7);            /* enable + clear FIFOs, 14-byte threshold */

	/* Loopback self-check so we don't spin forever on a missing UART */
	outb(COM1_PORT + UART_MODEM_CTRL, 0x1E);
	outb(COM1_PORT + UART_DATA, 0xAE);
	if (inb(COM1_PORT + UART_DATA) != 0xAE) {
		return false;
	}

	outb(COM1_PORT + UART_MODEM_CTRL, 0x0F);           /* normal operation: DTR, RTS, OUT1, OUT2 */
	serial_ready = true;
	return true;
}

static void serial_putraw(char c)
{
	while ((inb(COM1_PORT + UART_LINE_STATUS) & UART_LSR_THR_EMPTY) == 0)
		;
	outb(COM1_PORT + UART_DATA, (uint8_t)c);
}

void serial_putchar(char c)
{
	if (!serial_ready) {
		return;
	}
	if (c == '\n') {
		serial_putraw('\r');
	}
	serial_putraw(c);
}
