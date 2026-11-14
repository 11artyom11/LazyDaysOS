#include "http.h"
#include "tcp.h"
#include "ipv4.h"
#include "nat.h"
#include "net.h"
#include <strbuf.h>
#include <pit.h>
#include "../heap.h"

struct http$route
{
    const char* method;
    const char* path;
    http_handler handler;
    uint32_t flags;
};

struct http$conn
{
    bool in_use;
    bool responded;
    struct tcp_conn* tcp;
    uint32_t start_ms;
    char request[HTTP_REQUEST_MAX + 1];
    size_t request_len;

    /* Response being streamed: header, then body */
    char header[640];
    size_t header_len;
    size_t header_sent;
    const uint8_t* body;
    size_t body_len;
    size_t body_sent;
    void* owned;
};

static struct http$route http$routes[HTTP_MAX_ROUTES];
static http_guard http$guard = NULL;
static struct http$conn http$conns[TCP_MAX_CONNS];

struct http_stats http_stats;

static bool http$$streq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Case-insensitive prefix match for header names */
static bool http$$header_is(const char* line, const char* name)
{
    for (; *name; line++, name++) {
        char c = *line;
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != *name) {
            return false;
        }
    }
    return *line == ':';
}

static const char* http$$reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 421: return "Misdirected Request";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

bool http_route(const char* method, const char* path, http_handler handler, uint32_t flags)
{
    for (int i = 0; i < HTTP_MAX_ROUTES; i++) {
        if (http$routes[i].handler == NULL) {
            http$routes[i].method = method;
            http$routes[i].path = path;
            http$routes[i].handler = handler;
            http$routes[i].flags = flags;
            return true;
        }
    }
    return false;
}

void http_set_guard(http_guard guard)
{
    http$guard = guard;
}

void http_respond_text(struct http_response* response, int status, const char* content_type, const char* text)
{
    size_t len = 0;
    while (text[len]) {
        len++;
    }
    response->status = status;
    response->content_type = content_type;
    response->body = (const uint8_t*)text;
    response->body_len = len;
}

bool http_query_param(const char* query, const char* name, char* out, size_t out_cap)
{
    const char* p = query;
    while (*p) {
        const char* n = name;
        const char* q = p;
        while (*n && *q == *n) {
            n++;
            q++;
        }
        if (*n == '\0' && *q == '=') {
            q++;
            size_t len = 0;
            while (q[len] && q[len] != '&') {
                if (len + 1 >= out_cap) {
                    return false;
                }
                out[len] = q[len];
                len++;
            }
            out[len] = '\0';
            return true;
        }
        while (*p && *p != '&') {
            p++;
        }
        if (*p == '&') {
            p++;
        }
    }
    return false;
}

static int http$$hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool http_form_param(const char* body, size_t body_len, const char* name, char* out, size_t out_cap)
{
    size_t i = 0;
    while (i < body_len) {
        size_t n = 0;
        while (name[n] && i + n < body_len && body[i + n] == name[n]) {
            n++;
        }
        if (name[n] == '\0' && i + n < body_len && body[i + n] == '=') {
            size_t len = 0;
            for (i += n + 1; i < body_len && body[i] != '&'; i++) {
                char c = body[i];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    int hi = i + 2 < body_len ? http$$hex(body[i + 1]) : -1;
                    int lo = i + 2 < body_len ? http$$hex(body[i + 2]) : -1;
                    if (hi < 0 || lo < 0) {
                        return false;
                    }
                    c = (char)(hi * 16 + lo);
                    i += 2;
                }
                if (len + 1 >= out_cap || c == '\0') {
                    return false;
                }
                out[len++] = c;
            }
            out[len] = '\0';
            return true;
        }
        while (i < body_len && body[i] != '&') {
            i++;
        }
        i++;
    }
    return false;
}

/* Host header names the router: one of its addresses, or localhost (port ignored) */
static bool http$$host_allowed(const char* host)
{
    char name[32];
    size_t len = 0;
    while (host[len] && host[len] != ':') {
        if (len + 1 >= sizeof(name)) {
            return false;
        }
        char c = host[len];
        name[len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 'a' - 'A') : c;
    }
    name[len] = '\0';
    uint32_t ip;
    if (net_parse_ip(name, &ip)) {
        return ipv4_is_own_address(ip) || ip == NET_IP(127, 0, 0, 1);
    }
    return http$$streq(name, "localhost");
}

/* Origin, if present, must be "http://" + Host */
static bool http$$same_origin(const char* origin, const char* host)
{
    const char* prefix = "http://";
    while (*prefix) {
        if (*origin++ != *prefix++) {
            return false;
        }
    }
    return http$$streq(origin, host);
}

