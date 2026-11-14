#include "auth.h"
#include "http.h"
#include "net.h"
#include "events.h"
#include "../random.h"
#include "../cmdline.h"
#include "../heap.h"
#include <kio.h>
#include <pit.h>
#include <strbuf.h>

#define AUTH$COOKIE         "lazydays_session"
#define AUTH$TOKEN_HEX      32              /* 128-bit session and CSRF tokens */
#define AUTH$GENERATED_LEN  12              /* boot-time password: ~59 bits */
#define AUTH$MAX_LOCKOUT_SHIFT 4            /* lockout grows to 16 x AUTH_LOCKOUT_MS */

struct auth$session
{
    bool used;
    char token[AUTH$TOKEN_HEX + 1];
    char csrf[AUTH$TOKEN_HEX + 1];
    uint32_t created_ms;
    uint32_t last_ms;
};

struct auth$address
{
    uint32_t ip;                            /* 0 = unused */
    uint8_t failures;                       /* since the last success or lockout */
    uint8_t lockouts;                       /* since the last success: sets the lockout length */
    bool locked;
    uint32_t locked_until_ms;
    uint32_t last_ms;
};

enum auth$verdict { AUTH$OK, AUTH$WRONG, AUTH$LOCKED };

static char auth$password[AUTH_PASSWORD_MAX + 1];
static struct auth$session auth$sessions[AUTH_MAX_SESSIONS];
static struct auth$address auth$addresses[AUTH_TRACKED_ADDRESSES];

/* What the guard found, for the handler that runs right after it */
static struct auth$session* auth$current = NULL;
static bool auth$current_basic = false;

/* Extra response headers; http copies them before the next request can run */
static char auth$headers[192];

struct auth_stats auth_stats;

/* ---- helpers --------------------------------------------------------------- */

static size_t auth$$len(const char* s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* Compares secrets without stopping at the first difference */
static bool auth$$equal(const char* a, const char* b)
{
    uint8_t diff = 0;
    bool a_end = false, b_end = false;
    for (size_t i = 0; i <= AUTH_PASSWORD_MAX; i++) {
        char ca = a_end ? 0 : a[i];
        char cb = b_end ? 0 : b[i];
        diff |= (uint8_t)(ca ^ cb);
        a_end |= ca == 0;
        b_end |= cb == 0;
    }
    return diff == 0;
}

static void auth$$token(char out[AUTH$TOKEN_HEX + 1])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[AUTH$TOKEN_HEX / 2];
    random_bytes(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0xF];
        bytes[i] = 0;
    }
    out[AUTH$TOKEN_HEX] = '\0';
}

static void auth$$event(const char* text, uint32_t ip, const char* tail)
{
    char buf[EVENTS_TEXT_LEN];
    struct strbuf sb;
    sb_init(&sb, buf, sizeof(buf));
    sb_str(&sb, text);
    if (ip) {
        sb_ip(&sb, ip);
    }
    if (tail) {
        sb_str(&sb, tail);
    }
    events_add("auth", buf);
}

/* JSON body in a kmalloc'd copy (freed by http after sending), plus status and headers */
static void auth$$reply(struct http_response* res, int status, const char* json, const char* headers)
{
    size_t len = auth$$len(json);
    char* copy = kmalloc(len + 1);
    if (!copy) {
        http_respond_text(res, 500, "text/plain", "out of memory\n");
        return;
    }
    for (size_t i = 0; i <= len; i++) {
        copy[i] = json[i];
    }
    res->status = status;
    res->content_type = "application/json";
    res->body = (const uint8_t*)copy;
    res->body_len = len;
    res->owned = copy;
    res->extra_headers = headers;
}

static void auth$$login_required(struct http_response* res)
{
    auth$$reply(res, 401, "{\"error\":\"login required\"}", "WWW-Authenticate: Session realm=\"LazyDaysOS\"\r\n");
}

/* ---- guessing protection ---------------------------------------------------- */

