#!/bin/sh
# Tests the kernel as a router against real Linux network stacks, without root.
# An unprivileged user + network namespace plays the LAN host; a second network
# namespace inside it plays the upstream (WAN) host. QEMU attaches to a tap
# device in each. Every scenario boots the kernel in fresh namespaces:
#
#   LAN host                     LazyDaysOS                     WAN host
#   tap0 10.0.9.1/24  <---->  eth0 10.0.9.2/24
#   default via 10.0.9.2      eth1 10.0.8.2/24  <---->  tap1 10.0.8.1/24
#                                                            lo 192.168.50.1/32
#                                                            route 10.0.9.0/24 via 10.0.8.2
#
#   static  eth1 configured statically, 192.168.50.0/24 via a static route:
#           host behaviour, forwarding, ICMP errors, UDP
#   home    eth1=dhcp from dnsmasq on the WAN host (which reserves 10.0.8.2 and
#           names itself the gateway), the router serves DHCP on the LAN:
#           real udhcpc and crafted DHCP requests against the router's server
#
# Kernel serial output goes to qemu_<scenario>.log, traffic to qemu_<scenario>_{lan,wan}.pcap.

KERNEL=${1:-LazyDaysOS.bin}
SCENARIOS="static home"
LAN_HOST=10.0.9.1
ROUTER_LAN=10.0.9.2
ROUTER_WAN=10.0.8.2
WAN_HOST=10.0.8.1
REMOTE=192.168.50.1
ROUTER_MAC=52:54:00:12:34:56
ROUTER_WAN_MAC=52:54:00:12:34:57

if [ "$LAZY_IN_NETNS" != 1 ]; then
    if ! unshare --user --map-root-user --net true 2>/dev/null; then
        echo "[SKIP] unprivileged network namespaces are not available; network checks not run"
        exit 0
    fi
    status=0
    for scenario in $SCENARIOS; do
        echo "=== scenario: $scenario"
        LAZY_IN_NETNS=1 unshare --user --map-root-user --net sh "$0" "$KERNEL" $scenario || status=1
    done

    # Outside the namespaces: tcpdump drops privileges to a user that doesn't exist in there
    if command -v tcpdump >/dev/null; then
        for scenario in $SCENARIOS; do
            for pcap in qemu_${scenario}_lan.pcap qemu_${scenario}_wan.pcap; do
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
        done
    fi
    exit $status
fi

# ---- inside the LAN host's namespace from here on ----

SCENARIO=$2
LOG=qemu_$SCENARIO.log
TMP=$(mktemp -d)

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

# Wait up to $2 tenths of a second for regex $1 in the kernel log
wait_for_log() {
    waited=0
    until grep -qE "$1" $LOG 2>/dev/null; do
        waited=$((waited + 1))
        if [ $waited -gt $2 ]; then
            return 1
        fi
        sleep 0.1
    done
}

# $1 kernel command line. Sets QEMU_PID, WAN_PID and the wan() helper.
start_topology() {
    ip link set lo up
    ip tuntap add dev tap0 mode tap
    ip tuntap add dev tap1 mode tap
    ip addr add $LAN_HOST/24 dev tap0
    ip link set tap0 up
    ip route add default via $ROUTER_LAN

    rm -f $LOG qemu_${SCENARIO}_lan.pcap qemu_${SCENARIO}_wan.pcap
    qemu-system-i386 -kernel "$KERNEL" -append "$1" \
        -display none -no-reboot -serial file:$LOG \
        -netdev tap,id=t0,ifname=tap0,script=no,downscript=no -device e1000,netdev=t0 \
        -object filter-dump,id=d0,netdev=t0,file=qemu_${SCENARIO}_lan.pcap \
        -netdev tap,id=t1,ifname=tap1,script=no,downscript=no -device e1000,netdev=t1 \
        -object filter-dump,id=d1,netdev=t1,file=qemu_${SCENARIO}_wan.pcap &
    QEMU_PID=$!

    # The WAN host: a second network namespace. QEMU has opened tap1 already, so
    # the device can move there while QEMU keeps using it.
    unshare --net sleep 600 &
    WAN_PID=$!
    trap 'kill $QEMU_PID $WAN_PID $DNSMASQ_PID 2>/dev/null; rm -rf $TMP' EXIT

    sleep 0.3
    ip link set tap1 netns $WAN_PID
    wan ip link set lo up
    wan ip addr add $REMOTE/32 dev lo
    wan ip addr add $WAN_HOST/24 dev tap1
    wan ip link set tap1 up
    wan ip route add 10.0.9.0/24 via $ROUTER_WAN

    if ! wait_for_log 'getting kernel ready' 150; then
        echo "[ERROR] kernel did not finish booting within 15 s"
        cat $LOG
        exit 1
    fi
    # QEMU's e1000 model holds received frames for 1 s after the driver enables
    # receive; let that pass so it doesn't show up in round trip times
    sleep 1.2
}
wan() { nsenter --target $WAN_PID --net "$@"; }

# ---------------------------------------------------------------- static ----

scenario_static() {
    start_topology "eth0=$ROUTER_LAN/24 eth1=$ROUTER_WAN/24 route=192.168.50.0/24,$WAN_HOST udp_echo"

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
print(n)" > $TMP/udp_count &
    listener=$!
    sleep 0.3
    python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(2000):
    s.sendto(bytes(512), ('$WAN_HOST', 5001))
    if i % 10 == 9:
        time.sleep(0.002)"
    wait $listener
    received=$(cat $TMP/udp_count)
    if [ "$received" = 2000 ]; then
        pass "forwarded 2000 UDP datagrams LAN -> WAN without loss"
    else
        fail "forwarded UDP: $received of 2000 datagrams arrived"
    fi
}

