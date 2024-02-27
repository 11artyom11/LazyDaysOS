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

void register_routines()
{
    k_print("[INFO] Registering routines\n");
    isr_map[KBD_INTERRUPT] = &keyboard_read;
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