static struct auth$address* auth$$address(uint32_t ip, bool create)
{
    /* Reuse a free entry, else the least recently seen unlocked one, else the least recently seen */
    struct auth$address* victim = NULL;
    for (int i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        struct auth$address* a = &auth$addresses[i];
        if (a->ip == ip) {
            return a;
        }
        if (!victim || victim->ip == 0) {
            victim = victim ? victim : a;
            continue;
        }
        if (a->ip == 0 || (victim->locked && !a->locked) ||
            (victim->locked == a->locked && (int32_t)(a->last_ms - victim->last_ms) < 0)) {
            victim = a;
        }
    }
    if (!create) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(*victim); i++) {
        ((uint8_t*)victim)[i] = 0;
    }
    victim->ip = ip;
    return victim;
}

/* Check a password attempt from ip; on AUTH$LOCKED the response is filled in (429) */
static enum auth$verdict auth$$check(uint32_t ip, bool user_ok, const char* password, struct http_response* res)
{
    uint32_t now = pit_uptime_ms();
    struct auth$address* a = auth$$address(ip, false);
    if (a && a->locked) {
        int32_t left = (int32_t)(a->locked_until_ms - now);
        if (left > 0) {
            auth_stats.locked_rejected++;
            uint32_t seconds = ((uint32_t)left + 999) / 1000;
            char body[96];
            struct strbuf sb;
            sb_init(&sb, body, sizeof(body));
            sb_str(&sb, "{\"error\":\"too many wrong passwords\",\"retry_after_s\":");
            sb_uint(&sb, seconds);
            sb_char(&sb, '}');
            sb_init(&sb, auth$headers, sizeof(auth$headers));
            sb_str(&sb, "Retry-After: ");
            sb_uint(&sb, seconds);
            sb_str(&sb, "\r\n");
            auth$$reply(res, 429, body, auth$headers);
            return AUTH$LOCKED;
        }
        a->locked = false;
    }

    bool ok = auth$$equal(password, auth$password) && user_ok;
    if (ok) {
        if (a) {
            a->failures = 0;
            a->lockouts = 0;
        }
        return AUTH$OK;
    }

    auth_stats.failures++;
    a = auth$$address(ip, true);
    a->last_ms = now;
    if (++a->failures >= AUTH_MAX_FAILURES) {
        uint32_t shift = a->lockouts < AUTH$MAX_LOCKOUT_SHIFT ? a->lockouts : AUTH$MAX_LOCKOUT_SHIFT;
        uint32_t duration = (uint32_t)AUTH_LOCKOUT_MS << shift;
        a->lockouts++;
        a->failures = 0;
        a->locked = true;
        a->locked_until_ms = now + duration;
        auth_stats.lockouts++;
        char tail[48];
        struct strbuf sb;
        sb_init(&sb, tail, sizeof(tail));
        sb_str(&sb, ", locked for ");
        sb_uint(&sb, duration / 1000);
        sb_str(&sb, " s");
        auth$$event("too many wrong passwords from ", ip, tail);
    }
    return AUTH$WRONG;
}

/* ---- sessions --------------------------------------------------------------- */

static void auth$$end(struct auth$session* s)
{
    for (size_t i = 0; i < sizeof(*s); i++) {
        ((uint8_t*)s)[i] = 0;
    }
}

/* The session named by the Cookie header, if it exists and hasn't expired */
static struct auth$session* auth$$session_find(const char* cookie, uint32_t now)
{
    static const char name[] = AUTH$COOKIE "=";
    const char* p = cookie;
    while (*p) {
        size_t n = 0;
        while (name[n] && p[n] == name[n]) {
            n++;
        }
        if (name[n] == '\0') {
            char token[AUTH$TOKEN_HEX + 1];
            size_t len = 0;
            p += n;
            while (p[len] && p[len] != ';' && len < AUTH$TOKEN_HEX) {
                token[len] = p[len];
                len++;
            }
            token[len] = '\0';
            if (len != AUTH$TOKEN_HEX || (p[len] && p[len] != ';')) {
                return NULL;
            }
            for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
                struct auth$session* s = &auth$sessions[i];
                if (s->used && auth$$equal(token, s->token)) {
                    if (now - s->last_ms > AUTH_IDLE_MS || now - s->created_ms > AUTH_SESSION_MS) {
                        auth$$end(s);
                        return NULL;
                    }
                    s->last_ms = now;
                    return s;
                }
            }
            return NULL;
        }
        while (*p && *p != ';') {
            p++;
        }
        while (*p == ';' || *p == ' ') {
            p++;
        }
    }
    return NULL;
}

