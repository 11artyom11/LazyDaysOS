#include "tcp.h"
#include "ipv4.h"
#include "net.h"
#include <kio.h>
#include <pit.h>
#include "../heap.h"
#include "../random.h"

#define TCP_FIN             0x01
#define TCP_SYN             0x02
#define TCP_RST             0x04
#define TCP_PSH             0x08
#define TCP_ACK             0x10

#define TCP$HEADER_LEN      20
#define TCP$OPT_END         0
#define TCP$OPT_NOP         1
#define TCP$OPT_MSS         2

/* Sequence number comparisons modulo 2^32 */
#define SEQ_LT(a, b)        ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)       ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)        ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)       ((int32_t)((a) - (b)) >= 0)

struct tcp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_offset;                /* header length in 32-bit words, upper 4 bits */
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

enum tcp$state
{
    TCP$CLOSED = 0, TCP$SYN_RCVD, TCP$ESTABLISHED, TCP$CLOSE_WAIT, TCP$LAST_ACK,
    TCP$FIN_WAIT_1, TCP$FIN_WAIT_2, TCP$CLOSING, TCP$TIME_WAIT
};

struct tcp$ooo
{
    uint8_t* data;                      /* NULL = slot unused */
    uint32_t seq;
    uint16_t len;
};

struct tcp_conn
{
    enum tcp$state state;
    const struct tcp_callbacks* cb;
    void* user;
    bool accepted_notified;
    bool closed_notified;

    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    /* Send side: the ring buffer holds [snd_una, snd_una + buf_used) */
    uint32_t iss;
    uint32_t snd_una;                   /* oldest unacknowledged */
    uint32_t snd_nxt;                   /* next to send (rewound on timeout) */
    uint32_t snd_max;                   /* highest ever sent */
    uint32_t snd_wnd;                   /* peer's window */
    uint16_t peer_mss;
    uint8_t* buf;
    size_t buf_head;
    size_t buf_used;
    bool close_requested;
    bool fin_sent;
    uint32_t fin_seq;
    bool small_window_advertised;

    /* Receive side */
    uint32_t rcv_nxt;
    struct tcp$ooo ooo[TCP_OOO_SEGMENTS];

    /* Timers (RFC 6298) */
    bool rtt_valid;
    uint32_t srtt_ms, rttvar_ms, rto_ms;
    bool rtt_timing;
    uint32_t rtt_seq, rtt_start_ms;
    bool rtx_armed;
    uint32_t rtx_deadline_ms;
    uint8_t retries;
    uint8_t dupacks;
    bool in_recovery;                   /* after a fast retransmit, until snd_max is acknowledged */
    uint32_t recover;
    uint32_t timewait_deadline_ms;

    uint32_t segments_sent;             /* tells whether an ACK was piggybacked */
    uint32_t created_ms;                /* SYN arrival: the oldest half-open connection is evicted first */
};

struct tcp$listener
{
    uint16_t port;                      /* 0 = free */
    const struct tcp_callbacks* cb;
};

static struct tcp_conn tcp$conns[TCP_MAX_CONNS];
static struct tcp$listener tcp$listeners[TCP_MAX_LISTENERS];

struct tcp_stats tcp_stats;

static bool tcp$$reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static size_t tcp$$buf_free(const struct tcp_conn* c)
{
    return TCP_SNDBUF - c->buf_used;
}

static uint16_t tcp$$window(const struct tcp_conn* c)
{
    size_t free = tcp$$buf_free(c);
    return free > TCP_RCV_WINDOW ? TCP_RCV_WINDOW : (uint16_t)free;
}

/* ---- sending segments ----------------------------------------------------- */

/* Send one segment; payload is len bytes of the send buffer starting offset bytes after snd_una */
static void tcp$$segment(struct tcp_conn* c, uint8_t flags, uint32_t seq, size_t offset, size_t len, bool mss_option)
{
    uint8_t seg[TCP$HEADER_LEN + 4 + TCP_MSS];
    struct tcp_header* h = (struct tcp_header*)seg;
    size_t header_len = TCP$HEADER_LEN + (mss_option ? 4 : 0);
    uint16_t window = tcp$$window(c);

    h->src_port = net_htons(c->local_port);
    h->dst_port = net_htons(c->remote_port);
    h->seq = net_htonl(seq);
    h->ack = net_htonl((flags & TCP_ACK) ? c->rcv_nxt : 0);
    h->data_offset = (uint8_t)((header_len / 4) << 4);
    h->flags = flags;
    h->window = net_htons(window);
    h->checksum = 0;
    h->urgent = 0;
    if (mss_option) {
        seg[20] = TCP$OPT_MSS;
        seg[21] = 4;
        seg[22] = TCP_MSS >> 8;
        seg[23] = TCP_MSS & 0xFF;
    }
    for (size_t i = 0; i < len; i++) {
        seg[header_len + i] = c->buf[(c->buf_head + offset + i) % TCP_SNDBUF];
    }
    h->checksum = net_transport_checksum(c->local_ip, c->remote_ip, IP_PROTO_TCP, seg, header_len + len);

    ipv4_send(c->local_ip, c->remote_ip, IP_PROTO_TCP, seg, header_len + len);
    c->small_window_advertised = window < TCP_MSS;
    c->segments_sent++;
    tcp_stats.tx_segments++;
}

