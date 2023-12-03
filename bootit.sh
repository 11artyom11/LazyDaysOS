FLAGS=
BINROOT="sysroot/boot/kernel"
EXECBOOT=$BINROOT"/LazyDaysOS"

if [ "$1" = "-d" ]; then
    FLAGS=$FLAGS"-s -S"
fi
if [ "$1" = "--iso" ]; then
    FLAGS=$FLAGS" -cdrom"
    EXECBOOT=$EXECBOOT".iso"
else
    FLAGS=$FLAGS" -kernel"
    EXECBOOT=$EXECBOOT".bin"
fi
sudo qemu-system-i386 $FLAGS $EXECBOOT  