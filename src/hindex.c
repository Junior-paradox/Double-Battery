#include "hindex.h"

void hindex_init(HospIndex *hi, uint32_t n) {
    hi->n_nodes = n;
    hi->layer = xmalloc(sizeof(uint64_t) * HI_SPECS * (size_t)n);
    hi->generation = 0;
}
void hindex_free(HospIndex *hi) { free(hi->layer); }
size_t hindex_bytes(const HospIndex *hi) {
    return sizeof(uint64_t) * HI_SPECS * (size_t)hi->n_nodes;
}

/* One multi-source backward Dijkstra. Every hospital carrying `spec` is a
 * source at distance 0; the source's identity rides along in the parent slot,
 * so when a node settles we know both how far the nearest such hospital is
 * and which one it is. Cost is a single sweep, not one per hospital. */
static void build_layer(HospIndex *hi, const Graph *g, const World *w,
                        Search *s, uint32_t spec) {
    search_reset(s);
    uint32_t bit = 1u << spec;
    for (uint32_t h = 0; h < w->n_hosp; h++) {
        if (!(w->hosp[h].spec_mask & bit)) continue;
        srelax(s, w->hosp[h].node, 0, h);
        heap_push(s, 0, w->hosp[h].node);
    }
    uint64_t *out = hi->layer + (size_t)spec * hi->n_nodes;
    for (uint32_t v = 0; v < hi->n_nodes; v++) out[v] = UINT64_MAX;

    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
        if (d > sdist(s, u)) continue;
        out[u] = ((uint64_t)d << 32) | s->st[u].parent;
        for (uint32_t e = g->in_head[u]; e < g->in_head[u + 1]; e++) {
            Edge ed = g->in_e[e];
            if (ed.w == INF32) continue;
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) {
                srelax(s, v, nd, s->st[u].parent);
                heap_push(s, nd, v);
            }
        }
    }
}

void hindex_build(HospIndex *hi, const Graph *g, const World *w, Search *s) {
    for (uint32_t spec = 0; spec < HI_SPECS; spec++)
        build_layer(hi, g, w, s, spec);
    hi->generation++;
}

void dispatch_indexed(const Graph *g, Search *back, Search *fwd,
                      const World *w, const HospIndex *hi,
                      const Request *r, Decision *d, uint32_t *fallbacks) {
    uint64_t s0 = back->settled + fwd->settled;

    /* Ambulance side always needs the live search: vehicles move. */
    uint32_t ta;
    uint32_t a = dispatch_nearest_ambulance(g, back, w, r, &ta);

    /* Hospital side: O(1) index read when the request names one specialty and
     * the indexed hospital still has a bed. Otherwise fall back, exactly. */
    uint32_t th = INF32, h = INF32;
    int single = (r->need_hosp & (r->need_hosp - 1)) == 0 && r->need_hosp != 0;
    if (single) {
        uint32_t spec = (uint32_t)__builtin_ctz(r->need_hosp);
        uint64_t cell = hi->layer[(size_t)spec * hi->n_nodes + r->node];
        if (cell != UINT64_MAX) {
            uint32_t cand = (uint32_t)cell;
            if (w->hosp[cand].beds_free > 0) {
                h = cand; th = (uint32_t)(cell >> 32);
            }
        }
        /* cell == UINT64_MAX means "unreachable at build time". Under live
         * road closures the index can be stale, so that is treated as a miss
         * and re-verified by search rather than trusted. */
    }
    if (h == INF32) {
        if (fallbacks) (*fallbacks)++;
        h = dispatch_nearest_hospital(g, fwd, w, r, &th);
    }
    d->amb = a; d->hosp = h;
    d->t_to_scene = ta; d->t_to_hosp = th;
    d->ok = (a != INF32 && h != INF32);
    d->t_total = d->ok ? ta + th : INF32;
    d->sla_met = d->ok && ta <= r->sla_ms;
    d->horizon_hit = !d->ok && r->max_reach_ms != 0;
    d->settled = (back->settled + fwd->settled) - s0;
}
