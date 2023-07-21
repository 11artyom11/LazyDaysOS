#ifndef __INTERRUPT_TABLE__
#define __INTERRUPT_TABLE__

/* Remove integer typedefs into separate file */
#include <stdint.h>

/* 
 *
 * The special, CPU-dedicated interrupts are shown below.
 *
 *     0 - Division by zero exception
 *     1 - Debug exception
 *     2 - Non maskable interrupt
 *     3 - Breakpoint exception
 *     4 - 'Into detected overflow'
 *     5 - Out of bounds exception
 *     6 - Invalid opcode exception
 *     7 - No coprocessor exception
 *     8 - Double fault (pushes an error code)
 *     9 - Coprocessor segment overrun
 *     10 - Bad TSS (pushes an error code)
 *     11 - Segment not present (pushes an error code)
 *     12 - Stack fault (pushes an error code)
 *     13 - General protection fault (pushes an error code)
 *     14 - Page fault (pushes an error code)
 *     15 - Unknown interrupt exception
 *     16 - Coprocessor fault
 *     17 - Alignment check exception
 *     18 - Machine check exception
 *     19-31 - Reserved
 *     32-255 - Are free to be used for I/O devices or other interruptable stuff
 *
 * Following are the function call to above 32 interrupt handlers.
 * Extern allows to access ASM from this C code.
 */

/* The processor will sometimes need to signal your kernel. Something major may
 * have happened, such as a divide-by-zero, or a page fault. To do this, it uses
 * the first 32 interrupts. It is therefore doubly important that all of these
 * are mapped and non-NULL - else the CPU will triple-fault and reset.
 *
 *
 * The special, CPU-dedicated interrupts are shown below.
 *
 *     0 - Division by zero exception
 *     1 - Debug exception
 *     2 - Non maskable interrupt
 *     3 - Breakpoint exception
 *     4 - 'Into detected overflow'
 *     5 - Out of bounds exception
 *     6 - Invalid opcode exception
 *     7 - No coprocessor exception
 *     8 - Double fault (pushes an error code)
 *     9 - Coprocessor segment overrun
 *     10 - Bad TSS (pushes an error code)
 *     11 - Segment not present (pushes an error code)
 *     12 - Stack fault (pushes an error code)
 *     13 - General protection fault (pushes an error code)
 *     14 - Page fault (pushes an error code)
 *     15 - Unknown interrupt exception
 *     16 - Coprocessor fault
 *     17 - Alignment check exception
 *     18 - Machine check exception
 *     19-31 - Reserved
 *
 *
 * Following are the function call to above 32 interrupt handlers.
 * Extern allows to access ASM from this C code.
 */
extern void ISR0();
extern void ISR1();
extern void ISR2();
extern void ISR3();
extern void ISR4();
extern void ISR5();
extern void ISR6();
extern void ISR7();
extern void ISR8();
extern void ISR9();
extern void ISR10();
extern void ISR11();
extern void ISR12();
extern void ISR13();
extern void ISR14();
extern void ISR15();
extern void ISR16();
extern void ISR17();
extern void ISR18();
extern void ISR19();
extern void ISR20();
extern void ISR21();
extern void ISR22();
extern void ISR23();
extern void ISR24();
extern void ISR25();
extern void ISR26();
extern void ISR27();
extern void ISR28();
extern void ISR29();
extern void ISR30();
extern void ISR31();

/* Maximum entries allowed in IDT */
#define IDT_LIMIT 256

/* Common Definitions for PIC */
#define PIC1 0x20 /* IO base address for master PIC */
#define PIC2 0xA0 /* IO base address for slave PIC */
#define PIC1_COMMAND PIC1
#define PIC1_DATA (PIC1 + 1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA (PIC2 + 1)


/* reinitialize the PIC controllers, giving them specified vector offsets
 * rather than 8h and 70h, as configured by default
 */
