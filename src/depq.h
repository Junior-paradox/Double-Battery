#ifndef DEPQ_H
#define DEPQ_H

#include "common.h"

/* Double-ended priority queue (min-max heap) for the pending-request backlog.
 *
 * Key layout:  ((3 - urgency) << 32) | arrival_seq
 *   -> pop_min() = most urgent, oldest-first among equals  (who to dispatch
 *      when a vehicle frees up; a critical case preempts a queued minor one)
 *   -> pop_max() = least urgent, newest                    (who to shed or
 *      defer when the backlog exceeds capacity)
 * Both ends in O(log n), one flat array, no allocation while draining.
 */
typedef struct { uint64_t *a; uint32_t n, cap; } Depq;

static inline void depq_init(Depq *q, uint32_t cap) {
    q->a = xmalloc(sizeof(uint64_t) * cap); q->n = 0; q->cap = cap;
}
static inline void depq_free(Depq *q) { free(q->a); }
static inline uint64_t depq_key(uint32_t urgency, uint32_t seq) {
    return ((uint64_t)(3u - urgency) << 32) | seq;
}
static inline void dswap(Depq *q, uint32_t i, uint32_t j) {
    uint64_t t = q->a[i]; q->a[i] = q->a[j]; q->a[j] = t;
}
static inline int on_min_level(uint32_t i) {
    return ((31 - __builtin_clz(i + 1)) & 1) == 0;
}

static inline void sift_up_min(Depq *q, uint32_t i) {
    while (i > 2) { uint32_t gp = (((i - 1) >> 1) - 1) >> 1;
        if (q->a[i] < q->a[gp]) { dswap(q, i, gp); i = gp; } else break; }
}
static inline void sift_up_max(Depq *q, uint32_t i) {
    while (i > 2) { uint32_t gp = (((i - 1) >> 1) - 1) >> 1;
        if (q->a[i] > q->a[gp]) { dswap(q, i, gp); i = gp; } else break; }
}
static inline void depq_push(Depq *q, uint64_t key) {
    if (q->n == q->cap) { q->cap *= 2; q->a = realloc(q->a, sizeof(uint64_t) * q->cap); }
    uint32_t i = q->n++;
    q->a[i] = key;
    if (i == 0) return;
    uint32_t p = (i - 1) >> 1;
    if (on_min_level(i)) {
        if (q->a[i] > q->a[p]) { dswap(q, i, p); sift_up_max(q, p); }
        else sift_up_min(q, i);
    } else {
        if (q->a[i] < q->a[p]) { dswap(q, i, p); sift_up_min(q, p); }
        else sift_up_max(q, i);
    }
}

#define DEPQ_TRICKLE(NAME, CMP)                                               \
static inline void NAME(Depq *q, uint32_t i) {                                \
    for (;;) {                                                                \
        uint32_t m = UINT32_MAX; int gc = 0;                                   \
        uint32_t c1 = 2 * i + 1, c2 = c1 + 1;                                  \
        for (uint32_t c = c1; c <= c2 && c < q->n; c++)                        \
            if (m == UINT32_MAX || CMP(q->a[c], q->a[m])) { m = c; gc = 0; }   \
        for (uint32_t c = c1; c <= c2 && c < q->n; c++)                        \
            for (uint32_t h = 2 * c + 1; h <= 2 * c + 2 && h < q->n; h++)      \
                if (m == UINT32_MAX || CMP(q->a[h], q->a[m])) { m = h; gc = 1; }\
        if (m == UINT32_MAX) return;                                           \
        if (gc) {                                                              \
            if (CMP(q->a[m], q->a[i])) {                                       \
                dswap(q, m, i);                                                \
                uint32_t pm = (m - 1) >> 1;                                    \
                if (CMP(q->a[pm], q->a[m])) dswap(q, m, pm);                   \
                i = m; continue;                                               \
            }                                                                  \
            return;                                                            \
        }                                                                      \
        if (CMP(q->a[m], q->a[i])) dswap(q, m, i);                             \
        return;                                                                \
    }                                                                          \
}
#define LT(a, b) ((a) < (b))
#define GT(a, b) ((a) > (b))
DEPQ_TRICKLE(trickle_min, LT)
DEPQ_TRICKLE(trickle_max, GT)

static inline uint64_t depq_pop_min(Depq *q) {          /* most urgent */
    uint64_t r = q->a[0];
    q->a[0] = q->a[--q->n];
    if (q->n) trickle_min(q, 0);
    return r;
}
static inline uint32_t depq_max_idx(const Depq *q) {
    if (q->n <= 1) return 0;
    if (q->n == 2) return 1;
    return q->a[1] > q->a[2] ? 1 : 2;
}
static inline uint64_t depq_pop_max(Depq *q) {          /* least urgent */
    uint32_t i = depq_max_idx(q);
    uint64_t r = q->a[i];
    q->a[i] = q->a[--q->n];
    if (q->n > i) trickle_max(q, i);
    return r;
}
static inline uint32_t depq_seq(uint64_t k)     { return (uint32_t)k; }
static inline uint32_t depq_urgency(uint64_t k) { return 3u - (uint32_t)(k >> 32); }

#endif
