#include "eth.h"
#include "net.h"
#include "arp.h"
#include "ipv4.h"

const uint8_t eth_broadcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct eth_stats eth_stats;

static bool eth$$is_for(const struct netdev* dev, const uint8_t dst[ETH_ALEN])
{
    bool broadcast = true;
    bool ours = true;
    for (int i = 0; i < ETH_ALEN; i++) {
        broadcast &= dst[i] == 0xFF;
        ours &= dst[i] == dev->mac[i];
    }
    return broadcast || ours;
}

void eth_receive(struct netdev* dev, const uint8_t* frame, size_t len)
{
    const struct eth_header* eth = (const struct eth_header*)frame;
    const uint8_t* payload = frame + ETH_HEADER_LEN;
    size_t payload_len = len - ETH_HEADER_LEN;

    if (!eth$$is_for(dev, eth->dst)) {
        eth_stats.rx_not_for_us++;
        return;
    }

    switch (net_ntohs(eth->ethertype)) {
    case ETHERTYPE_ARP:
        arp_receive(dev, payload, payload_len);
        break;
    case ETHERTYPE_IPV4:
        ipv4_receive(dev, payload, payload_len);
        break;
    default:
        eth_stats.rx_unknown_type++;
        break;
    }
}

bool eth_send(struct netdev* dev, const uint8_t dst[ETH_ALEN], uint16_t ethertype,
              const void* payload, size_t len)
{
    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header* eth = (struct eth_header*)frame;

    if (len > ETH_PAYLOAD_MAX) {
        return false;
    }
    for (int i = 0; i < ETH_ALEN; i++) {
        eth->dst[i] = dst[i];
        eth->src[i] = dev->mac[i];
    }
    eth->ethertype = net_htons(ethertype);

    const uint8_t* src = payload;
    for (size_t i = 0; i < len; i++) {
        frame[ETH_HEADER_LEN + i] = src[i];
    }
    return netdev_transmit(dev, frame, ETH_HEADER_LEN + len);
}
