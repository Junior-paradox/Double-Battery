#ifndef SEARCH_H
#define SEARCH_H

#include "graph.h"

/* Per-thread search workspace. Allocated ONCE at startup; nothing inside the
 * query hot loop ever calls malloc.
 *
 * The dist[] array is never memset between queries. Instead each node carries
 * a generation stamp; a stale stamp means "distance is infinity". This turns
 * the per-query reset from O(V) into O(1), which is the difference between
 * ~50 us of pure memset and a sub-100 us total query at V=50k.
 */
/* Node state is one 16-byte struct, NOT three parallel arrays. A Dijkstra
 * relaxation touches dist+stamp+parent of the same node; as separate arrays
 * that is three independent random cache misses per touch, as one struct it
 * is one. Four nodes share a 64-byte cache line. */
typedef struct { uint32_t dist, stamp, parent, _pad; } NodeState;

typedef struct {
    uint32_t  n;
    NodeState *st;      /* n, 16B-aligned */
    uint32_t  gen;
    uint64_t *heap;     /* binary heap of (dist<<32 | node) */
    uint32_t  hsize, hcap;
    /* instrumentation */
    uint64_t  settled, pushed;
} Search;

static inline void search_init(Search *s, uint32_t n) {
    s->n = n;
    if (posix_memalign((void **)&s->st, 64, sizeof(NodeState) * n) != 0) {
        fprintf(stderr, "OOM\n"); exit(1);
    }
    memset(s->st, 0, sizeof(NodeState) * n);
    s->gen = 0;
    s->hcap = n + 16;
    s->heap = xmalloc(sizeof(uint64_t) * s->hcap);
    s->hsize = 0;
    s->settled = s->pushed = 0;
}
static inline void search_free(Search *s) { free(s->st); free(s->heap); }
static inline size_t search_bytes(const Search *s) {
    return (size_t)s->n * sizeof(NodeState) + (size_t)s->hcap * 8;
}

/* O(1) logical clear */
static inline void search_reset(Search *s) {
    s->hsize = 0;
    if (++s->gen == 0) { memset(s->st, 0, sizeof(NodeState) * s->n); s->gen = 1; }
}
static inline uint32_t sdist(const Search *s, uint32_t v) {
    return s->st[v].stamp == s->gen ? s->st[v].dist : INF32;
}
static inline void srelax(Search *s, uint32_t v, uint32_t d, uint32_t p) {
    s->st[v].dist = d; s->st[v].stamp = s->gen; s->st[v].parent = p;
}

/* ---- lazy-deletion binary heap: push O(log n), pop O(log n), no decrease-key
   bookkeeping and no position array to keep cache-hot ---- */
static inline void heap_push(Search *s, uint32_t d, uint32_t v) {
    if (s->hsize == s->hcap) {
        s->hcap *= 2;
        s->heap = realloc(s->heap, sizeof(uint64_t) * s->hcap);
    }
    uint64_t key = ((uint64_t)d << 32) | v;
    uint32_t i = s->hsize++;
    while (i) {
        uint32_t p = (i - 1) >> 1;
        if (s->heap[p] <= key) break;
        s->heap[i] = s->heap[p];
        i = p;
    }
    s->heap[i] = key;
    s->pushed++;
}
static inline uint64_t heap_pop(Search *s) {
    uint64_t top = s->heap[0];
    uint64_t last = s->heap[--s->hsize];
    if (s->hsize) {
        uint32_t i = 0;
        for (;;) {
            uint32_t l = 2 * i + 1, r = l + 1, m = i;
            uint64_t best = last;
            if (l < s->hsize && s->heap[l] < best) { m = l; best = s->heap[l]; }
            if (r < s->hsize && s->heap[r] < best) { m = r; best = s->heap[r]; }
            if (m == i) break;
            s->heap[i] = s->heap[m];
            i = m;
        }
        s->heap[i] = last;
    }
    return top;
}

#endif
