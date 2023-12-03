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

.global gdt_buf_drain
.type gdt_buf_drain, @function

gdt_buf_drain:
    lgdt (%eax)
    ret
