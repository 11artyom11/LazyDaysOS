#include "random.h"
#include <stdbool.h>
#include <kio.h>
#include <util.h>

#define RANDOM$RESEED_MS    1000
#define RANDOM$SEED_WORDS   64

static uint32_t random$key[8];
static uint32_t random$counter = 0;
static uint32_t random$nonce[3] = { 0x4c617a79, 0x44617973, 0x4f532121 };   /* "LazyDaysOS!!": fixed domain */

/* The pool is written from interrupt handlers and read by random_tick; a torn
   read only changes which bits get folded in, never makes the key weaker */
static volatile uint32_t random$pool[8];
static volatile uint32_t random$pool_pos = 0;
static uint32_t random$last_reseed_ms = 0;

static inline uint32_t random$$rotl(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

#define QUARTER(a, b, c, d) \
    a += b; d ^= a; d = random$$rotl(d, 16); \
    c += d; b ^= c; b = random$$rotl(b, 12); \
    a += b; d ^= a; d = random$$rotl(d, 8);  \
    c += d; b ^= c; b = random$$rotl(b, 7);

void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint32_t out[16])
{
    uint32_t in[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,     /* "expand 32-byte k" */
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = in[i];
    }
    for (int round = 0; round < 10; round++) {
        QUARTER(x[0], x[4], x[8], x[12]);
        QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]);
        QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]);
        QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]);
        QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        out[i] = x[i] + in[i];
    }
}

/* One block: the first 8 words become the new key (fast key erasure), the other 8 are output */
static void random$$generate(uint32_t out[8])
{
    uint32_t block[16];
    chacha20_block(random$key, random$counter++, random$nonce, block);
    for (int i = 0; i < 8; i++) {
        random$key[i] = block[i];
        out[i] = block[8 + i];
        block[i] = 0;
    }
}

/* Fold words into the key: xor them in, then run the block function so every
   input bit affects every key bit */
static void random$$absorb(const uint32_t* words, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        random$key[i % 8] ^= words[i];
        if (i % 8 == 7 || i + 1 == count) {
            uint32_t discard[8];
            random$$generate(discard);
        }
    }
}

void random_mix(uint32_t event)
{
    uint32_t pos = random$pool_pos;
    uint32_t i = pos & 7;
    random$pool[i] = random$$rotl(random$pool[i] ^ (event * 0x9E3779B1u), 7) + random$pool[(i + 1) & 7];
    random$pool_pos = pos + 1;
}

static void random$$reseed(void)
{
    uint32_t snapshot[9];
    for (int i = 0; i < 8; i++) {
        snapshot[i] = random$pool[i];
    }
    snapshot[8] = random_arch_cycles();
    random$$absorb(snapshot, 9);
}

void random_initialize(void)
{
    uint32_t seed[RANDOM$SEED_WORDS];
    size_t n = random_arch_seed(seed, RANDOM$SEED_WORDS);
    random$$absorb(seed, n);
    for (size_t i = 0; i < n; i++) {
        seed[i] = 0;
    }
    k_print("[K_INFO] random: seeded from %u words of entropy sources\n", (uint32_t)n);
}

void random_tick(uint32_t now_ms)
{
    if (now_ms - random$last_reseed_ms >= RANDOM$RESEED_MS) {
        random$last_reseed_ms = now_ms;
        random$$reseed();
    }
}

void random_bytes(void* out, size_t len)
{
    uint8_t* dst = out;
    while (len > 0) {
        uint32_t words[8];
        random$$generate(words);
        const uint8_t* src = (const uint8_t*)words;
        size_t n = len < sizeof(words) ? len : sizeof(words);
        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
        for (int i = 0; i < 8; i++) {
            words[i] = 0;
        }
        dst += n;
        len -= n;
    }
}

uint32_t random32(void)
{
    uint32_t value;
    random_bytes(&value, sizeof(value));
    return value;
}

size_t random_self_test(void)
{
    /* RFC 8439 2.3.2: key 00 01 .. 1f, nonce 00:00:00:09 00:00:00:4a 00:00:00:00, counter 1 */
    static const uint32_t expected[16] = {
        0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3, 0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
        0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9, 0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2,
    };
    /* RFC 8439 A.1 test vector #1: all-zero key, nonce and counter */
    static const uint32_t expected_zero[4] = { 0xade0b876, 0x903df1a0, 0xe56a5d40, 0x28bd8653 };
    uint32_t key[8], out[16];
    for (int i = 0; i < 8; i++) {
        key[i] = (uint32_t)(4 * i) | (uint32_t)(4 * i + 1) << 8 | (uint32_t)(4 * i + 2) << 16 | (uint32_t)(4 * i + 3) << 24;
    }
    const uint32_t nonce[3] = { 0x09000000, 0x4a000000, 0x00000000 };
    chacha20_block(key, 1, nonce, out);
    for (int i = 0; i < 16; i++) {
        if (out[i] != expected[i]) {
            k_print("[UTIL_FAIL] chacha20 RFC 8439 2.3.2 word %u: got 0x%x, want 0x%x\n", (uint32_t)i, out[i], expected[i]);
            return UTIL$FAIL;
        }
    }
    const uint32_t zero_key[8] = { 0 }, zero_nonce[3] = { 0 };
    chacha20_block(zero_key, 0, zero_nonce, out);
    for (int i = 0; i < 4; i++) {
        if (out[i] != expected_zero[i]) {
            k_print("[UTIL_FAIL] chacha20 RFC 8439 A.1 word %u: got 0x%x, want 0x%x\n", (uint32_t)i, out[i], expected_zero[i]);
            return UTIL$FAIL;
        }
    }

    /* The generator must not repeat itself or stick at one value, and the key must change */
    uint32_t key_before[8];
    for (int i = 0; i < 8; i++) {
        key_before[i] = random$key[i];
    }
    uint32_t a = random32(), b = random32(), c = random32();
    bool key_changed = false;
    for (int i = 0; i < 8; i++) {
        key_changed |= key_before[i] != random$key[i];
    }
    if (a == b || b == c || a == c || !key_changed) {
        k_print("[UTIL_FAIL] random: outputs 0x%x 0x%x 0x%x, key changed %u\n", a, b, c, (uint32_t)key_changed);
        return UTIL$FAIL;
    }

    /* Roughly balanced bits over 4 KiB of output (expected 16384 ones; 6 sigma is about 550) */
    uint32_t ones = 0;
    for (int i = 0; i < 1024; i++) {
        uint32_t v = random32();
        while (v) {
            ones += v & 1;
            v >>= 1;
        }
    }
    if (ones < 16384 - 550 || ones > 16384 + 550) {
        k_print("[UTIL_FAIL] random: %u one bits in 32768, expected about 16384\n", ones);
        return UTIL$FAIL;
    }
    return UTIL$SUCCESS;
}
