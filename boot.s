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
.skip 16386 #16KiB
stack_top:


/*As the linker script specifies _start as the entry point to the kernel and the bootloader will jump to this position once the kernel has been loaded. It does'nt make sense to return from this routine, as parent called - bootloader is gone by that time*/

.section .text
.global _start
.type _start, @function
_start:
	mov $stack_top, %esp
	call kernel_main
	cli
1:	hlt
	jmp 1b

.size _start, . - _start
