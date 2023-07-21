#include "idt.h"
#include "hwio.h"
#include "kio.h"

extern void idt_buf_drain(uint32_t _idt_ptr_);

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

void __set_entry__(uint8_t _tab_pos, uint32_t IRQAddr, uint16_t _seg_selector, uint8_t _type_att)
{
    _idt_[_tab_pos].__lower_offset =        IRQAddr & 0xFFFF;
    _idt_[_tab_pos].__segment_selector =    _seg_selector;
    _idt_[_tab_pos].__zero =                0;
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

	__set_entry__(0, (uint32_t)ISR0, 0x08, 0x8E);   // ISR 0
	__set_entry__(1, (uint32_t)ISR1, 0x08, 0x8E);   // ISR 1
	__set_entry__(2, (uint32_t)ISR2, 0x08, 0x8E);   // ISR 2
	__set_entry__(3, (uint32_t)ISR3, 0x08, 0x8E);   // ISR 3
	__set_entry__(4, (uint32_t)ISR4, 0x08, 0x8E);   // ISR 4
	__set_entry__(5, (uint32_t)ISR5, 0x08, 0x8E);   // ISR 5
	__set_entry__(6, (uint32_t)ISR6, 0x08, 0x8E);   // ISR 6
	__set_entry__(7, (uint32_t)ISR7, 0x08, 0x8E);   // ISR 7
	__set_entry__(8, (uint32_t)ISR8, 0x08, 0x8E);   // ISR 8
	__set_entry__(9, (uint32_t)ISR9, 0x08, 0x8E);   // ISR 9
	__set_entry__(10, (uint32_t)ISR10, 0x08, 0x8E); // ISR 10
	__set_entry__(11, (uint32_t)ISR11, 0x08, 0x8E); // ISR 11
	__set_entry__(12, (uint32_t)ISR12, 0x08, 0x8E); // ISR 12
	__set_entry__(13, (uint32_t)ISR13, 0x08, 0x8E); // ISR 13
	__set_entry__(14, (uint32_t)ISR14, 0x08, 0x8E); // ISR 14
	__set_entry__(15, (uint32_t)ISR15, 0x08, 0x8E); // ISR 15
	__set_entry__(16, (uint32_t)ISR16, 0x08, 0x8E); // ISR 16
	__set_entry__(17, (uint32_t)ISR17, 0x08, 0x8E); // ISR 17
	__set_entry__(18, (uint32_t)ISR18, 0x08, 0x8E); // ISR 18
	__set_entry__(19, (uint32_t)ISR19, 0x08, 0x8E); // ISR 19
	__set_entry__(20, (uint32_t)ISR20, 0x08, 0x8E); // ISR 20
	__set_entry__(21, (uint32_t)ISR21, 0x08, 0x8E); // ISR 21
	__set_entry__(22, (uint32_t)ISR22, 0x08, 0x8E); // ISR 22
	__set_entry__(23, (uint32_t)ISR23, 0x08, 0x8E); // ISR 23
	__set_entry__(24, (uint32_t)ISR24, 0x08, 0x8E); // ISR 24
	__set_entry__(25, (uint32_t)ISR25, 0x08, 0x8E); // ISR 25
	__set_entry__(26, (uint32_t)ISR26, 0x08, 0x8E); // ISR 26
	__set_entry__(27, (uint32_t)ISR27, 0x08, 0x8E); // ISR 27
	__set_entry__(28, (uint32_t)ISR28, 0x08, 0x8E); // ISR 28
	__set_entry__(29, (uint32_t)ISR29, 0x08, 0x8E); // ISR 29
	__set_entry__(30, (uint32_t)ISR30, 0x08, 0x8E); // ISR 30
	__set_entry__(31, (uint32_t)ISR31, 0x08, 0x8E); // ISR 31

	
	__set_entry__(32, (uint32_t)IRQ0, 0x08, 0x8E);  // IRQ 0
	__set_entry__(33, (uint32_t)IRQ1, 0x08, 0x8E);  // IRQ 1
	__set_entry__(34, (uint32_t)IRQ2, 0x08, 0x8E);  // IRQ 2
	__set_entry__(35, (uint32_t)IRQ3, 0x08, 0x8E);  // IRQ 3
	__set_entry__(36, (uint32_t)IRQ4, 0x08, 0x8E);  // IRQ 4
	__set_entry__(37, (uint32_t)IRQ5, 0x08, 0x8E);  // IRQ 5
	__set_entry__(38, (uint32_t)IRQ6, 0x08, 0x8E);  // IRQ 6
	__set_entry__(39, (uint32_t)IRQ7, 0x08, 0x8E);  // IRQ 7
	__set_entry__(40, (uint32_t)IRQ8, 0x08, 0x8E);  // IRQ 8
	__set_entry__(41, (uint32_t)IRQ9, 0x08, 0x8E);  // IRQ 9
	__set_entry__(42, (uint32_t)IRQ10, 0x08, 0x8E); // IRQ 10
	__set_entry__(43, (uint32_t)IRQ11, 0x08, 0x8E); // IRQ 11
	__set_entry__(44, (uint32_t)IRQ12, 0x08, 0x8E); // IRQ 12
	__set_entry__(45, (uint32_t)IRQ13, 0x08, 0x8E); // IRQ 13
	__set_entry__(46, (uint32_t)IRQ14, 0x08, 0x8E); // IRQ 14
	__set_entry__(47, (uint32_t)IRQ15, 0x08, 0x8E); // IRQ 15

    k_printf("All is ok\n");
    idt_buf_drain((uint32_t)&_idt_ptr_);

    return;
}

