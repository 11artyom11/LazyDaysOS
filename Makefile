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

all: kernel iso_image test

kernel: boot.o kernel.o $(KERNEL_SRC_DIR)/compat/i386/linker.ld libk.a
	$(CC) -T $(KERNEL_SRC_DIR)/compat/i386/linker.ld -o $(KERN_NAME).bin $(CFLAGS) -O2  boot.o kernel.o libk.a -lgcc

boot.o: $(KERNEL_ASM_SOURCES)
	$(ASM) $(KERNEL_ASM_SOURCES) -o boot.o

kernel.o: $(KERNEL_C_SOURCES) $(KERNEL_HEADERS)
	$(CC) $(CFLAGS) $(KERNEL_C_SOURCES) -c -o  kernel.o

libk.a: $(C_LIB_SOURCES) $(C_LIB_HEADERS)
	$(CC) $(CFLAGS)  $(C_LIB_SOURCES) -fpic  -c -o libk.a
	
iso_image: 
	mkdir -p isodir/boot/grub
	cp $(KERN_NAME).bin isodir/boot/$(KERN_NAME).bin
	sh gen_grubcfg.sh isodir/boot/grub/grub.cfg $(KERN_NAME)
	grub-mkrescue -o $(KERN_NAME).iso isodir
	cp $(KERN_NAME).iso isodir/boot/$(KERN_NAME).iso
	rm $(KERN_NAME).bin
	rm $(KERN_NAME).iso

clean:
	rm -f *.o
	rm -f *.bin
	rm -f -r ./isodir
	rm -f *.a
	
test:
	sh test_multiboot.sh ./isodir/boot/$(KERN_NAME).bin
