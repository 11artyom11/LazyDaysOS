#ifndef __HTTP_H__
#define __HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/*
    Minimal HTTP/1.1 server on top of the TCP endpoint: one request per
    connection (every response says "Connection: close"), exact-path routes,
    GET and POST with a small body. Only reachable from inside ports: requests
    arriving on a NAT outside port get 403, so the admin interface never faces
    the WAN.

    Limits: request line + headers + body <= HTTP_REQUEST_MAX bytes (431 / 413
    beyond), POST body <= HTTP_BODY_MAX, a request must be complete within
    HTTP_IDLE_TIMEOUT_MS (408). Responses are streamed through the TCP send
    buffer, so bodies can be larger than it.
*/

#define HTTP_REQUEST_MAX        2048
#define HTTP_BODY_MAX           512
#define HTTP_IDLE_TIMEOUT_MS    5000
#define HTTP_MAX_ROUTES         16

struct http_request
{
    const char* method;
    const char* path;
    const char* query;                  /* after '?', "" if none */
    const char* body;
    size_t body_len;
    uint32_t remote_ip;
};

struct http_response
{
    int status;                         /* defaults to 200 */
    const char* content_type;           /* defaults to text/plain */
    const char* extra_headers;          /* e.g. "Content-Encoding: gzip\r\n", or NULL */
    const uint8_t* body;
    size_t body_len;
    void* owned;                        /* kfree'd once the response is sent, or NULL */
};

typedef void (*http_handler)(const struct http_request* request, struct http_response* response);

bool http_route(const char* method, const char* path, http_handler handler);
bool http_start(uint16_t port);
void http_tick(uint32_t now_ms);

/* Copy query parameter name into out; false if missing or too long */
bool http_query_param(const char* query, const char* name, char* out, size_t out_cap);

/* Convenience: respond with a static string body */
void http_respond_text(struct http_response* response, int status, const char* content_type, const char* text);

struct http_stats
{
    uint32_t requests;
    uint32_t errors;                    /* 4xx/5xx sent */
    uint32_t forbidden;                 /* from an outside port */
};

extern struct http_stats http_stats;

#endif /* __HTTP_H__ */
