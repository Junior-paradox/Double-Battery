#ifndef HINDEX_H
#define HINDEX_H

#include "dispatch.h"

/* Hospital proximity index.
 *
 * 81% of a dispatch query's work was the forward search hunting for the
 * nearest hospital with the right specialty. That target set changes rarely
 * (hospitals do not move), so it can be precomputed.
 *
 * For each of the 8 specialties we run ONE multi-source backward Dijkstra
 * seeded from every hospital offering it. The result, for every node v, is
 * (travel time v -> nearest such hospital, that hospital's id). The runtime
 * query then becomes a single array read.
 *
 *   build : 8 x O(E log V), done once / on closure invalidation
 *   space : 8 * V * 8 bytes  (3.2 MB at V = 50k)
 *   query : O(1)
 *
 * Two cases fall back to the live search, so the answer is never wrong:
 *   - the indexed hospital has no free bed (state changed since build)
 *   - the request needs 2+ specialties (the per-specialty nearest need not
 *     carry the second one)
 */
#define HI_SPECS 8

typedef struct {
    uint32_t  n_nodes;
    uint64_t *layer;      /* HI_SPECS * n_nodes: (dist << 32) | hospital id */
    uint32_t  generation; /* bumped on rebuild; used to detect staleness */
} HospIndex;

void hindex_init(HospIndex *hi, uint32_t n_nodes);
void hindex_free(HospIndex *hi);
void hindex_build(HospIndex *hi, const Graph *g, const World *w, Search *scratch);
size_t hindex_bytes(const HospIndex *hi);

/* Index-accelerated dispatch. `fallbacks` is incremented when the index could
 * not answer and the live search was used instead. Result is identical to
 * dispatch_fast in every case. */
void dispatch_indexed(const Graph *g, Search *back, Search *fwd,
                      const World *w, const HospIndex *hi,
                      const Request *r, Decision *d, uint32_t *fallbacks);

#endif
