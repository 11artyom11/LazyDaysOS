CC=i686-elf-gcc
ASM=i686-elf-as
MACROS=__LF
KERN_NAME=DudeOS
LIBK_SRC_DIR=libk
LIBK_INC_DIR=$(LIBK_SRC_DIR)/include
KERNEL_SRC_DIR=kernel
INC=-I$(LIBK_INC_DIR)
CFLAGS=-nostdlib -ffreestanding -D $(MACROS) $(INC)

C_LIB_SOURCES=$(LIBK_SRC_DIR)/tty/tty.c
C_LIB_HEADERS=$(LIBK_INC_DIR)/* 

KERNEL_C_SOURCES=$(KERNEL_SRC_DIR)/kernel.c
KERNEL_ASM_SOURCES=$(KERNEL_SRC_DIR)/boot.s
KERNEL_HEADERS=$(KERNEL_SRC_DIR)

.PHONY: all kernel iso_image test 

all: $(KERN_NAME).bin $(KERN_NAME).iso test

$(KERN_NAME).bin: boot.o kernel.o $(KERNEL_SRC_DIR)/compat/i386/linker.ld libk.a
	$(CC) -T $(KERNEL_SRC_DIR)/compat/i386/linker.ld  $(CFLAGS) -O2  boot.o kernel.o libk.a -lgcc -o $@

boot.o: $(KERNEL_ASM_SOURCES)
	$(ASM) $(KERNEL_ASM_SOURCES) -o $@

kernel.o: $(KERNEL_C_SOURCES) $(KERNEL_HEADERS)
	$(CC) $(CFLAGS) $(KERNEL_C_SOURCES) -c -o $@

libk.a: $(C_LIB_SOURCES) $(C_LIB_HEADERS)
	$(CC) $(CFLAGS)  $(C_LIB_SOURCES) -fpic  -c -o $@
	
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
