#!/bin/sh
# Boots the kernel headless with "test" on its command line. The kernel runs its
# self tests and reports through QEMU's isa-debug-exit device:
#   exit status 1 = all tests passed, 3 = a test failed or the kernel panicked.
# Serial output (COM1) is saved to qemu_check.log.

KERNEL=${1:-LazyDaysOS.bin}
LOG=qemu_check.log
TIMEOUT=30

timeout $TIMEOUT qemu-system-i386 \
    -kernel "$KERNEL" -append test \
    -display none -no-reboot \
    -serial file:$LOG \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
status=$?

cat $LOG
echo
case $status in
    1)   echo "[SUCCE] self tests passed"; exit 0 ;;
    3)   echo "[ERROR] self tests failed or kernel panicked"; exit 1 ;;
    124) echo "[ERROR] no result after ${TIMEOUT}s (hang?)"; exit 1 ;;
    0)   echo "[ERROR] QEMU exited without a test result (triple fault?)"; exit 1 ;;
    *)   echo "[ERROR] unexpected QEMU exit status $status"; exit 1 ;;
esac