static void tcp$$send_ack(struct tcp_conn* c)
{
    tcp$$segment(c, TCP_ACK, c->snd_nxt, 0, 0, false);
}

/* RST for a segment that belongs to no connection (RFC 9293 3.10.7.1) */
static void tcp$$reset_reply(uint32_t local_ip, uint32_t remote_ip, const struct tcp_header* in, size_t data_len)
{
    if (in->flags & TCP_RST) {
        return;
    }
    uint8_t seg[TCP$HEADER_LEN];
    struct tcp_header* h = (struct tcp_header*)seg;
    h->src_port = in->dst_port;
    h->dst_port = in->src_port;
    if (in->flags & TCP_ACK) {
        h->seq = in->ack;
        h->ack = 0;
        h->flags = TCP_RST;
    } else {
        uint32_t ack = net_ntohl(in->seq) + data_len + ((in->flags & TCP_SYN) ? 1 : 0) + ((in->flags & TCP_FIN) ? 1 : 0);
        h->seq = 0;
        h->ack = net_htonl(ack);
        h->flags = TCP_RST | TCP_ACK;
    }
    h->data_offset = (TCP$HEADER_LEN / 4) << 4;
    h->window = 0;
    h->checksum = 0;
    h->urgent = 0;
    h->checksum = net_transport_checksum(local_ip, remote_ip, IP_PROTO_TCP, seg, sizeof(seg));
    ipv4_send(local_ip, remote_ip, IP_PROTO_TCP, seg, sizeof(seg));
    tcp_stats.resets_sent++;
}

static void tcp$$arm(struct tcp_conn* c, uint32_t now)
{
    c->rtx_armed = true;
    c->rtx_deadline_ms = now + c->rto_ms;
}

/* Send whatever the window allows, then FIN if a close is pending and all data is out */
static void tcp$$output(struct tcp_conn* c, bool probe)
{
    if (c->state != TCP$ESTABLISHED && c->state != TCP$CLOSE_WAIT && c->state != TCP$FIN_WAIT_1 &&
        c->state != TCP$LAST_ACK && c->state != TCP$CLOSING) {
        return;
    }
    uint32_t now = pit_uptime_ms();

    for (;;) {
        uint32_t offset = c->snd_nxt - c->snd_una;
        if (offset < c->buf_used) {
            uint32_t window_left = c->snd_wnd > offset ? c->snd_wnd - offset : 0;
            if (window_left == 0 && probe && offset == 0) {
                window_left = 1;        /* zero-window probe */
            }
            size_t n = c->buf_used - offset;
            if (n > c->peer_mss) {
                n = c->peer_mss;
            }
            if (n > window_left) {
                n = window_left;
            }
            if (n == 0) {
                /* Data waits for a zero window: the persist timer (RFC 9293 3.8.6.1) must
                   run, or a lost window update, or a peer that never reads, leaves the
                   connection stuck forever. It probes with backoff and gives up after
                   TCP_MAX_RETRIES unanswered probes (found by fuzzing). */
                if (offset == 0 && !c->rtx_armed) {
                    tcp$$arm(c, now);
                }
                break;
            }
            uint8_t flags = TCP_ACK | (offset + n == c->buf_used ? TCP_PSH : 0);
            tcp$$segment(c, flags, c->snd_nxt, offset, n, false);
            /* Karn: only time segments that are not retransmissions */
            if (!c->rtt_timing && SEQ_GEQ(c->snd_nxt, c->snd_max)) {
                c->rtt_timing = true;
                c->rtt_seq = c->snd_nxt;
                c->rtt_start_ms = now;
            }
            c->snd_nxt += n;
            if (SEQ_GT(c->snd_nxt, c->snd_max)) {
                c->snd_max = c->snd_nxt;
            }
            if (!c->rtx_armed) {
                tcp$$arm(c, now);
            }
            probe = false;
            continue;
        }
        if (c->close_requested && !c->fin_sent && offset == c->buf_used) {
            tcp$$segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, 0, 0, false);
            c->fin_seq = c->snd_nxt;
            c->fin_sent = true;
            c->snd_nxt++;
            if (SEQ_GT(c->snd_nxt, c->snd_max)) {
                c->snd_max = c->snd_nxt;
            }
            if (c->state == TCP$ESTABLISHED) {
                c->state = TCP$FIN_WAIT_1;
            } else if (c->state == TCP$CLOSE_WAIT) {
                c->state = TCP$LAST_ACK;
            }
            if (!c->rtx_armed) {
                tcp$$arm(c, now);
            }
        }
        break;
    }
}

/* ---- connection lifecycle ------------------------------------------------- */

static void tcp$$notify_closed(struct tcp_conn* c, bool reset)
{
    if (c->accepted_notified && !c->closed_notified) {
        c->closed_notified = true;
        if (c->cb && c->cb->closed) {
            c->cb->closed(c, reset);
        }
    }
}

static void tcp$$release(struct tcp_conn* c, bool reset)
{
    tcp$$notify_closed(c, reset);
    for (int i = 0; i < TCP_OOO_SEGMENTS; i++) {
        kfree(c->ooo[i].data);
        c->ooo[i].data = NULL;
    }
    kfree(c->buf);
    c->buf = NULL;
    c->state = TCP$CLOSED;
}

