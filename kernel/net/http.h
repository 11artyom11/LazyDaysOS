#ifndef __HTTP_H__
#define __HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/*
    Minimal HTTP/1.1 server on top of the TCP endpoint: one request per
    connection (every response says "Connection: close"), exact-path routes,
    GET and POST with a small body. Only reachable from inside ports: SYNs
    arriving on a NAT outside port are dropped (and a request that got through
    anyway would get 403), so the admin interface never faces the WAN.

    Checks on every request, before any route runs:
    - Host must name the router: one of its IPv4 addresses, "localhost" or
      "127.0.0.1" (any port), else 421. This defeats DNS rebinding, where a web
      page's own domain is re-pointed at the router to read its API.
    - A POST that carries an Origin header must come from that same host
      ("http://" + Host), else 403: browsers send Origin on cross-site POSTs.
    - Routes not registered with HTTP_PUBLIC go through the guard set with
      http_set_guard (the login check, kernel/net/auth.c).
    Every response carries headers that forbid framing, MIME sniffing and referrers.

    Limits: request line + headers + body <= HTTP_REQUEST_MAX bytes (431 / 413
    beyond), POST body <= HTTP_BODY_MAX, a request must be complete within
    HTTP_IDLE_TIMEOUT_MS (408). Responses are streamed through the TCP send
    buffer, so bodies can be larger than it.
*/

#define HTTP_REQUEST_MAX        2048
#define HTTP_BODY_MAX           512
#define HTTP_IDLE_TIMEOUT_MS    5000
#define HTTP_MAX_ROUTES         16
#define HTTP_PUBLIC             0x1     /* route flag: no login needed */

struct http_request
{
    const char* method;
    const char* path;
    const char* query;                  /* after '?', "" if none */
    const char* body;
    size_t body_len;
    uint32_t remote_ip;
    /* Header values (NULL if absent) */
    const char* host;
    const char* origin;
    const char* cookie;
    const char* authorization;
    const char* csrf_token;             /* X-CSRF-Token */
};

struct http_response
{
    int status;                         /* defaults to 200 */
    const char* content_type;           /* defaults to text/plain */
    const char* extra_headers;          /* e.g. "Content-Encoding: gzip\r\n", or NULL; copied before the handler's caller returns */
    const uint8_t* body;
    size_t body_len;
    void* owned;                        /* kfree'd once the response is sent, or NULL */
};

typedef void (*http_handler)(const struct http_request* request, struct http_response* response);

/* Runs before every non-public route; returns true to let it run, or fills in the
   response (401, 403, 429, ...) and returns false */
typedef bool (*http_guard)(const struct http_request* request, struct http_response* response);

/* flags: 0 or HTTP_PUBLIC */
bool http_route(const char* method, const char* path, http_handler handler, uint32_t flags);
void http_set_guard(http_guard guard);
bool http_start(uint16_t port);
void http_tick(uint32_t now_ms);

/* Copy query parameter name into out; false if missing or too long */
bool http_query_param(const char* query, const char* name, char* out, size_t out_cap);

/* The same for an application/x-www-form-urlencoded body, percent-decoded ('+' = space) */
bool http_form_param(const char* body, size_t body_len, const char* name, char* out, size_t out_cap);

/* Convenience: respond with a static string body */
void http_respond_text(struct http_response* response, int status, const char* content_type, const char* text);

struct http_stats
{
    uint32_t requests;
    uint32_t errors;                    /* 4xx/5xx sent */
    uint32_t forbidden;                 /* from an outside port, or a cross-site POST */
    uint32_t wrong_host;                /* Host header not naming the router (DNS rebinding) */
};

extern struct http_stats http_stats;

#endif /* __HTTP_H__ */
