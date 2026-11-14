#ifndef __FUZZ_COMMON_H__
#define __FUZZ_COMMON_H__

/*
    Host-side harness for fuzzing the network stack (kernel/net, kernel/random.c,
    kernel/cmdline.c, libk/util/strbuf.c, compiled unchanged for Linux).

    The router has two fake ports, eth0 (LAN 10.0.9.2/24) and eth1 (WAN, NAT
    outside port, 10.0.8.2/24 or DHCP). Frames it sends are captured. Time is a
    fake clock the harness advances.

    Every input starts from the same state: the stack's global variables live in
    their own linker sections (the Makefile renames .data/.bss of its objects),
    are snapshotted once after boot and restored after each input, together with
    heap blocks allocated during boot. Heap blocks allocated by an input are
    freed when it ends, after the leak check.

    Invariants checked after every input (abort = finding):
    - AddressSanitizer / UndefinedBehaviorSanitizer: memory errors, UB
    - after 20 simulated minutes without traffic, the stack holds no heap block
      older than 10 minutes (connections, held packets and responses must all
      have been released; periodic activity such as DHCP renewals may hold fresh ones)
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../kernel/net/netdev.h"

#define FUZZ_LAN 0
#define FUZZ_WAN 1

#define FUZZ_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define FUZZ_ROUTER_LAN     FUZZ_IP(10, 0, 9, 2)
#define FUZZ_LAN_HOST       FUZZ_IP(10, 0, 9, 1)
#define FUZZ_ROUTER_WAN     FUZZ_IP(10, 0, 8, 2)
#define FUZZ_WAN_GATEWAY    FUZZ_IP(10, 0, 8, 1)
#define FUZZ_PASSWORD       "fuzz-password"

extern const uint8_t fuzz_router_mac[2][6];
extern const uint8_t fuzz_host_mac[2][6];     /* LAN host 10.0.9.1, WAN gateway 10.0.8.1 */
extern struct netdev* fuzz_dev[2];

/* Clock (fuzz/stubs.c) */
extern uint32_t fuzz_now_ms;

/* Frames the router sent since the input started */
#define FUZZ_TX_MAX 256
struct fuzz_frame
{
    int port;
    size_t len;
    uint8_t data[ETH_FRAME_MAX];
};
extern struct fuzz_frame fuzz_tx[FUZZ_TX_MAX];
extern size_t fuzz_tx_count;
extern size_t fuzz_boot_tx;             /* frames sent during boot: fuzz_tx[0..fuzz_boot_tx) until the first input sends */

/* Hash of every frame the router sent during the last finished input: the same
   input must always give the same hash (the replay driver checks) */
extern uint64_t fuzz_last_digest;

/* Boot the stack once with this command line (with nothing but the fuzz password
   and addresses decided by the target), then snapshot it */
void fuzz_boot(const char* cmdline);
/* After an input: idle long enough for every timer to fire, check for leaked heap
   memory, then restore the boot state */
void fuzz_finish(void);

/* Hand a frame to a port, as the NIC would */
void fuzz_receive(int port, const uint8_t* frame, size_t len);
/* Advance the clock in 100 ms network ticks */
void fuzz_run_ms(uint32_t ms);

/* Fix the IPv4 header checksum and the TCP/UDP/ICMP checksum of an Ethernet frame in place */
void fuzz_fix_checksums(uint8_t* frame, size_t len);

/* Build Ethernet + IPv4 + transport frames (checksums filled in); return the frame length */
size_t fuzz_udp_frame(uint8_t* out, int port, const uint8_t dst_mac[6], uint32_t src, uint16_t sport,
                      uint32_t dst, uint16_t dport, const uint8_t* payload, size_t len);
size_t fuzz_tcp_frame(uint8_t* out, int port, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                      uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                      const uint8_t* payload, size_t len);

#define FUZZ_TCP_FIN 0x01
#define FUZZ_TCP_SYN 0x02
#define FUZZ_TCP_RST 0x04
#define FUZZ_TCP_PSH 0x08
#define FUZZ_TCP_ACK 0x10

/* A TCP client on the LAN host talking to the router */
struct fuzz_tcp_client
{
    uint16_t sport, dport;
    uint32_t snd_nxt;                   /* our next sequence number */
    uint32_t rcv_nxt;                   /* end of the router's data we've seen */
    bool established;
    size_t scanned;                     /* fuzz_tx frames already looked at */
    uint8_t received[16384];            /* router's payload, in order of arrival (truncated) */
    size_t received_len;
    bool fin;
};
/* Three-way handshake; false if the router didn't answer the SYN */
bool fuzz_tcp_open(struct fuzz_tcp_client* c, uint16_t sport, uint16_t dport);
/* Send a segment with the given flags; seq/ack follow the connection */
void fuzz_tcp_send(struct fuzz_tcp_client* c, uint8_t flags, const uint8_t* data, size_t len);
/* Read the router's new segments for this connection; true if rcv_nxt advanced */
bool fuzz_tcp_scan(struct fuzz_tcp_client* c);
/* The same, then acknowledge what arrived */
void fuzz_tcp_pump(struct fuzz_tcp_client* c);

uint32_t fuzz_load32(const uint8_t* p);
uint16_t fuzz_load16(const uint8_t* p);

#endif /* __FUZZ_COMMON_H__ */
