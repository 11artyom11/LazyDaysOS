#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/net/net.h"
#include "../kernel/net/arp.h"
#include "../kernel/cmdline.h"
#include "../kernel/random.h"

const uint8_t fuzz_router_mac[2][6] = { {0x52, 0x54, 0x00, 0x12, 0x34, 0x56}, {0x52, 0x54, 0x00, 0x12, 0x34, 0x57} };
const uint8_t fuzz_host_mac[2][6] = { {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}, {0x02, 0x00, 0x00, 0x00, 0x00, 0x02} };
struct netdev* fuzz_dev[2];
struct fuzz_frame fuzz_tx[FUZZ_TX_MAX];
size_t fuzz_tx_count;
size_t fuzz_boot_tx;
bool fuzz_no_restore = false;           /* replay --fork: each input runs in its own process */

/* The stack's globals (fuzz/Makefile renames its objects' .data and .bss) */
extern char __start_fuzz_state_data[] __attribute__((weak));
extern char __stop_fuzz_state_data[] __attribute__((weak));
extern char __start_fuzz_state_bss[] __attribute__((weak));
extern char __stop_fuzz_state_bss[] __attribute__((weak));

void fuzz_heap_snapshot(void);
size_t fuzz_heap_outstanding(uint32_t min_age_ms);
void fuzz_heap_restore(void);

/* Copies whole sections, including the red zones AddressSanitizer keeps between
   globals: not instrumented, and volatile so it can't become an (intercepted) memcpy */
__attribute__((no_sanitize_address))
static void fuzz$$copy(void* dst, const void* src, size_t len)
{
    volatile uint64_t* d = dst;
    const volatile uint64_t* s = src;
    size_t words = len / 8;
    for (size_t i = 0; i < words; i++) {
        d[i] = s[i];
    }
    volatile uint8_t* db = (volatile uint8_t*)dst + words * 8;
    const volatile uint8_t* sb = (const volatile uint8_t*)src + words * 8;
    for (size_t i = 0; i < len % 8; i++) {
        db[i] = sb[i];
    }
}

static char* fuzz$data_saved;
static char* fuzz$bss_saved;
static uint32_t fuzz$boot_now;

uint16_t fuzz_load16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t fuzz_load32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void fuzz$$store16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void fuzz$$store32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ---- the fake NIC ---- */

static bool fuzz$$transmit(struct netdev* dev, const void* frame, size_t len)
{
    struct fuzz_frame* f = &fuzz_tx[fuzz_tx_count % FUZZ_TX_MAX];
    f->port = dev == fuzz_dev[FUZZ_WAN] ? FUZZ_WAN : FUZZ_LAN;
    f->len = len;
    memcpy(f->data, frame, len);
    fuzz_tx_count++;
    return true;
}

static size_t fuzz$$poll(struct netdev* dev, size_t budget)
{
    (void)dev;
    (void)budget;
    return 0;                           /* frames are handed over directly by fuzz_receive */
}

static const struct netdev_ops fuzz$ops = { .transmit = fuzz$$transmit, .poll = fuzz$$poll };

void fuzz_receive(int port, const uint8_t* frame, size_t len)
{
    if (len > ETH_FRAME_MAX) {
        len = ETH_FRAME_MAX;            /* what a NIC would deliver at most */
    }
    /* An exact-size copy, so reading past the end of the frame is caught */
    uint8_t* copy = malloc(len ? len : 1);
    memcpy(copy, frame, len);
    netdev_receive(fuzz_dev[port], copy, len);
    free(copy);
}

void fuzz_run_ms(uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms < 100 ? ms : 100;
        fuzz_now_ms += step;
        ms -= step;
        net_run_once();
    }
}

/* ---- checksums and frame building ---- */

static uint32_t fuzz$$sum(uint32_t sum, const uint8_t* p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    }
    if (n & 1) {
        sum += (uint32_t)p[n - 1] << 8;
    }
    return sum;
}

