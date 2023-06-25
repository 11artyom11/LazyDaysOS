.section .text
.global outb
.type outb, @function

outb:
    mov 8(%esp), %al 
    mov 4(%esp), %dx
    out  %al, %dx 
    ret

.global inb
.type inb, @function

inb:
    mov %dx, 4(%esp)
    in %dx, %al
    ret

.global idt_buf_drain
.type idt_buf_drain, @function

idt_buf_drain:
    mov  %eax, 4(%esp)
    lidt (%eax)
    ret