static void tcp$$enter_time_wait(struct tcp_conn* c, uint32_t now)
{
    c->state = TCP$TIME_WAIT;
    c->rtx_armed = false;
    c->timewait_deadline_ms = now + TCP_TIMEWAIT_MS;
    /* The application is done with the connection; only the tuple lingers */
    tcp$$notify_closed(c, false);
}

/* RTO from the current estimate (RFC 6298 2.3), or the initial value without one */
static uint32_t tcp$$rto_estimate(const struct tcp_conn* c)
{
    if (!c->rtt_valid) {
        return TCP_RTO_INITIAL_MS;
    }
    uint32_t variance = 4 * c->rttvar_ms;
    uint32_t granularity = 1000 / pit_get_hz();
    uint32_t rto = c->srtt_ms + (variance > granularity ? variance : granularity);
    return rto < TCP_RTO_MIN_MS ? TCP_RTO_MIN_MS : rto > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : rto;
}

static void tcp$$rtt_sample(struct tcp_conn* c, uint32_t sample)
{
    if (!c->rtt_valid) {
        c->srtt_ms = sample;
        c->rttvar_ms = sample / 2;
        c->rtt_valid = true;
    } else {
        uint32_t diff = c->srtt_ms > sample ? c->srtt_ms - sample : sample - c->srtt_ms;
        c->rttvar_ms = (3 * c->rttvar_ms + diff) / 4;
        c->srtt_ms = (7 * c->srtt_ms + sample) / 8;
    }
    c->rto_ms = tcp$$rto_estimate(c);
}

/* Resend one segment from snd_una (fast retransmit, partial ACK) */
static void tcp$$retransmit_first(struct tcp_conn* c)
{
    size_t n = c->buf_used < c->peer_mss ? c->buf_used : c->peer_mss;
    if (n > 0) {
        tcp$$segment(c, TCP_ACK, c->snd_una, 0, n, false);
        c->rtt_timing = false;
        tcp_stats.fast_retransmits++;
    }
}

static struct tcp_conn* tcp$$find(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &tcp$conns[i];
        if (c->state != TCP$CLOSED && c->local_ip == local_ip && c->local_port == local_port &&
            c->remote_ip == remote_ip && c->remote_port == remote_port) {
            return c;
        }
    }
    return NULL;
}

/* Closed by the application but stuck behind the peer's zero window: only the peer
   can end it, and a peer that never reads won't */
static bool tcp$$stalled_orphan(const struct tcp_conn* c)
{
    return c->close_requested && c->snd_wnd == 0 && c->buf_used > 0 &&
           (c->state == TCP$ESTABLISHED || c->state == TCP$CLOSE_WAIT);
}

/* A free slot, else the TIME_WAIT or FIN_WAIT_2 connection closest to expiry (both
   are done for the application), else a stalled orphan (reset), else (only when
   allow_half_open) the oldest half-open connection. Evicting a half-open
   connection loses nothing: its sequence numbers are SYN cookies, so its peer's
   ACK recreates it. */
static struct tcp_conn* tcp$$allocate(bool allow_half_open)
{
    struct tcp_conn* waiting = NULL;
    struct tcp_conn* stalled = NULL;
    struct tcp_conn* half_open = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &tcp$conns[i];
        if (c->state == TCP$CLOSED) {
            return c;
        }
        if ((c->state == TCP$TIME_WAIT || c->state == TCP$FIN_WAIT_2) &&
            (!waiting || SEQ_LT(c->timewait_deadline_ms, waiting->timewait_deadline_ms))) {
            waiting = c;
        }
        if (c->state == TCP$SYN_RCVD && (!half_open || SEQ_LT(c->created_ms, half_open->created_ms))) {
            half_open = c;
        }
        if (tcp$$stalled_orphan(c) && (!stalled || SEQ_LT(c->created_ms, stalled->created_ms))) {
            stalled = c;
        }
    }
    if (waiting) {
        if (waiting->state == TCP$FIN_WAIT_2) {
            tcp_stats.fin_wait_dropped++;
        }
        tcp$$release(waiting, false);
        return waiting;
    }
    if (stalled) {
        tcp_stats.stalled_dropped++;
        tcp$$segment(stalled, TCP_RST | TCP_ACK, stalled->snd_nxt, 0, 0, false);
        tcp$$release(stalled, true);
        return stalled;
    }
    if (half_open && allow_half_open) {
        tcp_stats.syn_evicted++;
        tcp$$release(half_open, false);
        return half_open;
    }
    return NULL;
}

/* ---- initial sequence numbers: RFC 6528 + SYN cookies ---------------------

    Every ISN is a SYN cookie (the construction Linux uses):

        ISN = H1(tuple) + client ISN + (t << 24) + ((H2(tuple, t) + mss index) mod 2^24)

    with t the uptime in 64 s units and H1/H2 keyed ChaCha20 hashes of the
    addresses and ports. Over time the ISN grows by 2^24 per 64 s, about the 4 us
    clock of RFC 6528, and without the key nobody can predict it from other
    connections' ISNs. Because the ISN proves on its own that we sent a SYN-ACK
    to that tuple recently (and which MSS the peer announced), the ACK that
    completes the handshake can create the connection even if no state was kept:
    when the table is full, SYNs are answered without allocating anything. */

