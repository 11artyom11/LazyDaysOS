#ifndef __INTERRUPT_TABLE__
#define __INTERRUPT_TABLE__

#include <stdint.h>

/*
    This struct represents a single entry in interrupt descriptor table (8bytes' length)
    Ref: https://wiki.osdev.org/Interrupt_Descriptor_Table#IDTR
 */
struct _interrupt_descriptor_table_element_ {
    /* TOneverDO add typedefs to uintXX_t's */
    uint16_t __lower_offset;
    uint16_t __segment_selector;
    uint8_t  __res_memory;
    uint8_t  __gate_type:4;
    uint8_t  __zbit:1;
    uint8_t  __dpl:2;
    uint8_t  __present:1;
    uint16_t __higher_offset;
} __attribute__((packed));
typedef struct _interrupt_descriptor_table_element_ _idt_element_;

/* According to LIDT instruction the operand structure has to be in this shape */
struct _interrupt_descriptor_table_pointer_ {
    uint16_t __limit;
    uint32_t __base;
} __attribute__((packed));
typedef struct _interrupt_descriptor_table_pointer_ _idt_p_;


/**
 * @brief Initializes IDT
 */
void __init_idt__(void);


/**
 * @brief Initialize PIC for futher interrupt handler setup
 */
void __init_pic__(void);

#endif /* __INTERRUPT_TABLE__ */