.section .text
.global outb
.type outb, @function

outb:
    xorl %eax, %eax
    xorl %edx, %edx
    mov 8(%esp), %al
    mov 4(%esp), %dx
    out  %al, %dx
    ret

.global inb
.type inb, @function

inb:
    xorl %eax, %eax
    xorl %edx, %edx
    mov 4(%esp), %dx
    in %dx, %al
    ret

/* void idt_buf_drain(uint32_t idt_ptr) */
.global idt_buf_drain
.type idt_buf_drain, @function

idt_buf_drain:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

/* void i686_GDT_Load(GDTDescriptor* descriptor, uint16_t codeSegment, uint16_t dataSegment) */
.global i686_GDT_Load
.type i686_GDT_Load, @function

i686_GDT_Load:
    push %ebp
    mov %esp, %ebp

    mov 8(%ebp), %eax
    lgdt (%eax)

    /* Reload data segment registers with dataSegment */
    mov 16(%ebp), %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Reload CS with codeSegment via far return, then return normally */
    movzwl 12(%ebp), %eax
    push %eax
    push $.Lreload_cs
    lret

.Lreload_cs:
    mov %ebp, %esp
    pop %ebp
    ret