/* Queue as much of the response as the TCP send buffer takes; close when all is queued */
static void http$$pump(struct http$conn* h)
{
    if (h->header_sent < h->header_len) {
        h->header_sent += tcp_send(h->tcp, h->header + h->header_sent, h->header_len - h->header_sent);
    }
    if (h->header_sent == h->header_len && h->body_sent < h->body_len) {
        h->body_sent += tcp_send(h->tcp, h->body + h->body_sent, h->body_len - h->body_sent);
    }
    if (h->header_sent == h->header_len && h->body_sent == h->body_len) {
        tcp_close(h->tcp);
    }
}

static void http$$respond(struct http$conn* h, struct http_response* r)
{
    struct strbuf sb;
    sb_init(&sb, h->header, sizeof(h->header));
    sb_str(&sb, "HTTP/1.1 ");
    sb_uint(&sb, (uint32_t)r->status);
    sb_char(&sb, ' ');
    sb_str(&sb, http$$reason(r->status));
    sb_str(&sb, "\r\nServer: LazyDaysOS\r\nContent-Type: ");
    sb_str(&sb, r->content_type ? r->content_type : "text/plain");
    sb_str(&sb, "\r\nContent-Length: ");
    sb_uint(&sb, (uint32_t)r->body_len);
    sb_str(&sb, "\r\nCache-Control: no-store\r\nConnection: close\r\n"
                "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
                "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
                "style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; form-action 'self'\r\n");
    if (r->extra_headers) {
        sb_str(&sb, r->extra_headers);
    }
    sb_str(&sb, "\r\n");

    h->responded = true;
    h->header_len = sb.len;
    h->header_sent = 0;
    h->body = r->body;
    h->body_len = r->body_len;
    h->body_sent = 0;
    h->owned = r->owned;
    if (r->status >= 400) {
        http_stats.errors++;
    }
    http$$pump(h);
}

static void http$$error(struct http$conn* h, int status, const char* message)
{
    struct http_response r = {0};
    http_respond_text(&r, status, "text/plain", message);
    http$$respond(h, &r);
}

/* A complete request is in h->request: parse it in place and run the route */
static void http$$dispatch(struct http$conn* h, char* body, size_t body_len)
{
    char* method = h->request;
    char* target = method;
    while (*target && *target != ' ') {
        target++;
    }
    if (*target != ' ') {
        http$$error(h, 400, "malformed request line\n");
        return;
    }
    *target++ = '\0';
    char* version = target;
    while (*version && *version != ' ' && *version != '\r') {
        version++;
    }
    if (*version != ' ' || target[0] != '/') {
        http$$error(h, 400, "malformed request line\n");
        return;
    }
    *version++ = '\0';
    if (version[0] != 'H' || version[1] != 'T' || version[2] != 'T' || version[3] != 'P' || version[4] != '/') {
        http$$error(h, 400, "not HTTP\n");
        return;
    }

    /* Headers we use: terminate each value in place */
    struct http_request request = {0};
    char* line = version;
    while (*line && *line != '\n') {
        line++;
    }
    while (*line == '\n' && line[1]) {
        line++;
        char* value = line;
        while (*value && *value != ':' && *value != '\r') {
            value++;
        }
        char* end = value;
        while (*end && *end != '\r') {
            end++;
        }
        if (*value == ':') {
            const char** field = http$$header_is(line, "host") ? &request.host
                               : http$$header_is(line, "origin") ? &request.origin
                               : http$$header_is(line, "cookie") ? &request.cookie
                               : http$$header_is(line, "authorization") ? &request.authorization
                               : http$$header_is(line, "x-csrf-token") ? &request.csrf_token : NULL;
            value++;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            if (field) {
                *field = value;
            }
        }
        line = end;
        if (*line == '\r') {
            *line++ = '\0';            /* now at the '\n' ending this header */
        }
    }
    char* query = target;
    while (*query && *query != '?') {
        query++;
    }
    if (*query == '?') {
        *query++ = '\0';
    }

    /* The admin interface is for inside ports only. http$$admit already refuses
       connections from outside ports; this is a second line of defence. */
    struct netdev* port = ipv4_iface_with_address(tcp_local_ip(h->tcp));
    if (port && nat_is_outside(port)) {
        http_stats.forbidden++;
        http$$error(h, 403, "the router is managed from the LAN only\n");
        return;
    }

    /* Requests must name the router (DNS rebinding), POSTs must not be cross-site (CSRF) */
    if (!request.host) {
        http$$error(h, 400, "Host header required\n");
        return;
    }
    if (!http$$host_allowed(request.host)) {
        http_stats.wrong_host++;
        http$$error(h, 421, "this is not the host you asked for\n");
        return;
    }
    if (request.origin && http$$streq(method, "POST") && !http$$same_origin(request.origin, request.host)) {
        http_stats.forbidden++;
        http$$error(h, 403, "cross-site request refused\n");
        return;
    }

    request.method = method;
    request.path = target;
    request.query = query;
    request.body = body;
    request.body_len = body_len;
    request.remote_ip = tcp_remote_ip(h->tcp);
    bool path_known = false;
    for (int i = 0; i < HTTP_MAX_ROUTES && http$routes[i].handler; i++) {
        if (!http$$streq(http$routes[i].path, target)) {
            continue;
        }
        path_known = true;
        if (http$$streq(http$routes[i].method, method)) {
            struct http_response response = {.status = 200};
            http_stats.requests++;
            if ((http$routes[i].flags & HTTP_PUBLIC) || !http$guard || http$guard(&request, &response)) {
                http$routes[i].handler(&request, &response);
            }
            http$$respond(h, &response);
            return;
        }
    }
    if (path_known) {
        http$$error(h, 405, "method not allowed\n");
    } else {
        http$$error(h, 404, "not found\n");
    }
}

