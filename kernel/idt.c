#include "idt.h"
#include "hwio.h"
#include "isr.h"

extern void idt_buf_drain(uint32_t);


idt_entry_t idt_entries[256];

#define PIC1_COMMAND_PORT           0x20
#define PIC1_DATA_PORT              0x21
#define PIC2_COMMAND_PORT           0xA0
#define PIC2_DATA_PORT              0xA1

enum {
    PIC_ICW1_ICW4           = 0x01,
    PIC_ICW1_SINGLE         = 0x02,
    PIC_ICW1_INTERVAL4      = 0x04,
    PIC_ICW1_LEVEL          = 0x08,
    PIC_ICW1_INITIALIZE     = 0x10
} PIC_ICW1;


// Initialization Control Word 4
// -----------------------------
//  0   uPM     if set, PIC is in 80x86 mode; if cleared, in MCS-80/85 mode
//  1   AEOI    if set, on last interrupt acknowledge pulse, controller automatically performs 
//              end of interrupt operation
//  2   M/S     only use if BUF is set; if set, selects buffer master; otherwise, selects buffer slave
//  3   BUF     if set, controller operates in buffered mode
//  4   SFNM    specially fully nested mode; used in systems with large number of cascaded controllers
//  5-7         reserved, set to 0
enum {
    PIC_ICW4_8086           = 0x1,
    PIC_ICW4_AUTO_EOI       = 0x2,
    PIC_ICW4_BUFFER_MASTER  = 0x4,
    PIC_ICW4_BUFFER_SLAVE   = 0x0,
    PIC_ICW4_BUFFERRED      = 0x8,
    PIC_ICW4_SFNM           = 0x10,
} PIC_ICW4;


#define UNUSED_PORT         0x80

void iowait()
{
    outb(UNUSED_PORT, 0);
}

void init_pic(uint16_t master_offset, uint16_t slave_offset) {
 // initialization control word 1
    outb(PIC1_COMMAND_PORT, PIC_ICW1_ICW4 | PIC_ICW1_INITIALIZE);
    iowait();
    outb(PIC2_COMMAND_PORT, PIC_ICW1_ICW4 | PIC_ICW1_INITIALIZE);
    iowait();

    // initialization control word 2 - the offsets
    outb(PIC1_DATA_PORT, master_offset);
    iowait();
    outb(PIC2_DATA_PORT, slave_offset);
    iowait();

    // initialization control word 3
    outb(PIC1_DATA_PORT, 0x4);             // tell PIC1 that it has a slave at IRQ2 (0000 0100)
    iowait();
    outb(PIC2_DATA_PORT, 0x2);             // tell PIC2 its cascade identity (0000 0010)
    iowait();

    // initialization control word 4
    outb(PIC1_DATA_PORT, PIC_ICW4_8086);
    iowait();
    outb(PIC2_DATA_PORT, PIC_ICW4_8086);
    iowait();

    // clear data registers
    outb(PIC1_DATA_PORT, 0);
    iowait();
    outb(PIC2_DATA_PORT, 0);
    iowait();
}


static void idt_set_gate(uint8_t num, uint32_t addressISR, uint16_t segmentSelector,
                         uint8_t accessGran) {
  idt_entries[num].offset_low = (addressISR & 0xFFFF);
  idt_entries[num].segment_selector = segmentSelector;
  idt_entries[num].alwaysZero = 0;

  idt_entries[num].access_gran = accessGran;

  idt_entries[num].offset_high = addressISR >> 16;
}

