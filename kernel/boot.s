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
	//0x53f000ff
	//0x11
	// mov   %cr0, %eax
    // and    0xFFFFFE, %eax       //Set the Protected Mode bit
    // mov   %eax, %cr0      //We're now in Protected Mode!
	
	// cli
	// call init_gdt

// reloadSegments:
//     // Reload CS register containing code selector:

    cli
    call safe_switch_protected
    jmp $0x8, $reload_CS
    
.code32
reload_CS:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss
    sti
	call kernel_main
	cli
1:	hlt
	jmp 1b

.section .text
.type safe_switch_protected, @function
safe_switch_protected:
	mov   %cr0, %eax
    or   %eax, 0x1       //Set the Protected Mode bit
    mov   %eax, %cr0      //We're now in Protected Mode!
    ret

.section .text
.type safe_real, @function
safe_switch_real:
    mov %cr0, %eax
    mov %eax, %ebx
    dec %ebx
    and  %ebx, %eax
    mov %eax, %cr0
    ret



.size _start, . - _start
