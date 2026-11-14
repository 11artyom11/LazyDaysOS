#ifndef __AUTH_H__
#define __AUTH_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
    Login for the management API: one account, "admin".

    Password: admin_password= on the kernel command line, otherwise a random
    12-character password generated at boot and printed on the console (the
    "unique per-device password" that ETSI EN 303 645 and the UK PSTI rules ask
    for instead of a shared default). POST /api/password changes it until the
    next boot (there is no persistent storage yet).

    Two ways to authenticate a request:
    - a session (browsers): POST /api/login with password=... sets the cookie
      lazydays_session (HttpOnly, SameSite=Strict) and returns a CSRF token that
      every POST in the session must repeat in an X-CSRF-Token header. Sessions
      end after AUTH_IDLE_MS without use or AUTH_SESSION_MS in total.
    - HTTP Basic (scripts: curl -u admin:password). Never offered to browsers
      (401s carry no Basic challenge), so a browser can't be tricked into
      sending it; cross-site POSTs are refused by the Origin check anyway.

    Guessing: AUTH_MAX_FAILURES wrong passwords from one address lock that
    address out for AUTH_LOCKOUT_MS, doubling with each further lockout up to
    16 times as long. While locked, even the right password gets 429.

    Routes: POST /api/login (public), POST /api/logout, GET /api/session,
    POST /api/password (current=...&new=...). Everything else the API serves,
    except the dashboard page itself, needs a login.
*/

#define AUTH_USER               "admin"
#define AUTH_PASSWORD_MIN       8       /* for passwords set through the API */
#define AUTH_PASSWORD_MAX       64
#define AUTH_MAX_SESSIONS       8
#define AUTH_IDLE_MS            (30 * 60 * 1000)
#define AUTH_SESSION_MS         (12 * 60 * 60 * 1000)
#define AUTH_MAX_FAILURES       5
#define AUTH_LOCKOUT_MS         60000
#define AUTH_TRACKED_ADDRESSES  16

/* Choose the password (command line or random; needs random_initialize() done),
   register the auth routes and install the login check as the HTTP guard */
bool auth_start(void);

struct auth_stats
{
    uint32_t logins;
    uint32_t failures;                  /* wrong password, by session login or Basic */
    uint32_t lockouts;
    uint32_t locked_rejected;           /* requests refused while their address was locked */
    uint32_t csrf_rejected;
};

extern struct auth_stats auth_stats;

#endif /* __AUTH_H__ */
