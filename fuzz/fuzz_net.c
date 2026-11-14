/*
    Raw Ethernet frames into the router: everything from ARP to NAT, forwarding,
    ICMP errors, UDP, TCP and HTTP. Built twice: "net" (both ports) and "wan"
    (-DFUZZ_WAN_ONLY: only what an Internet attacker can send).

    Input: records of [op][length, 2 bytes little endian][frame]
      op bit 0   port (LAN/WAN; ignored for wan)
      op bit 1   fix the IPv4 and TCP/UDP/ICMP checksums
      op bit 2   set the destination MAC to the router's
      op 3-7     advance the clock by 40 ms per step first
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=10.0.8.2/24 gw=10.0.8.1 nat=eth1 dhcpd=eth0,10.0.9.100,10.0.9.150 " \
                     "route=192.168.60.0/24,10.0.9.250 rip=eth0 http udp_echo admin_password=" FUZZ_PASSWORD

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fuzz_boot(FUZZ_CMDLINE);
    size_t i = 0;
    while (i + 3 <= size) {
        uint8_t op = data[i];
        size_t len = (size_t)data[i + 1] | (size_t)data[i + 2] << 8;
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
#ifdef FUZZ_WAN_ONLY
        int port = FUZZ_WAN;
#else
        int port = op & 1;
#endif
        if (op >> 3) {
            fuzz_run_ms((uint32_t)(op >> 3) * 40);
        }
        uint8_t frame[ETH_FRAME_MAX];
        size_t n = len < sizeof(frame) ? len : sizeof(frame);
        memcpy(frame, data + i, n);
        if ((op & 4) && n >= 6) {
            memcpy(frame, fuzz_router_mac[port], 6);
        }
        if (op & 2) {
            fuzz_fix_checksums(frame, n);
        }
        fuzz_receive(port, frame, n);
        i += len;
    }
    fuzz_finish();
    return 0;
}
