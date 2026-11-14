#!/bin/sh
# Attack-surface measurement (thesis plan WP0). Boots a router between a LAN host
# and a WAN host in network namespaces (no root needed, like check_net.sh) and
# records what each side can reach, plus the router's own view of itself.
#
#   sh measure/attack_surface.sh lazydays [kernel]   LazyDaysOS in its home-router configuration
#   sh measure/attack_surface.sh openwrt  [image]    OpenWrt 19.07.10 x86-64, factory defaults
#
#   LAN host  ----  router  ----  WAN host (dnsmasq hands the router 10.0.8.2, gateway 10.0.8.1)
#
# For each router:
#   - TCP and UDP port scans of all 65535 ports from the LAN and from the WAN (nmap)
#   - service versions on the open ports (nmap -sV, LAN)
#   - the router's own view: listening sockets, processes, memory, flash use (OpenWrt
#     over the serial console; LazyDaysOS from /api/stats and the image)
#   - login without a password (OpenWrt ships without a root password)
# Output: measure/results/<router>.txt. Needs nmap; dnsmasq, qemu as for check_net.sh.
# SKIP_SCANS=1 skips the nmap scans (everything else still runs).
# OpenWrt rate-limits what a full scan needs: ICMP errors (icmp_ratelimit,
# icmp_msgs_per_sec) and, in its default firewall, incoming SYNs (syn_flood, 25/s).
# Probes that hit those limits look "filtered", and an open port could hide among
# them, so the script lifts the limits over the console first and logs that it did.
# This is a measurement convenience: no service or firewall rule changes.

ROUTER=${1:-lazydays}
REPO=$(cd "$(dirname "$0")/.." && pwd)
case $ROUTER in
    lazydays) IMAGE=${2:-$REPO/LazyDaysOS.bin} ;;
    openwrt) IMAGE=${2:-$HOME/opt/openwrt/openwrt-19.07.10-x86-64-combined-ext4.img} ;;
    *) echo "usage: $0 lazydays|openwrt [image]"; exit 2 ;;
esac
OUT=$REPO/measure/results/$ROUTER.txt
LAZY_PASSWORD=measure-only-pw

if [ "$LAZY_IN_NETNS" != 1 ]; then
    tools="dnsmasq unshare nsenter curl python3"
    [ "$SKIP_SCANS" = 1 ] || tools="nmap $tools"
    for tool in $tools; do
        command -v $tool >/dev/null || { echo "attack_surface.sh needs $tool"; exit 1; }
    done
    [ -f "$IMAGE" ] || { echo "no image $IMAGE"; exit 1; }
    mkdir -p "$REPO/measure/results"
    LAZY_IN_NETNS=1 exec unshare --user --map-root-user --net sh "$0" "$ROUTER" "$IMAGE"
fi

# ---- inside the LAN host's namespace ----
TMP=$(mktemp -d /tmp/as.XXXXXX)
LOG=$TMP/serial.log
WAN_IP=10.0.8.2
if [ "$ROUTER" = lazydays ]; then
    LAN_IP=10.0.9.2; LAN_HOST=10.0.9.1
else
    LAN_IP=192.168.1.1; LAN_HOST=192.168.1.2
fi
section() { printf '\n==== %s\n' "$1" | tee -a "$OUT"; }
record() { "$@" 2>&1 | tee -a "$OUT"; }

ip link set lo up
ip tuntap add dev tap0 mode tap
ip tuntap add dev tap1 mode tap
ip addr add $LAN_HOST/24 dev tap0
ip link set tap0 up

KVM=""
[ -w /dev/kvm ] && KVM=-enable-kvm
NICS="-netdev tap,id=t0,ifname=tap0,script=no,downscript=no -device e1000,netdev=t0,mac=52:54:00:12:34:56
      -netdev tap,id=t1,ifname=tap1,script=no,downscript=no -device e1000,netdev=t1,mac=52:54:00:12:34:57"
if [ "$ROUTER" = lazydays ]; then
    qemu-system-i386 $KVM -m 128M -kernel "$IMAGE" -display none -no-reboot -serial file:$LOG $NICS \
        -append "eth0=$LAN_IP/24 eth1=dhcp dhcpd=eth0,10.0.9.100,10.0.9.150 nat=eth1 http admin_password=$LAZY_PASSWORD" &
else
    qemu-system-x86_64 $KVM -m 128M -drive file="$IMAGE",format=raw,if=virtio -snapshot -display none -no-reboot \
        -serial unix:$TMP/console.sock,server,nowait $NICS &
fi
QEMU_PID=$!
unshare --net sleep 3600 &
WAN_PID=$!
trap 'kill $QEMU_PID $WAN_PID $DNSMASQ_PID 2>/dev/null; rm -rf $TMP' EXIT
wan() { nsenter --target $WAN_PID --net "$@"; }
sleep 0.5
ip link set tap1 netns $WAN_PID
wan ip link set lo up
wan ip addr add 10.0.8.1/24 dev tap1
wan ip link set tap1 up
wan dnsmasq --no-daemon --conf-file=/dev/null --user=root --port=0 --interface=tap1 --bind-interfaces \
    --dhcp-authoritative --dhcp-range=10.0.8.100,10.0.8.150,10m --dhcp-host=52:54:00:12:34:57,$WAN_IP \
    --dhcp-option=3,10.0.8.1 --dhcp-leasefile=$TMP/leases > $TMP/dnsmasq.log 2>&1 &
DNSMASQ_PID=$!