#define INITIALISE_COMMAND 0x11
#define PIC1_VECTOR_OFFSET 0x20 /* re-map to ISR 32 */
#define PIC2_VECTOR_OFFSET 0x28 /* re-map to ISR 40 */
#define PIC2_PORT_IN_PIC1 0x04  /* PIC1's port address for PIC2 - 00000100b */
#define PIC2_CASCADED_IDEN 0x02 /* PIC2's identity is 2 for Master (PIC1)*/
#define ICW4_8086 0x01          /* 8086/88 (MCS-80/85) mode */

/* To start using hardware interrupts Programmable Interrupt Controller (PIC)
 * needs to be configured. The PIC makes it possible to map signals from the
 * hardware to interrupts. The reasons for configuring the PIC are: Remap the
 * interrupts. The PIC uses interrupts 0 - 15 for hardware interrupts by
 * default, which conflicts with the CPU interrupts. Therefore the PIC
 * interrupts must be remapped to another interval. Select which interrupts to
 * receive. Set up the correct mode for the PIC.
 *
 * The hardware interrupts are shown in the table below:
 *
 * PIC1:
 *      0 - Timer
 *      1 - Keyboard
 *      2 - PIC 2
 *      3 - COM 2
 *      4 - COM 1
 *      5 - LPT 2
 *      6 - Floppy disk
 *      7 - LPT 1
 *
 * PIC2:
 *      8 - Real Time Clock
 *      9 - General I/O
 *     10 - General I/O
 *     11 - General I/O
 *     12 - General I/O
 *     13 - Coprocessor
 *     14 - IDE Bus
 *     15 - IDE Bus
 *
 * Following are the function call to above 16 interrupt requests.
 * Extern allows to access ASM from this C code.
 */
extern void IRQ0();
extern void IRQ1();
extern void IRQ2();
extern void IRQ3();
extern void IRQ4();
extern void IRQ5();
extern void IRQ6();
extern void IRQ7();                            
extern void IRQ8();
extern void IRQ9();       /* These are reserved for example for keyboard or mouse usage, exactly what we needed */
extern void IRQ10();      /* These are reserved for example for keyboard or mouse usage, exactly what we needed */
extern void IRQ11();      /* These are reserved for example for keyboard or mouse usage, exactly what we needed */
extern void IRQ12();      /* These are reserved for example for keyboard or mouse usage, exactly what we needed */
extern void IRQ13();
extern void IRQ14();
extern void IRQ15();

/*
    This struct represents a single entry in interrupt descriptor table (8bytes' length)
    Ref: https://wiki.osdev.org/Interrupt_Descriptor_Table#IDTR
 */
struct _interrupt_descriptor_table_element_ {
    /* TOneverDO add typedefs to uintXX_t's */
    uint16_t __lower_offset;    // offset bits [0,15]
    uint16_t __segment_selector;// a code segment selector
    uint8_t  __zero;            // unused, set to 0
    uint8_t  __type_attributes; // gate type, dpl, and p fields
    uint16_t __higher_offset;   // offset bits [16-31]
} __attribute__((packed));
typedef struct _interrupt_descriptor_table_element_ _idt_element_;

/* According to LIDT instruction the operand structure has to be in this shape */
struct _interrupt_descriptor_table_pointer_ {
    uint16_t __limit;   // the maximum length of IDT in bytes
    uint32_t __base;    // the first IDT entry address (longword)
} __attribute__((packed));
typedef struct _interrupt_descriptor_table_pointer_ _idt_p_;


/**
 * @brief Initializes IDT
 */
void __init_idt__(void);

/**
 * @brief Register an interrupt entry to IDT to specific vector number
 * 
 * @param _tab_pos       Position in the IDT
 * @param _lower_offset  Lower 16 bits of the address
 * @param _seg_selector  Code segment selector
 * @param _zerob         Just let it be zero
 * @param _type_att      
 * @param _higher_offset Higher 16 bits of the address
 */
void __set_entry__(uint8_t _tab_pos, uint32_t IRQAddr, uint16_t _seg_selector, uint8_t _type_att);

/**
 * @brief Initialize PIC for futher interrupt handler setup
 */
void __init_pic__(void);

#endif /* __INTERRUPT_TABLE__ */