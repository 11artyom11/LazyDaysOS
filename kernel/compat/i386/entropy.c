#include "../../random.h"
#include <stdbool.h>
#include "../../hwio.h"

/*
    i386 entropy sources for kernel/random.c:
    - RDRAND, when CPUID says the CPU has it (KVM with a modern host CPU; not
      QEMU's default TCG CPU)
    - the time-stamp counter around port I/O to the PIT: each access is slow
      and its duration varies with bus timing (and, in a VM, with every exit to
      the hypervisor), so the low bits of the TSC deltas are hard to predict
    - the PIT channel 0 count itself, whose phase against the TSC drifts
    None of this is credited or measured; the generator relies on the sum of
    all sources plus the interrupt timing mixed in afterwards.
*/

#define PIT$CHANNEL0    0x40
#define PIT$COMMAND     0x43
#define PIT$LATCH_CH0   0x00
#define CPUID1_ECX_RDRAND (1u << 30)

static inline uint64_t entropy$$rdtsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint32_t random_arch_cycles(void)
{
    return (uint32_t)entropy$$rdtsc();
}

static bool entropy$$has_rdrand(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (ecx & CPUID1_ECX_RDRAND) != 0;
}

static bool entropy$$rdrand(uint32_t* value)
{
    for (int attempt = 0; attempt < 10; attempt++) {
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok) :: "cc");
        if (ok) {
            return true;
        }
    }
    return false;
}

/* Latch and read the PIT's current count; returns it with the TSC cycles the access took */
static uint32_t entropy$$pit_sample(void)
{
    uint64_t start = entropy$$rdtsc();
    outb(PIT$COMMAND, PIT$LATCH_CH0);
    uint32_t count = inb(PIT$CHANNEL0);
    count |= (uint32_t)inb(PIT$CHANNEL0) << 8;
    uint32_t took = (uint32_t)(entropy$$rdtsc() - start);
    return (took << 16) ^ (took >> 16) ^ count;
}

size_t random_arch_seed(uint32_t* out, size_t words)
{
    size_t n = 0;
    bool rdrand = entropy$$has_rdrand();
    while (n < words) {
        uint32_t word = random_arch_cycles();
        uint32_t hw;
        if (rdrand && entropy$$rdrand(&hw)) {
            word ^= hw;
        }
        /* Several PIT samples per word: each contributes a few unpredictable low bits */
        for (int i = 0; i < 8; i++) {
            word = ((word << 5) | (word >> 27)) ^ entropy$$pit_sample();
        }
        out[n++] = word;
    }
    return n;
}
