/*
    HTTP requests to the management server, each over its own TCP connection from
    the LAN host (the harness does the TCP part, so the fuzzer works on HTTP:
    parser, Host/Origin checks, login, sessions, CSRF, API handlers).

    Input: records of [chunk][length, 2 bytes little endian][request bytes]
      chunk      the request goes out in segments of (chunk % 32 + 1) * 16 bytes
    In a request, {SESSION} and {CSRF} are replaced by the last session cookie and
    CSRF token the router handed out in this input (after a successful login), so
    the fuzzer can reach the session paths. {AUTH} becomes the Basic credentials.
*/

#include "common.h"
#include <string.h>

#define FUZZ_CMDLINE "eth0=10.0.9.2/24 eth1=10.0.8.2/24 gw=10.0.8.1 nat=eth1 dhcpd=eth0,10.0.9.100,10.0.9.150 " \
                     "http admin_password=" FUZZ_PASSWORD

#define FUZZ_REQUEST_MAX 4096

static char fuzz$session[33], fuzz$csrf[33];

/* Copy the 32 hex characters after marker in text, if present */
static void fuzz$$token(const uint8_t* text, size_t len, const char* marker, char out[33])
{
    size_t m = strlen(marker);
    for (size_t i = 0; i + m + 32 <= len; i++) {
        if (memcmp(text + i, marker, m) == 0) {
            memcpy(out, text + i + m, 32);
            out[32] = '\0';
            return;
        }
    }
}

static size_t fuzz$$expand(const uint8_t* in, size_t len, uint8_t* out)
{
    static const struct { const char* name; const char* value; } vars[] = {
        { "{SESSION}", fuzz$session }, { "{CSRF}", fuzz$csrf },
        { "{AUTH}", "Basic YWRtaW46ZnV6ei1wYXNzd29yZA==" },     /* admin:fuzz-password */
    };
    size_t n = 0;
    for (size_t i = 0; i < len && n < FUZZ_REQUEST_MAX;) {
        bool replaced = false;
        for (size_t v = 0; v < sizeof(vars) / sizeof(vars[0]) && !replaced; v++) {
            size_t k = strlen(vars[v].name), vl = strlen(vars[v].value);
            if (i + k <= len && memcmp(in + i, vars[v].name, k) == 0 && n + vl <= FUZZ_REQUEST_MAX) {
                memcpy(out + n, vars[v].value, vl);
                n += vl;
                i += k;
                replaced = true;
            }
        }
        if (!replaced) {
            out[n++] = in[i++];
        }
    }
    return n;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fuzz_boot(FUZZ_CMDLINE);
    memset(fuzz$session, 0, sizeof(fuzz$session));
    memset(fuzz$csrf, 0, sizeof(fuzz$csrf));
    static struct fuzz_tcp_client c;
    static uint8_t request[FUZZ_REQUEST_MAX];
    size_t i = 0;
    for (uint16_t conn = 0; i + 3 <= size && conn < 64; conn++) {
        size_t chunk = ((size_t)data[i] % 32 + 1) * 16;
        size_t len = (size_t)data[i + 1] | (size_t)data[i + 2] << 8;
        i += 3;
        if (len > size - i) {
            len = size - i;
        }
        size_t n = fuzz$$expand(data + i, len, request);
        i += len;

        if (!fuzz_tcp_open(&c, (uint16_t)(40000 + conn), 80)) {
            fuzz_run_ms(1000);
            continue;
        }
        for (size_t off = 0; off < n; off += chunk) {
            fuzz_tcp_send(&c, FUZZ_TCP_ACK | FUZZ_TCP_PSH, request + off, n - off < chunk ? n - off : chunk);
            fuzz_tcp_pump(&c);
        }
        for (int t = 0; t < 30 && !c.fin; t++) {
            fuzz_run_ms(100);
            fuzz_tcp_pump(&c);
        }
        fuzz$$token(c.received, c.received_len, "lazydays_session=", fuzz$session);
        fuzz$$token(c.received, c.received_len, "\"csrf\":\"", fuzz$csrf);
        fuzz_tcp_send(&c, FUZZ_TCP_ACK | FUZZ_TCP_FIN, NULL, 0);
        fuzz_tcp_pump(&c);
    }
    fuzz_finish();
    return 0;
}