static uint16_t fuzz$$fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t fuzz$$transport_sum(const uint8_t* ip, uint8_t proto, const uint8_t* seg, size_t len)
{
    uint8_t pseudo[12];
    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0;
    pseudo[9] = proto;
    fuzz$$store16(pseudo + 10, (uint16_t)len);
    return fuzz$$fold(fuzz$$sum(fuzz$$sum(0, pseudo, sizeof(pseudo)), seg, len));
}

void fuzz_fix_checksums(uint8_t* f, size_t len)
{
    if (len < 14 + 20 || fuzz_load16(f + 12) != 0x0800) {
        return;
    }
    uint8_t* ip = f + 14;
    size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len) {
        return;
    }
    ip[10] = ip[11] = 0;
    fuzz$$store16(ip + 10, fuzz$$fold(fuzz$$sum(0, ip, ihl)));
    if (fuzz_load16(ip + 6) & 0x3FFF) {
        return;                         /* a fragment: the rest isn't a whole segment */
    }
    size_t total = fuzz_load16(ip + 2), avail = len - 14;
    size_t end = (total >= ihl && total <= avail) ? total : avail;
    uint8_t* seg = ip + ihl;
    size_t seg_len = end - ihl;
    if (ip[9] == 6 && seg_len >= 20) {
        seg[16] = seg[17] = 0;
        fuzz$$store16(seg + 16, fuzz$$transport_sum(ip, 6, seg, seg_len));
    } else if (ip[9] == 17 && seg_len >= 8) {
        seg[6] = seg[7] = 0;
        uint16_t c = fuzz$$transport_sum(ip, 17, seg, seg_len);
        fuzz$$store16(seg + 6, c ? c : 0xFFFF);
    } else if (ip[9] == 1 && seg_len >= 4) {
        seg[2] = seg[3] = 0;
        fuzz$$store16(seg + 2, fuzz$$fold(fuzz$$sum(0, seg, seg_len)));
    }
}

static size_t fuzz$$ipv4_frame(uint8_t* out, int port, const uint8_t dst_mac[6], uint32_t src, uint32_t dst,
                               uint8_t proto, size_t payload_len)
{
    memcpy(out, dst_mac, 6);
    memcpy(out + 6, fuzz_host_mac[port], 6);
    fuzz$$store16(out + 12, 0x0800);
    uint8_t* ip = out + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    fuzz$$store16(ip + 2, (uint16_t)(20 + payload_len));
    ip[8] = 64;
    ip[9] = proto;
    fuzz$$store32(ip + 12, src);
    fuzz$$store32(ip + 16, dst);
    return 14 + 20 + payload_len;
}

size_t fuzz_udp_frame(uint8_t* out, int port, const uint8_t dst_mac[6], uint32_t src, uint16_t sport,
                      uint32_t dst, uint16_t dport, const uint8_t* payload, size_t len)
{
    if (len > ETH_FRAME_MAX - 14 - 20 - 8) {
        len = ETH_FRAME_MAX - 14 - 20 - 8;
    }
    uint8_t* udp = out + 14 + 20;
    fuzz$$store16(udp, sport);
    fuzz$$store16(udp + 2, dport);
    fuzz$$store16(udp + 4, (uint16_t)(8 + len));
    if (len) {
        memcpy(udp + 8, payload, len);
    }
    size_t n = fuzz$$ipv4_frame(out, port, dst_mac, src, dst, 17, 8 + len);
    fuzz_fix_checksums(out, n);
    return n;
}

size_t fuzz_tcp_frame(uint8_t* out, int port, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                      uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                      const uint8_t* payload, size_t len)
{
    if (len > ETH_FRAME_MAX - 14 - 20 - 20) {
        len = ETH_FRAME_MAX - 14 - 20 - 20;
    }
    uint8_t* tcp = out + 14 + 20;
    memset(tcp, 0, 20);
    fuzz$$store16(tcp, sport);
    fuzz$$store16(tcp + 2, dport);
    fuzz$$store32(tcp + 4, seq);
    fuzz$$store32(tcp + 8, ack);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    fuzz$$store16(tcp + 14, window);
    if (len) {
        memcpy(tcp + 20, payload, len);
    }
    size_t n = fuzz$$ipv4_frame(out, port, fuzz_router_mac[port], src, dst, 6, 20 + len);
    fuzz_fix_checksums(out, n);
    return n;
}

