#ifndef __RANDOM_H__
#define __RANDOM_H__

#include <stddef.h>
#include <stdint.h>

/*
    Cryptographically secure random numbers: a ChaCha20 generator (RFC 8439
    block function) with fast key erasure. Every request runs the block
    function on the current key and a counter; the first 32 bytes of the
    output replace the key, the rest is handed out. A captured key therefore
    reveals nothing about earlier output.

    Entropy:
    - random_initialize() seeds the key from the architecture's sources
      (i386: RDRAND when the CPU has it, the time-stamp counter sampled around
      slow port I/O, the PIT's current count).
    - random_mix() folds cheap events into a 256-bit pool. The interrupt entry
      path mixes the cycle counter at every interrupt (timer, NICs, keyboard),
      whose low bits can't be observed from the network.
    - random_tick() folds the pool into the key once a second.

    Use it for anything an attacker must not predict: TCP initial sequence
    numbers, NAT ports, DHCP transaction ids, secrets.
*/

/* Seed from the architecture's entropy sources; call once during bring-up */
void random_initialize(void);

/* Fold a 32-bit event into the entropy pool. Cheap enough for every interrupt;
   safe to call from interrupt handlers. */
void random_mix(uint32_t event);

/* Every network tick: reseed the generator from the pool once a second */
void random_tick(uint32_t now_ms);

void random_bytes(void* out, size_t len);
uint32_t random32(void);

/* The RFC 8439 block function: 16 words of output for key, block counter and nonce */
void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint32_t out[16]);

/* RFC 8439 test vectors and generator sanity checks */
size_t random_self_test(void);

/*
    Architecture hooks (i386: kernel/compat/i386/entropy.c)
*/

/* Fast-changing cycle counter, for mixing event timing */
uint32_t random_arch_cycles(void);

/* Fill up to words words with seed material; returns how many were written */
size_t random_arch_seed(uint32_t* out, size_t words);

#endif /* __RANDOM_H__ */
