#!/bin/sh
# Live demo: LazyDaysOS as a home router with a few simulated devices, no root
# needed. Open the dashboard at http://localhost:8080 (DEMO_PORT to change).
#
#   devices on the LAN          LazyDaysOS             "the Internet"
#   examiner-phone  \                                  WAN host 10.0.8.1:
#   thesis-laptop    |  tap0  eth0 10.0.9.2/24          - DHCP server (dnsmasq)
#   smart-tv         |------  DHCP server, NAT  eth1 -- - 192.168.50.1 serves
#   game-console    /         dashboard on :80   DHCP     files and a UDP sink
#   admin PC 10.0.9.1 (static) <- the dashboard is reached from here
#
# Each device is a macvlan interface with its own MAC, so it gets its own DHCP
# lease and hostname, and generates its own kind of traffic through the NAT.
# The dashboard is bridged out of the private namespace through a UNIX socket.
#
#   DEMO_SECONDS=N   stop after N seconds instead of following the kernel log
#   DEMO_PORT=N      local port for the dashboard (default 8080)
#   DEMO_PASSWORD=P  the dashboard's admin password (default lazydays)

KERNEL=${1:-LazyDaysOS.bin}
PORT=${DEMO_PORT:-8080}
PASSWORD=${DEMO_PASSWORD:-lazydays}
LOG=qemu_demo.log

if [ "$LAZY_IN_NETNS" != 1 ]; then
    for tool in unshare socat dnsmasq busybox curl python3 qemu-system-i386; do
        command -v $tool >/dev/null || { echo "demo.sh needs $tool"; exit 1; }
    done
    if [ ! -f "$KERNEL" ]; then
        echo "Building $KERNEL first..."
        make "$KERNEL" >/dev/null || exit 1
    fi
    if ! unshare --user --map-root-user --net true 2>/dev/null; then
        echo "unprivileged network namespaces are not available"
        exit 1
    fi
    SOCK_DIR=$(mktemp -d /tmp/lazydays.XXXXXX)
    export LAZY_DEMO_SOCK=$SOCK_DIR/dashboard.sock
    socat TCP-LISTEN:$PORT,bind=127.0.0.1,reuseaddr,fork UNIX-CONNECT:$LAZY_DEMO_SOCK 2>/dev/null &
    BRIDGE=$!
    trap 'kill $BRIDGE 2>/dev/null; rm -rf $SOCK_DIR' EXIT INT TERM
    LAZY_IN_NETNS=1 unshare --user --map-root-user --net sh "$0" "$KERNEL"
    exit $?
fi

# ---- inside the LAN's private network namespace ----

TMP=$(mktemp -d)
PIDS=""
started() { PIDS="$PIDS $1"; }
cleanup() {
    trap - EXIT INT TERM
    kill $PIDS 2>/dev/null
    rm -rf $TMP
    echo
    echo "Demo stopped."
    exit 0
}
trap cleanup EXIT INT TERM
wan() { nsenter --target $WAN_PID --net "$@"; }

# Several hosts share one LAN segment here: make each answer ARP only for itself
sysctl -qw net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2 \
    net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0 2>/dev/null

ip link set lo up
ip tuntap add dev tap0 mode tap
ip tuntap add dev tap1 mode tap
ip addr add 10.0.9.1/24 dev tap0
ip link set tap0 up

rm -f $LOG
qemu-system-i386 -kernel "$KERNEL" \
    -append "eth0=10.0.9.2/24 eth1=dhcp dhcpd=eth0,10.0.9.100,10.0.9.150 nat=eth1 http admin_password=$PASSWORD" \
    -display none -no-reboot -serial file:$LOG \
    -netdev tap,id=t0,ifname=tap0,script=no,downscript=no -device e1000,netdev=t0 \
    -netdev tap,id=t1,ifname=tap1,script=no,downscript=no -device e1000,netdev=t1 &
started $!

