COLOUR_GREEN=\033[0;32m
COLOUR_RED=\033[0;31m
COLOUR_BLUE=\033[0;34m
COLOUR_END=\033[0m

CC=i686-elf-gcc -m32
ASM=i686-elf-as --32
AR=i686-elf-ar
MACROS=__LF
KERN_NAME=DudeOS
LIBK_SRC_DIR=libk
LIBK_INC_DIR=$(LIBK_SRC_DIR)/include
KERNEL_SRC_DIR=kernel
INC=-I$(LIBK_INC_DIR)
CFLAGS=-nostdlib -ffreestanding -D $(MACROS) $(INC) -g -O0
BIN_SRC=isodir
BOOT_SRC=$(BIN_SRC)/boot
GRUB_SRC=$(BOOT_SRC)/grub
KERNEL_SRC=$(BOOT_SRC)/bin
SYSROOT=sysroot
DESTDIR=$(SYSROOT)/usr
BOOTDIR=$(SYSROOT)/boot
LIB_DEST=$(DESTDIR)/lib
INC_DEST=$(DESTDIR)/include
GRUB_DEST=$(BOOTDIR)/grub
KERNEL_DEST=$(BOOTDIR)/kernel

.PHONY: all kernel iso_image test 
.SUFFIXES: .o .c .s .a .ld .libk.a

KERN_OBJ=boot.o kernel.o idt.o hwio.o idt_hndlr_setup.o isr.o
LIBK_OBJS=tty.o kio.o
VPATH=kernel:libk/tty:libk/kio

all: $(KERN_NAME).bin

$(KERN_NAME).bin: $(KERN_OBJ) $(KERNEL_SRC_DIR)/compat/i386/linker.ld libk.a
	$(info )
	@echo "$(COLOUR_GREEN)> Linking all together ==> $@ $(COLOUR_END)"	

	$(CC) -T $(KERNEL_SRC_DIR)/compat/i386/linker.ld  $(CFLAGS) -O2 $(KERN_OBJ) libk.a -lgcc -o $@

libk.a: $(LIBK_OBJS)
	$(info )
	@echo "$(COLOUR_GREEN)> Building $@ kernel library $(COLOUR_END)"	

	$(AR) rcs $@ $(LIBK_OBJS)

.s.o:
	$(info )
	@echo "$(COLOUR_GREEN)> Compiling ASM units $(COLOUR_END)"	

	$(ASM) $< -g -o $@

.c.o:
	$(info )
	@echo "$(COLOUR_GREEN)> Compiling C units $(COLOUR_END)"	

	$(CC) $(CFLAGS) $< -c -o $@

$(KERN_NAME).iso: $(KERN_NAME).bin
	$(info )
	@echo "$(COLOUR_GREEN)> Generating iso image from bin$(COLOUR_END)"	

	mkdir -p $(KERNEL_DEST)
	mkdir -p $(GRUB_DEST)

	cp $< $(KERNEL_DEST)/$<
	sh gen_grubcfg.sh $(GRUB_DEST)/grub.cfg $(KERN_NAME) /boot/kernel/$(KERN_NAME).bin
	grub-mkrescue -o $@ $(SYSROOT)
	cp $@ $(KERNEL_DEST)/$@
	rm $<
	rm $@

clean:
	$(info )
	@echo "$(COLOUR_GREEN)> Removing build artefacts$(COLOUR_END)"	

	rm -f *.o
	rm -f *.bin
	rm -f -r ./isodir
	rm -f *.a
#   TOneverTHINK : Clean won't delete sysroot?
	rm -f -r $(SYSROOT)
	
install: install-libs install-headers install-kernel $(KERN_NAME).iso test

install-libs: libk.a
	$(info )
	@echo "$(COLOUR_GREEN)> Installing libraries to $(LIB_DEST)$(COLOUR_END)"	
	
	$(info )
	$(info Installing libs to $(LIB_DEST) ...)
	mkdir -p $(LIB_DEST)
	cp $< --preserve=timestamps	$(LIB_DEST)

install-headers: 
	$(info )
	@echo "$(COLOUR_GREEN)> Installing headers to $(INC_DEST)$(COLOUR_END)"	
	
	mkdir -p $(INC_DEST)
	cp -R --preserve=timestamps libk/include/. $(INC_DEST)

install-grub:
	$(info )
	@echo "$(COLOUR_GREEN)> Installing GRUB files to $(GRUB_DEST)$(COLOUR_END)"	
	mkdir -p $(GRUB_DEST)
	cp -R --preserve=timestamps $(GRUB_SRC)/. $(GRUB_DEST) 

install-kernel:
	$(info )
	@echo "$(COLOUR_GREEN)> Installing kernel binaries to $(GRUB_DEST)$(COLOUR_END)"	
	mkdir -p $(KERNEL_DEST)
	cp -R --preserve=timestamps $(KERN_NAME).bin $(KERNEL_DEST) 

test:	
	$(info )
	@echo "$(COLOUR_GREEN)> Running tests $(COLOUR_END)"	
	$(info )

	sh test_multiboot.sh $(KERNEL_DEST)/$(KERN_NAME).bin
