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
SYSROOT=sysroot
DESTDIR=$(SYSROOT)/usr
LIB_DEST=$(DESTDIR)/lib
INC_DEST=$(DESTDIR)/include

.PHONY: all kernel iso_image test 
.SUFFIXES: .o .c .s .a .ld .libk.a

KERN_OBJ=boot.o kernel.o
LIBK_OBJS=tty.o kio.o
VPATH=kernel:libk/tty:libk/kio

all: $(KERN_NAME).bin $(KERN_NAME).iso test

$(KERN_NAME).bin: $(KERN_OBJ) $(KERNEL_SRC_DIR)/compat/i386/linker.ld libk.a
	$(CC) -T $(KERNEL_SRC_DIR)/compat/i386/linker.ld  $(CFLAGS) -O2 $(KERN_OBJ) libk.a -lgcc -o $@

libk.a: $(LIBK_OBJS)
	$(AR) rcs $@ $(LIBK_OBJS)

.s.o:
	$(ASM) $< -o $@

.c.o:
	$(CC) $(CFLAGS) $< -c -o $@

$(KERN_NAME).iso: $(KERN_NAME).bin
	mkdir -p isodir/boot/grub
	cp $< isodir/boot/$<
	sh gen_grubcfg.sh isodir/boot/grub/grub.cfg $(KERN_NAME)
	grub-mkrescue -o $@ isodir
	cp $@ isodir/boot/$@
	rm $<
	rm $@

clean:
	rm -f *.o
	rm -f *.bin
	rm -f -r ./isodir
	rm -f *.a
	rm -f -r sysroot
	
install: install-libs install-headers

install-libs: libk.a
	mkdir -p $(LIB_DEST)
	cp $< --preserve=timestamps	$(LIB_DEST)

install-headers: 
	mkdir -p $(INC_DEST)
	cp -R --preserve=timestamps libk/include/. $(INC_DEST)

test:
	sh test_multiboot.sh ./isodir/boot/$(KERN_NAME).bin
