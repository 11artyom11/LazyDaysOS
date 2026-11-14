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
#           names itself the gateway), the router serves DHCP on the LAN and
#           NATs to the WAN (nat=eth1). The WAN host has NO route back to the
#           LAN, like the Internet: DHCP both ways, NAT, firewall, and the
#           HTTP management API (stats, events, block/unblock, error handling)
#   tcp     the kernel's own TCP endpoint (tcp_test: echo on 7, 1 MiB chargen
#           on 19): bulk transfers, many and concurrent connections, then again
#           with 10% packet loss in both directions (netem; ifb for the
#           router -> LAN direction)
#   rip     three routers in a triangle speaking RIPv2, joined by bridges in the
#           LAN host's namespace: convergence, metrics, poisoned reverse, a cut
#           cable (QEMU set_link) and a silent link failure (timeout) rerouted
#           around, forged and malformed RIP refused
#
# Kernel serial output goes to qemu_<scenario>.log, traffic to qemu_<scenario>_{lan,wan}.pcap.

KERNEL=${1:-LazyDaysOS.bin}
SCENARIOS="${SCENARIOS:-static home tcp rip}"
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
                bad=$(tcpdump -nn -vv -r $pcap 2>/dev/null | grep -ciE 'bad cksum|bad udp cksum|incorrect ->|wrong icmp cksum')
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
    if [ "$WAN_ROUTE_BACK" != no ]; then
        wan ip route add 10.0.9.0/24 via $ROUTER_WAN
    fi

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
    start_topology "eth0=$ROUTER_LAN/24 eth1=$ROUTER_WAN/24 route=192.168.50.0/24,$WAN_HOST udp_echo http"

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

    echo "--- management password when none is configured"
    pw=$(grep -oE 'admin password: [a-z2-9]{12}' $LOG | cut -d' ' -f3)
    check_output "a random 12-character password is printed on the console at boot" "^[a-z2-9]{12}$" echo "$pw"
    check_output "the API refuses requests without it and accepts it" "^401 200$" sh -c \
        "curl -s --max-time 5 -o /dev/null -w '%{http_code} ' http://$ROUTER_LAN/api/stats;
         curl -s -u 'admin:$pw' --max-time 5 -o /dev/null -w '%{http_code}' http://$ROUTER_LAN/api/stats"
}

# ------------------------------------------------------------------ home ----

# Send one hand-made DHCPREQUEST from the LAN host and print the reply type.
# $1 client MAC, $2 requested address. INIT-REBOOT form: no server id, no ciaddr.
# Optional $3 ciaddr (and no broadcast flag), $4 source address (must be on tap0);
# with $3 the output also says where the reply was sent ("to=a.b.c.d").
dhcp_probe() {
    python3 - "$@" <<'PY'
import socket, struct, sys, random
mac = bytes.fromhex(sys.argv[1].replace(':', ''))
requested = socket.inet_aton(sys.argv[2])
ciaddr = socket.inet_aton(sys.argv[3]) if len(sys.argv) > 3 else bytes(4)
source = sys.argv[4] if len(sys.argv) > 4 else ''
flags = 0 if len(sys.argv) > 3 else 0x8000
xid = random.getrandbits(32)
msg = struct.pack('!BBBBIHH', 1, 1, 6, 0, xid, 0, flags) + ciaddr + bytes(12)
msg += mac + bytes(10) + bytes(64 + 128) + struct.pack('!I', 0x63825363)
msg += bytes([53, 1, 3, 50, 4]) + requested + bytes([255])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'tap0')
s.setsockopt(socket.IPPROTO_IP, socket.IP_PKTINFO, 1)
s.bind((source, 68))
s.settimeout(3)
s.sendto(msg, ('255.255.255.255', 67))
names = {2: 'OFFER', 5: 'ACK', 6: 'NAK'}
try:
    while True:
        reply, cmsgs, _, _ = s.recvmsg(1500, socket.CMSG_SPACE(12))
        if len(reply) < 240 or struct.unpack('!I', reply[4:8])[0] != xid:
            continue
        opts, i, kind = reply[240:], 0, None
        while i < len(opts) and opts[i] != 255:
            if opts[i] == 0:
                i += 1; continue
            if opts[i] == 53:
                kind = opts[i + 2]
            i += 2 + opts[i + 1]
        to = ''
        if len(sys.argv) > 3:
            # struct in_pktinfo: ifindex, local address, header destination address
            to = ' to=' + next(socket.inet_ntoa(d[8:12]) for lvl, typ, d in cmsgs
                               if lvl == socket.IPPROTO_IP and typ == socket.IP_PKTINFO)
        print('%s yiaddr=%s%s' % (names.get(kind, kind), socket.inet_ntoa(reply[16:20]), to))
        break
except socket.timeout:
    print('no reply')
PY
}

# On the WAN host for $2 seconds: UDP echo on 5002, TCP echo on 8080; logs each peer to $1
wan_echo_server() {
    wan python3 - "$1" "$2" <<'PY' &
import socket, sys, threading, time
log = open(sys.argv[1], 'w', buffering=1)
def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('10.0.8.1', 5002))
    while True:
        data, peer = s.recvfrom(2048)
        log.write('udp %s:%d\n' % peer)
        s.sendto(data, peer)
def tcp_client(c):
    while True:
        data = c.recv(65536)
        if not data:
            break
        c.sendall(data)
    c.close()
def tcp():
    l = socket.socket()
    l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    l.bind(('10.0.8.1', 8080))
    l.listen(4)
    while True:
        c, peer = l.accept()
        log.write('tcp %s:%d\n' % peer)
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()
threading.Thread(target=udp, daemon=True).start()
threading.Thread(target=tcp, daemon=True).start()
time.sleep(float(sys.argv[2]))
PY
}

# From the LAN host: send a UDP datagram from $1:$2 to the WAN echo server, print "reply" if it comes back
lan_udp_roundtrip() {
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.bind(('$1', $2))
s.sendto(b'through the NAT', ('$WAN_HOST', 5002))
try:
    print('reply' if s.recv(100) == b'through the NAT' else 'garbled')
except socket.timeout:
    print('timeout')"
}