void init_idt() {
  idt_ptr_t idt_ptr;
  idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
  idt_ptr.base = (uint32_t)&idt_entries;

  
  init_pic(PIC1_VECTOR_OFFSET, PIC2_VECTOR_OFFSET);

 
  idt_set_gate(0, (uint32_t)ISR0, 0x08, 0x8E);   // ISR 0
  idt_set_gate(1, (uint32_t)ISR1, 0x08, 0x8E);   // ISR 1
  idt_set_gate(2, (uint32_t)ISR2, 0x08, 0x8E);   // ISR 2
  idt_set_gate(3, (uint32_t)ISR3, 0x08, 0x8E);   // ISR 3
  idt_set_gate(4, (uint32_t)ISR4, 0x08, 0x8E);   // ISR 4
  idt_set_gate(5, (uint32_t)ISR5, 0x08, 0x8E);   // ISR 5
  idt_set_gate(6, (uint32_t)ISR6, 0x08, 0x8E);   // ISR 6
  idt_set_gate(7, (uint32_t)ISR7, 0x08, 0x8E);   // ISR 7
  idt_set_gate(8, (uint32_t)ISR8, 0x08, 0x8E);   // ISR 8
  idt_set_gate(9, (uint32_t)ISR9, 0x08, 0x8E);   // ISR 9
  idt_set_gate(10, (uint32_t)ISR10, 0x08, 0x8E); // ISR 10
  idt_set_gate(11, (uint32_t)ISR11, 0x08, 0x8E); // ISR 11
  idt_set_gate(12, (uint32_t)ISR12, 0x08, 0x8E); // ISR 12
  idt_set_gate(13, (uint32_t)ISR13, 0x08, 0x8E); // ISR 13
  idt_set_gate(14, (uint32_t)ISR14, 0x08, 0x8E); // ISR 14
  idt_set_gate(15, (uint32_t)ISR15, 0x08, 0x8E); // ISR 15
  idt_set_gate(16, (uint32_t)ISR16, 0x08, 0x8E); // ISR 16
  idt_set_gate(17, (uint32_t)ISR17, 0x08, 0x8E); // ISR 17
  idt_set_gate(18, (uint32_t)ISR18, 0x08, 0x8E); // ISR 18
  idt_set_gate(19, (uint32_t)ISR19, 0x08, 0x8E); // ISR 19
  idt_set_gate(20, (uint32_t)ISR20, 0x08, 0x8E); // ISR 20
  idt_set_gate(21, (uint32_t)ISR21, 0x08, 0x8E); // ISR 21
  idt_set_gate(22, (uint32_t)ISR22, 0x08, 0x8E); // ISR 22
  idt_set_gate(23, (uint32_t)ISR23, 0x08, 0x8E); // ISR 23
  idt_set_gate(24, (uint32_t)ISR24, 0x08, 0x8E); // ISR 24
  idt_set_gate(25, (uint32_t)ISR25, 0x08, 0x8E); // ISR 25
  idt_set_gate(26, (uint32_t)ISR26, 0x08, 0x8E); // ISR 26
  idt_set_gate(27, (uint32_t)ISR27, 0x08, 0x8E); // ISR 27
  idt_set_gate(28, (uint32_t)ISR28, 0x08, 0x8E); // ISR 28
  idt_set_gate(29, (uint32_t)ISR29, 0x08, 0x8E); // ISR 29
  idt_set_gate(30, (uint32_t)ISR30, 0x08, 0x8E); // ISR 30
  idt_set_gate(31, (uint32_t)ISR31, 0x08, 0x8E); // ISR 31

  
  idt_set_gate(32, (uint32_t)IRQ0, 0x08, 0x8E);  // IRQ 0
  idt_set_gate(33, (uint32_t)IRQ1, 0x08, 0x8E);  // IRQ 1
  idt_set_gate(34, (uint32_t)IRQ2, 0x08, 0x8E);  // IRQ 2
  idt_set_gate(35, (uint32_t)IRQ3, 0x08, 0x8E);  // IRQ 3
  idt_set_gate(36, (uint32_t)IRQ4, 0x08, 0x8E);  // IRQ 4
  idt_set_gate(37, (uint32_t)IRQ5, 0x08, 0x8E);  // IRQ 5
  idt_set_gate(38, (uint32_t)IRQ6, 0x08, 0x8E);  // IRQ 6
  idt_set_gate(39, (uint32_t)IRQ7, 0x08, 0x8E);  // IRQ 7
  idt_set_gate(40, (uint32_t)IRQ8, 0x08, 0x8E);  // IRQ 8
  idt_set_gate(41, (uint32_t)IRQ9, 0x08, 0x8E);  // IRQ 9
  idt_set_gate(42, (uint32_t)IRQ10, 0x08, 0x8E); // IRQ 10
  idt_set_gate(43, (uint32_t)IRQ11, 0x08, 0x8E); // IRQ 11
  idt_set_gate(44, (uint32_t)IRQ12, 0x08, 0x8E); // IRQ 12
  idt_set_gate(45, (uint32_t)IRQ13, 0x08, 0x8E); // IRQ 13
  idt_set_gate(46, (uint32_t)IRQ14, 0x08, 0x8E); // IRQ 14
  idt_set_gate(47, (uint32_t)IRQ15, 0x08, 0x8E); // IRQ 15

  idt_buf_drain((uint32_t)&idt_ptr);
  register_routines();
}

void idt_enable_entry(int interrupt)
{
    FLAG_SET(idt_entries[interrupt].access_gran, IDT_FLAG_PRESENT);
}

void idt_disable_entry(int interrupt)
{
    FLAG_UNSET(idt_entries[interrupt].access_gran, IDT_FLAG_PRESENT);
}