#define TCP$COOKIE_PERIOD_MS    64000
#define TCP$COOKIE_MAX_AGE      1       /* periods: a cookie is valid for 64-128 s */

static const uint16_t tcp$cookie_mss[4] = { TCP_DEFAULT_MSS, 1220, 1440, TCP_MSS };

static uint32_t tcp$$hash(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port, uint32_t round)
{
    static uint32_t secret[8];
    static bool keyed = false;
    if (!keyed) {
        random_bytes(secret, sizeof(secret));
        keyed = true;
    }
    const uint32_t nonce[3] = { local_ip, remote_ip, ((uint32_t)local_port << 16) | remote_port };
    uint32_t block[16];
    chacha20_block(secret, round, nonce, block);
    return block[0];
}

static uint32_t tcp$$cookie(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                            uint32_t client_isn, uint16_t peer_mss)
{
    uint32_t t = pit_uptime_ms() / TCP$COOKIE_PERIOD_MS;
    uint32_t mss_index = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (tcp$cookie_mss[i] <= peer_mss) {
            mss_index = i;
        }
    }
    return tcp$$hash(local_ip, local_port, remote_ip, remote_port, 0) + client_isn + (t << 24) +
           ((tcp$$hash(local_ip, local_port, remote_ip, remote_port, t + 1) + mss_index) & 0xFFFFFF);
}

/* The peer's MSS if cookie is one we issued to this tuple for this client ISN recently, else 0 */
static uint16_t tcp$$cookie_check(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                                  uint32_t client_isn, uint32_t cookie)
{
    uint32_t value = cookie - tcp$$hash(local_ip, local_port, remote_ip, remote_port, 0) - client_isn;
    uint32_t now = pit_uptime_ms() / TCP$COOKIE_PERIOD_MS;
    uint32_t age = (now - (value >> 24)) & 0xFF;
    if (age > TCP$COOKIE_MAX_AGE || age > now) {
        return 0;
    }
    uint32_t t = now - age;
    uint32_t mss_index = (value - tcp$$hash(local_ip, local_port, remote_ip, remote_port, t + 1)) & 0xFFFFFF;
    return mss_index < 4 ? tcp$cookie_mss[mss_index] : 0;
}

static uint16_t tcp$$parse_mss(const uint8_t* options, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t kind = options[i];
        if (kind == TCP$OPT_END) {
            break;
        }
        if (kind == TCP$OPT_NOP) {
            i++;
            continue;
        }
        if (i + 1 >= len || options[i + 1] < 2 || i + options[i + 1] > len) {
            break;
        }
        if (kind == TCP$OPT_MSS && options[i + 1] == 4) {
            uint16_t mss = (uint16_t)((options[i + 2] << 8) | options[i + 3]);
            return mss < TCP_MSS ? mss : TCP_MSS;
        }
        i += options[i + 1];
    }
    return TCP_DEFAULT_MSS;
}

static const struct tcp$listener* tcp$$listener(uint16_t port)
{
    /* Free slots have port 0: a SYN to port 0 must not match one (it was accepted with
       no application and no admit() check, from the WAN too, and held a slot and
       64 KiB forever; found by fuzzing) */
    for (int i = 0; port != 0 && i < TCP_MAX_LISTENERS; i++) {
        if (tcp$listeners[i].port == port) {
            return &tcp$listeners[i];
        }
    }
    return NULL;
}

/* Set up a slot for a handshake in progress: SYN-ACK sent with ISN iss */
static void tcp$$init_conn(struct tcp_conn* c, const struct tcp$listener* listener, uint32_t local_ip, uint16_t local_port,
                           uint32_t remote_ip, uint16_t remote_port, uint32_t iss, uint32_t rcv_nxt,
                           uint32_t window, uint16_t peer_mss, uint32_t now)
{
    uint8_t* zero = (uint8_t*)c;
    for (size_t i = 0; i < sizeof(*c); i++) {
        zero[i] = 0;
    }
    c->state = TCP$SYN_RCVD;
    c->cb = listener ? listener->cb : NULL;
    c->created_ms = now;
    c->local_ip = local_ip;
    c->local_port = local_port;
    c->remote_ip = remote_ip;
    c->remote_port = remote_port;
    c->rcv_nxt = rcv_nxt;
    c->iss = iss;
    c->snd_una = iss;
    c->snd_nxt = iss + 1;
    c->snd_max = c->snd_nxt;
    c->snd_wnd = window;
    c->peer_mss = peer_mss;
    c->rto_ms = TCP_RTO_INITIAL_MS;
}