scenario_home() {
    # The upstream: dnsmasq on the WAN host reserves 10.0.8.2 for the router
    UDHCPC_SCRIPT=$TMP/udhcpc.sh
    printf '#!/bin/sh\n[ "$1" = bound ] && echo "bound ip=$ip subnet=$subnet router=$router dns=$dns lease=$lease"\nexit 0\n' > $UDHCPC_SCRIPT
    chmod +x $UDHCPC_SCRIPT

    WAN_ROUTE_BACK=no
    ADMIN_PW=correct-horse-42
    export API_AUTH="Basic $(printf 'admin:%s' "$ADMIN_PW" | base64)"
    start_topology "eth0=$ROUTER_LAN/24 eth1=dhcp dhcpd=eth0,10.0.9.100,10.0.9.150 dhcp_lease=600 nat=eth1 http admin_password=$ADMIN_PW"
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
    check_ping "default route from DHCP + NAT: LAN host reaches $REMOTE" "0%" ping -c 3 -i 0.2 -W 3 $REMOTE

    echo "--- DHCP server on the LAN port"
    check_output "udhcpc gets a lease from the router" \
        "bound ip=10\.0\.9\.1[0-5][0-9] subnet=255\.255\.255\.0 router=$ROUTER_LAN dns=10\.0\.8\.53 lease=600" \
        busybox udhcpc -i tap0 -n -q -f -t 4 -T 2 -x hostname:lazy-laptop -s $UDHCPC_SCRIPT
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
    # GNS3's VPCS fills in ciaddr before it owns the address (sending from 0.0.0.0):
    # a unicast ACK would wait for an ARP reply that never comes, so it must be broadcast
    check_output "REQUEST with ciaddr from another source address: ACK is broadcast" \
        "^ACK yiaddr=10\.0\.9\.141 to=255\.255\.255\.255$" dhcp_probe 02:00:00:00:00:04 10.0.9.141 10.0.9.141
    ip addr add 10.0.9.142/24 dev tap0
    check_output "REQUEST from the client's own ciaddr (renewal): ACK is unicast" \
        "^ACK yiaddr=10\.0\.9\.142 to=10\.0\.9\.142$" dhcp_probe 02:00:00:00:00:05 10.0.9.142 10.0.9.142 10.0.9.142
    ip addr del 10.0.9.142/24 dev tap0

    echo "--- NAT"
    ip addr add 10.0.9.3/24 dev tap0                # a second LAN host
    wan_echo_server $TMP/peers 30
    sleep 0.5
    check_ping "LAN host pings WAN host through the NAT" "0%" ping -c 5 -i 0.2 -W 3 $WAN_HOST
    check_ping "forwarded burst through the NAT: 1000 pings, 1 ms apart" "0%" ping -q -c 1000 -i 0.001 -W 1 $WAN_HOST
    check_output "UDP round trip through the NAT" "^reply$" lan_udp_roundtrip $LAN_HOST 40000
    check_output "WAN host sees the router's address, source port preserved" "^udp $ROUTER_WAN:40000$" cat $TMP/peers
    check_output "second LAN host, same source port: round trip works" "^reply$" lan_udp_roundtrip 10.0.9.3 40000
    second=$(grep '^udp ' $TMP/peers | tail -1)
    if [ -n "$second" ] && [ "$second" != "udp $ROUTER_WAN:40000" ] && echo "$second" | grep -q "^udp $ROUTER_WAN:"; then
        pass "port collision: second LAN host got another external port ($second)"
    else
        fail "port collision: second mapping is '$second'"
    fi
    check_output "1 MiB over TCP through the NAT arrives intact" "^intact 1048576$" python3 -c "
import socket, threading, hashlib, os
data = os.urandom(1 << 20)
c = socket.create_connection(('$WAN_HOST', 8080), timeout=10)
threading.Thread(target=c.sendall, args=(data,), daemon=True).start()
got = bytearray()
while len(got) < len(data):
    chunk = c.recv(65536)
    if not chunk:
        break
    got += chunk
c.close()
print(('intact %d' if bytes(got) == data else 'corrupt %d') % len(got))"
    check_output "TCP connection seen from the router's address" "^tcp $ROUTER_WAN:" cat $TMP/peers
    check_output "ICMP error through the NAT: closed WAN port is reported to the LAN host" "^refused$" python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.connect(('$WAN_HOST', 9)); s.send(b'anyone?')
try:
    s.recv(100); print('answered')
except ConnectionRefusedError:
    print('refused')
except socket.timeout:
    print('timeout')"

    # Filtering: after 10.0.9.1:40010 talked to WAN:5004, only WAN:5004 may answer
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.bind(('$LAN_HOST', 40010))
s.sendto(b'open a mapping', ('$WAN_HOST', 5004))
got = []
try:
    while True:
        got.append(s.recv(100).decode())
except (socket.timeout, ConnectionRefusedError):
    pass
print(' '.join(got) or 'nothing')" > $TMP/filtered &
    lan_listener=$!
    sleep 0.5
    wan python3 -c "
import socket
for port, text in ((6000, 'wrong-port'), (5004, 'right-port')):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('$WAN_HOST', port))
    s.sendto(text.encode(), ('$ROUTER_WAN', 40010))"
    wait $lan_listener
    check_output "only the contacted remote port gets back in" "^right-port$" cat $TMP/filtered

    # Firewall: give the WAN host a route to the LAN and let it try. A ping alone
    # can't show this (a reply would come back NATed and be ignored by ping), so
    # a LAN listener must receive nothing.
    wan ip route add 10.0.9.0/24 via $ROUTER_WAN
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.bind(('$LAN_HOST', 7777))
try:
    print('received ' + s.recv(100).decode())
except socket.timeout:
    print('nothing arrived')" > $TMP/firewall &
    lan_listener=$!
    sleep 0.3
    wan python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(3):
    s.sendto(b'unsolicited', ('$LAN_HOST', 7777))"
    wan ping -c 2 -i 0.2 -W 1 $LAN_HOST >/dev/null 2>&1
    wait $lan_listener
    check_output "firewall: unsolicited traffic from the WAN doesn't reach the LAN" "^nothing arrived$" cat $TMP/firewall
    wan ip route del 10.0.9.0/24 via $ROUTER_WAN
    check_ping "router still answers on its WAN address" "0%" wan ping -c 2 -i 0.2 -W 2 $ROUTER_WAN

    echo "--- HTTP management API"
    API=http://$ROUTER_LAN
    check_output "GET / serves the dashboard: gzip, identical to www/index.html" "^200 gzip identical$" sh -c \
        "curl -s --compressed --max-time 5 -D $TMP/headers -o $TMP/index -w '%{http_code} ' $API/ &&
         grep -qi '^content-encoding: gzip' $TMP/headers && printf 'gzip ' && cmp -s $TMP/index www/index.html && echo identical"
    curl -s -u admin:$ADMIN_PW --max-time 5 $API/api/stats > $TMP/stats.json
    check_output "/api/stats is valid JSON describing the router" "^consistent$" python3 - $TMP/stats.json <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
ports = {i['name']: i for i in s['interfaces']}
clients = {c['ip']: c for c in s['clients']}
problems = []
if ports['eth0']['role'] != 'lan' or ports['eth0']['dhcp'] != 'server' or ports['eth0']['ipv4'] != '10.0.9.2/24':
    problems.append('eth0 %r' % ports['eth0'])
if ports['eth1']['role'] != 'wan' or ports['eth1']['dhcp'] != 'client' or ports['eth1']['ipv4'] != '10.0.8.2/24':
    problems.append('eth1 %r' % ports['eth1'])
if not any(r['network'] == '0.0.0.0/0' and r['gateway'] == '10.0.8.1' for r in s['routes']):
    problems.append('no DHCP default route')
if s['dhcp']['leases'] < 2:
    problems.append('leases %d' % s['dhcp']['leases'])