console() { python3 "$REPO/measure/console.py" $TMP/console.sock "$@"; }

echo "booting $ROUTER ($IMAGE)..."
if [ "$ROUTER" = openwrt ]; then
    sleep 1
    console wait "Please press Enter to activate this console" 180 || { echo "OpenWrt did not boot"; exit 1; }
    console run "sysctl -w net.ipv4.icmp_ratelimit=0 net.ipv4.icmp_msgs_per_sec=1000000 net.ipv4.icmp_msgs_burst=1000000" \
        "uci set firewall.@defaults[0].syn_flood=0 && uci commit firewall && /etc/init.d/firewall reload" > /dev/null
fi
for i in $(seq 1 120); do
    grep -q "DHCPACK(tap1) $WAN_IP" $TMP/dnsmasq.log 2>/dev/null && ping -c 1 -W 1 $LAN_IP >/dev/null 2>&1 && break
    sleep 1
done
grep -q "DHCPACK(tap1) $WAN_IP" $TMP/dnsmasq.log || { echo "the router got no WAN lease"; cat $TMP/dnsmasq.log; exit 1; }
sleep 5                                         # let services settle

: > "$OUT"
section "router: $ROUTER, image $(basename "$IMAGE") ($(stat -c %s "$IMAGE") bytes), measured $(date -u +%Y-%m-%dT%H:%MZ)"
[ "$ROUTER" = openwrt ] && echo "(ICMP rate limits and the firewall's syn_flood limit lifted for the scan)" | tee -a "$OUT"

# ---- from outside ----
scan() {    # $1 side (LAN/WAN), $2 target, rest: how to run nmap on that side
    side=$1; target=$2; shift 2
    section "$side: TCP, all ports"
    record "$@" nmap -n -Pn -sS -p- -T4 --min-rate 3000 --max-retries 2 $target
    section "$side: UDP, all ports"
    record "$@" nmap -n -Pn -sU -p- -T4 --min-rate 3000 --max-retries 2 $target
}
if [ "$SKIP_SCANS" != 1 ]; then
section "nmap $(nmap --version | head -1 | cut -d' ' -f3)"
scan LAN $LAN_IP
section "LAN: service versions on open TCP ports"
open=$(grep -E "^[0-9]+/tcp +open " "$OUT" | cut -d/ -f1 | sort -un | paste -sd, -)
[ -n "$open" ] && record nmap -n -Pn -sV -p "$open" $LAN_IP
section "LAN: IPv6, link-local address, TCP and UDP, all ports"
LL=fe80::5054:ff:fe12:3456%tap0                  # from the router's LAN MAC
record nmap -6 -n -Pn -sS -p- -T4 --min-rate 3000 --max-retries 2 $LL
record nmap -6 -n -Pn -sU -p- -T4 --min-rate 3000 --max-retries 2 $LL
wan ip route add 10.0.9.0/24 via $WAN_IP 2>/dev/null; wan ip route add 192.168.1.0/24 via $WAN_IP 2>/dev/null
scan WAN $WAN_IP wan
section "WAN: TCP to the router's LAN address (routed via its WAN address)"
record wan nmap -n -Pn -sS -p- -T4 --min-rate 3000 --max-retries 2 $LAN_IP
fi

# ---- the router's own view ----
if [ "$ROUTER" = openwrt ]; then
    section "OpenWrt: login over SSH from the LAN with no password"
    if command -v ssh >/dev/null; then
        # -F /dev/null: in the user namespace the host's root-owned ssh config looks foreign
        # An empty password, answered by an askpass that prints nothing; OpenWrt's dropbear
        # only offers the legacy ssh-rsa host key, which current OpenSSH has to be told to accept
        record env SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force ssh -F /dev/null \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o HostKeyAlgorithms=+ssh-rsa \
            -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive \
            -o NumberOfPasswordPrompts=1 -o ConnectTimeout=5 root@$LAN_IP 'echo logged in as $(id -un) on $(uname -n)'
    else
        echo "(no ssh client)" | tee -a "$OUT"
    fi
    section "OpenWrt: web interface (LuCI) login from the LAN with an empty password"
    record curl -s -o /dev/null -D - -d 'luci_username=root&luci_password=' http://$LAN_IP/cgi-bin/luci \
        | grep -iE "^HTTP/|^set-cookie|^location" | tr -d '\r'
    section "OpenWrt: inside view (serial console)"
    record console run "cat /etc/openwrt_release" "uname -a" "netstat -lntu" "ps w" "free" "df -k /" \
        "opkg list-installed | wc -l" "opkg list-installed" \
        "ls -l --color=never /usr/sbin/dnsmasq /usr/sbin/dropbear /usr/sbin/uhttpd /usr/sbin/odhcpd /sbin/netifd /sbin/procd /bin/busybox /lib/libc.so" \
        "du -sk /usr/lib/lua/luci /www" "iptables -S | wc -l" "iptables -S zone_wan_input"
else
    section "LazyDaysOS: API without the password, and with an empty one"
    record curl -s -o /dev/null -w '%{http_code}\n' http://$LAN_IP/api/stats
    record curl -s -o /dev/null -w '%{http_code}\n' --data-urlencode password= http://$LAN_IP/api/login
    section "LazyDaysOS: inside view"
    record curl -s -u admin:$LAZY_PASSWORD http://$LAN_IP/api/stats
    echo | tee -a "$OUT"
    record grep -E "^\[K_INFO\] (HTTP|dhcp|nat|auth)" $LOG
fi
echo
echo "results: $OUT"