/* Called whenever request bytes arrived: dispatch once the request is complete */
static void http$$try_parse(struct http$conn* h)
{
    char* end = NULL;
    for (size_t i = 0; i + 3 < h->request_len; i++) {
        if (h->request[i] == '\r' && h->request[i + 1] == '\n' && h->request[i + 2] == '\r' && h->request[i + 3] == '\n') {
            end = &h->request[i];
            break;
        }
    }
    if (!end) {
        if (h->request_len >= HTTP_REQUEST_MAX) {
            http$$error(h, 431, "request header too large\n");
        }
        return;
    }

    /* Content-Length, if any */
    size_t content_length = 0;
    for (char* line = h->request; line < end; ) {
        if (line != h->request && http$$header_is(line, "content-length")) {
            const char* v = line + 15;
            while (*v == ' ') {
                v++;
            }
            content_length = 0;
            while (*v >= '0' && *v <= '9') {
                content_length = content_length * 10 + (*v++ - '0');
                if (content_length > HTTP_BODY_MAX) {
                    http$$error(h, 413, "request body too large\n");
                    return;
                }
            }
        }
        while (line < end && *line != '\n') {
            line++;
        }
        line++;
    }
    char* body = end + 4;
    size_t have = h->request_len - (size_t)(body - h->request);
    if (have < content_length) {
        if (h->request_len >= HTTP_REQUEST_MAX) {
            http$$error(h, 413, "request too large\n");
        }
        return;
    }
    end[2] = '\0';                      /* terminate the header block */
    http$$dispatch(h, body, content_length);
}

static void http$$accepted(struct tcp_conn* tcp)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct http$conn* h = &http$conns[i];
        if (!h->in_use) {
            h->in_use = true;
            h->responded = false;
            h->tcp = tcp;
            h->request_len = 0;
            h->owned = NULL;
            h->start_ms = pit_uptime_ms();
            tcp_set_user(tcp, h);
            return;
        }
    }
    tcp_close(tcp);
}

static void http$$received(struct tcp_conn* tcp, const uint8_t* data, size_t len)
{
    struct http$conn* h = tcp_get_user(tcp);
    if (!h || h->responded) {
        return;                         /* one request per connection; ignore the rest */
    }
    if (len == 0) {
        tcp_close(tcp);                 /* peer gave up before finishing the request */
        return;
    }
    size_t room = HTTP_REQUEST_MAX - h->request_len;
    size_t n = len < room ? len : room;
    for (size_t i = 0; i < n; i++) {
        h->request[h->request_len + i] = (char)data[i];
    }
    h->request_len += n;
    h->request[h->request_len] = '\0';
    http$$try_parse(h);
}

static void http$$writable(struct tcp_conn* tcp)
{
    struct http$conn* h = tcp_get_user(tcp);
    if (h && h->responded) {
        http$$pump(h);
    }
}

static void http$$closed(struct tcp_conn* tcp, bool reset)
{
    (void)reset;
    struct http$conn* h = tcp_get_user(tcp);
    if (h) {
        kfree(h->owned);
        h->owned = NULL;
        h->in_use = false;
    }
}

/* The router is managed from inside ports only: SYNs from a NAT outside port are
   dropped before the TCP handshake, so the WAN can neither reach the HTTP parser
   nor tie up connection slots */
static bool http$$admit(struct netdev* dev, uint32_t remote_ip)
{
    (void)remote_ip;
    return !nat_is_outside(dev);
}

static const struct tcp_callbacks http$callbacks = {
    .admit = http$$admit,
    .accepted = http$$accepted,
    .received = http$$received,
    .writable = http$$writable,
    .closed = http$$closed,
};

bool http_start(uint16_t port)
{
    return tcp_listen(port, &http$callbacks);
}

void http_tick(uint32_t now_ms)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct http$conn* h = &http$conns[i];
        if (h->in_use && !h->responded && now_ms - h->start_ms >= HTTP_IDLE_TIMEOUT_MS) {
            http$$error(h, 408, "request timeout\n");
            tcp_output(h->tcp);         /* not inside a TCP callback: flush ourselves */
        }
    }
}
