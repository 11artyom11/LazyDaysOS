#include "dhcp.h"
#include "net.h"

struct dhcp_stats dhcp_stats;

void dhcp_init_message(struct dhcp_message* msg, uint8_t op, uint32_t xid, const uint8_t mac[ETH_ALEN])
{
    uint8_t* bytes = (uint8_t*)msg;
    for (size_t i = 0; i < sizeof(*msg); i++) {
        bytes[i] = 0;
    }
    msg->op = op;
    msg->htype = 1;
    msg->hlen = ETH_ALEN;
    msg->xid = xid;                     /* opaque: copied, never interpreted */
    for (int i = 0; i < ETH_ALEN; i++) {
        msg->chaddr[i] = mac[i];
    }
    msg->magic = net_htonl(DHCP_MAGIC_COOKIE);
}

bool dhcp_put_option(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint8_t len, const void* data)
{
    /* Keep one byte for the END option */
    if (*cursor + 2 + len + 1 > DHCP_OPTIONS_LEN) {
        return false;
    }
    msg->options[(*cursor)++] = code;
    msg->options[(*cursor)++] = len;
    const uint8_t* src = data;
    for (uint8_t i = 0; i < len; i++) {
        msg->options[(*cursor)++] = src[i];
    }
    return true;
}

bool dhcp_put_u8(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint8_t value)
{
    return dhcp_put_option(msg, cursor, code, 1, &value);
}

bool dhcp_put_u32(struct dhcp_message* msg, size_t* cursor, uint8_t code, uint32_t value)
{
    uint32_t net = net_htonl(value);
    return dhcp_put_option(msg, cursor, code, 4, &net);
}

size_t dhcp_finish(struct dhcp_message* msg, size_t cursor)
{
    msg->options[cursor++] = DHCP_OPT_END;
    /* BOOTP relays and some clients expect at least the classic 300-byte message */
    size_t len = DHCP_FIXED_LEN + cursor;
    return len < 300 ? 300 : len;
}

bool dhcp_valid(const struct dhcp_message* msg, size_t len, uint8_t expected_op)
{
    if (len < DHCP_FIXED_LEN || msg->op != expected_op || msg->htype != 1 ||
        msg->hlen != ETH_ALEN || net_ntohl(msg->magic) != DHCP_MAGIC_COOKIE) {
        dhcp_stats.invalid++;
        return false;
    }
    return true;
}

const uint8_t* dhcp_find_option(const struct dhcp_message* msg, size_t len, uint8_t code, uint8_t* opt_len)
{
    if (len < DHCP_FIXED_LEN) {
        return NULL;
    }
    size_t options_len = len - DHCP_FIXED_LEN;
    if (options_len > DHCP_OPTIONS_LEN) {
        options_len = DHCP_OPTIONS_LEN;
    }

    /* Option overloading into sname/file (option 52) is not supported */
    size_t i = 0;
    while (i < options_len) {
        uint8_t c = msg->options[i];
        if (c == DHCP_OPT_END) {
            break;
        }
        if (c == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= options_len || i + 2 + msg->options[i + 1] > options_len) {
            break;                      /* truncated option */
        }
        uint8_t l = msg->options[i + 1];
        if (c == code) {
            *opt_len = l;
            return &msg->options[i + 2];
        }
        i += 2 + l;
    }
    return NULL;
}

bool dhcp_get_u8(const struct dhcp_message* msg, size_t len, uint8_t code, uint8_t* value)
{
    uint8_t l;
    const uint8_t* data = dhcp_find_option(msg, len, code, &l);
    if (!data || l != 1) {
        return false;
    }
    *value = data[0];
    return true;
}

bool dhcp_get_u32(const struct dhcp_message* msg, size_t len, uint8_t code, uint32_t* value)
{
    uint8_t l;
    const uint8_t* data = dhcp_find_option(msg, len, code, &l);
    /* Lists (routers, DNS servers) carry several addresses; take the first */
    if (!data || l < 4 || l % 4 != 0) {
        return false;
    }
    *value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    return true;
}
