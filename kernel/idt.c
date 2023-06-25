#include "idt.h"
#include "hwio.h"
#include "kio.h"

extern void idt_buf_drain();

/* Create IDT_LIMIT lenght entry table for interrupts */
static _idt_element_ _idt_[IDT_LIMIT];

void __init_pic__(void)
{
    outb(PIC1, INITIALISE_COMMAND);
    outb(PIC2, INITIALISE_COMMAND);

    outb(PIC1_DATA, PIC1_VECTOR_OFFSET);
    outb(PIC2_DATA, PIC2_VECTOR_OFFSET);

    outb(PIC1_DATA, PIC2_PORT_IN_PIC1);

    outb(PIC2_DATA, PIC2_CASCADED_IDEN);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0x0);
    outb(PIC2_DATA, 0x0);
    return;
}

void __set_entry__(uint8_t _tab_pos, uint32_t IRQAddr, uint16_t _seg_selector, uint8_t _zerob, uint8_t _type_att)
{
    _idt_[_tab_pos].__lower_offset =        IRQAddr & 0xFFFF;
    _idt_[_tab_pos].__segment_selector =    _seg_selector;
    _idt_[_tab_pos].__zero =                _zerob;
    _idt_[_tab_pos].__type_attributes =     _type_att;
    _idt_[_tab_pos].__higher_offset =       IRQAddr >> 16;
    return;
}

void __init_idt__(void)
{
    k_printf("[INFO] Setting up interrupt tables...\n");

    _idt_p_ _idt_ptr_;
    _idt_ptr_.__limit = sizeof(_idt_element_) * IDT_LIMIT - 1; /* Number of bytes of ID table (IDT) */
    _idt_ptr_.__base = (uint32_t)&_idt_; /* Address of the first entry in IDT (the base of the table) */
    k_printf("Sizeof limit %d\n", _idt_ptr_.__limit);
    __init_pic__();

    /* 
        Register all interrupts in here....
        ...
        
     */

    idt_buf_drain();

    return;
}

