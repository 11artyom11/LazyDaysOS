#ifndef __TCP_H__
#define __TCP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/*
    TCP server endpoint (RFC 793/9293 passive open only; no active connect).

    - in-order delivery to the application; up to TCP_OOO_SEGMENTS segments that
      arrive after a gap (inside the advertised window, without FIN) are kept and
      delivered once the gap is filled, so one retransmission repairs a loss
    - the advertised receive window is min(TCP_RCV_WINDOW, free space in the
      connection's send buffer), so an application that answers each request in
      the same callback (echo, HTTP) never receives more than it can queue a
      reply for, while the window stays constant in normal operation (a peer
      only counts an ACK as a duplicate if the window didn't change)
    - an out-of-order segment is answered with an immediate pure duplicate ACK
      (RFC 5681 4.2) so the peer can fast-retransmit
    - retransmission: fast retransmit on three duplicate ACKs with NewReno-style
      partial-ACK recovery (RFC 5681/6582), otherwise go-back-N from snd_una on
      timeout; RTO per RFC 6298 (SRTT/RTTVAR, Karn's rule, exponential backoff
      that is dropped again when new data is acknowledged) with a 200 ms floor
      like Linux; there is no congestion control: the peer's window is the only limit
    - MSS option honoured; other options (window scale, SACK, timestamps) are
      ignored, which makes the peer fall back to plain RFC 793 behaviour
    - TIME_WAIT is shortened to TCP_TIMEWAIT_MS, and the oldest TIME_WAIT slot is
      reused when the table is full
*/

#define TCP_MAX_CONNS       16
#define TCP_MAX_LISTENERS   4
#define TCP_SNDBUF          65536       /* per connection, from the kernel heap */
#define TCP_RCV_WINDOW      16384       /* advertised while the send buffer has that much room */
#define TCP_MSS             1460
#define TCP_DEFAULT_MSS     536         /* RFC 9293 default when the peer sends none */
#define TCP_RTO_INITIAL_MS  1000
#define TCP_RTO_MIN_MS      200
#define TCP_RTO_MAX_MS      60000
#define TCP_MAX_RETRIES     8           /* then the connection is aborted */
#define TCP_TIMEWAIT_MS     2000
#define TCP_OOO_SEGMENTS    8           /* out-of-order segments kept per connection */

struct tcp_conn;

struct tcp_callbacks
{
    void (*accepted)(struct tcp_conn* conn);                        /* handshake completed */
    void (*received)(struct tcp_conn* conn, const uint8_t* data, size_t len); /* len 0: peer finished sending */
    void (*writable)(struct tcp_conn* conn);                        /* send buffer space was freed */
    void (*closed)(struct tcp_conn* conn, bool reset);              /* last call; conn is gone afterwards */
};

/* Accept connections on port; callbacks may be NULL */
bool tcp_listen(uint16_t port, const struct tcp_callbacks* callbacks);

/*
    Queue data; returns how many bytes fit into the send buffer. Nothing is sent
    until the callback that called tcp_send returns (the stack then transmits
    full-sized segments) or tcp_output() is called, which is only needed when
    sending from outside a callback.
*/
size_t tcp_send(struct tcp_conn* conn, const void* data, size_t len);
size_t tcp_send_space(const struct tcp_conn* conn);
void tcp_output(struct tcp_conn* conn);

/* Send FIN once all queued data is out (same deferral as tcp_send) */
void tcp_close(struct tcp_conn* conn);

void tcp_set_user(struct tcp_conn* conn, void* user);
void* tcp_get_user(const struct tcp_conn* conn);
uint32_t tcp_remote_ip(const struct tcp_conn* conn);
uint32_t tcp_local_ip(const struct tcp_conn* conn);

/* IPv4 handler for protocol 6 (local delivery); gets the whole IP packet */
void tcp_receive(struct netdev* dev, const uint8_t* packet, size_t len, size_t header_len);

void tcp_tick(uint32_t now_ms);

/* Test services: echo on port 7, a 1 MiB "chargen" stream on port 19 */
void tcp_enable_test_services(void);

size_t tcp_active_connections(void);

struct tcp_stats
{
    uint32_t accepted;
    uint32_t rx_segments;
    uint32_t rx_invalid;                /* bad length or checksum */
    uint32_t rx_out_of_order;           /* arrived after a gap */
    uint32_t rx_ooo_queued;             /* ... and were kept for later delivery */
    uint32_t tx_segments;
    uint32_t retransmits;               /* on timeout */
    uint32_t fast_retransmits;          /* on three duplicate ACKs */
    uint32_t resets_sent;
    uint32_t aborted;                   /* gave up after TCP_MAX_RETRIES, or reset by peer */
    uint32_t refused;                   /* SYN dropped: connection table full */
};

extern struct tcp_stats tcp_stats;

#endif /* __TCP_H__ */