static struct auth$session* auth$$session_new(uint32_t now)
{
    struct auth$session* s = &auth$sessions[0];
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        struct auth$session* c = &auth$sessions[i];
        if (!c->used) {
            s = c;
            break;
        }
        if ((int32_t)(c->last_ms - s->last_ms) < 0) {
            s = c;                          /* least recently used */
        }
    }
    s->used = true;
    s->created_ms = now;
    s->last_ms = now;
    auth$$token(s->token);
    auth$$token(s->csrf);
    return s;
}

/* ---- HTTP Basic (scripts) --------------------------------------------------- */

static int auth$$b64(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
           c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* Decode "Basic <base64 of user:password>"; false if malformed */
static bool auth$$basic(const char* header, char* out, size_t cap)
{
    const char* scheme = "basic ";
    for (size_t i = 0; scheme[i]; i++) {
        char c = header[i];
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != scheme[i]) {
            return false;
        }
    }
    const char* p = header + 6;
    uint32_t bits = 0;
    int nbits = 0;
    size_t len = 0;
    for (; *p && *p != '='; p++) {
        int v = auth$$b64(*p);
        if (v < 0) {
            return false;
        }
        bits = (bits << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (len + 1 >= cap) {
                return false;
            }
            out[len++] = (char)((bits >> nbits) & 0xFF);
        }
    }
    out[len] = '\0';
    return true;
}

/* ---- the guard: runs before every non-public route ---------------------------- */

static bool auth$$guard(const struct http_request* req, struct http_response* res)
{
    uint32_t now = pit_uptime_ms();
    auth$current = NULL;
    auth$current_basic = false;

    if (req->authorization) {
        char credentials[sizeof(AUTH_USER) + AUTH_PASSWORD_MAX + 1];
        if (!auth$$basic(req->authorization, credentials, sizeof(credentials))) {
            auth$$login_required(res);
            return false;
        }
        char* password = credentials;
        while (*password && *password != ':') {
            password++;
        }
        bool user_ok = *password == ':';
        if (user_ok) {
            *password++ = '\0';
            user_ok = auth$$equal(credentials, AUTH_USER);
        }
        enum auth$verdict verdict = auth$$check(req->remote_ip, user_ok, password, res);
        if (verdict == AUTH$WRONG) {
            auth$$login_required(res);
        }
        auth$current_basic = verdict == AUTH$OK;
        return auth$current_basic;
    }

    struct auth$session* s = req->cookie ? auth$$session_find(req->cookie, now) : NULL;
    if (!s) {
        auth$$login_required(res);
        return false;
    }
    /* A session rides along on any request the browser makes, so every state
       change must also prove it comes from our page: the CSRF token */
    bool post = req->method[0] == 'P';
    if (post && (!req->csrf_token || !auth$$equal(req->csrf_token, s->csrf))) {
        auth_stats.csrf_rejected++;
        auth$$reply(res, 403, "{\"error\":\"missing or wrong X-CSRF-Token\"}", NULL);
        return false;
    }
    auth$current = s;
    return true;
}

/* ---- routes ------------------------------------------------------------------- */

static void auth$$session_json(struct http_response* res, const char* csrf, const char* headers)
{
    char body[128];
    struct strbuf sb;
    sb_init(&sb, body, sizeof(body));
    sb_str(&sb, "{\"ok\":true,\"user\":\"" AUTH_USER "\",\"via\":");
    sb_str(&sb, csrf ? "\"session\",\"csrf\":\"" : "\"basic\",\"csrf\":null");
    if (csrf) {
        sb_str(&sb, csrf);
        sb_char(&sb, '"');
    }
    sb_char(&sb, '}');
    auth$$reply(res, 200, body, headers);
}

