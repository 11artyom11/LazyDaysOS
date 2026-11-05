#include "api.h"
#include "http.h"
#include "net.h"
#include "netdev.h"
#include "ipv4.h"
#include "nat.h"
#include "tcp.h"
#include "udp.h"
#include "dhcp.h"
#include "clients.h"
#include "events.h"
#include <strbuf.h>
#include <pit.h>
#include "../heap.h"
#include "../system.h"

/* www/index.html, gzipped by the Makefile and linked in with ld -b binary */
extern const uint8_t _binary_dashboard_html_gz_start[];
extern const uint8_t _binary_dashboard_html_gz_end[];

static void api$$index(const struct http_request* req, struct http_response* res)
{
    (void)req;
    /* Every browser accepts gzip; it keeps the kernel image small */
    res->status = 200;
    res->content_type = "text/html; charset=utf-8";
    res->extra_headers = "Content-Encoding: gzip\r\n";
    res->body = _binary_dashboard_html_gz_start;
    res->body_len = (size_t)(_binary_dashboard_html_gz_end - _binary_dashboard_html_gz_start);
}

/* Give the response a heap buffer to build JSON in; false (and a 503) if there's no memory */
static bool api$$json_buffer(struct http_response* res, struct strbuf* sb)
{
    char* buffer = kmalloc(API_JSON_MAX);
    if (!buffer) {
        http_respond_text(res, 503, "application/json", "{\"error\":\"out of memory\"}");
        return false;
    }
    sb_init(sb, buffer, API_JSON_MAX);
    res->owned = buffer;
    return true;
}

static void api$$finish_json(struct http_response* res, struct strbuf* sb)
{
    if (sb->overflow) {
        /* res->owned is still freed after sending */
        http_respond_text(res, 500, "application/json", "{\"error\":\"response too large\"}");
        return;
    }
    res->status = 200;
    res->content_type = "application/json";
    res->body = (const uint8_t*)sb->data;
    res->body_len = sb->len;
}

static void api$$key_uint(struct strbuf* sb, const char* key, uint32_t value, bool first)
{
    sb_json_key(sb, key, first);
    sb_uint(sb, value);
}

static void api$$interfaces(struct strbuf* sb)
{
    sb_char(sb, '[');
    for (size_t i = 0; i < netdev_count(); i++) {
        struct netdev* dev = netdev_get(i);
        uint32_t addr = ipv4_iface_address(dev);
        if (i) {
            sb_char(sb, ',');
        }
        sb_char(sb, '{');
        sb_json_key(sb, "name", true);
        sb_json_str(sb, dev->name);
        sb_json_key(sb, "mac", false);
        sb_char(sb, '"');
        sb_mac(sb, dev->mac);
        sb_char(sb, '"');
        sb_json_key(sb, "role", false);
        sb_json_str(sb, nat_is_outside(dev) ? "wan" : "lan");
        sb_json_key(sb, "link", false);
        sb_str(sb, dev->link_up ? "true" : "false");
        sb_json_key(sb, "ipv4", false);
        if (addr) {
            sb_char(sb, '"');
            sb_ip(sb, addr);
            sb_char(sb, '/');
            sb_uint(sb, ipv4_prefix_len(ipv4_iface_netmask(dev)));
            sb_char(sb, '"');
        } else {
            sb_str(sb, "null");
        }
        sb_json_key(sb, "dhcp", false);
        sb_json_str(sb, dhcp_server_has_pool(dev) ? "server" : dhcp_client_running(dev) ? "client" : "none");
        api$$key_uint(sb, "rx_packets", dev->stats.rx_packets, false);
        api$$key_uint(sb, "rx_bytes", dev->stats.rx_bytes, false);
        api$$key_uint(sb, "rx_dropped", dev->stats.rx_dropped, false);
        api$$key_uint(sb, "tx_packets", dev->stats.tx_packets, false);
        api$$key_uint(sb, "tx_bytes", dev->stats.tx_bytes, false);
        api$$key_uint(sb, "tx_dropped", dev->stats.tx_dropped, false);
        sb_char(sb, '}');
    }
    sb_char(sb, ']');
}