/* ---- a TCP client on the LAN host ---- */

void fuzz_tcp_send(struct fuzz_tcp_client* c, uint8_t flags, const uint8_t* data, size_t len)
{
    uint8_t frame[ETH_FRAME_MAX];
    size_t n = fuzz_tcp_frame(frame, FUZZ_LAN, FUZZ_LAN_HOST, c->sport, FUZZ_ROUTER_LAN, c->dport,
                              c->snd_nxt, (flags & FUZZ_TCP_ACK) ? c->rcv_nxt : 0, flags, 65535, data, len);
    c->snd_nxt += (uint32_t)len + ((flags & FUZZ_TCP_SYN) ? 1 : 0) + ((flags & FUZZ_TCP_FIN) ? 1 : 0);
    fuzz_receive(FUZZ_LAN, frame, n);
}

/* Look at the router's new segments for this connection; returns true if any advanced rcv_nxt */
bool fuzz_tcp_scan(struct fuzz_tcp_client* c)
{
    bool advanced = false;
    if (fuzz_tx_count - c->scanned > FUZZ_TX_MAX) {
        c->scanned = fuzz_tx_count - FUZZ_TX_MAX;
    }
    for (; c->scanned < fuzz_tx_count; c->scanned++) {
        const struct fuzz_frame* f = &fuzz_tx[c->scanned % FUZZ_TX_MAX];
        const uint8_t* ip = f->data + 14;
        if (f->port != FUZZ_LAN || f->len < 14 + 40 || fuzz_load16(f->data + 12) != 0x0800 || ip[9] != 6) {
            continue;
        }
        size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
        const uint8_t* tcp = ip + ihl;
        if (fuzz_load32(ip + 12) != FUZZ_ROUTER_LAN || fuzz_load16(tcp) != c->dport || fuzz_load16(tcp + 2) != c->sport) {
            continue;
        }
        size_t hdr = (size_t)(tcp[12] >> 4) * 4;
        size_t total = fuzz_load16(ip + 2);
        size_t data_len = total > ihl + hdr ? total - ihl - hdr : 0;
        uint32_t seq = fuzz_load32(tcp + 4);
        uint8_t flags = tcp[13];
        if (flags & FUZZ_TCP_SYN) {
            c->rcv_nxt = seq + 1;
            c->established = true;
            advanced = true;
            continue;
        }
        if (!c->established || seq != c->rcv_nxt) {
            continue;                   /* a retransmission or out of order: the ACK below repeats */
        }
        const uint8_t* data = tcp + hdr;
        for (size_t i = 0; i < data_len && c->received_len < sizeof(c->received); i++) {
            c->received[c->received_len++] = data[i];
        }
        c->rcv_nxt += (uint32_t)data_len;
        if (flags & FUZZ_TCP_FIN) {
            c->rcv_nxt++;
            c->fin = true;
        }
        advanced |= data_len > 0 || (flags & FUZZ_TCP_FIN);
    }
    return advanced;
}

void fuzz_tcp_pump(struct fuzz_tcp_client* c)
{
    if (fuzz_tcp_scan(c) && c->established) {
        fuzz_tcp_send(c, FUZZ_TCP_ACK, NULL, 0);
    }
}

bool fuzz_tcp_open(struct fuzz_tcp_client* c, uint16_t sport, uint16_t dport)
{
    memset(c, 0, sizeof(*c));
    c->sport = sport;
    c->dport = dport;
    c->snd_nxt = 0x10000000u + sport;
    c->scanned = fuzz_tx_count;
    fuzz_tcp_send(c, FUZZ_TCP_SYN, NULL, 0);
    fuzz_tcp_scan(c);
    if (!c->established) {
        return false;
    }
    fuzz_tcp_send(c, FUZZ_TCP_ACK, NULL, 0);
    return true;
}

