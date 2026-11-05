# Boot the installed kernel in QEMU. Serial output (COM1) goes to this terminal.
#   -d     wait for gdb on localhost:1234 (see gdbconf / debug.sh)
#   --iso  boot the GRUB ISO instead of loading the kernel directly
FLAGS="-serial stdio"
BINROOT="sysroot/boot/kernel"
EXECBOOT=$BINROOT"/LazyDaysOS"
MODE="-kernel"
EXT=".bin"

for arg in "$@"; do
    case $arg in
        -d)    FLAGS="$FLAGS -s -S" ;;
        --iso) MODE="-cdrom"; EXT=".iso" ;;
    esac
done

qemu-system-i386 $FLAGS $MODE $EXECBOOT$EXT
