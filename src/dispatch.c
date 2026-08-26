#include "dispatch.h"

void world_build(World *w, const Graph *g, uint32_t n_hosp, uint32_t n_amb,
                 uint32_t n_village, uint64_t seed) {
    Rng rng; rng_seed(&rng, seed);
    uint32_t V = g->n_nodes;

    w->n_hosp = n_hosp; w->n_amb = n_amb; w->n_village = n_village;
    w->hosp = xmalloc(sizeof(Hospital) * n_hosp);
    w->amb  = xmalloc(sizeof(Ambulance) * n_amb);
    w->village = xmalloc(sizeof(uint32_t) * n_village);
    w->hosp_at  = xmalloc(sizeof(uint32_t) * V);
    w->amb_head = xmalloc(sizeof(uint32_t) * V);
    w->amb_next = xmalloc(sizeof(uint32_t) * n_amb);
    for (uint32_t v = 0; v < V; v++) { w->hosp_at[v] = INF32; w->amb_head[v] = INF32; }

    /* Hospitals on distinct nodes. Tier-1 centres carry every specialty and a
     * big bed pool; district hospitals carry a random subset. */
    for (uint32_t i = 0; i < n_hosp; i++) {
        uint32_t nd;
        do { nd = rng_u32(&rng, V); } while (w->hosp_at[nd] != INF32);
        w->hosp_at[nd] = i;
        int tier1 = (i % 6 == 0);
        uint32_t mask = tier1 ? 0xFFu
                              : (CAP_TRAUMA | (rng_next(&rng) & 0xFEu));
        int32_t beds = tier1 ? 40 + (int32_t)rng_u32(&rng, 60)
                             : 5  + (int32_t)rng_u32(&rng, 20);
        w->hosp[i] = (Hospital){ nd, mask, beds, beds };
    }

    for (uint32_t i = 0; i < n_amb; i++) {
        uint32_t caps = CAP_ALS;
        if (i % 3 == 0) caps |= CAP_VENTILATOR;
        if (i % 7 == 0) caps |= CAP_NEONATAL;
        if (i % 4 == 1) caps &= ~(uint32_t)CAP_ALS;   /* basic life support */
        w->amb[i] = (Ambulance){ 0, caps, 0 };
        world_place_ambulance(w, i, rng_u32(&rng, V));
    }

    for (uint32_t i = 0; i < n_village; i++) w->village[i] = rng_u32(&rng, V);
}

void world_place_ambulance(World *w, uint32_t a, uint32_t node) {
    w->amb[a].node = node;
    w->amb_next[a] = w->amb_head[node];
    w->amb_head[node] = a;
}

void world_reset_state(World *w, const Graph *g, uint64_t seed) {
    Rng rng; rng_seed(&rng, seed);
    for (uint32_t v = 0; v < g->n_nodes; v++) w->amb_head[v] = INF32;
    for (uint32_t i = 0; i < w->n_amb; i++) {
        w->amb[i].busy = 0;
        world_place_ambulance(w, i, rng_u32(&rng, g->n_nodes));
    }
    for (uint32_t i = 0; i < w->n_hosp; i++) w->hosp[i].beds_free = w->hosp[i].beds_total;
}

void world_free(World *w) {
    free(w->hosp); free(w->amb); free(w->village);
    free(w->hosp_at); free(w->amb_head); free(w->amb_next);
}
size_t world_bytes(const World *w) {
    return sizeof(Hospital) * w->n_hosp + sizeof(Ambulance) * w->n_amb
         + sizeof(uint32_t) * w->n_village + sizeof(uint32_t) * w->n_amb;
}

