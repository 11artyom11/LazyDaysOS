#ifndef __ISR_H__
#define __ISR_H__

#include <stddef.h>
#include <stdint.h>

/* Register state saved by the stubs in idt_hndlr_setup.s, lowest address first */
struct int_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha */
    uint32_t intno;
    uint32_t errcode;                                       /* 0 if the CPU pushes none */
    uint32_t eip, cs, eflags;                               /* pushed by the CPU */
};

void register_routines();
void interrupt_handler(struct int_frame* frame);

/* Checks that an interrupt preserves every general purpose register. Needs interrupts enabled */
size_t isr_self_test(void);

#endif /* __ISR_H__ */
