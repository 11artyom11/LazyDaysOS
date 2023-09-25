#include "kio.h"
#include "hwio.h"
#include "idt.h"
#include "../libk/include/ttyin.h"

#define PIC1_PORT_A 0x20
#define PIC2_PORT_A 0xA0

/* The PIC interrupts have been remapped */
#define PIC1_START_INTERRUPT 0x20
#define PIC2_START_INTERRUPT 0x28
#define PIC2_END_INTERRUPT   PIC2_START_INTERRUPT + 7

#define PIC_ACK     0x20

typedef void (*isr_routine_ptr) (void);

static isr_routine_ptr isr_map[255] = {NULL};

void timer_tick()
{
    k_printf(".");
}

void register_routines()
{
    isr_map[KBD_INTERRUPT] = &keyboard_read;
    isr_map[KBD_INTERRUPT-1] = &timer_tick;
}

void interrupt_handler(int intno)
{
    if (intno >= 40) {
        outb(0xA0, 0x20);
    }    
    if (intno >= 32) {
        outb(0x20, 0x20);
    }
    if (isr_map[intno] != NULL) {
        isr_map[intno]();
    }
    /* Call corresponding isr routine and return wait for new interrupts */
    return;        
}