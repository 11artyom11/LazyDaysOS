#include "panic.h"
#include <kio.h>
#include <kern_powerup.h>

#define PF_INTERRUPT 14

void k_panic(const char* msg)
{
	k_panic_frame(msg, NULL);
}

void k_panic_frame(const char* msg, const struct int_frame* frame)
{
	asm volatile("cli");
	terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
	k_print("\n[K_PANIC] %s\n", msg);

	if (frame) {
		k_print("  int=%u err=0x%x eip=0x%x cs=0x%x eflags=0x%x\n",
				frame->intno, frame->errcode, frame->eip, frame->cs, frame->eflags);
		k_print("  eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
				frame->eax, frame->ebx, frame->ecx, frame->edx);
		k_print("  esi=0x%x edi=0x%x ebp=0x%x\n", frame->esi, frame->edi, frame->ebp);
		if (frame->intno == PF_INTERRUPT) {
			uint32_t cr2;
			asm volatile("mov %%cr2, %0" : "=r"(cr2));
			k_print("  faulting address cr2=0x%x\n", cr2);
		}
	}

	if (kern$test_mode) {
		kern$test_exit(KERN$TEST_FAIL);
	}

	for (;;) {
		asm volatile("cli; hlt");
	}
}
