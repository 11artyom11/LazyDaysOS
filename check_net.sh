#!/bin/sh
# Tests the kernel as a router against real Linux network stacks, without root.
# An unprivileged user + network namespace plays the LAN host; a second network
# namespace inside it plays the upstream (WAN) host. QEMU attaches to a tap
# device in each.
#
#   LAN host                     LazyDaysOS                     WAN host
#   tap0 10.0.9.1/24  <---->  eth0 10.0.9.2/24
#   default via 10.0.9.2      eth1 10.0.8.2/24  <---->  tap1 10.0.8.1/24
#                             route 192.168.50.0/24          lo 192.168.50.1/32
#                               via 10.0.8.1                 route 10.0.9.0/24 via 10.0.8.2
#
# Kernel serial output goes to qemu_net.log, traffic to qemu_lan.pcap / qemu_wan.pcap.

KERNEL=${1:-LazyDaysOS.bin}
LAN_HOST=10.0.9.1
ROUTER_LAN=10.0.9.2
ROUTER_WAN=10.0.8.2
WAN_HOST=10.0.8.1
REMOTE=192.168.50.1
ROUTER_MAC=52:54:00:12:34:56
LOG=qemu_net.log
PCAPS="qemu_lan.pcap qemu_wan.pcap"
KERNEL_ARGS="eth0=$ROUTER_LAN/24 eth1=$ROUTER_WAN/24 route=192.168.50.0/24,$WAN_HOST udp_echo"

if [ "$LAZY_IN_NETNS" != 1 ]; then
    if ! unshare --user --map-root-user --net true 2>/dev/null; then
        echo "[SKIP] unprivileged network namespaces are not available; network checks not run"
        exit 0
    fi
    LAZY_IN_NETNS=1 unshare --user --map-root-user --net sh "$0" "$KERNEL"
    status=$?

    # Outside the namespace: tcpdump drops privileges to a user that doesn't exist in there
    if command -v tcpdump >/dev/null; then
        for pcap in $PCAPS; do
            frames=$(tcpdump -nn -r $pcap 2>/dev/null | wc -l)
            bad=$(tcpdump -nn -vv -r $pcap 2>/dev/null | grep -ciE 'bad cksum|bad udp cksum|wrong|incorrect')
            if [ "$frames" -eq 0 ]; then
                echo "[ERROR] no frames captured in $pcap"
                status=1
            elif [ "$bad" -ne 0 ]; then
                echo "[ERROR] tcpdump reports $bad checksum errors in $pcap"
                status=1
            else
                echo "[SUCCE] tcpdump verified checksums in $frames frames of $pcap"
            fi
        done
    fi
    exit $status
fi

# ---- inside the LAN host's namespace from here on ----

ip link set lo up
ip tuntap add dev tap0 mode tap
ip tuntap add dev tap1 mode tap
ip addr add $LAN_HOST/24 dev tap0
ip link set tap0 up
ip route add default via $ROUTER_LAN

rm -f $LOG $PCAPS
qemu-system-i386 -kernel "$KERNEL" -append "$KERNEL_ARGS" \
    -display none -no-reboot -serial file:$LOG \
    -netdev tap,id=t0,ifname=tap0,script=no,downscript=no -device e1000,netdev=t0 \
    -object filter-dump,id=d0,netdev=t0,file=qemu_lan.pcap \
    -netdev tap,id=t1,ifname=tap1,script=no,downscript=no -device e1000,netdev=t1 \
    -object filter-dump,id=d1,netdev=t1,file=qemu_wan.pcap &
QEMU_PID=$!

# The WAN host: a second network namespace. QEMU has opened tap1 already, so the
# device can move there while QEMU keeps using it.
unshare --net sleep 600 &
WAN_PID=$!
trap 'kill $QEMU_PID $WAN_PID 2>/dev/null' EXIT
wan() { nsenter --target $WAN_PID --net "$@"; }

sleep 0.3
ip link set tap1 netns $WAN_PID
wan ip link set lo up
wan ip addr add $REMOTE/32 dev lo
wan ip addr add $WAN_HOST/24 dev tap1
wan ip link set tap1 up
wan ip route add 10.0.9.0/24 via $ROUTER_WAN

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

# $1 description, $2 expected loss ("0%" or "100%"), rest: ping command
check_ping() {
    desc=$1; expected=$2; shift 2
    out=$("$@" 2>&1)
    summary=$(echo "$out" | grep -E 'packet loss')
    rtt=$(echo "$out" | grep -oE 'rtt [^=]+= [^ ]+ ms')
    if echo "$summary" | grep -q " $expected packet loss"; then
        pass "$desc: $summary${rtt:+, $rtt}"
    else
        fail "$desc: expected $expected loss, got: ${summary:-$out}"
    fi
}

