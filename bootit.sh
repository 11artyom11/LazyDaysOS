#!/bin/sh
# Boot the installed kernel in QEMU with one e1000 NIC on QEMU user networking.
# Serial output (COM1) goes to this terminal.
#   -d     wait for gdb on localhost:1234 (see gdbconf / debug.sh)
#   --iso  boot the GRUB ISO instead of loading the kernel directly
FLAGS="-serial stdio -nic user,model=e1000"
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

# Network configuration for the kernel command line (only possible with -kernel)
NETCONF="eth0=10.0.2.15/24 gw=10.0.2.2"
if [ "$MODE" = "-kernel" ]; then
    qemu-system-i386 $FLAGS $MODE $EXECBOOT$EXT -append "$NETCONF"
else
    qemu-system-i386 $FLAGS $MODE $EXECBOOT$EXT
fi