/* ------------------------------------------------------------------ */
/* backward early-exit Dijkstra: nearest FREE, CAPABLE ambulance       */
uint32_t dispatch_nearest_ambulance(const Graph *g, Search *s, const World *w,
                                  const Request *r, uint32_t *out_t) {
    const uint32_t horizon = r->max_reach_ms;
    search_reset(s);
    srelax(s, r->node, 0, r->node);
    heap_push(s, 0, r->node);

    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
        if (d > sdist(s, u)) continue;              /* stale heap entry */
        if (horizon && d > horizon) break;          /* nothing useful remains */
        s->settled++;

        for (uint32_t a = w->amb_head[u]; a != INF32; a = w->amb_next[a]) {
            if (w->amb[a].busy) continue;
            if ((w->amb[a].caps_mask & r->need_amb) != r->need_amb) continue;
            *out_t = d;
            return a;                                /* first settled = optimal */
        }
        for (uint32_t e = g->in_head[u]; e < g->in_head[u + 1]; e++) {
            Edge ed = g->in_e[e];
            if (ed.w == INF32) continue;             /* closed road */
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) {
                srelax(s, v, nd, u);
                heap_push(s, nd, v);
            }
        }
    }
    *out_t = INF32;
    return INF32;
}

/* forward early-exit Dijkstra: nearest hospital with the specialty AND a bed */
uint32_t dispatch_nearest_hospital(const Graph *g, Search *s, const World *w,
                                 const Request *r, uint32_t *out_t) {
    const uint32_t horizon = r->max_reach_ms;
    search_reset(s);
    srelax(s, r->node, 0, r->node);
    heap_push(s, 0, r->node);

    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
        if (d > sdist(s, u)) continue;
        if (horizon && d > horizon) break;
        s->settled++;

        uint32_t h = w->hosp_at[u];
        if (h != INF32 && w->hosp[h].beds_free > 0
            && (w->hosp[h].spec_mask & r->need_hosp) == r->need_hosp) {
            *out_t = d;
            return h;
        }
        for (uint32_t e = g->out_head[u]; e < g->out_head[u + 1]; e++) {
            Edge ed = g->out_e[e];
            if (ed.w == INF32) continue;
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) {
                srelax(s, v, nd, u);
                heap_push(s, nd, v);
            }
        }
    }
    *out_t = INF32;
    return INF32;
}

void dispatch_fast(const Graph *g, Search *back, Search *fwd,
                   const World *w, const Request *r, Decision *d) {
    uint64_t s0 = back->settled + fwd->settled;
    uint32_t ta, th;
    uint32_t a = dispatch_nearest_ambulance(g, back, w, r, &ta);
    uint32_t h = dispatch_nearest_hospital(g, fwd, w, r, &th);

    d->amb = a; d->hosp = h;
    d->t_to_scene = ta; d->t_to_hosp = th;
    d->ok = (a != INF32 && h != INF32);
    d->t_total = d->ok ? ta + th : INF32;
    d->sla_met = d->ok && ta <= r->sla_ms;
    d->horizon_hit = !d->ok && r->max_reach_ms != 0;
    d->settled = (back->settled + fwd->settled) - s0;
}

/* ---- Baseline A: A* per candidate ---- */
uint32_t dispatch_astar(const Graph *g, Search *s, uint32_t src, uint32_t dst) {
    search_reset(s);
    srelax(s, src, 0, src);
    heap_push(s, astar_h(g, src, dst), src);

    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t u = (uint32_t)top;
        uint32_t d = sdist(s, u);
        if ((uint32_t)(top >> 32) > d + astar_h(g, u, dst)) continue;
        s->settled++;
        if (u == dst) return d;
        for (uint32_t e = g->out_head[u]; e < g->out_head[u + 1]; e++) {
            Edge ed = g->out_e[e];
            if (ed.w == INF32) continue;
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) {
                srelax(s, v, nd, u);
                heap_push(s, nd + astar_h(g, v, dst), v);
            }
        }
    }
    return INF32;
}

