#!/bin/sh
# Boots the kernel headless with "test" on its command line. The kernel runs its
# self tests and reports through QEMU's isa-debug-exit device:
#   exit status 1 = all tests passed, 3 = a test failed or the kernel panicked.
# Two e1000 NICs on separate QEMU user networks (10.0.2.0/24 and 10.0.3.0/24);
# eth0 gets its address from QEMU's built-in DHCP server, eth1 is static, and the
# kernel pings each network's gateway.
#
# Two passes:
#   TCG (QEMU's software CPU): serial log in qemu_check.log, traffic in qemu_net0/1.pcap
#   KVM (hardware virtualization, what GNS3 uses): qemu_check_kvm.log. The real CPU
#       enforces things TCG doesn't (segment limits, for one), so bugs can hide from
#       the TCG pass alone. Skipped if /dev/kvm isn't usable; KVM=no skips it too.

KERNEL=${1:-LazyDaysOS.bin}
TIMEOUT=30

# run_pass <name> <log> <extra qemu args...>; prints the result, returns 0 on pass
run_pass() {
    name=$1 log=$2
    shift 2
    timeout $TIMEOUT qemu-system-i386 "$@" \
        -kernel "$KERNEL" \
        -append "test eth0=dhcp eth1=10.0.3.15/24 ping=10.0.2.2 ping=10.0.3.2" \
        -display none -no-reboot \
        -serial file:$log \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1 \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04
    status=$?
    case $status in
        1)   echo "[SUCCE] $name: self tests passed"; return 0 ;;
        3)   echo "[ERROR] $name: self tests failed or kernel panicked (see $log)" ;;
        124) echo "[ERROR] $name: no result after ${TIMEOUT}s (hang?)" ;;
        0)   echo "[ERROR] $name: QEMU exited without a test result (triple fault?)" ;;
        *)   echo "[ERROR] $name: unexpected QEMU exit status $status" ;;
    esac
    return 1
}

result=0
run_pass "TCG" qemu_check.log \
    -object filter-dump,id=d0,netdev=n0,file=qemu_net0.pcap \
    -object filter-dump,id=d1,netdev=n1,file=qemu_net1.pcap > qemu_check.result || result=1
cat qemu_check.log
echo
cat qemu_check.result
rm -f qemu_check.result

if [ "${KVM:-yes}" = no ]; then
    echo "[SKIP] KVM pass: KVM=no"
elif [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    if ! run_pass "KVM" qemu_check_kvm.log -enable-kvm; then
        result=1
        echo "--- qemu_check_kvm.log"
        cat qemu_check_kvm.log
    fi
else
    echo "[SKIP] KVM pass: /dev/kvm is not readable and writable (add yourself to the kvm group)"
fi
exit $result