static void api$$stats(const struct http_request* req, struct http_response* res)
{
    (void)req;
    struct strbuf sb;
    if (!api$$json_buffer(res, &sb)) {
        return;
    }
    struct mem$heap_stats heap;
    mem$heap_get_stats(&heap);

    sb_char(&sb, '{');
    sb_json_key(&sb, "system", true);
    sb_char(&sb, '{');
    sb_json_key(&sb, "name", true);
    sb_json_str(&sb, "LazyDaysOS");
    api$$key_uint(&sb, "uptime_ms", pit_uptime_ms(), false);
    api$$key_uint(&sb, "ready_ms", system_ready_ms, false);
    api$$key_uint(&sb, "heap_size", heap.hs$size, false);
    api$$key_uint(&sb, "heap_used", heap.hs$used, false);
    sb_char(&sb, '}');

    sb_json_key(&sb, "interfaces", false);
    api$$interfaces(&sb);
    sb_json_key(&sb, "clients", false);
    clients_json(&sb);
    sb_json_key(&sb, "routes", false);
    ipv4_routes_json(&sb);

    sb_json_key(&sb, "ipv4", false);
    sb_char(&sb, '{');
    api$$key_uint(&sb, "rx_packets", ipv4_stats.rx_packets, true);
    api$$key_uint(&sb, "tx_packets", ipv4_stats.tx_packets, false);
    api$$key_uint(&sb, "forwarded", ipv4_stats.fwd_packets, false);
    api$$key_uint(&sb, "ttl_exceeded", ipv4_stats.fwd_ttl_exceeded, false);
    api$$key_uint(&sb, "no_route", ipv4_stats.fwd_no_route, false);
    api$$key_uint(&sb, "bad_header", ipv4_stats.rx_bad_header, false);
    sb_char(&sb, '}');

    sb_json_key(&sb, "nat", false);
    sb_char(&sb, '{');
    api$$key_uint(&sb, "active", (uint32_t)nat_active_entries(), true);
    api$$key_uint(&sb, "created", nat_stats.created, false);
    api$$key_uint(&sb, "expired", nat_stats.expired, false);
    api$$key_uint(&sb, "outbound", nat_stats.outbound, false);
    api$$key_uint(&sb, "inbound", nat_stats.inbound, false);
    api$$key_uint(&sb, "icmp_errors", nat_stats.icmp_errors, false);
    api$$key_uint(&sb, "table_full", nat_stats.table_full, false);
    api$$key_uint(&sb, "firewall_dropped", nat_stats.fw_dropped, false);
    api$$key_uint(&sb, "blocked_dropped", clients_stats.blocked_packets, false);
    sb_char(&sb, '}');

    sb_json_key(&sb, "dhcp", false);
    sb_char(&sb, '{');
    api$$key_uint(&sb, "leases", (uint32_t)dhcp_server_lease_count(), true);
    api$$key_uint(&sb, "offers", dhcp_stats.server_offers, false);
    api$$key_uint(&sb, "acks", dhcp_stats.server_acks, false);
    api$$key_uint(&sb, "naks", dhcp_stats.server_naks, false);
    sb_char(&sb, '}');

    sb_json_key(&sb, "tcp", false);
    sb_char(&sb, '{');
    api$$key_uint(&sb, "active", (uint32_t)tcp_active_connections(), true);
    api$$key_uint(&sb, "accepted", tcp_stats.accepted, false);
    api$$key_uint(&sb, "retransmits", tcp_stats.retransmits, false);
    api$$key_uint(&sb, "fast_retransmits", tcp_stats.fast_retransmits, false);
    api$$key_uint(&sb, "resets_sent", tcp_stats.resets_sent, false);
    sb_char(&sb, '}');

    sb_json_key(&sb, "http", false);
    sb_char(&sb, '{');
    api$$key_uint(&sb, "requests", http_stats.requests, true);
    api$$key_uint(&sb, "errors", http_stats.errors, false);
    api$$key_uint(&sb, "forbidden", http_stats.forbidden, false);
    sb_char(&sb, '}');
    sb_char(&sb, '}');

    api$$finish_json(res, &sb);
}

static void api$$events(const struct http_request* req, struct http_response* res)
{
    char value[12];
    uint32_t since = 0;
    if (http_query_param(req->query, "since", value, sizeof(value))) {
        for (const char* p = value; *p >= '0' && *p <= '9'; p++) {
            since = since * 10 + (uint32_t)(*p - '0');
        }
    }
    struct strbuf sb;
    if (api$$json_buffer(res, &sb)) {
        events_json(&sb, since);
        api$$finish_json(res, &sb);
    }
}

static void api$$set_blocked(const struct http_request* req, struct http_response* res, bool blocked)
{
    char value[20];
    uint32_t ip;
    if (!http_query_param(req->query, "ip", value, sizeof(value)) || !net_parse_ip(value, &ip)) {
        http_respond_text(res, 400, "application/json", "{\"error\":\"expected ?ip=a.b.c.d\"}");
        return;
    }
    if (!clients_set_blocked(ip, blocked)) {
        http_respond_text(res, 503, "application/json", "{\"error\":\"client table full\"}");
        return;
    }
    struct strbuf sb;
    if (api$$json_buffer(res, &sb)) {
        sb_str(&sb, "{\"ok\":true,\"ip\":\"");
        sb_ip(&sb, ip);
        sb_str(&sb, blocked ? "\",\"blocked\":true}" : "\",\"blocked\":false}");
        api$$finish_json(res, &sb);
    }
}

static void api$$block(const struct http_request* req, struct http_response* res)
{
    api$$set_blocked(req, res, true);
}

static void api$$unblock(const struct http_request* req, struct http_response* res)
{
    api$$set_blocked(req, res, false);
}

bool api_start(uint16_t port)
{
    return http_route("GET", "/", api$$index) &&
           http_route("GET", "/api/stats", api$$stats) &&
           http_route("GET", "/api/events", api$$events) &&
           http_route("POST", "/api/block", api$$block) &&
           http_route("POST", "/api/unblock", api$$unblock) &&
           http_start(port);
}