void dispatch_naive_astar(const Graph *g, Search *s, const World *w,
                          const Request *r, Decision *d) {
    uint64_t s0 = s->settled;
    uint32_t best_a = INF32, best_ta = INF32;
    for (uint32_t i = 0; i < w->n_amb; i++) {
        if (w->amb[i].busy) continue;
        if ((w->amb[i].caps_mask & r->need_amb) != r->need_amb) continue;
        uint32_t t = dispatch_astar(g, s, w->amb[i].node, r->node);
        if (t < best_ta) { best_ta = t; best_a = i; }
    }
    uint32_t best_h = INF32, best_th = INF32;
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        if (w->hosp[i].beds_free <= 0) continue;
        if ((w->hosp[i].spec_mask & r->need_hosp) != r->need_hosp) continue;
        uint32_t t = dispatch_astar(g, s, r->node, w->hosp[i].node);
        if (t < best_th) { best_th = t; best_h = i; }
    }
    d->amb = best_a; d->hosp = best_h;
    d->t_to_scene = best_ta; d->t_to_hosp = best_th;
    d->ok = (best_a != INF32 && best_h != INF32);
    d->t_total = d->ok ? best_ta + best_th : INF32;
    d->sla_met = d->ok && best_ta <= r->sla_ms;
    d->horizon_hit = 0;
    d->settled = s->settled - s0;
}

/* ---- Baseline B: full Dijkstra, no early exit ---- */
static void full_dijkstra(const Graph *g, Search *s, uint32_t src, int reverse) {
    search_reset(s);
    srelax(s, src, 0, src);
    heap_push(s, 0, src);
    const uint32_t *head = reverse ? g->in_head : g->out_head;
    const Edge     *ee   = reverse ? g->in_e    : g->out_e;
    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
        if (d > sdist(s, u)) continue;
        s->settled++;
        for (uint32_t e = head[u]; e < head[u + 1]; e++) {
            Edge ed = ee[e];
            if (ed.w == INF32) continue;
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) {
                srelax(s, v, nd, u);
                heap_push(s, nd, v);
            }
        }
    }
}

void dispatch_full_dijkstra(const Graph *g, Search *back, Search *fwd,
                            const World *w, const Request *r, Decision *d) {
    uint64_t s0 = back->settled + fwd->settled;
    full_dijkstra(g, back, r->node, 1);
    full_dijkstra(g, fwd,  r->node, 0);

    uint32_t best_a = INF32, best_ta = INF32;
    for (uint32_t i = 0; i < w->n_amb; i++) {
        if (w->amb[i].busy) continue;
        if ((w->amb[i].caps_mask & r->need_amb) != r->need_amb) continue;
        uint32_t t = sdist(back, w->amb[i].node);
        if (t < best_ta) { best_ta = t; best_a = i; }
    }
    uint32_t best_h = INF32, best_th = INF32;
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        if (w->hosp[i].beds_free <= 0) continue;
        if ((w->hosp[i].spec_mask & r->need_hosp) != r->need_hosp) continue;
        uint32_t t = sdist(fwd, w->hosp[i].node);
        if (t < best_th) { best_th = t; best_h = i; }
    }
    d->amb = best_a; d->hosp = best_h;
    d->t_to_scene = best_ta; d->t_to_hosp = best_th;
    d->ok = (best_a != INF32 && best_h != INF32);
    d->t_total = d->ok ? best_ta + best_th : INF32;
    d->sla_met = d->ok && best_ta <= r->sla_ms;
    d->horizon_hit = 0;
    d->settled = (back->settled + fwd->settled) - s0;
}

void decision_commit(World *w, const Decision *d) {
    if (!d->ok) return;
    w->amb[d->amb].busy = 1;
    w->hosp[d->hosp].beds_free--;
}

/* Parent pointers point toward the search root, so walking them from any
 * settled node yields that node's shortest path. The root is its own parent,
 * which terminates the walk. */
uint32_t search_path(const Search *s, uint32_t from, uint32_t *out, uint32_t cap) {
    if (s->st[from].stamp != s->gen) return 0;
    uint32_t n = 0, v = from;
    for (;;) {
        if (n == cap) return 0;
        out[n++] = v;
        uint32_t p = s->st[v].parent;
        if (p == v) break;
        v = p;
    }
    return n;
}