static void auth$$login(const struct http_request* req, struct http_response* res)
{
    char password[AUTH_PASSWORD_MAX + 1];
    if (!http_form_param(req->body, req->body_len, "password", password, sizeof(password))) {
        auth$$reply(res, 400, "{\"error\":\"send password=... (form encoded)\"}", NULL);
        return;
    }
    enum auth$verdict verdict = auth$$check(req->remote_ip, true, password, res);
    for (size_t i = 0; i < sizeof(password); i++) {
        password[i] = 0;
    }
    if (verdict == AUTH$LOCKED) {
        return;
    }
    if (verdict == AUTH$WRONG) {
        auth$$reply(res, 401, "{\"error\":\"wrong password\"}", "WWW-Authenticate: Session realm=\"LazyDaysOS\"\r\n");
        return;
    }
    struct auth$session* s = auth$$session_new(pit_uptime_ms());
    struct strbuf sb;
    sb_init(&sb, auth$headers, sizeof(auth$headers));
    sb_str(&sb, "Set-Cookie: " AUTH$COOKIE "=");
    sb_str(&sb, s->token);
    sb_str(&sb, "; Path=/; HttpOnly; SameSite=Strict\r\n");
    auth_stats.logins++;
    auth$$event("login from ", req->remote_ip, NULL);
    auth$$session_json(res, s->csrf, auth$headers);
}

static void auth$$logout(const struct http_request* req, struct http_response* res)
{
    (void)req;
    if (auth$current) {
        auth$$end(auth$current);
        auth$current = NULL;
    }
    auth$$reply(res, 200, "{\"ok\":true}",
                "Set-Cookie: " AUTH$COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");
}

static void auth$$session(const struct http_request* req, struct http_response* res)
{
    (void)req;
    auth$$session_json(res, auth$current ? auth$current->csrf : NULL, NULL);
}

static void auth$$change_password(const struct http_request* req, struct http_response* res)
{
    char current[AUTH_PASSWORD_MAX + 1], fresh[AUTH_PASSWORD_MAX + 1];
    if (!http_form_param(req->body, req->body_len, "current", current, sizeof(current)) ||
        !http_form_param(req->body, req->body_len, "new", fresh, sizeof(fresh))) {
        auth$$reply(res, 400, "{\"error\":\"send current=...&new=... (form encoded)\"}", NULL);
        return;
    }
    size_t len = auth$$len(fresh);
    bool printable = len >= AUTH_PASSWORD_MIN && len <= AUTH_PASSWORD_MAX;
    for (size_t i = 0; i < len; i++) {
        printable &= fresh[i] >= 0x20 && fresh[i] <= 0x7E;
    }
    enum auth$verdict verdict = auth$$check(req->remote_ip, true, current, res);
    if (verdict == AUTH$LOCKED) {
        return;
    }
    if (verdict == AUTH$WRONG) {
        auth$$reply(res, 403, "{\"error\":\"current password is wrong\"}", NULL);
        return;
    }
    if (!printable) {
        auth$$reply(res, 400, "{\"error\":\"the new password needs 8-64 printable ASCII characters\"}", NULL);
        return;
    }
    for (size_t i = 0; i <= len; i++) {
        auth$password[i] = fresh[i];
    }
    /* Everyone else logged in with the old password is logged out */
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (&auth$sessions[i] != auth$current) {
            auth$$end(&auth$sessions[i]);
        }
    }
    for (size_t i = 0; i < sizeof(fresh); i++) {
        fresh[i] = current[i] = 0;
    }
    auth$$event("admin password changed from ", req->remote_ip, NULL);
    auth$$reply(res, 200, "{\"ok\":true}", NULL);
}

/* ---- start-up ----------------------------------------------------------------- */

static void auth$$choose_password(void)
{
    if (cmdline_get("admin_password", 0, auth$password, sizeof(auth$password)) && auth$password[0]) {
        k_print("[K_INFO] auth: admin password set on the command line\n");
        return;
    }
    /* No shared default: a random password, shown only on the console */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";      /* no 0/o, 1/l/i */
    const uint32_t size = sizeof(alphabet) - 1;
    size_t len = 0;
    while (len < AUTH$GENERATED_LEN) {
        uint8_t b;
        random_bytes(&b, 1);
        if (b < 256 - 256 % size) {         /* rejection sampling: every character equally likely */
            auth$password[len++] = alphabet[b % size];
        }
    }
    auth$password[len] = '\0';
    k_print("[K_INFO] auth: admin password: %s (choose one with admin_password=)\n", auth$password);
}

bool auth_start(void)
{
    auth$$choose_password();
    http_set_guard(auth$$guard);
    return http_route("POST", "/api/login", auth$$login, HTTP_PUBLIC) &&
           http_route("POST", "/api/logout", auth$$logout, 0) &&
           http_route("GET", "/api/session", auth$$session, 0) &&
           http_route("POST", "/api/password", auth$$change_password, 0);
}