static void tcp$$accept_syn(struct netdev* dev, uint32_t local_ip, uint32_t remote_ip, const struct tcp_header* h,
                            const uint8_t* options, size_t options_len)
{
    uint16_t port = net_ntohs(h->dst_port);
    uint16_t remote_port = net_ntohs(h->src_port);
    const struct tcp$listener* listener = tcp$$listener(port);
    if (!listener) {
        tcp$$reset_reply(local_ip, remote_ip, h, 0);
        return;
    }
    if (listener->cb && listener->cb->admit && !listener->cb->admit(dev, remote_ip)) {
        tcp_stats.filtered++;           /* silently, like a firewall: no SYN-ACK, no RST */
        return;
    }

    uint32_t now = pit_uptime_ms();
    uint32_t client_isn = net_ntohl(h->seq);
    uint16_t peer_mss = tcp$$parse_mss(options, options_len);
    uint32_t iss = tcp$$cookie(local_ip, port, remote_ip, remote_port, client_isn, peer_mss);

    struct tcp_conn* c = tcp$$allocate(false);
    if (c) {
        /* Room to spare: keep state, so a lost SYN-ACK is retransmitted */
        tcp$$init_conn(c, listener, local_ip, port, remote_ip, remote_port, iss, client_isn + 1,
                       net_ntohs(h->window), peer_mss, now);
        c->rtt_timing = true;
        c->rtt_seq = c->iss;
        c->rtt_start_ms = now;
        tcp$$segment(c, TCP_SYN | TCP_ACK, c->iss, 0, 0, true);
        tcp$$arm(c, now);
        return;
    }

    /* Table full: answer without keeping anything; the cookie brings the connection back */
    struct tcp_conn stateless;
    tcp$$init_conn(&stateless, listener, local_ip, port, remote_ip, remote_port, iss, client_isn + 1,
                   net_ntohs(h->window), peer_mss, now);
    tcp$$segment(&stateless, TCP_SYN | TCP_ACK, iss, 0, 0, true);
    tcp_stats.syn_cookies_sent++;
}

/*
    An ACK for a connection we have no state for: if it acknowledges one of our
    SYN cookies, recreate the half-open connection so the normal handshake path
    completes it (and handles any data in the segment). *silent is set when the
    segment must be dropped without a reset (refused by admit(), or no slot free:
    the peer retransmits).
*/
static struct tcp_conn* tcp$$accept_cookie(struct netdev* dev, uint32_t local_ip, uint32_t remote_ip,
                                           const struct tcp_header* h, bool* silent)
{
    uint16_t port = net_ntohs(h->dst_port);
    uint16_t remote_port = net_ntohs(h->src_port);
    uint32_t seq = net_ntohl(h->seq);
    uint32_t ack = net_ntohl(h->ack);
    *silent = false;

    const struct tcp$listener* listener = tcp$$listener(port);
    if (!listener) {
        return NULL;
    }
    if (listener->cb && listener->cb->admit && !listener->cb->admit(dev, remote_ip)) {
        tcp_stats.filtered++;
        *silent = true;
        return NULL;
    }
    uint16_t peer_mss = tcp$$cookie_check(local_ip, port, remote_ip, remote_port, seq - 1, ack - 1);
    if (peer_mss == 0) {
        return NULL;
    }
    struct tcp_conn* c = tcp$$allocate(true);
    if (!c) {
        tcp_stats.refused++;
        *silent = true;
        return NULL;
    }
    tcp$$init_conn(c, listener, local_ip, port, remote_ip, remote_port, ack - 1, seq,
                   net_ntohs(h->window), peer_mss, pit_uptime_ms());
    tcp_stats.syn_cookies_accepted++;
    return c;
}

/* ---- receiving ------------------------------------------------------------ */

static bool tcp$$accepts_data(const struct tcp_conn* c)
{
    return c->state == TCP$ESTABLISHED || c->state == TCP$FIN_WAIT_1 || c->state == TCP$FIN_WAIT_2;
}

/* Keep a segment that arrived after a gap, if it lies inside the advertised window */
static void tcp$$ooo_store(struct tcp_conn* c, uint32_t seq, const uint8_t* data, size_t len)
{
    struct tcp$ooo* slot = NULL;
    if (len == 0 || SEQ_GT(seq + len, c->rcv_nxt + tcp$$window(c))) {
        return;
    }
    for (int i = 0; i < TCP_OOO_SEGMENTS; i++) {
        if (c->ooo[i].data && c->ooo[i].seq == seq) {
            return;                     /* already have it */
        }
        if (!c->ooo[i].data && !slot) {
            slot = &c->ooo[i];
        }
    }
    if (!slot || (slot->data = kmalloc(len)) == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        slot->data[i] = data[i];
    }
    slot->seq = seq;
    slot->len = (uint16_t)len;
    tcp_stats.rx_ooo_queued++;
}

/* Deliver kept segments that now continue the in-order stream */
static void tcp$$ooo_drain(struct tcp_conn* c)
{
    bool progress = true;
    while (progress && tcp$$accepts_data(c)) {
        progress = false;
        for (int i = 0; i < TCP_OOO_SEGMENTS; i++) {
            struct tcp$ooo* s = &c->ooo[i];
            if (!s->data || SEQ_GT(s->seq, c->rcv_nxt)) {
                continue;
            }
            size_t skip = c->rcv_nxt - s->seq;
            if (skip < s->len) {
                size_t n = s->len - skip;
                if (n > tcp$$buf_free(c)) {
                    return;             /* no room for the reply yet; try again later */
                }
                c->rcv_nxt += n;
                if (c->cb && c->cb->received) {
                    c->cb->received(c, s->data + skip, n);
                }
            }
            kfree(s->data);
            s->data = NULL;
            progress = true;
        }
    }
}