# $1 description, $2 regex the output must match, rest: command
check_output() {
    desc=$1; pattern=$2; shift 2
    out=$("$@" 2>&1)
    match=$(echo "$out" | grep -E -m1 "$pattern")
    if [ -n "$match" ]; then
        pass "$desc: $match"
    else
        fail "$desc: no line matching '$pattern' in: $(echo "$out" | head -3)"
    fi
}

echo "--- the router as a host"
check_ping "ping 5 x 56 bytes" "0%" ping -c 5 -i 0.2 -W 3 $ROUTER_LAN
check_ping "ping 1472 bytes (largest unfragmented)" "0%" ping -c 3 -i 0.2 -s 1472 -M do -W 3 $ROUTER_LAN
check_ping "burst of 1000 pings, 1 ms apart" "0%" ping -q -c 1000 -i 0.001 -W 1 $ROUTER_LAN
check_ping "broadcast ping is ignored" "100%" ping -b -c 2 -i 0.2 -W 1 10.0.9.255
check_ping "unassigned address gets no reply" "100%" ping -c 2 -i 0.2 -W 1 10.0.9.3
check_ping "router's WAN address answers from the LAN" "0%" ping -c 3 -i 0.2 -W 2 $ROUTER_WAN

neigh=$(ip neigh show $ROUTER_LAN dev tap0)
if echo "$neigh" | grep -qi "lladdr $ROUTER_MAC"; then
    pass "Linux resolved $ROUTER_LAN to $ROUTER_MAC via ARP"
else
    fail "ARP entry for $ROUTER_LAN: '${neigh}'"
fi

echo "--- forwarding"
check_ping "LAN host pings WAN host through the router" "0%" ping -c 5 -i 0.2 -W 3 $WAN_HOST
check_output "forwarded packets have their TTL decremented" "ttl=63" ping -c 1 -W 3 $WAN_HOST
check_ping "WAN host pings LAN host through the router" "0%" wan ping -c 5 -i 0.2 -W 3 $LAN_HOST
check_ping "static route: LAN host reaches $REMOTE via $WAN_HOST" "0%" ping -c 3 -i 0.2 -W 3 $REMOTE
check_ping "forwarded burst: 1000 pings, 1 ms apart" "0%" ping -q -c 1000 -i 0.001 -W 1 $WAN_HOST

echo "--- ICMP errors"
check_output "TTL 1: router reports time exceeded" "From $ROUTER_LAN.*Time to live exceeded" ping -c 1 -t 1 -W 2 $WAN_HOST
check_output "no route: router reports net unreachable" "From $ROUTER_LAN.*Destination Net Unreachable" ping -c 1 -W 2 10.66.0.1
check_output "no ARP answer: router reports host unreachable" "From $ROUTER_LAN.*Destination Host Unreachable" ping -c 1 -W 6 10.0.8.77

echo "--- UDP"
check_output "UDP echo service answers" "^echoed$" python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
msg = b'LazyDaysOS UDP echo test ' * 40
s.sendto(msg, ('$ROUTER_LAN', 7))
data, peer = s.recvfrom(2048)
print('echoed' if data == msg and peer == ('$ROUTER_LAN', 7) else 'mismatch %r' % (peer,))"
check_output "closed UDP port: router reports port unreachable" "^refused$" python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.connect(('$ROUTER_LAN', 9999)); s.send(b'anyone?')
try:
    s.recv(100); print('answered')
except ConnectionRefusedError:
    print('refused')
except socket.timeout:
    print('timeout')"

# 2000 x 512-byte datagrams LAN -> WAN, paced at roughly 5000/s; all must arrive
wan python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('$WAN_HOST', 5001)); s.settimeout(1.5)
n = 0
try:
    while True:
        s.recv(2048); n += 1
except socket.timeout:
    pass
print(n)" > udp_count.tmp &
LISTENER=$!
sleep 0.3
python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(2000):
    s.sendto(bytes(512), ('$WAN_HOST', 5001))
    if i % 10 == 9:
        time.sleep(0.002)"
wait $LISTENER
received=$(cat udp_count.tmp); rm -f udp_count.tmp
if [ "$received" = 2000 ]; then
    pass "forwarded 2000 UDP datagrams LAN -> WAN without loss"
else
    fail "forwarded UDP: $received of 2000 datagrams arrived"
fi

kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
exit $FAILED
