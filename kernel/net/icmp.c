#include "icmp.h"
#include "ipv4.h"
#include "net.h"
#include <pit.h>

#define ICMP$PING_ID        0x4C44      /* "LD" */
#define ICMP$PING_DATA_LEN  56          /* same default as Linux ping */

struct icmp_stats icmp_stats;

static uint16_t icmp$ping_seq = 0;
static volatile bool icmp$ping_answered;

void icmp_receive(struct netdev* dev, uint32_t src, uint32_t dst, const uint8_t* payload, size_t len)
{
    (void)dev;
    const struct icmp_echo* icmp = (const struct icmp_echo*)payload;

    if (len < sizeof(struct icmp_echo) || net_checksum(payload, len) != 0) {
        icmp_stats.rx_invalid++;
        return;
    }

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        icmp_stats.rx_echo_requests++;
        /* Like Linux, only answer pings to one of our own addresses, never broadcasts */
        if (!ipv4_is_own_address(dst)) {
            return;
        }

        /* The reply echoes id, sequence and data unchanged */
        uint8_t reply[IPV4_PAYLOAD_MAX];
        for (size_t i = 0; i < len; i++) {
            reply[i] = payload[i];
        }
        struct icmp_echo* out = (struct icmp_echo*)reply;
        out->type = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = net_checksum(reply, len);

        /* Reply from the address that was pinged */
        if (ipv4_send(dst, src, IP_PROTO_ICMP, reply, len)) {
            icmp_stats.tx_echo_replies++;
        }
        return;
    }

    if (icmp->type == ICMP_ECHO_REPLY) {
        icmp_stats.rx_echo_replies++;
        if (net_ntohs(icmp->id) == ICMP$PING_ID && net_ntohs(icmp->seq) == icmp$ping_seq) {
            icmp$ping_answered = true;
        }
        return;
    }

    icmp_stats.rx_other++;
}

int32_t icmp_ping(uint32_t dst, uint32_t timeout_ms)
{
    uint8_t request[sizeof(struct icmp_echo) + ICMP$PING_DATA_LEN];
    struct icmp_echo* icmp = (struct icmp_echo*)request;

    icmp$ping_seq++;
    icmp$ping_answered = false;

    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->id = net_htons(ICMP$PING_ID);
    icmp->seq = net_htons(icmp$ping_seq);
    for (size_t i = 0; i < ICMP$PING_DATA_LEN; i++) {
        request[sizeof(struct icmp_echo) + i] = (uint8_t)i;
    }
    icmp->checksum = 0;
    icmp->checksum = net_checksum(request, sizeof(request));

    uint32_t start = pit_uptime_ms();
    if (!ipv4_send(0, dst, IP_PROTO_ICMP, request, sizeof(request))) {
        return -1;
    }
    icmp_stats.tx_echo_requests++;

    while (!icmp$ping_answered) {
        if (pit_uptime_ms() - start >= timeout_ms) {
            return -1;
        }
        net_run_once();
    }
    return (int32_t)(pit_uptime_ms() - start);
}