# ------------------------------------------------------------------ home ----

# Send one hand-made DHCPREQUEST from the LAN host and print the reply type.
# $1 client MAC, $2 requested address. INIT-REBOOT form: no server id, no ciaddr.
dhcp_probe() {
    python3 - "$1" "$2" <<'PY'
import socket, struct, sys, random
mac = bytes.fromhex(sys.argv[1].replace(':', ''))
requested = socket.inet_aton(sys.argv[2])
xid = random.getrandbits(32)
msg = struct.pack('!BBBBIHHIIII', 1, 1, 6, 0, xid, 0, 0x8000, 0, 0, 0, 0)
msg += mac + bytes(10) + bytes(64 + 128) + struct.pack('!I', 0x63825363)
msg += bytes([53, 1, 3, 50, 4]) + requested + bytes([255])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'tap0')
s.bind(('', 68))
s.settimeout(3)
s.sendto(msg, ('255.255.255.255', 67))
names = {2: 'OFFER', 5: 'ACK', 6: 'NAK'}
try:
    while True:
        reply = s.recv(1500)
        if len(reply) < 240 or struct.unpack('!I', reply[4:8])[0] != xid:
            continue
        opts, i, kind = reply[240:], 0, None
        while i < len(opts) and opts[i] != 255:
            if opts[i] == 0:
                i += 1; continue
            if opts[i] == 53:
                kind = opts[i + 2]
            i += 2 + opts[i + 1]
        print('%s yiaddr=%s' % (names.get(kind, kind), socket.inet_ntoa(reply[16:20])))
        break
except socket.timeout:
    print('no reply')
PY
}

scenario_home() {
    # The upstream: dnsmasq on the WAN host reserves 10.0.8.2 for the router
    UDHCPC_SCRIPT=$TMP/udhcpc.sh
    printf '#!/bin/sh\n[ "$1" = bound ] && echo "bound ip=$ip subnet=$subnet router=$router dns=$dns lease=$lease"\nexit 0\n' > $UDHCPC_SCRIPT
    chmod +x $UDHCPC_SCRIPT

    start_topology "eth0=$ROUTER_LAN/24 eth1=dhcp dhcpd=eth0,10.0.9.100,10.0.9.150 dhcp_lease=600"
    wan dnsmasq --no-daemon --conf-file=/dev/null --user=root --port=0 \
        --interface=tap1 --bind-interfaces --dhcp-authoritative \
        --dhcp-range=10.0.8.100,10.0.8.150,10m --dhcp-host=$ROUTER_WAN_MAC,$ROUTER_WAN \
        --dhcp-option=3,$WAN_HOST --dhcp-option=6,10.0.8.53 \
        --dhcp-leasefile=$TMP/leases --pid-file=$TMP/dnsmasq.pid > $TMP/dnsmasq.log 2>&1 &
    DNSMASQ_PID=$!

    echo "--- DHCP client on the WAN port"
    if wait_for_log 'dhcp: eth1 bound' 250; then
        check_output "router's WAN port got its lease from dnsmasq" \
            "eth1 bound $ROUTER_WAN/24 router $WAN_HOST dns 10.0.8.53" cat $LOG
    else
        fail "router's WAN port got no DHCP lease within 25 s"
    fi
    check_output "dnsmasq acknowledged the router" "DHCPACK\(tap1\) $ROUTER_WAN $ROUTER_WAN_MAC" cat $TMP/dnsmasq.log
    check_ping "WAN host pings the router's DHCP address" "0%" wan ping -c 3 -i 0.2 -W 2 $ROUTER_WAN
    check_ping "default route from DHCP: LAN host reaches $REMOTE" "0%" ping -c 3 -i 0.2 -W 3 $REMOTE

    echo "--- DHCP server on the LAN port"
    check_output "udhcpc gets a lease from the router" \
        "bound ip=10\.0\.9\.1[0-5][0-9] subnet=255\.255\.255\.0 router=$ROUTER_LAN dns=10\.0\.8\.53 lease=600" \
        busybox udhcpc -i tap0 -n -q -f -t 4 -T 2 -s $UDHCPC_SCRIPT
    first=$(busybox udhcpc -i tap0 -n -q -f -t 4 -T 2 -s $UDHCPC_SCRIPT 2>/dev/null | grep -oE 'ip=[0-9.]+')
    second=$(busybox udhcpc -i tap0 -n -q -f -t 4 -T 2 -s $UDHCPC_SCRIPT 2>/dev/null | grep -oE 'ip=[0-9.]+')
    if [ -n "$first" ] && [ "$first" = "$second" ]; then
        pass "the same client gets the same address again ($first)"
    else
        fail "address not stable across requests: '$first' then '$second'"
    fi
    check_output "a requested address outside the pool is not given out" \
        "bound ip=10\.0\.9\.1[0-5][0-9] " busybox udhcpc -i tap0 -n -q -f -t 4 -T 2 -r 10.0.9.200 -s $UDHCPC_SCRIPT
    check_output "INIT-REBOOT for a free pool address: ACK" "^ACK yiaddr=10.0.9.140$" dhcp_probe 02:00:00:00:00:01 10.0.9.140
    check_output "INIT-REBOOT for an address leased to someone else: NAK" "^NAK" dhcp_probe 02:00:00:00:00:02 10.0.9.140
    check_output "INIT-REBOOT for an address on another network: NAK" "^NAK" dhcp_probe 02:00:00:00:00:03 172.16.0.5
}

scenario_$SCENARIO

kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
exit $FAILED