/* ---- boot, snapshot, restore ---- */

static void fuzz$$announce(int port, uint32_t host, uint32_t router)
{
    /* An ARP request from the host for the router: the router learns the host's MAC */
    uint8_t f[42];
    memset(f, 0xFF, 6);
    memcpy(f + 6, fuzz_host_mac[port], 6);
    fuzz$$store16(f + 12, 0x0806);
    const uint8_t arp[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };
    memcpy(f + 14, arp, 8);
    memcpy(f + 22, fuzz_host_mac[port], 6);
    fuzz$$store32(f + 28, host);
    memset(f + 32, 0, 6);
    fuzz$$store32(f + 38, router);
    fuzz_receive(port, f, sizeof(f));
}

void fuzz_boot(const char* cmdline)
{
    static bool booted = false;
    if (booted) {
        return;
    }
    booted = true;
    fuzz_now_ms = 1000;
    cmdline_init(cmdline);
    for (int i = 0; i < 2; i++) {
        fuzz_dev[i] = netdev_register(&fuzz$ops, NULL, fuzz_router_mac[i]);
        fuzz_dev[i]->link_up = true;
    }
    random_initialize();
    net_initialize();
    fuzz_run_ms(500);
    fuzz$$announce(FUZZ_LAN, FUZZ_LAN_HOST, FUZZ_ROUTER_LAN);
    fuzz$$announce(FUZZ_WAN, FUZZ_WAN_GATEWAY, FUZZ_ROUTER_WAN);
    fuzz_run_ms(200);

    size_t data_len = (size_t)(__stop_fuzz_state_data - __start_fuzz_state_data);
    size_t bss_len = (size_t)(__stop_fuzz_state_bss - __start_fuzz_state_bss);
    if (!__start_fuzz_state_bss || bss_len == 0) {
        fprintf(stderr, "FUZZ: the stack's state sections are missing (objects not renamed?)\n");
        abort();
    }
    fuzz$data_saved = malloc(data_len + 1);
    fuzz$bss_saved = malloc(bss_len);
    fuzz$$copy(fuzz$data_saved, __start_fuzz_state_data, data_len);
    fuzz$$copy(fuzz$bss_saved, __start_fuzz_state_bss, bss_len);
    fuzz_heap_snapshot();
    fuzz$boot_now = fuzz_now_ms;
    fuzz_boot_tx = fuzz_tx_count < FUZZ_TX_MAX ? fuzz_tx_count : FUZZ_TX_MAX;
    fuzz_tx_count = 0;
}

void fuzz_finish(void)
{
    /* Let retransmissions, timeouts and expiries run: first at the normal 100 ms
       tick, then in big steps until 20 minutes have passed */
    fuzz_run_ms(5000);
    for (int i = 0; i < 40; i++) {
        fuzz_now_ms += 30000;
        net_run_once();
    }
    /* Periodic activity (DHCP renewals, their ARP lookups) may hold fresh blocks at
       this moment; a block older than 10 minutes was never going to be freed */
    if (fuzz_heap_outstanding(10 * 60 * 1000) > 0) {
        if (getenv("FUZZ_LOG")) {
            arp_print_table();          /* where held packets live */
        }
        fprintf(stderr, "FUZZ: the stack still holds heap memory from more than 10 idle minutes ago (leak)\n");
        abort();
    }
    if (fuzz_no_restore) {
        return;
    }
    fuzz$$copy(__start_fuzz_state_data, fuzz$data_saved, (size_t)(__stop_fuzz_state_data - __start_fuzz_state_data));
    fuzz$$copy(__start_fuzz_state_bss, fuzz$bss_saved, (size_t)(__stop_fuzz_state_bss - __start_fuzz_state_bss));
    fuzz_heap_restore();
    fuzz_now_ms = fuzz$boot_now;
    fuzz_tx_count = 0;
}
