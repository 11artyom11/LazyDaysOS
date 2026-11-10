#!/bin/sh
# Throughput presets for the "LazyDaysOS throughput" GNS3 lab. Runs on the
# client containers (installed as /lab/tests.sh); the iperf3 server address is
# in /lab/server.
#
#   sh /lab/tests.sh [preset] [seconds]      default: all, 10 s each
#
#   tcp     one TCP stream, client -> server
#   tcp4    four parallel TCP streams
#   rev     one TCP stream, server -> client (downloads; through the NAT inbound)
#   udp100  UDP at 100 Mbit/s: loss and jitter at a fixed rate
#   udp500  UDP at 500 Mbit/s
#   small   UDP with 64-byte payloads at 20 Mbit/s: packets per second
#   all     every preset in turn, one summary line each

SRV=$(cat /lab/server)
T=${2:-10}

run() {
    name=$1; shift
    out=$(iperf3 -c "$SRV" -t "$T" -J "$@" 2>&1)
    if ! echo "$out" | jq -e '.end.sum_received // .end.sum' >/dev/null 2>&1; then
        printf '%-7s  error: %s\n' "$name" \
            "$(echo "$out" | jq -r '.error // empty' 2>/dev/null || echo "$out" | head -1)"
        return
    fi
    echo "$out" | jq -r --arg n "$name" --argjson t "$T" '
        def r(x): (x * 10 | round) / 10;
        if .start.test_start.protocol == "UDP" then
            (.end.sum_received // .end.sum) as $s |
            "\($n | . + "       " | .[0:7])  \(r($s.bits_per_second / 1e6)) Mbit/s  loss \(r($s.lost_percent))%  jitter \(r($s.jitter_ms)) ms  \(($s.packets - $s.lost_packets) / $t | floor) pkt/s delivered"
        else
            "\($n | . + "       " | .[0:7])  \(r(.end.sum_received.bits_per_second / 1e6)) Mbit/s  retransmits \(.end.sum_sent.retransmits // 0)"
        end'
}

preset() {
    case $1 in
        tcp)    run tcp ;;
        tcp4)   run tcp4 -P 4 ;;
        rev)    run rev -R ;;
        udp100) run udp100 -u -b 100M ;;
        udp500) run udp500 -u -b 500M ;;
        small)  run small -u -l 64 -b 20M ;;
        *)      echo "unknown preset '$1' (tcp tcp4 rev udp100 udp500 small all)"; exit 1 ;;
    esac
}

echo "iperf3 to $SRV, ${T}s per test"
if [ "${1:-all}" = all ]; then
    for p in tcp tcp4 rev udp100 udp500 small; do
        preset $p
        sleep 1
    done
else
    preset "$1"
fi