void tcp_receive(struct netdev* dev, const uint8_t* packet, size_t len, size_t header_len)
{
    const struct ipv4_header* ip = (const struct ipv4_header*)packet;
    const struct tcp_header* h = (const struct tcp_header*)(packet + header_len);
    size_t seg_len = len - header_len;
    uint32_t remote_ip = net_ntohl(ip->src);
    uint32_t local_ip = net_ntohl(ip->dst);

    if (seg_len < TCP$HEADER_LEN || local_ip == NET_IP_BROADCAST || !ipv4_is_own_address(local_ip)) {
        tcp_stats.rx_invalid++;
        return;
    }
    /* No connection can come from these (RFC 1122 4.2.3.10, 3.2.1.3) */
    if (remote_ip == 0 || remote_ip >= NET_IP(224, 0, 0, 0) || (remote_ip >> 24) == 127 ||
        ipv4_is_own_address(remote_ip)) {
        tcp_stats.rx_invalid++;
        return;
    }
    size_t tcp_header_len = (h->data_offset >> 4) * 4;
    if (tcp_header_len < TCP$HEADER_LEN || tcp_header_len > seg_len ||
        net_transport_checksum(remote_ip, local_ip, IP_PROTO_TCP, h, seg_len) != 0) {
        tcp_stats.rx_invalid++;
        return;
    }
    tcp_stats.rx_segments++;

    uint8_t flags = h->flags;
    uint32_t seq = net_ntohl(h->seq);
    uint32_t ack = net_ntohl(h->ack);
    uint32_t window = net_ntohs(h->window);
    const uint8_t* data = (const uint8_t*)h + tcp_header_len;
    size_t data_len = seg_len - tcp_header_len;
    uint32_t now = pit_uptime_ms();

    struct tcp_conn* c = tcp$$find(local_ip, net_ntohs(h->dst_port), remote_ip, net_ntohs(h->src_port));

    /* A new SYN may reuse a tuple in TIME_WAIT if it starts beyond the old one (RFC 6191) */
    if (c && c->state == TCP$TIME_WAIT && (flags & TCP_SYN) && !(flags & TCP_ACK) && SEQ_GT(seq, c->rcv_nxt)) {
        tcp$$release(c, false);
        c = NULL;
    }
    if (!c) {
        if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
            tcp$$accept_syn(dev, local_ip, remote_ip, h, (const uint8_t*)h + TCP$HEADER_LEN,
                            tcp_header_len - TCP$HEADER_LEN);
            return;
        }
        bool silent = false;
        if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_ACK) {
            c = tcp$$accept_cookie(dev, local_ip, remote_ip, h, &silent);
        }
        if (!c) {
            if (!silent) {
                tcp$$reset_reply(local_ip, remote_ip, h, data_len);
            }
            return;
        }
    }

    if (flags & TCP_RST) {
        /* Accept only a reset that lands inside our receive window */
        uint32_t span = tcp$$window(c) ? tcp$$window(c) : 1;
        if (SEQ_GEQ(seq, c->rcv_nxt) && SEQ_LT(seq, c->rcv_nxt + span)) {
            tcp_stats.aborted++;
            tcp$$release(c, true);
        }
        return;
    }

    if (c->state == TCP$TIME_WAIT) {
        /* Our ACK of the peer's FIN got lost: acknowledge again and restart the wait */
        if (flags & TCP_FIN) {
            tcp$$send_ack(c);
            c->timewait_deadline_ms = now + TCP_TIMEWAIT_MS;
        }
        return;
    }

    if (flags & TCP_SYN) {
        if (c->state == TCP$SYN_RCVD && seq + 1 == c->rcv_nxt) {
            tcp$$segment(c, TCP_SYN | TCP_ACK, c->iss, 0, 0, true);      /* our SYN-ACK was lost */
        } else {
            tcp$$send_ack(c);           /* SYN on a synchronized connection: challenge ACK (RFC 5961) */
        }
        return;
    }
    if (!(flags & TCP_ACK)) {
        return;                         /* every segment after the SYN carries an ACK */
    }

    /* ---- acknowledgement ---- */
    bool freed = false;
    if (c->state == TCP$SYN_RCVD) {
        if (ack != c->snd_nxt) {
            tcp$$reset_reply(local_ip, remote_ip, h, data_len);
            return;
        }
        /* Only a completed handshake gets a send buffer */
        if ((c->buf = kmalloc(TCP_SNDBUF)) == NULL) {
            tcp_stats.refused++;
            tcp$$reset_reply(local_ip, remote_ip, h, data_len);
            tcp$$release(c, false);
            return;
        }
        c->snd_una = ack;
        c->snd_wnd = window;
        c->state = TCP$ESTABLISHED;
        c->rtx_armed = false;
        c->retries = 0;
        if (c->rtt_timing) {
            tcp$$rtt_sample(c, now - c->rtt_start_ms);
            c->rtt_timing = false;
        }
        tcp_stats.accepted++;
        c->accepted_notified = true;
        if (c->cb && c->cb->accepted) {
            c->cb->accepted(c);
        }
    } else {
        if (SEQ_GT(ack, c->snd_max)) {
            tcp$$send_ack(c);           /* acknowledges something never sent */
            return;
        }
        if (SEQ_GT(ack, c->snd_una)) {
            uint32_t acked = ack - c->snd_una;
            size_t data_acked = acked > c->buf_used ? c->buf_used : acked;
            c->buf_head = (c->buf_head + data_acked) % TCP_SNDBUF;
            c->buf_used -= data_acked;
            c->snd_una = ack;
            if (SEQ_GT(ack, c->snd_nxt)) {
                c->snd_nxt = ack;       /* we had rewound for a retransmission */
            }
            if (c->rtt_timing && SEQ_GT(ack, c->rtt_seq)) {
                tcp$$rtt_sample(c, now - c->rtt_start_ms);
                c->rtt_timing = false;
            }
            c->retries = 0;
            c->dupacks = 0;
            /* New data got through: drop the exponential backoff (as Linux does) */
            c->rto_ms = tcp$$rto_estimate(c);
            if (c->in_recovery) {
                if (SEQ_GEQ(ack, c->recover)) {
                    c->in_recovery = false;
                } else {
                    tcp$$retransmit_first(c);   /* partial ACK: the next hole (RFC 6582) */
                }
            }
            if (c->snd_una == c->snd_max) {
                c->rtx_armed = false;
            } else {
                tcp$$arm(c, now);
            }
            freed = data_acked > 0;

            if (c->fin_sent && SEQ_GT(ack, c->fin_seq)) {
                if (c->state == TCP$FIN_WAIT_1) {
                    /* Everything we sent is acknowledged: only the peer's FIN is missing */
                    c->state = TCP$FIN_WAIT_2;
                    c->timewait_deadline_ms = now + TCP_FIN_WAIT_2_MS;
                    kfree(c->buf);
                    c->buf = NULL;
                    c->buf_used = 0;
                } else if (c->state == TCP$CLOSING) {
                    tcp$$enter_time_wait(c, now);
                    return;
                } else if (c->state == TCP$LAST_ACK) {
                    tcp$$release(c, false);
                    return;
                }
            }
        } else if (ack == c->snd_una && data_len == 0 && !(flags & TCP_FIN) && window == c->snd_wnd &&
                   c->snd_una != c->snd_max && c->buf_used > 0) {
            /* Fast retransmit (RFC 5681): the third duplicate ACK means the
               segment at snd_una was lost while later ones arrived */
            if (++c->dupacks == 3 && !c->in_recovery) {
                c->in_recovery = true;
                c->recover = c->snd_max;
                tcp$$retransmit_first(c);
            }
        }
        if (SEQ_GEQ(ack, c->snd_una)) {
            c->snd_wnd = window;
        }
    }

    /* ---- data and FIN ---- */
    uint32_t sent_before = c->segments_sent;
    bool need_ack = false;
    bool fin = (flags & TCP_FIN) != 0;

    if (data_len > 0 || fin) {
        need_ack = true;
        if (!tcp$$accepts_data(c)) {
            fin = false;                /* peer already finished; just acknowledge */
        } else if (SEQ_GT(seq, c->rcv_nxt)) {
            /* A gap: drop it and send a pure duplicate ACK right away; one riding
               on data wouldn't count as a duplicate for the peer's fast retransmit */
            tcp_stats.rx_out_of_order++;
            if (!fin) {
                tcp$$ooo_store(c, seq, data, data_len);
            }
            tcp$$send_ack(c);
            need_ack = false;
            fin = false;
        } else {
            size_t skip = c->rcv_nxt - seq;     /* bytes we already have */
            size_t fresh = skip < data_len ? data_len - skip : 0;
            if (skip > data_len) {
                fin = false;            /* the FIN, too, was seen before */
            }
            size_t room = tcp$$buf_free(c);
            if (fresh > room) {
                fresh = room;           /* beyond the window we advertised */
                fin = false;
            }
            if (fresh > 0) {
                c->rcv_nxt += fresh;
                if (c->cb && c->cb->received) {
                    c->cb->received(c, data + skip, fresh);
                }
                if (!fin) {
                    tcp$$ooo_drain(c);
                }
            }
            if (fin && c->state != TCP$CLOSED && c->rcv_nxt == seq + data_len) {
                c->rcv_nxt++;
                if (c->state == TCP$ESTABLISHED) {
                    c->state = TCP$CLOSE_WAIT;
                } else if (c->state == TCP$FIN_WAIT_1) {
                    c->state = TCP$CLOSING;
                } else if (c->state == TCP$FIN_WAIT_2) {
                    tcp$$send_ack(c);
                    tcp$$enter_time_wait(c, now);
                    return;
                }
                if (c->cb && c->cb->received) {
                    c->cb->received(c, NULL, 0);
                }
            }
        }
    }
    if (c->state == TCP$CLOSED) {
        return;
    }

    if (freed && c->cb && c->cb->writable) {
        c->cb->writable(c);
    }
    if (freed) {
        tcp$$ooo_drain(c);              /* space for replies opened up */
    }
    tcp$$output(c, false);

    /* Acknowledge unless output already carried the ACK; also announce a window
       that reopened after we had advertised less than one segment */
    bool window_reopened = freed && c->small_window_advertised && tcp$$window(c) >= TCP_MSS;
    if ((need_ack || window_reopened) && c->segments_sent == sent_before) {
        tcp$$send_ack(c);
    }
}

