/*
    DHCP messages to the router's DHCP server on the LAN port (the harness wraps
    them in UDP/IPv4/Ethernet with valid checksums).

    Input: records of [op][length, 2 bytes little endian][DHCP message]
      op bit 0   0: from 0.0.0.0:68 to 255.255.255.255:67 (DISCOVER, INIT-REBOOT)
                 1: from 10.0.9.(100 + op>>2 % 64):68 to the router (renewal)
      op 1-7     advance the clock by 100 ms per step first (bits 1-7)
    The source MAC is the message's chaddr when there is one.
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=10.0.8.2/24 gw=10.0.8.1 nat=eth1 dhcpd=eth0,10.0.9.100,10.0.9.150 " \
                     "dhcp_lease=60 http admin_password=" FUZZ_PASSWORD

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    fuzz_boot(FUZZ_CMDLINE);
    size_t i = 0;
    while (i + 3 <= size) {
        uint8_t op = data[i];
        size_t len = (size_t)data[i + 1] | (size_t)data[i + 2] << 8;
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
        if (op >> 1) {
            fuzz_run_ms((uint32_t)(op >> 1) * 100);
        }
        uint8_t frame[ETH_FRAME_MAX];
        bool renew = op & 1;
        uint32_t src = renew ? FUZZ_IP(10, 0, 9, 100 + (op >> 2) % 64) : 0;
        uint32_t dst = renew ? FUZZ_ROUTER_LAN : 0xFFFFFFFF;
        size_t n = fuzz_udp_frame(frame, FUZZ_LAN, renew ? fuzz_router_mac[FUZZ_LAN] : broadcast,
                                  src, 68, dst, 67, data + i, len);
        if (len >= 34) {
            memcpy(frame + 6, data + i + 28, 6);   /* chaddr as the source MAC */
        }
        fuzz_receive(FUZZ_LAN, frame, n);
        i += len;
    }
    fuzz_finish();
    return 0;
}
