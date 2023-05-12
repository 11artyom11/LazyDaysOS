CC=i686-elf-gcc
ASM=i686-elf-as
MACROS=__LF
CFLAGS=-nostdlib -ffreestanding -D $(MACROS)
KERN_NAME=DudeOS
C_LIB_SOURCES=c_lib/stdout.c

all: boot_image iso_image test

boot_image: boot.o kernel.o linker.ld c_lib.o
	$(CC) -T linker.ld -o $(KERN_NAME).bin $(CFLAGS) -O2  boot.o kernel.o c_lib.o -lgcc

boot.o:
	$(ASM) boot.s -o boot.o

kernel.o:	
	$(CC) $(CFLAGS) kernel.c -c -o  kernel.o

c_lib.o: 
	$(CC) $(CFLAGS)  c_lib/stdout.c -fpic  -c -o c_lib.o
	
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

test:
	sh test_multiboot.sh ./isodir/boot/$(KERN_NAME).bin