if not any(c['hostname'] == 'lazy-laptop' and c['lease_s'] for c in s['clients']):
    problems.append('no lazy-laptop lease among clients')
lan = clients.get('10.0.9.1')
if not lan or lan['up_bytes'] < 1000000 or lan['down_bytes'] < 1000000 or lan['mac'] is None:
    problems.append('client 10.0.9.1 %r' % lan)
if s['nat']['created'] < 3 or s['nat']['firewall_dropped'] < 1 or s['ipv4']['forwarded'] < 2000:
    problems.append('counters nat=%r ipv4=%r' % (s['nat'], s['ipv4']))
if s['tcp']['active'] < 1 or s['system']['uptime_ms'] <= s['system']['ready_ms']:
    problems.append('system/tcp %r %r' % (s['system'], s['tcp']))
print('; '.join(problems) or 'consistent')
PY
    curl -s -u admin:$ADMIN_PW --max-time 5 "$API/api/events" > $TMP/events.json
    check_output "/api/events logs leases, devices and the WAN lease" "^logged$" python3 -c "
import json
e = json.load(open('$TMP/events.json'))
texts = ' | '.join(x['category'] + ': ' + x['text'] for x in e)
need = ['dhcp: 10.0.9.1', 'lazy-laptop', 'client: new device 10.0.9.1 on eth0', 'wan: eth1 got 10.0.8.2', 'system: kernel ready']
missing = [n for n in need if n not in texts]
print('missing %s in %s' % (missing, texts[:300]) if missing else 'logged')"
    last=$(python3 -c "import json; print(json.load(open('$TMP/events.json'))[-1]['id'])")
    check_output "/api/events?since=<last id> returns nothing new" "^\[\]$" curl -s -u admin:$ADMIN_PW --max-time 5 "$API/api/events?since=$last"

    # CPU load (time not spent asleep in hlt, per one-second window): low after a quiet
    # second, clearly higher while a ping flood is forwarded through the NAT
    check_output "/api/stats CPU load: low when idle, higher under a forwarded ping flood" "^tracks load " \
        python3 - "$API" "$WAN_HOST" <<'PY'
import json, os, subprocess, sys, time, urllib.request
api, target = sys.argv[1], sys.argv[2]
auth = {'Authorization': os.environ['API_AUTH']}
stats = lambda: json.load(urllib.request.urlopen(urllib.request.Request(api + '/api/stats', headers=auth), timeout=5))
time.sleep(2.2)                       # a whole window with nothing but this request in it
idle = stats()['cpu']['busy_permille']
flood = subprocess.Popen(['ping', '-f', '-q', '-s', '1000', '-w', '4', target], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.5)
busy = stats()['cpu']['busy_permille']
flood.wait()
s = stats()
ok = idle < 300 and busy > idle + 150 and s['cpu']['idle_ms'] > 0 and all(isinstance(i.get('rx_missed'), int) for i in s['interfaces'])
print(('tracks load (' if ok else 'wrong: ') + 'idle %d, under flood %d permille; idle_ms %d; rx_missed %s'
      % (idle, busy, s['cpu']['idle_ms'], [i.get('rx_missed') for i in s['interfaces']]) + (')' if ok else ''))
PY

    check_ping "second LAN host reaches the WAN" "0%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "POST /api/block?ip=10.0.9.3" '"blocked":true' curl -s -u admin:$ADMIN_PW --max-time 5 -X POST "$API/api/block?ip=10.0.9.3"
    check_ping "blocked device can't reach the WAN" "100%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 1 $WAN_HOST
    check_ping "other devices are unaffected" "0%" ping -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "blocked device can still open the dashboard" "^200$" \
        curl -s -u admin:$ADMIN_PW --max-time 5 --interface 10.0.9.3 -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "stats show the device as blocked" '"ip":"10.0.9.3"[^}]*"blocked":true' curl -s -u admin:$ADMIN_PW --max-time 5 $API/api/stats
    check_output "POST /api/unblock?ip=10.0.9.3" '"blocked":false' curl -s -u admin:$ADMIN_PW --max-time 5 -X POST "$API/api/unblock?ip=10.0.9.3"
    check_ping "unblocked device reaches the WAN again" "0%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "block and unblock were logged" "unblocked 10.0.9.3" curl -s -u admin:$ADMIN_PW --max-time 5 "$API/api/events?since=$last"

    echo "--- login, CSRF and DNS rebinding"
    check_output "API without credentials: 401" "^401$" curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "API with a wrong password: 401" "^401$" \
        curl -s -u admin:wrong-password --max-time 5 -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "the dashboard page itself needs no login (it shows the login form)" "^200$" \
        curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/
    check_output "login sets an HttpOnly, SameSite=Strict session cookie" \
        "^Set-Cookie: lazydays_session=[0-9a-f]{32}; Path=/; HttpOnly; SameSite=Strict$" sh -c \
        "curl -s --max-time 5 -c $TMP/jar -D - -o $TMP/login.json --data-urlencode 'password=$ADMIN_PW' $API/api/login | tr -d '\r'"
    csrf=$(python3 -c "import json; print(json.load(open('$TMP/login.json')).get('csrf'))" 2>/dev/null)
    check_output "the session cookie works" "^200$" curl -s --max-time 5 -b $TMP/jar -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "POST in a session without the CSRF token: 403" "^403$" \
        curl -s --max-time 5 -b $TMP/jar -o /dev/null -w '%{http_code}' -X POST "$API/api/unblock?ip=10.0.9.3"
    check_output "POST in a session with a wrong CSRF token: 403" "^403$" curl -s --max-time 5 -b $TMP/jar \
        -H "X-CSRF-Token: 0123456789abcdef0123456789abcdef" -o /dev/null -w '%{http_code}' -X POST "$API/api/unblock?ip=10.0.9.3"
    check_output "POST in a session with its CSRF token: 200" "^200$" curl -s --max-time 5 -b $TMP/jar \
        -H "X-CSRF-Token: $csrf" -o /dev/null -w '%{http_code}' -X POST "$API/api/unblock?ip=10.0.9.3"
    check_output "cross-site POST (Origin of another site): 403" "^403$" curl -s -u admin:$ADMIN_PW --max-time 5 \
        -H "Origin: http://evil.example" -o /dev/null -w '%{http_code}' -X POST "$API/api/unblock?ip=10.0.9.3"
    check_output "DNS rebinding (Host of another site, right password): 421" "^421$" curl -s -u admin:$ADMIN_PW --max-time 5 \
        -H "Host: evil.example" -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "request without Host: 400" "^400$" curl -s -u admin:$ADMIN_PW --max-time 5 -H "Host:" -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "responses forbid framing and MIME sniffing" "^3$" sh -c \
        "curl -s --max-time 5 -D - -o /dev/null $API/ | tr -d '\r' |
         grep -ciE '^(x-frame-options: deny|x-content-type-options: nosniff|content-security-policy: .*frame-ancestors .none.)'"
    check_output "logout ends the session" "^200 401$" sh -c \
        "curl -s --max-time 5 -b $TMP/jar -H 'X-CSRF-Token: $csrf' -o /dev/null -w '%{http_code} ' -X POST $API/api/logout;
         curl -s --max-time 5 -b $TMP/jar -o /dev/null -w '%{http_code}' $API/api/stats"
    # Guessing, from the second LAN address so its lockout doesn't affect the rest
    check_output "5 wrong passwords lock the address out; then even the right one gets 429" "^401 401 401 401 401 429$" sh -c \
        "for pw in a b c d e '$ADMIN_PW'; do curl -s --interface 10.0.9.3 --max-time 5 -o /dev/null -w '%{http_code} ' \
            --data-urlencode password=\$pw $API/api/login; done | sed 's/ \$//'"
    check_output "the lockout says when to retry" "^Retry-After: (5[0-9]|60)$" sh -c \
        "curl -s --interface 10.0.9.3 --max-time 5 -D - -o /dev/null --data-urlencode password=x $API/api/login | tr -d '\r'"
    check_output "other addresses can still log in during the lockout" "^200$" \
        curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "a new password must have 8-64 characters" "^400$" curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code}' \
        --data-urlencode "current=$ADMIN_PW" --data-urlencode "new=short" $API/api/password
    check_output "password change: the old password stops working, the new one works" "^200 401 200$" sh -c \
        "curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code} ' \
            --data-urlencode 'current=$ADMIN_PW' --data-urlencode 'new=new-pass-1234' $API/api/password;
         curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code} ' $API/api/stats;
         curl -s -u admin:new-pass-1234 --max-time 5 -o /dev/null -w '%{http_code}' $API/api/stats"
    check_output "password changed back" "^200$" curl -s -u admin:new-pass-1234 --max-time 5 -o /dev/null -w '%{http_code}' \
        --data-urlencode "current=new-pass-1234" --data-urlencode "new=$ADMIN_PW" $API/api/password
    check_output "/api/stats counts logins, failures, lockouts, CSRF and Host rejections" "^counted " python3 -c "