# The upstream: DHCP for the router's WAN port, a file server and a UDP sink
unshare --net sleep 100000 &
WAN_PID=$!
started $WAN_PID
sleep 0.3
ip link set tap1 netns $WAN_PID
wan ip link set lo up
wan ip addr add 192.168.50.1/32 dev lo
wan ip addr add 10.0.8.1/24 dev tap1
wan ip link set tap1 up
wan dnsmasq --no-daemon --conf-file=/dev/null --user=root --port=0 --interface=tap1 --bind-interfaces \
    --dhcp-authoritative --dhcp-range=10.0.8.100,10.0.8.150,1h --dhcp-option=3,10.0.8.1 \
    --dhcp-option=6,10.0.8.53 --dhcp-leasefile=$TMP/wan.leases --pid-file=$TMP/dnsmasq.pid >/dev/null 2>&1 &
started $!
mkdir -p $TMP/www
head -c 64M /dev/urandom > $TMP/www/video
wan python3 -m http.server 8000 --bind 192.168.50.1 --directory $TMP/www >/dev/null 2>&1 &
started $!
wan socat -u UDP-RECV:9000,bind=192.168.50.1 /dev/null &
started $!

# The dashboard bridge: UNIX socket (shared filesystem) -> router port 80
socat UNIX-LISTEN:$LAZY_DEMO_SOCK,fork TCP:10.0.9.2:80 2>/dev/null &
started $!

echo "Booting LazyDaysOS..."
until grep -q 'getting kernel ready' $LOG 2>/dev/null; do sleep 0.2; done
until grep -q 'dhcp: eth1 bound' $LOG 2>/dev/null; do sleep 0.2; done

# The devices: each one gets a DHCP lease from the router under its own name
cat > $TMP/udhcpc.sh <<'EOF'
#!/bin/sh
case "$1" in
    bound|renew)
        ip addr flush dev "$interface"
        ip addr add "$ip/$mask" dev "$interface"
        ip route replace default via "$router" dev "$interface" metric "$METRIC"
        ;;
esac
EOF
chmod +x $TMP/udhcpc.sh
n=0
for name in examiner-phone thesis-laptop smart-tv game-console; do
    n=$((n + 1))
    ip link add link tap0 name $name address 02:00:00:00:10:0$n type macvlan mode bridge
    ip link set $name up
    # One default route per device (distinct metrics); sockets bound to a device use its own
    METRIC=$((100 + n)) busybox udhcpc -i $name -f -x hostname:$name -s $TMP/udhcpc.sh >/dev/null 2>&1 &
    started $!
done
for name in examiner-phone thesis-laptop smart-tv game-console; do
    until ip -4 addr show $name | grep -q inet; do sleep 0.2; done
done

# Their traffic, all through the NAT to "the Internet"
download() {   # device, rate, seconds per transfer, pause between transfers
    while true; do
        curl -s -o /dev/null --interface $1 --limit-rate $2 -m $3 http://192.168.50.1:8000/video
        sleep $4
    done
}
download smart-tv 1500k 30 0 &                  # streaming, ~12 Mbit/s
started $!
download thesis-laptop 250k 20 1 &              # steady, ~2 Mbit/s
started $!
download examiner-phone 800k 3 5 &              # bursts
started $!
python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'game-console')
while True:
    for _ in range(25):
        s.sendto(bytes(1000), ('192.168.50.1', 9000))
    time.sleep(0.1)" &                          # uploads ~2 Mbit/s
started $!
ping -q -I game-console -i 0.2 192.168.50.1 >/dev/null 2>&1 &
started $!

echo
echo "  LazyDaysOS dashboard:  http://localhost:$PORT   (password: $PASSWORD)"
echo
echo "  4 devices online: examiner-phone, thesis-laptop, smart-tv, game-console"
echo "  Try blocking one; its traffic graph drops to zero within a second."
echo "  Ctrl+C stops the demo. Kernel log follows."
echo

# In the background plus wait, so Ctrl+C or a TERM reaches the cleanup trap at once
if [ -n "$DEMO_SECONDS" ]; then
    sleep "$DEMO_SECONDS" &
else
    tail -n +1 -f $LOG &
fi
started $!
wait $!
