#!/bin/sh
# Boots the kernel headless with "test" on its command line. The kernel runs its
# self tests and reports through QEMU's isa-debug-exit device:
#   exit status 1 = all tests passed, 3 = a test failed or the kernel panicked.
# Serial output (COM1) is saved to qemu_check.log.
# Two e1000 NICs on separate QEMU user networks (10.0.2.0/24 and 10.0.3.0/24);
# eth0 gets its address from QEMU's built-in DHCP server, eth1 is static, and the
# kernel pings each network's gateway. Traffic is captured to qemu_net0/1.pcap.

KERNEL=${1:-LazyDaysOS.bin}
LOG=qemu_check.log
TIMEOUT=30

timeout $TIMEOUT qemu-system-i386 \
    -kernel "$KERNEL" \
    -append "test eth0=dhcp eth1=10.0.3.15/24 ping=10.0.2.2 ping=10.0.3.2" \
    -display none -no-reboot \
    -serial file:$LOG \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file=qemu_net0.pcap \
    -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1 \
    -object filter-dump,id=d1,netdev=n1,file=qemu_net1.pcap \
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