import json, urllib.request
s = json.load(urllib.request.urlopen(urllib.request.Request('$API/api/stats', headers={'Authorization': '$API_AUTH'}), timeout=5))
a, h = s['auth'], s['http']
ok = a['logins'] >= 1 and a['failures'] >= 7 and a['lockouts'] >= 1 and a['locked_rejected'] >= 2 and a['csrf_rejected'] >= 2 and h['wrong_host'] >= 1
print(('counted ' if ok else 'wrong: ') + json.dumps(a) + ' wrong_host %d' % h['wrong_host'])"

    echo "--- management is unreachable from the WAN"
    check_output "WAN can't connect to the dashboard on the WAN address (SYN dropped)" "^silently dropped$" \
        tcp_connect_result wan $ROUTER_WAN 80
    # A WAN neighbour (anyone on the ISP's segment) can address the router's LAN address directly
    wan ip route add $ROUTER_LAN/32 via $ROUTER_WAN
    check_output "WAN can't reach the dashboard by addressing the LAN address" "^silently dropped$" \
        tcp_connect_result wan $ROUTER_LAN 80
    check_ping "WAN gets no answer from the LAN address at all" "100%" wan ping -c 2 -i 0.2 -W 1 $ROUTER_LAN
    wan ip route del $ROUTER_LAN/32 via $ROUTER_WAN
    check_output "TCP initial sequence numbers can't be predicted from earlier ones" "^unpredictable: 0 of 1[0-9] " \
        attack isn $ROUTER_LAN 80 20
    check_output "unknown path: 404" "^404$" curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/nope
    check_output "wrong method: 405" "^405 405$" sh -c \
        "curl -s --max-time 5 -o /dev/null -w '%{http_code} ' -X DELETE $API/api/stats; curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/api/block"
    check_output "bad parameter: 400" "^400$" curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code}' -X POST "$API/api/block?ip=999.1.1.1"
    check_output "raw protocol errors: 400, 431, 413" "^400 431 413$" python3 -c "
import socket
def ask(raw):
    c = socket.create_connection(('$ROUTER_LAN', 80), timeout=5)
    c.sendall(raw)
    reply = c.recv(200).decode(errors='replace')
    c.close()
    return reply.split(' ')[1] if reply.startswith('HTTP/1.1 ') else repr(reply[:40])
print(ask(b'HELLO\r\n\r\n'), ask(b'GET / HTTP/1.1\r\nX-Pad: ' + b'a' * 3000 + b'\r\n\r\n'),
      ask(b'POST /api/block HTTP/1.1\r\nContent-Length: 1000\r\n\r\n'))"
    check_output "incomplete request times out with 408" "^HTTP/1.1 408" python3 -c "
import socket
c = socket.create_connection(('$ROUTER_LAN', 80), timeout=10)
c.sendall(b'GET / HTTP/1.1\r\n')
print(c.recv(200).decode(errors='replace').splitlines()[0])"
    check_output "30 parallel requests all answered" "^30 x 200$" python3 -c "
import threading, urllib.request
codes = []
def get():
    try:
        codes.append(urllib.request.urlopen(urllib.request.Request('$API/api/stats', headers={'Authorization': '$API_AUTH'}), timeout=15).status)
    except Exception as e:
        codes.append(repr(e))
threads = [threading.Thread(target=get) for _ in range(30)]
for t in threads: t.start()
for t in threads: t.join()
print('30 x 200' if codes.count(200) == 30 else codes)"
    # Found by fuzzing: a client that never closes its side kept a slot (and 64 KiB) forever
    check_output "16 clients that never close can't lock others out (FIN_WAIT_2)" "^200 after 16 lingering$" python3 -c "
import socket, urllib.request
held = []
for i in range(16):
    s = socket.create_connection(('$ROUTER_LAN', 80), timeout=5)
    s.sendall(b'GET /nope HTTP/1.1\r\nHost: $ROUTER_LAN\r\n\r\n')
    while s.recv(4096):
        pass                            # 404, then the router closes; this side never does
    held.append(s)
try:
    code = urllib.request.urlopen(urllib.request.Request('$API/api/stats', headers={'Authorization': '$API_AUTH'}), timeout=10).status
except Exception as e:
    code = repr(e)
print('%s after %d lingering' % (code, len(held)))"
    # Found by fuzzing: a client that stops reading (zero window) kept a slot forever
    check_output "16 clients that never read can't lock others out (zero window)" "^200 after 16 stalled$" python3 -c "
import socket, time, urllib.request
held = []
for i in range(16):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    s.settimeout(5)
    s.connect(('$ROUTER_LAN', 80))
    s.sendall(b'GET / HTTP/1.1\r\nHost: $ROUTER_LAN\r\n\r\n')
    held.append(s)                      # the dashboard doesn't fit the tiny window; this side never reads
time.sleep(1)
try:
    code = urllib.request.urlopen(urllib.request.Request('$API/api/stats', headers={'Authorization': '$API_AUTH'}), timeout=10).status
except Exception as e:
    code = repr(e)
