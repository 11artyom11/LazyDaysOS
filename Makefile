CC=i686-elf-gcc
ASM=i686-elf-as
AR=i686-elf-ar
MACROS=__LF
KERN_NAME=DudeOS
LIBK_SRC_DIR=libk
LIBK_INC_DIR=$(LIBK_SRC_DIR)/include
KERNEL_SRC_DIR=kernel
INC=-I$(LIBK_INC_DIR)
CFLAGS=-nostdlib -ffreestanding -D $(MACROS) $(INC)

C_LIB_SOURCES=$(LIBK_SRC_DIR)/tty/tty.c
C_LIB_HEADERS=$(LIBK_INC_DIR)/* 

KERNEL_HEADERS=$(KERNEL_SRC_DIR)

.PHONY: all kernel iso_image test 
.SUFFIXES: .o .c .s .a .ld .libk.a

KERN_OBJ=boot.o kernel.o
LIBK_OBJS=tty.o
VPATH=kernel:libk/tty


all: $(KERN_NAME).bin $(KERN_NAME).iso test

$(KERN_NAME).bin: $(KERN_OBJ) $(KERNEL_SRC_DIR)/compat/i386/linker.ld libk.a
	$(CC) -T $(KERNEL_SRC_DIR)/compat/i386/linker.ld  $(CFLAGS) -O2  boot.o kernel.o libk.a -lgcc -o $@


libk.a: $(LIBK_OBJS)
	$(AR) rcs $@ $<

.s.o:
	$(ASM) $< -o $@

.c.o:
	$(CC) $(CFLAGS) $< -c -o $@

$(KERN_NAME).iso: $(KERN_NAME).bin
	mkdir -p isodir/boot/grub
	cp $< isodir/boot/$<
	sh gen_grubcfg.sh isodir/boot/grub/grub.cfg $(KERN_NAME)
	grub-mkrescue -o $@ isodir
	cp $(KERN_NAME).iso isodir/boot/$@
	rm $<
	rm $@

clean:
	rm -f *.o
	rm -f *.bin
	rm -f -r ./isodir
	rm -f *.a
	
test:
	sh test_multiboot.sh ./isodir/boot/$(KERN_NAME).bin
