/*
    RIPv2 messages to the router's RIP ports (both eth0 and eth1 speak RIP here),
    wrapped in valid UDP/IPv4/Ethernet: responses and requests from neighbours
    and from addresses that aren't, with link flaps and time passing, so the
    route table, timers, triggered updates and garbage collection all run.

    Input: records of [op][length, 2 bytes little endian][RIP message]
      op bit 0   port: eth0 (10.0.9.0/24) or eth1 (10.0.8.0/24)
      op bit 1   source port 520, or 5555
      op 2-3     source: neighbour .1, .3, .77 on that port's network, or 10.99.0.1 (off-link)
      op bit 4   flip the port's link state first
      op 5-7     advance the clock by 1 s per step first
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=10.0.8.2/24 rip=eth0,eth1 rip_timers=2,6,4 " \
                     "http admin_password=" FUZZ_PASSWORD

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t hosts[3] = { 1, 3, 77 };
    fuzz_boot(FUZZ_CMDLINE);
    size_t i = 0;
    while (i + 3 <= size) {
        uint8_t op = data[i];
        size_t len = (size_t)data[i + 1] | (size_t)data[i + 2] << 8;
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
        int port = op & 1;
        if (op & 0x10) {
            fuzz_dev[port]->link_up = !fuzz_dev[port]->link_up;
        }
        if (op >> 5) {
            fuzz_run_ms((uint32_t)(op >> 5) * 1000);
        }
        uint32_t net = port == FUZZ_LAN ? FUZZ_IP(10, 0, 9, 0) : FUZZ_IP(10, 0, 8, 0);
        int who = (op >> 2) & 3;
        uint32_t src = who < 3 ? net | hosts[who] : FUZZ_IP(10, 99, 0, 1);
        uint8_t frame[ETH_FRAME_MAX];
        size_t n = fuzz_udp_frame(frame, port, broadcast, src, (op & 2) ? 5555 : 520,
                                  0xFFFFFFFF, 520, data + i, len);
        fuzz_receive(port, frame, n);
        i += len;
    }
    fuzz_finish();
    return 0;
}