print('%s after %d stalled' % (code, len(held)))"

    echo "--- floods with spoofed sources"
    # From the WAN: must not take a single connection slot
    check_output "WAN SYN flood" "^sent " attack wan syn-flood $ROUTER_WAN 80 2 1000 203.0.113.
    check_output "LAN still opens the dashboard right after the WAN flood" "^200$" \
        curl -s -u admin:$ADMIN_PW --max-time 5 -o /dev/null -w '%{http_code}' $API/api/stats
    # From the LAN: half-open connections must not lock real clients out, and the
    # router's ARP requests for ~250 nonexistent hosts must not push out real ones
    attack syn-flood $ROUTER_LAN 80 6 1000 10.0.9. > $TMP/lan_flood &
    flood=$!
    sleep 1
    check_output "LAN clients get the dashboard during a LAN SYN flood" "^200 200 200$" sh -c \
        "for i in 1 2 3; do curl -s -u admin:$ADMIN_PW --max-time 4 -o /dev/null -w '%{http_code}' $API/api/stats; [ \$i = 3 ] || printf ' '; sleep 0.5; done; echo"
    wait $flood
    check_output "LAN SYN flood" "^sent " cat $TMP/lan_flood
    # From the WAN segment, pings from nonexistent neighbours: the router's replies need
    # ARP; the default gateway's entry must survive or the whole LAN loses the Internet
    attack wan ping-flood $ROUTER_WAN 5 1000 10.0.8. > $TMP/wan_ping_flood &
    flood=$!
    sleep 1
    check_ping "LAN keeps Internet access during a WAN ping flood from spoofed neighbours" "0%" \
        ping -c 10 -i 0.2 -W 1 $WAN_HOST
    wait $flood
    check_output "WAN ping flood" "^sent " cat $TMP/wan_ping_flood
    check_output "/api/stats counts filtered SYNs and SYN cookies" "^counted " python3 -c "
import json, urllib.request
s = json.load(urllib.request.urlopen(urllib.request.Request('$API/api/stats', headers={'Authorization': '$API_AUTH'}), timeout=5))
t = s['tcp']
ok = t['filtered'] >= 1000 and t['syn_cookies_sent'] >= 1000 and t['syn_cookies_accepted'] >= 3 and s['ipv4']['wrong_port'] >= 2
print(('counted ' if ok else 'wrong: ') + 'filtered %d, cookies sent %d, accepted %d, half-open evicted %d, refused %d, wrong_port %d'
      % (t['filtered'], t['syn_cookies_sent'], t['syn_cookies_accepted'], t['syn_evicted'], t['refused'], s['ipv4']['wrong_port']))"

    echo "--- port 0 (found by fuzzing: it matched the free slots of the port tables)"
    check_output "a TCP SYN to port 0 from the WAN gets RST (was accepted with no application, held forever)" "^RST$" \
        attack wan syn-probe $ROUTER_WAN 0
    check_output "a UDP datagram to port 0 from the WAN" "^sent$" attack wan udp $ROUTER_WAN 0
    check_ping "the router survives it (it called a NULL handler: remote crash)" "0%" ping -c 2 -i 0.2 -W 2 $ROUTER_LAN
    if grep -q "K_PANIC\|panic" $LOG; then
        fail "the kernel log shows a panic: $(grep -m1 -i panic $LOG)"
    fi
}

# Raw packets as an attacker would send them. A leading "wan" runs it on the WAN host.
#   attack [wan] isn <dst> <port> <count>
#       SYNs from successive source ports; reads the router's initial sequence
#       numbers from its SYN-ACKs (the host's own stack then resets them) and
#       checks whether each one follows from the previous one through xorshift32,
#       the generator the kernel used before. Prints "unpredictable: 0 of <n> ..."
#       or "PREDICTABLE: <hits> of <n> ...".
#   attack [wan] syn-flood <dst> <port> <seconds> <packets/s> <source prefix, e.g. 203.0.113.>
#       TCP SYNs from random spoofed sources prefix.3-254; prints "sent <n>"
#   attack [wan] ping-flood <dst> <seconds> <packets/s> <source prefix>
#       ICMP echo requests from random spoofed sources prefix.3-254; prints "sent <n>"
#   attack [wan] udp <dst> <port>
#       one UDP datagram from this host to any port, 0 included; prints "sent"
#   attack [wan] syn-probe <dst> <port>
#       one SYN; prints how the router answers: "SYN-ACK", "RST" or "no reply"
attack() {
    run=""
    if [ "$1" = wan ]; then run=wan; shift; fi
    $run python3 - "$@" <<'PY'
import random, socket, struct, sys, time

def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF

def syn(src, dst, sport, dport):
    tcp = struct.pack('!HHIIBBHHH', sport, dport, random.getrandbits(32), 0, 5 << 4, 0x02, 64240, 0, 0)
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack('!BBH', 0, 6, len(tcp))
    tcp = tcp[:16] + struct.pack('!H', csum(pseudo + tcp)) + tcp[18:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), random.getrandbits(16), 0, 64, 6, 0,
                     socket.inet_aton(src), socket.inet_aton(dst))
    return ip[:10] + struct.pack('!H', csum(ip)) + ip[12:] + tcp

def ping(src, dst):
    icmp = struct.pack('!BBHHH', 8, 0, 0, random.getrandbits(16), 1) + b'spoofed'
    icmp = icmp[:2] + struct.pack('!H', csum(icmp)) + icmp[4:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(icmp), random.getrandbits(16), 0, 64, 1, 0,
                     socket.inet_aton(src), socket.inet_aton(dst))
    return ip[:10] + struct.pack('!H', csum(ip)) + ip[12:] + icmp

def xorshift32(x):
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    return x

mode, dst = sys.argv[1], sys.argv[2]
out = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe.connect((dst, 9))
own = probe.getsockname()[0]
if mode == 'udp':
    port = int(sys.argv[3])
    udp = struct.pack('!HHHH', 40001, port, 8 + 5, 0) + b'hello'
    pseudo = socket.inet_aton(own) + socket.inet_aton(dst) + struct.pack('!BBH', 0, 17, len(udp))
    udp = udp[:6] + struct.pack('!H', csum(pseudo + udp) or 0xFFFF) + udp[8:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0, socket.inet_aton(own), socket.inet_aton(dst))
    out.sendto(ip[:10] + struct.pack('!H', csum(ip)) + ip[12:] + udp, (dst, 0))
    print('sent')
    sys.exit(0)
if mode == 'syn-probe':
    port = int(sys.argv[3])
    recv = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_TCP)
    recv.settimeout(2)
    out.sendto(syn(own, dst, 41999, port), (dst, 0))
    answer, deadline = 'no reply', time.time() + 2
    while time.time() < deadline:
        try:
            pkt = recv.recv(2000)
        except socket.timeout:
            break
        ihl = (pkt[0] & 0x0F) * 4
        s_port, d_port, _, _, _, flags = struct.unpack('!HHIIBB', pkt[ihl:ihl + 14])
        if pkt[12:16] == socket.inet_aton(dst) and s_port == port and d_port == 41999:
            answer = 'SYN-ACK' if flags & 0x12 == 0x12 else 'RST' if flags & 0x04 else 'flags %#x' % flags
            break
    print(answer)
    sys.exit(0)
