# This file initializes the interrupt handlers

.global interrupt_handler
.type interrupt_handler, @function

.macro design_interrupt_no_errcode intno
    .global ISR\intno
    .type ISR\intno, @function
    ISR\intno:
        cli
        pushl $0
        pushl $\intno
        jmp general_interrupt_handler
.endm

.macro design_interrupt_with_errcode intno
    .global ISR\intno
    .type ISR\intno, @function
    ISR\intno:
        cli
        pushl $\intno
        jmp general_interrupt_handler
.endm

.macro design_pic_interrupt intno picno
    .global IRQ\picno
    .type IRQ\picno, @function
    IRQ\picno:
        cli
        pushl $0
        pushl $\intno
        jmp general_interrupt_handler
.endm

design_interrupt_no_errcode 0
design_interrupt_no_errcode 1
design_interrupt_no_errcode 2
design_interrupt_no_errcode 3
design_interrupt_no_errcode 4
design_interrupt_no_errcode 5
design_interrupt_no_errcode 6
design_interrupt_no_errcode 7
design_interrupt_with_errcode 8 #These ones are with error code GI/O
design_interrupt_no_errcode 9
design_interrupt_with_errcode 10 #These ones are with error code GI/O
design_interrupt_with_errcode 11 #These ones are with error code GI/O
design_interrupt_with_errcode 12 #These ones are with error code GI/O
design_interrupt_with_errcode 13 #These ones are with error code GI/O
design_interrupt_with_errcode 14 #These ones are with error code GI/O
design_interrupt_no_errcode 15
design_interrupt_no_errcode 16
design_interrupt_with_errcode 17 #These ones are with error code GI/O
design_interrupt_no_errcode 18
design_interrupt_no_errcode 19
design_interrupt_no_errcode 20
design_interrupt_with_errcode 21 #These ones are with error code GI/O
design_interrupt_no_errcode 22
design_interrupt_no_errcode 23
design_interrupt_no_errcode 24
design_interrupt_no_errcode 25
design_interrupt_no_errcode 26
design_interrupt_no_errcode 27
design_interrupt_no_errcode 28
design_interrupt_with_errcode 29 #These ones are with error code GI/O
design_interrupt_with_errcode 30 #These ones are with error code GI/O
design_interrupt_no_errcode 31
# done with interrupts, now generate IRQ intances

design_pic_interrupt 32, 0
design_pic_interrupt 33, 1
design_pic_interrupt 34, 2
design_pic_interrupt 35, 3
design_pic_interrupt 36, 4
design_pic_interrupt 37, 5
design_pic_interrupt 38, 6
design_pic_interrupt 39, 7
design_pic_interrupt 40, 8
design_pic_interrupt 41, 9
design_pic_interrupt 42, 10
design_pic_interrupt 43, 11
design_pic_interrupt 44, 12
design_pic_interrupt 45, 13
design_pic_interrupt 46, 14
design_pic_interrupt 47, 15
# done with IRQ s too




/*
    Stack layout on entry (see struct int_frame in isr.h):
    [pusha regs][intno][errcode][eip][cs][eflags]
    The frame pointer is passed to interrupt_handler(struct int_frame*).
*/
general_interrupt_handler:
    pusha
    push %esp
    call interrupt_handler
    add $4, %esp
    popa
    add $8, %esp            # drop intno and errcode
    iret
