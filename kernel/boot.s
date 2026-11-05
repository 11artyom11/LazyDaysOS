/*Declare constants for multiboot image header*/
.set ALIGN, 1<<0
.set MEMINFO, 1<<1
.set FLAGS, ALIGN | MEMINFO
.set MAGIC, 0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

/*Declare multiboot kernel header that marks bootloader that this is bootable image*/

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/*Multiboot standard does not specify any stack-related stuff, thus, we need to specify it ourselves*/

.section .bss
.align 16
stack_bottom:
.skip 16384 #16KiB
stack_top:


/*As the linker script specifies _start as the entry point to the kernel and the bootloader will jump to this position once the kernel has been loaded. It does'nt make sense to return from this routine, as parent called - bootloader is gone by that time*/

/*
	GRUB enters here already in 32-bit protected mode with interrupts disabled,
	EAX = multiboot magic and EBX = physical address of the multiboot info.
	Interrupts stay disabled until kernel_main has installed the IDT.
*/
.section .text
.global _start
.type _start, @function
_start:
	cli
	mov $stack_top, %esp

	/* Keep the stack 16-byte aligned at the call to kernel_main */
	sub $8, %esp
	push %ebx						/* multiboot info */
	push %eax						/* multiboot magic */

	call i686_GDT_Initialize
	call kernel_main				/* kernel_main(magic, mbi) is not supposed to return */

	cli
1:	hlt
	jmp 1b

.size _start, . - _start