if mode == 'isn':
    port = int(sys.argv[3])
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((dst, 9))
    src = probe.getsockname()[0]
    recv = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_TCP)
    recv.settimeout(2)
    isns = []
    for i in range(int(sys.argv[4])):
        sport = 41000 + i
        out.sendto(syn(src, dst, sport, port), (dst, 0))
        deadline = time.time() + 2
        while time.time() < deadline:
            try:
                pkt = recv.recv(2000)
            except socket.timeout:
                break
            ihl = (pkt[0] & 0x0F) * 4
            s_port, d_port, seq, _, _, flags = struct.unpack('!HHIIBB', pkt[ihl:ihl + 14])
            if pkt[12:16] == socket.inet_aton(dst) and s_port == port and d_port == sport and flags & 0x12 == 0x12:
                isns.append(seq)
                break
    hits = 0
    for a, b in zip(isns, isns[1:]):
        x = a
        for _ in range(16):
            x = xorshift32(x)
            if x == b:
                hits += 1
                break
    verdict = 'PREDICTABLE' if hits else 'unpredictable'
    print('%s: %d of %d next ISNs follow from the previous one (%d SYN-ACKs, first %s)'
          % (verdict, hits, max(len(isns) - 1, 0), len(isns), ' '.join('%08x' % v for v in isns[:3])))
else:
    if mode == 'syn-flood':
        port = int(sys.argv[3])
        make = lambda src: syn(src, dst, random.randint(1024, 65535), port)
        args = sys.argv[4:]
    else:
        make = lambda src: ping(src, dst)
        args = sys.argv[3:]
    seconds, rate, prefix = float(args[0]), int(args[1]), args[2]
    sent, start = 0, time.time()
    while time.time() - start < seconds:
        for _ in range(max(rate // 100, 1)):
            out.sendto(make(prefix + str(random.randint(3, 254))), (dst, 0))
            sent += 1
        time.sleep(0.01)
    print('sent %d' % sent)
PY
}

# [wan] host port: how a TCP connection attempt ends: "refused", "silently dropped", or
# "connected" plus the status line of a GET /api/stats sent over it
tcp_connect_result() {
    run=""
    if [ "$1" = wan ]; then run=wan; shift; fi
    $run python3 -c "
import socket
try:
    c = socket.create_connection(('$1', $2), timeout=2)
except socket.timeout:
    print('silently dropped')
except ConnectionRefusedError:
    print('refused')
else:
    c.settimeout(3)
    c.sendall(b'GET /api/stats HTTP/1.1\\r\\nHost: $1\\r\\n\\r\\n')
    try:
        print('connected: ' + c.recv(100).decode(errors='replace').splitlines()[0])
    except Exception as e:
        print('connected: %r' % e)"
}

# ------------------------------------------------------------------- rip ----

# rip_inject DEV SRC_IP SRC_PORT COMMAND [addr,mask,metric ...]
# One RIPv2 message broadcast on DEV (raw Ethernet), e.g. from a rogue neighbour
rip_inject() {
    python3 - "$@" <<'PY'
import fcntl, socket, struct, sys
dev, src, sport, cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
ip = lambda a: struct.unpack('!I', socket.inet_aton(a))[0]
payload = struct.pack('!BBH', cmd, 2, 0)
for spec in sys.argv[5:]:
    addr, mask, metric = spec.split(',')
    payload += struct.pack('!HHIIII', 2, 0, ip(addr), ip(mask), 0, int(metric))
udp = struct.pack('!HHHH', sport, 520, 8 + len(payload), 0) + payload
hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 1, 17, 0, socket.inet_aton(src), b'\xff' * 4)
s = sum(struct.unpack('!10H', hdr))
while s >> 16:
    s = (s & 0xFFFF) + (s >> 16)
hdr = hdr[:10] + struct.pack('!H', ~s & 0xFFFF) + hdr[12:]
sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
# The interface's MAC (SIOCGIFHWADDR): /sys shows the host's interfaces, not this namespace's
mac = fcntl.ioctl(sock.fileno(), 0x8927, struct.pack('256s', dev.encode()))[18:24]
sock.bind((dev, 0))
sock.send(b'\xff' * 6 + mac + b'\x08\x00' + hdr + udp)
print('sent')
PY
}

# rip_listen DEV FROM_IP SECONDS NETWORK/LEN: the metric FROM_IP announces for
# NETWORK on DEV's link ("metric N", or "absent" if it never mentions it)
rip_listen() {
    python3 - "$@" <<'PY'
import socket, struct, sys, time
dev, src, seconds, target = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
net, plen = target.split('/')
want = (socket.inet_aton(net), struct.pack('!I', (0xFFFFFFFF << (32 - int(plen))) & 0xFFFFFFFF))
sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800))
sock.bind((dev, 0))
sock.settimeout(0.5)
found, deadline = None, time.time() + seconds
while time.time() < deadline:
    try:
        frame = sock.recv(2000)
    except socket.timeout:
        continue
    ip = frame[14:]
    ihl = (ip[0] & 15) * 4
    if ip[9] != 17 or ip[12:16] != socket.inet_aton(src) or struct.unpack('!H', ip[ihl + 2:ihl + 4])[0] != 520:
        continue
    rip = ip[ihl + 8:]
    if rip[0] != 2:
        continue
    for off in range(4, len(rip) - 19, 20):
        if (rip[off + 4:off + 8], rip[off + 8:off + 12]) == want:
            found = struct.unpack('!I', rip[off + 16:off + 20])[0]
print('metric %d' % found if found is not None else 'absent')
PY
}

# rip_route ROUTER_IP NETWORK/LEN: "via GATEWAY PORT metric N origin O" from /api/stats, or "none"
rip_route() {
    curl -s -u admin:$ADMIN_PW --max-time 5 http://$1/api/stats | python3 -c "
import json, sys
routes = [r for r in json.load(sys.stdin)['routes'] if r['network'] == '$2']
print(' '.join(['via %s %s metric %s origin %s' % (r['gateway'], r['port'], r.get('metric'), r['origin']) for r in routes]) or 'none')"
}

# ping_recovers HOST SECONDS: how long until a ping to HOST gets through (polls every 0.5 s)
ping_recovers() {
    python3 - "$1" "$2" <<'PY'
import subprocess, sys, time
host, limit = sys.argv[1], float(sys.argv[2])
start = time.time()
while time.time() - start < limit:
    if subprocess.run(['ping', '-c', '1', '-W', '1', host], stdout=subprocess.DEVNULL).returncode == 0:
        print('recovered after %.1f s' % (time.time() - start))
        sys.exit(0)
    time.sleep(0.5)
print('no reply within %d s' % limit)
PY
}

