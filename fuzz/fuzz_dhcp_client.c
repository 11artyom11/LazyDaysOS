/*
    DHCP replies to the router's DHCP client on the WAN port (eth1=dhcp): offers,
    ACKs, NAKs with arbitrary options, at arbitrary times in the client's state
    machine (selecting, requesting, bound, renewing, rebinding).

    Input: records of [op][length, 2 bytes little endian][DHCP message]
      op bit 0   overwrite xid with the client's current transaction id
      op bit 1   overwrite chaddr with the router's WAN MAC
      op 2-7     advance the clock by 1 s per step first
    Sent from 10.0.8.1:67 to 255.255.255.255:68.
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=dhcp nat=eth1 dhcpd=eth0,10.0.9.100,10.0.9.150 " \
                     "http admin_password=" FUZZ_PASSWORD

/* The xid of the last DHCP message the router sent on the WAN port */
static bool fuzz$$client_xid(uint8_t xid[4])
{
    for (size_t k = fuzz_tx_count; k > 0 && k + FUZZ_TX_MAX > fuzz_tx_count; k--) {
        const struct fuzz_frame* f = &fuzz_tx[(k - 1) % FUZZ_TX_MAX];
        const uint8_t* ip = f->data + 14;
        if (f->port == FUZZ_WAN && f->len >= 14 + 20 + 8 + 8 && fuzz_load16(f->data + 12) == 0x0800 &&
            ip[9] == 17 && fuzz_load16(ip + 20) == 68) {
            memcpy(xid, ip + 28 + 4, 4);
            return true;
        }
    }
    return false;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static uint8_t boot_xid[4];
    static bool have_boot_xid = false;
    fuzz_boot(FUZZ_CMDLINE);
    if (!have_boot_xid) {
        /* The DISCOVER sent during boot, before this first input overwrites fuzz_tx */
        have_boot_xid = true;
        size_t now = fuzz_tx_count;
        fuzz_tx_count = fuzz_boot_tx;
        fuzz$$client_xid(boot_xid);
        fuzz_tx_count = now;
    }
    size_t i = 0;
    while (i + 3 <= size) {
        uint8_t op = data[i];
        size_t len = (size_t)data[i + 1] | (size_t)data[i + 2] << 8;
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
        if (op >> 2) {
            fuzz_run_ms((uint32_t)(op >> 2) * 1000);
        }
        uint8_t message[ETH_FRAME_MAX];
        size_t n = len < sizeof(message) ? len : sizeof(message);
        memcpy(message, data + i, n);
        uint8_t xid[4];
        if ((op & 1) && n >= 8 && (fuzz$$client_xid(xid) || (memcpy(xid, boot_xid, 4), true))) {
            memcpy(message + 4, xid, 4);
        }
        if ((op & 2) && n >= 34) {
            memcpy(message + 28, fuzz_router_mac[FUZZ_WAN], 6);
        }
        uint8_t frame[ETH_FRAME_MAX];
        size_t fl = fuzz_udp_frame(frame, FUZZ_WAN, broadcast, FUZZ_WAN_GATEWAY, 67, 0xFFFFFFFF, 68, message, n);
        fuzz_receive(FUZZ_WAN, frame, fl);
        i += len;
    }
    fuzz_finish();
    return 0;
}
