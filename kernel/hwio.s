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

.global idt_buf_drain
.type idt_buf_drain, @function

idt_buf_drain:
    lidt (%eax)
    ret

.global i686_GDT_Load
.type i686_GDT_Load, @function

i686_GDT_Load:
    // push %ebp
    // mov %esp, %ebp
    cli
    pop %ebx
    xor %ebx, %ebx
    pop %eax
    lgdt (%eax)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov $0x08, %eax
    push %eax
    movl $kernel_main, %edx
    push %edx
    sti
    retf
    // mov %ebp, %esp
    // pop %ebp