scenario_rip() {
    ADMIN_PW=rip-test-pw
    TIMERS=rip_timers=2,6,4
    ip link set lo up
    ip tuntap add dev tap0 mode tap
    ip addr add $LAN_HOST/24 dev tap0
    ip link set tap0 up
    ip route add default via $ROUTER_LAN
    for link in 12 23 13; do
        ip link add br$link type bridge
        ip link set br$link up
        for side in a b; do
            ip tuntap add dev t$link$side mode tap
            ip link set t$link$side master br$link
            ip link set t$link$side up
        done
    done
    ip tuntap add dev tap1 mode tap
    # Two bridges carry an address of this namespace, for listening and injecting
    ip addr add 10.12.0.9/24 dev br12
    ip addr add 10.13.0.9/24 dev br13

    rm -f qemu_rip_r?.log qemu_rip_lan.pcap qemu_rip_wan.pcap
    # $1 name, $2 MAC prefix, $3 command line, then "tap[:pcap]" for each port in order
    router() {
        name=$1; mac=$2; append=$3; shift 3
        args=""; n=0
        for port in "$@"; do
            tap=${port%%:*}
            args="$args -netdev tap,id=n$n,ifname=$tap,script=no,downscript=no -device e1000,netdev=n$n,mac=$mac:0$n"
            [ "$tap" != "$port" ] && args="$args -object filter-dump,id=d$n,netdev=n$n,file=${port#*:}"
            n=$((n + 1))
        done
        qemu-system-i386 -kernel "$KERNEL" -append "$append" -display none -no-reboot \
            -serial file:qemu_rip_$name.log -monitor unix:$TMP/$name.mon,server,nowait $args &
    }
    router r1 52:54:00:00:01 "eth0=$ROUTER_LAN/24 eth1=10.12.0.1/24 eth2=10.13.0.1/24 rip=eth1,eth2 $TIMERS http admin_password=$ADMIN_PW" \
        tap0:qemu_rip_lan.pcap t12a t13a
    R1=$!
    router r2 52:54:00:00:02 "eth0=10.12.0.2/24 eth1=10.23.0.2/24 rip=eth0,eth1 $TIMERS" t12b t23a
    R2=$!
    router r3 52:54:00:00:03 "eth0=10.23.0.3/24 eth1=10.13.0.3/24 eth2=$ROUTER_WAN/24 rip=eth0,eth1 $TIMERS http admin_password=$ADMIN_PW" \
        t23b t13b tap1:qemu_rip_wan.pcap
    R3=$!
    unshare --net sleep 600 &
    WAN_PID=$!
    trap 'kill $R1 $R2 $R3 $WAN_PID 2>/dev/null; rm -rf $TMP' EXIT
    sleep 0.5
    ip link set tap1 netns $WAN_PID
    wan ip link set lo up
    wan ip addr add $WAN_HOST/24 dev tap1
    wan ip link set tap1 up
    wan ip route add default via $ROUTER_WAN
    monitor() { echo "$2" | socat - UNIX-CONNECT:$TMP/$1.mon > /dev/null; }
    for r in r1 r2 r3; do
        LOG=qemu_rip_$r.log
        wait_for_log 'getting kernel ready' 150 || { echo "[ERROR] $r did not boot"; cat $LOG; exit 1; }
    done
    LOG=qemu_rip_r1.log

    echo "--- convergence"
    check_output "LAN host reaches the far network through RIP routes" "^recovered after" ping_recovers $WAN_HOST 20
    check_output "R1 learned the far network from R3 directly: metric 2" "^via 10.13.0.3 eth2 metric 2 origin rip$" \
        rip_route $ROUTER_LAN 10.0.8.0/24
    check_output "R3 learned the LAN from R1 directly: metric 2" "^via 10.13.0.1 eth1 metric 2 origin rip$" \
        rip_route $ROUTER_WAN 10.0.9.0/24
    check_output "the path is two routers long (TTL 62)" "ttl=62" ping -c 1 -W 2 $WAN_HOST
    check_output "connected networks stay connected" "^via None eth0 metric None origin connected$" \
        rip_route $ROUTER_LAN 10.0.9.0/24
    check_output "poisoned reverse: R1 announces the far network back to R3 as unreachable" "^metric 16$" \
        rip_listen br13 10.13.0.1 5 10.0.8.0/24
    check_output "split horizon is per link: R1 announces it to R2 normally" "^metric 2$" \
        rip_listen br12 10.12.0.1 5 10.0.8.0/24

    echo "--- a cut cable between R1 and R3 (both NICs see the link go down)"
    monitor r1 "set_link n2 off"
    monitor r3 "set_link n1 off"
    check_output "traffic is rerouted through R2" "^recovered after" ping_recovers $WAN_HOST 15
    check_output "R1 now routes via R2: metric 3" "^via 10.12.0.2 eth1 metric 3 origin rip$" rip_route $ROUTER_LAN 10.0.8.0/24
    check_output "the path is three routers long (TTL 61)" "ttl=61" ping -c 1 -W 2 $WAN_HOST
    check_output "the link failure was logged" "eth2 link down" curl -s -u admin:$ADMIN_PW http://$ROUTER_LAN/api/events
    monitor r1 "set_link n2 on"
    monitor r3 "set_link n1 on"
    sleep 6
    check_output "with the cable back, R1 uses the direct path again" "^via 10.13.0.3 eth2 metric 2 origin rip$" \
        rip_route $ROUTER_LAN 10.0.8.0/24

    echo "--- a silent failure: frames between R1 and R3 vanish, the links stay up"
    ip link set t13b down
    check_output "detected by the RIP timeout and rerouted" "^recovered after" ping_recovers $WAN_HOST 20
    check_output "R1 routes via R2 again" "^via 10.12.0.2 eth1 metric 3 origin rip$" rip_route $ROUTER_LAN 10.0.8.0/24
    ip link set t13b up
    sleep 6

    echo "--- forged and malformed RIP"
    rip_inject tap0 $LAN_HOST 520 2 192.168.66.0,255.255.255.0,1 > /dev/null
    rip_inject br12 10.99.0.1 520 2 192.168.67.0,255.255.255.0,1 > /dev/null
    rip_inject br12 10.12.0.9 5555 2 192.168.68.0,255.255.255.0,1 > /dev/null
    rip_inject br12 10.12.0.9 520 2 127.0.0.0,255.0.0.0,1 224.0.0.0,240.0.0.0,1 10.66.0.1,255.255.255.0,1 \
        10.67.0.0,255.0.255.0,1 10.68.0.0,255.255.0.0,0 10.69.0.0,255.255.0.0,17 > /dev/null
    sleep 1
    check_output "RIP from a LAN host on a non-RIP port is ignored" "^none$" rip_route $ROUTER_LAN 192.168.66.0/24
    check_output "RIP from a source outside the link's network is ignored" "^none$" rip_route $ROUTER_LAN 192.168.67.0/24
    check_output "RIP responses not from port 520 are ignored" "^none$" rip_route $ROUTER_LAN 192.168.68.0/24
    check_output "invalid entries (loopback, multicast, host bits, broken mask, metric 0/17) are dropped" "^ignored, rx_bad " \
        sh -c "curl -s -u admin:$ADMIN_PW http://$ROUTER_LAN/api/stats | python3 -c \"
import json, sys
s = json.load(sys.stdin)
bad = [r['network'] for r in s['routes'] if r['network'] in ('127.0.0.0/8', '224.0.0.0/4', '10.66.0.0/24', '10.66.0.1/24', '10.67.0.0/16', '10.68.0.0/16', '10.69.0.0/16')]
print('installed %s' % bad if bad or s['rip']['rx_bad'] < 6 else 'ignored, rx_bad %d' % s['rip']['rx_bad'])\""
    # A neighbour on a RIP link is trusted (RIPv2 without authentication): its withdrawal works
    rip_inject br12 10.12.0.9 520 2 192.168.77.0,255.255.255.0,1 > /dev/null
    sleep 1
    check_output "a neighbour's route is learned (metric +1)" "^via 10.12.0.9 eth1 metric 2 origin rip$" \
        rip_route $ROUTER_LAN 192.168.77.0/24
    rip_inject br12 10.12.0.9 520 2 192.168.77.0,255.255.255.0,16 > /dev/null
    # Withdrawn with metric 16; around the triangle RIP may count to infinity first
    # (each router re-learns it from a neighbour that still has it, one hop worse,
    # until 16): the route must be gone within the bound that implies
    check_output "a withdrawn route disappears (after counting to infinity, if it does)" "^gone after " python3 - <<PY
import json, subprocess, time
start, seen = time.time(), []
while time.time() - start < 40:
    out = subprocess.run(['curl', '-s', '-u', 'admin:$ADMIN_PW', '--max-time', '5', 'http://$ROUTER_LAN/api/stats'],
                         capture_output=True, text=True).stdout
    r = [x for x in json.loads(out)['routes'] if x['network'] == '192.168.77.0/24']
    if not r:
        print('gone after %.1f s; metrics seen on the way: %s' % (time.time() - start, seen or 'none'))
        break
    if not seen or seen[-1] != r[0]['metric']:
        seen.append(r[0]['metric'])
    time.sleep(0.25)
else:
    print('still there after 40 s: %s' % seen)
PY
    check_output "/api/stats counts RIP traffic" "^counted " sh -c "curl -s -u admin:$ADMIN_PW http://$ROUTER_LAN/api/stats | python3 -c \"
import json, sys
r = json.load(sys.stdin)['rip']
# tx_triggered may be 0: with 2 s updates a periodic one often goes out before a trigger is due (1-5 s)
ok = r['rx_refused'] >= 2 and r['rx_bad'] >= 6 and r['tx_updates'] > 0 and r['route_changes'] > 0 and r['ports'] == ['eth1', 'eth2']
print(('counted ' if ok else 'wrong: ') + json.dumps({k: v for k, v in r.items() if k != 'routes'}))\""
    for r in r1 r2 r3; do
        if grep -q "K_PANIC" qemu_rip_$r.log; then
            fail "$r panicked: $(grep -m1 K_PANIC qemu_rip_$r.log)"
        fi
    done
    kill $R2 $R3 $WAN_PID 2>/dev/null
    QEMU_PID=$R1                        # the common ending stops it and waits for it
}

