#!/bin/sh
# Tests the kernel's network stack against a real Linux network stack, without
# root: an unprivileged user + network namespace gets a private tap device that
# QEMU attaches to, and the usual Linux tools talk to the kernel through it.
#
#   Linux (namespace)  tap0 10.0.9.1/24  <---->  eth0 10.0.9.2/24  LazyDaysOS
#
# Kernel serial output goes to qemu_net.log, traffic to qemu_tap.pcap.

KERNEL=${1:-LazyDaysOS.bin}
HOST_IP=10.0.9.1
GUEST_IP=10.0.9.2
GUEST_MAC=52:54:00:12:34:56
LOG=qemu_net.log
PCAP=qemu_tap.pcap

if [ "$LAZY_IN_NETNS" != 1 ]; then
    if ! unshare --user --map-root-user --net true 2>/dev/null; then
        echo "[SKIP] unprivileged network namespaces are not available; network checks not run"
        exit 0
    fi
    LAZY_IN_NETNS=1 unshare --user --map-root-user --net sh "$0" "$KERNEL"
    status=$?

    # Outside the namespace: tcpdump drops privileges to a user that doesn't exist in there
    if command -v tcpdump >/dev/null; then
        frames=$(tcpdump -nn -r $PCAP 2>/dev/null | wc -l)
        bad=$(tcpdump -nn -vv -r $PCAP 2>/dev/null | grep -ciE 'bad cksum|wrong|incorrect')
        if [ "$frames" -eq 0 ]; then
            echo "[ERROR] no frames captured in $PCAP"
            status=1
        elif [ "$bad" -ne 0 ]; then
            echo "[ERROR] tcpdump reports $bad checksum errors (see $PCAP)"
            status=1
        else
            echo "[SUCCE] tcpdump verified checksums in $frames captured frames"
        fi
    fi
    exit $status
fi

# ---- inside the namespace from here on ----

ip link set lo up
ip tuntap add dev tap0 mode tap
ip addr add $HOST_IP/24 dev tap0
ip link set tap0 up

rm -f $LOG $PCAP
qemu-system-i386 -kernel "$KERNEL" -append "eth0=$GUEST_IP/24" \
    -display none -no-reboot -serial file:$LOG \
    -netdev tap,id=t0,ifname=tap0,script=no,downscript=no -device e1000,netdev=t0 \
    -object filter-dump,id=d0,netdev=t0,file=$PCAP &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null' EXIT

waited=0
until grep -q 'getting kernel ready' $LOG 2>/dev/null; do
    waited=$((waited + 1))
    if [ $waited -gt 150 ]; then
        echo "[ERROR] kernel did not finish booting within 15 s"
        cat $LOG
        exit 1
    fi
    sleep 0.1
done

# QEMU's e1000 model holds received frames for 1 s after the driver enables
# receive; let that pass so it doesn't show up in round trip times
sleep 1.2

FAILED=0
pass() { echo "[SUCCE] $1"; }
fail() { echo "[ERROR] $1"; FAILED=1; }

# $1 description, $2 expected loss ("0%" or "100%"), rest: ping arguments
check_ping() {
    desc=$1; expected=$2; shift 2
    out=$(ping "$@" 2>&1)
    summary=$(echo "$out" | grep -E 'packet loss')
    rtt=$(echo "$out" | grep -oE 'rtt [^=]+= [^ ]+ ms')
    if echo "$summary" | grep -q " $expected packet loss"; then
        pass "$desc: $summary${rtt:+, $rtt}"
    else
        fail "$desc: expected $expected loss, got: ${summary:-$out}"
    fi
}

check_ping "ping 5 x 56 bytes" "0%" -c 5 -i 0.2 -W 3 $GUEST_IP
check_ping "ping 1472 bytes (largest unfragmented)" "0%" -c 3 -i 0.2 -s 1472 -M do -W 3 $GUEST_IP
check_ping "burst of 1000 pings, 1 ms apart" "0%" -q -c 1000 -i 0.001 -W 1 $GUEST_IP
check_ping "broadcast ping is ignored" "100%" -b -c 2 -i 0.2 -W 1 10.0.9.255
check_ping "unassigned address gets no reply" "100%" -c 2 -i 0.2 -W 1 10.0.9.3

neigh=$(ip neigh show $GUEST_IP dev tap0)
if echo "$neigh" | grep -qi "lladdr $GUEST_MAC"; then
    pass "Linux resolved $GUEST_IP to $GUEST_MAC via ARP"
else
    fail "ARP entry for $GUEST_IP: '${neigh}'"
fi

kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
exit $FAILED
