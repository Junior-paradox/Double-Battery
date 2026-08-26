#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INF32 0xFFFFFFFFu

/* ---- monotonic timing (nanoseconds) ---- */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- deterministic PRNG (xorshift128+) ----
   Fixed seed => identical dataset on every run, every machine. */
typedef struct { uint64_t s[2]; } Rng;

static inline void rng_seed(Rng *r, uint64_t seed) {
    /* splitmix64 to spread the seed */
    for (int i = 0; i < 2; i++) {
        seed += 0x9E3779B97F4A7C15ull;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        r->s[i] = z ^ (z >> 31);
    }
}
static inline uint64_t rng_next(Rng *r) {
    uint64_t x = r->s[0], y = r->s[1];
    r->s[0] = y;
    x ^= x << 23;
    r->s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s[1] + y;
}
static inline uint32_t rng_u32(Rng *r, uint32_t bound) {
    return (uint32_t)((rng_next(r) >> 32) % bound);
}
static inline double rng_f64(Rng *r) {
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

static inline void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "OOM requesting %zu bytes\n", n); exit(1); }
    return p;
}
static inline void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "OOM requesting %zu bytes\n", n * sz); exit(1); }
    return p;
}

#endif
