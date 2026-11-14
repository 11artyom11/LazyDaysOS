#ifndef __API_H__
#define __API_H__

#include <stdbool.h>
#include <stdint.h>

/*
    The router's management API, served by the HTTP server:

    GET  /                  dashboard page (public; it asks for the password)
    POST /api/login, /api/logout, GET /api/session, POST /api/password: see auth.h
    GET  /api/stats         everything the dashboard shows, as one JSON object
    GET  /api/events?since=N  events with id > N (JSON array)
    POST /api/block?ip=a.b.c.d    stop forwarding for that inside host
    POST /api/unblock?ip=a.b.c.d

    Everything except / and /api/login needs a login (auth.h).
*/

#define API_JSON_MAX    65536           /* /api/stats with a full 256-route table is ~30 KiB */

bool api_start(uint16_t port);

#endif /* __API_H__ */