/* ---- timers --------------------------------------------------------------- */

void tcp_tick(uint32_t now_ms)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &tcp$conns[i];
        if (c->state == TCP$CLOSED) {
            continue;
        }
        if (c->state == TCP$TIME_WAIT || c->state == TCP$FIN_WAIT_2) {
            if (tcp$$reached(now_ms, c->timewait_deadline_ms)) {
                if (c->state == TCP$FIN_WAIT_2) {
                    tcp_stats.fin_wait_dropped++;
                }
                tcp$$release(c, false);
            }
            continue;
        }
        if (!c->rtx_armed || !tcp$$reached(now_ms, c->rtx_deadline_ms)) {
            continue;
        }
        if (c->retries >= TCP_MAX_RETRIES) {
            tcp_stats.aborted++;
            tcp$$release(c, true);
            continue;
        }
        c->retries++;
        tcp_stats.retransmits++;
        c->rto_ms = c->rto_ms * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : c->rto_ms * 2;
        c->rtt_timing = false;          /* Karn's rule */
        c->in_recovery = false;
        c->dupacks = 0;

        if (c->state == TCP$SYN_RCVD) {
            tcp$$segment(c, TCP_SYN | TCP_ACK, c->iss, 0, 0, true);
        } else {
            /* Go back N: resend everything from the oldest unacknowledged byte */
            c->snd_nxt = c->snd_una;
            c->fin_sent = false;
            tcp$$output(c, c->snd_wnd == 0);
        }
        tcp$$arm(c, now_ms);
    }
}

