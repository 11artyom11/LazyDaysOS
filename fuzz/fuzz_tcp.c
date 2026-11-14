/*
    The TCP state machine: after a real handshake with the HTTP port, arbitrary
    segments whose sequence and acknowledgement numbers are given relative to
    the connection, so the fuzzer can explore in-window, old, future, partial
    and overlapping segments, resets, FINs, window changes and timing.

    Input: [first byte: 0 = handshake normally, else leave the SYN-ACK unanswered]
           records of [flags][seq delta, 2 bytes][ack delta, 2 bytes][window, 2 bytes][wait][len][payload]
      seq = our next sequence number + (int16) seq delta
      ack = the router's next sequence number + (int16) ack delta
      wait = advance the clock by 20 ms per step afterwards
    Nothing is acknowledged automatically after the handshake.
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=10.0.8.2/24 gw=10.0.8.1 nat=eth1 http admin_password=" FUZZ_PASSWORD

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fuzz_boot(FUZZ_CMDLINE);
    if (size < 1) {
        fuzz_finish();
        return 0;
    }
    static struct fuzz_tcp_client c;
    bool open = data[0] == 0 ? fuzz_tcp_open(&c, 41000, 80) : false;
    if (data[0] != 0) {
        memset(&c, 0, sizeof(c));
        c.sport = 41000;
        c.dport = 80;
        c.snd_nxt = 0x20000000u;
        c.scanned = fuzz_tx_count;
        fuzz_tcp_send(&c, FUZZ_TCP_SYN, NULL, 0);
        fuzz_tcp_scan(&c);              /* learn the router's ISN without answering */
        open = true;
    }
    size_t i = 1;
    while (open && i + 9 <= size) {
        uint8_t flags = data[i];
        int16_t seq_delta = (int16_t)(data[i + 1] | data[i + 2] << 8);
        int16_t ack_delta = (int16_t)(data[i + 3] | data[i + 4] << 8);
        uint16_t window = (uint16_t)(data[i + 5] | data[i + 6] << 8);
        uint8_t wait = data[i + 7];
        size_t len = data[i + 8];
        i += 9;
        if (len > size - i) {
            len = size - i;
        }
        uint8_t frame[ETH_FRAME_MAX];
        uint32_t seq = c.snd_nxt + (uint32_t)(int32_t)seq_delta;
        size_t n = fuzz_tcp_frame(frame, FUZZ_LAN, FUZZ_LAN_HOST, c.sport, FUZZ_ROUTER_LAN, c.dport,
                                  seq, c.rcv_nxt + (uint32_t)(int32_t)ack_delta, flags, window, data + i, len);
        if (seq_delta == 0) {
            c.snd_nxt += (uint32_t)len + ((flags & FUZZ_TCP_SYN) ? 1 : 0) + ((flags & FUZZ_TCP_FIN) ? 1 : 0);
        }
        fuzz_receive(FUZZ_LAN, frame, n);
        i += len;
        if (wait) {
            fuzz_run_ms((uint32_t)wait * 20);
        }
        fuzz_tcp_scan(&c);              /* track the router's sequence space, acknowledge nothing */
    }
    fuzz_finish();
    return 0;
}
