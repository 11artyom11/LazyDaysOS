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
#
# Kernel serial output goes to qemu_<scenario>.log, traffic to qemu_<scenario>_{lan,wan}.pcap.

KERNEL=${1:-LazyDaysOS.bin}
SCENARIOS="${SCENARIOS:-static home tcp}"
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
    start_topology "eth0=$ROUTER_LAN/24 eth1=dhcp dhcpd=eth0,10.0.9.100,10.0.9.150 dhcp_lease=600 nat=eth1 http"
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
    check_output "GET / serves the page" "^200 text/html.*LazyDaysOS" sh -c \
        "curl -s --max-time 5 -w '%{http_code} %{content_type} ' -o $TMP/index $API/ && head -c 300 $TMP/index | tr -d '\n'"
    curl -s --max-time 5 $API/api/stats > $TMP/stats.json
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
    curl -s --max-time 5 "$API/api/events" > $TMP/events.json
    check_output "/api/events logs leases, devices and the WAN lease" "^logged$" python3 -c "
import json
e = json.load(open('$TMP/events.json'))
texts = ' | '.join(x['category'] + ': ' + x['text'] for x in e)
need = ['dhcp: 10.0.9.1', 'lazy-laptop', 'client: new device 10.0.9.1 on eth0', 'wan: eth1 got 10.0.8.2', 'system: kernel ready']
missing = [n for n in need if n not in texts]
print('missing %s in %s' % (missing, texts[:300]) if missing else 'logged')"
    last=$(python3 -c "import json; print(json.load(open('$TMP/events.json'))[-1]['id'])")
    check_output "/api/events?since=<last id> returns nothing new" "^\[\]$" curl -s --max-time 5 "$API/api/events?since=$last"

    check_ping "second LAN host reaches the WAN" "0%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "POST /api/block?ip=10.0.9.3" '"blocked":true' curl -s --max-time 5 -X POST "$API/api/block?ip=10.0.9.3"
    check_ping "blocked device can't reach the WAN" "100%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 1 $WAN_HOST
    check_ping "other devices are unaffected" "0%" ping -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "blocked device can still open the dashboard" "^200$" \
        curl -s --max-time 5 --interface 10.0.9.3 -o /dev/null -w '%{http_code}' $API/api/stats
    check_output "stats show the device as blocked" '"ip":"10.0.9.3"[^}]*"blocked":true' curl -s --max-time 5 $API/api/stats
    check_output "POST /api/unblock?ip=10.0.9.3" '"blocked":false' curl -s --max-time 5 -X POST "$API/api/unblock?ip=10.0.9.3"
    check_ping "unblocked device reaches the WAN again" "0%" ping -I 10.0.9.3 -c 3 -i 0.2 -W 2 $WAN_HOST
    check_output "block and unblock were logged" "unblocked 10.0.9.3" curl -s --max-time 5 "$API/api/events?since=$last"

    check_output "from the WAN: 403" "^403$" wan curl -s --max-time 5 -o /dev/null -w '%{http_code}' http://$ROUTER_WAN/
    check_output "unknown path: 404" "^404$" curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/nope
    check_output "wrong method: 405" "^405 405$" sh -c \
        "curl -s --max-time 5 -o /dev/null -w '%{http_code} ' -X DELETE $API/api/stats; curl -s --max-time 5 -o /dev/null -w '%{http_code}' $API/api/block"
    check_output "bad parameter: 400" "^400$" curl -s --max-time 5 -o /dev/null -w '%{http_code}' -X POST "$API/api/block?ip=999.1.1.1"
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
        codes.append(urllib.request.urlopen('$API/api/stats', timeout=15).status)
    except Exception as e:
        codes.append(repr(e))
threads = [threading.Thread(target=get) for _ in range(30)]
for t in threads: t.start()
for t in threads: t.join()
print('30 x 200' if codes.count(200) == 30 else codes)"
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
