#include "kio.h"
#include "hwio.h"


#define PIC1_PORT_A 0x20
#define PIC2_PORT_A 0xA0

/* The PIC interrupts have been remapped */
#define PIC1_START_INTERRUPT 0x20
#define PIC2_START_INTERRUPT 0x28
#define PIC2_END_INTERRUPT   PIC2_START_INTERRUPT + 7

#define PIC_ACK     0x20

void interrupt_handler(int intno)
{
    if (intno >= 40) {
        outb(0xA0, 0x20);
        k_printf("%d", intno);
    }
    
    if (intno >= 32) {
        outb(0x20, 0x20);
        k_printf("%d", intno);
    }; 
    return;        
}