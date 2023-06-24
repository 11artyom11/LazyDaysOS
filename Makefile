COLOUR_GREEN=\033[0;32m
COLOUR_RED=\033[0;31m
COLOUR_BLUE=\033[0;34m
COLOUR_END=\033[0m

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

	$(ASM) $< -o $@

.c.o:
	$(info )
	@echo "$(COLOUR_GREEN)> Compiling C units $(COLOUR_END)"	

	$(CC) $(CFLAGS) $< -c -o $@

$(KERN_NAME).iso: $(KERN_NAME).bin
	$(info )
	@echo "$(COLOUR_GREEN)> Generating iso image from bin$(COLOUR_END)"	

	mkdir -p isodir/boot/grub
	cp $< isodir/boot/$<
	sh gen_grubcfg.sh isodir/boot/grub/grub.cfg $(KERN_NAME)
	grub-mkrescue -o $@ isodir
	cp $@ isodir/boot/$@
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
# 	rm -f -r $(SYSROOT)
	
install: install-libs install-headers

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

test:	
	$(info )
	@echo "$(COLOUR_GREEN)> Running tests $(LIB_DEST)$(COLOUR_END)"	
	$(info )

	sh test_multiboot.sh ./isodir/boot/$(KERN_NAME).bin