# ------------------------------------------------------------------- tcp ----

# $1 connections in parallel, $2 bytes each, against the router's echo service.
# Prints "intact <n>" if every connection got its data back byte for byte.
tcp_echo_bulk() {
    python3 - "$1" "$2" "$ROUTER_LAN" <<'PY'
import os, socket, sys, threading, time
conns, size, host = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
ok = []
def one():
    data = os.urandom(size)
    c = socket.create_connection((host, 7), timeout=30)
    threading.Thread(target=c.sendall, args=(data,), daemon=True).start()
    got = bytearray()
    while len(got) < size:
        chunk = c.recv(65536)
        if not chunk:
            break
        got += chunk
    c.close()
    ok.append(bytes(got) == data)
start = time.time()
threads = [threading.Thread(target=one) for _ in range(conns)]
for t in threads: t.start()
for t in threads: t.join()
elapsed = time.time() - start
mbit = conns * size * 2 * 8 / elapsed / 1e6
print(('intact %d' if all(ok) and len(ok) == conns else 'BROKEN %d') % sum(ok) + ' in %.2f s, %.1f Mbit/s both ways' % (elapsed, mbit))
PY
}

# Read the chargen stream to EOF; prints "stream <bytes> ok" if the pattern is right
tcp_chargen() {
    python3 -c "
import socket, time
start = time.time()
c = socket.create_connection(('$ROUTER_LAN', 19), timeout=30)
got = bytearray()
while True:
    chunk = c.recv(65536)
    if not chunk:
        break
    got += chunk
good = all(b == i % 251 for i, b in enumerate(got))
print('stream %d %s in %.2f s' % (len(got), 'ok' if good else 'CORRUPT', time.time() - start))"
}

scenario_tcp() {
    start_topology "eth0=$ROUTER_LAN/24 eth1=$ROUTER_WAN/24 tcp_test"

    echo "--- TCP"
    check_output "echo service answers" "^hello, router$" python3 -c "
import socket
c = socket.create_connection(('$ROUTER_LAN', 7), timeout=5)
c.sendall(b'hello, router'); print(c.recv(100).decode()); c.close()"
    check_output "1 MiB echoed intact" "^intact 1 " tcp_echo_bulk 1 1048576
    check_output "chargen: 1 MiB stream, then the router closes" "^stream 1048576 ok " tcp_chargen
    check_output "closed port: refused with RST" "^refused$" python3 -c "
import socket
try:
    socket.create_connection(('$ROUTER_LAN', 81), timeout=3); print('connected')
except ConnectionRefusedError:
    print('refused')"
    check_output "200 connections one after another" "^200 ok$" python3 -c "
import socket
ok = 0
for i in range(200):
    c = socket.create_connection(('$ROUTER_LAN', 7), timeout=5)
    msg = b'connection %d' % i
    c.sendall(msg)
    ok += c.recv(100) == msg
    c.close()
print('%d ok' % ok)"
    check_output "8 concurrent connections, 256 KiB each" "^intact 8 " tcp_echo_bulk 8 262144

    echo "--- TCP with 10% packet loss in both directions"
    tc qdisc add dev tap0 root netem loss 10%
    ip link add ifb0 type ifb
    ip link set ifb0 up
    tc qdisc add dev tap0 handle ffff: ingress
    tc filter add dev tap0 parent ffff: matchall action mirred egress redirect dev ifb0
    tc qdisc add dev ifb0 root netem loss 10%
    check_output "256 KiB echoed intact despite loss" "^intact 1 " tcp_echo_bulk 1 262144
    check_output "chargen intact despite loss" "^stream 1048576 ok " tcp_chargen
    check_output "4 concurrent connections intact despite loss" "^intact 4 " tcp_echo_bulk 4 65536
    tc qdisc del dev tap0 root
    tc qdisc del dev tap0 ingress
    check_ping "router is healthy afterwards" "0%" ping -c 3 -i 0.2 -W 2 $ROUTER_LAN
}

scenario_$SCENARIO

kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
exit $FAILED