/* ---- application interface ------------------------------------------------ */

bool tcp_listen(uint16_t port, const struct tcp_callbacks* callbacks)
{
    if (port == 0) {
        return false;                   /* 0 marks a free slot */
    }
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (tcp$listeners[i].port == port) {
            return false;
        }
    }
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (tcp$listeners[i].port == 0) {
            tcp$listeners[i].port = port;
            tcp$listeners[i].cb = callbacks;
            return true;
        }
    }
    return false;
}

size_t tcp_send(struct tcp_conn* c, const void* data, size_t len)
{
    if ((c->state != TCP$ESTABLISHED && c->state != TCP$CLOSE_WAIT) || c->close_requested) {
        return 0;
    }
    size_t n = tcp$$buf_free(c);
    if (n > len) {
        n = len;
    }
    const uint8_t* src = data;
    for (size_t i = 0; i < n; i++) {
        c->buf[(c->buf_head + c->buf_used + i) % TCP_SNDBUF] = src[i];
    }
    c->buf_used += n;
    return n;                           /* sent by tcp_output() or when the callback returns */
}

size_t tcp_send_space(const struct tcp_conn* c)
{
    if ((c->state != TCP$ESTABLISHED && c->state != TCP$CLOSE_WAIT) || c->close_requested) {
        return 0;
    }
    return tcp$$buf_free(c);
}

void tcp_close(struct tcp_conn* c)
{
    if (c->state == TCP$ESTABLISHED || c->state == TCP$CLOSE_WAIT) {
        c->close_requested = true;
    }
}

void tcp_output(struct tcp_conn* c)
{
    tcp$$output(c, false);
}

void tcp_set_user(struct tcp_conn* c, void* user)
{
    c->user = user;
}

void* tcp_get_user(const struct tcp_conn* c)
{
    return c->user;
}

uint32_t tcp_remote_ip(const struct tcp_conn* c)
{
    return c->remote_ip;
}

uint32_t tcp_local_ip(const struct tcp_conn* c)
{
    return c->local_ip;
}

size_t tcp_active_connections(void)
{
    size_t n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        n += tcp$conns[i].state != TCP$CLOSED && tcp$conns[i].state != TCP$TIME_WAIT;
    }
    return n;
}

/* ---- test services -------------------------------------------------------- */

#define TCP$CHARGEN_BYTES   (1024 * 1024)

static void tcp$$echo_received(struct tcp_conn* c, const uint8_t* data, size_t len)
{
    if (len == 0) {
        tcp_close(c);
        return;
    }
    /* The advertised window is our free send space, so this always fits */
    tcp_send(c, data, len);
}

static const struct tcp_callbacks tcp$echo_callbacks = {
    .received = tcp$$echo_received,
};

/* Sends TCP$CHARGEN_BYTES of (offset % 251), then closes; the count lives in the user pointer */
static void tcp$$chargen_fill(struct tcp_conn* c)
{
    uint32_t sent = (uint32_t)(uintptr_t)tcp_get_user(c);
    uint8_t chunk[512];
    while (sent < TCP$CHARGEN_BYTES && tcp_send_space(c) > 0) {
        size_t n = sizeof(chunk);
        if (n > TCP$CHARGEN_BYTES - sent) {
            n = TCP$CHARGEN_BYTES - sent;
        }
        if (n > tcp_send_space(c)) {
            n = tcp_send_space(c);
        }
        for (size_t i = 0; i < n; i++) {
            chunk[i] = (uint8_t)((sent + i) % 251);
        }
        sent += tcp_send(c, chunk, n);
    }
    tcp_set_user(c, (void*)(uintptr_t)sent);
    if (sent == TCP$CHARGEN_BYTES) {
        tcp_close(c);
    }
}

static const struct tcp_callbacks tcp$chargen_callbacks = {
    .accepted = tcp$$chargen_fill,
    .writable = tcp$$chargen_fill,
};

void tcp_enable_test_services(void)
{
    tcp_listen(7, &tcp$echo_callbacks);
    tcp_listen(19, &tcp$chargen_callbacks);
}
