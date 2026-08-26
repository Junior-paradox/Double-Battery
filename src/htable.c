#include "htable.h"

void htable_init(HospTable *t, uint32_t n_nodes, uint32_t n_hosp) {
    t->n_nodes = n_nodes; t->n_hosp = n_hosp;
    t->d = xmalloc(sizeof(uint32_t) * (size_t)n_nodes * n_hosp);
    t->generation = 0;
}
void htable_free(HospTable *t) { free(t->d); }
size_t htable_bytes(const HospTable *t) {
    return sizeof(uint32_t) * (size_t)t->n_nodes * t->n_hosp;
}

const char *reject_name(uint8_t r) {
    switch (r) {
        case REJ_NO_DEPT:     return "no such department";
        case REJ_NO_DOCTOR:   return "no specialist on duty";
        case REJ_NO_BED:      return "no free bed";
        case REJ_NO_MEDICINE: return "medicine batch depleted";
        case REJ_UNREACHABLE: return "no road route";
        case REJ_COSTLIER:    return "longer total cost";
        default:              return "eligible";
    }
}

/* One backward Dijkstra per hospital: sourced at the hospital over the
 * reverse graph, so the settled distance at node v is the travel time from v
 * TO that hospital -- the direction a patient actually travels. */
void htable_build(HospTable *t, const Graph *g, const World *w, Search *s) {
    const uint32_t H = t->n_hosp;
    for (uint32_t h = 0; h < H; h++) {
        search_reset(s);
        uint32_t src = w->hosp[h].node;
        srelax(s, src, 0, src);
        heap_push(s, 0, src);

        uint32_t *col = t->d + h;
        for (uint32_t v = 0; v < t->n_nodes; v++) col[(size_t)v * H] = INF32;

        while (s->hsize) {
            uint64_t top = heap_pop(s);
            uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
            if (d > sdist(s, u)) continue;
            col[(size_t)u * H] = d;
            for (uint32_t e = g->in_head[u]; e < g->in_head[u + 1]; e++) {
                Edge ed = g->in_e[e];
                if (ed.w == INF32) continue;
                uint32_t v = ed.to, nd = d + ed.w;
                if (nd < sdist(s, v)) { srelax(s, v, nd, u); heap_push(s, nd, v); }
            }
        }
    }
    t->generation++;
}

/* Keep the MAX_REJECT closest rejected alternatives, nearest first. */
static void note_reject(Decision *d, uint32_t hosp, uint32_t travel,
                        uint32_t wait, uint8_t reason) {
    uint32_t i = d->n_rejected;
    if (i < MAX_REJECT) d->n_rejected++;
    else if (travel >= d->rejected[MAX_REJECT - 1].travel_ms) return;
    else i = MAX_REJECT - 1;

    while (i > 0 && d->rejected[i - 1].travel_ms > travel) {
        d->rejected[i] = d->rejected[i - 1];
        i--;
    }
    d->rejected[i] = (Reject){ hosp, travel, wait, reason };
}

void dispatch_table(const Graph *g, Search *back, const World *w,
                    const HospTable *t, const Request *r, Decision *d) {
    uint64_t s0 = back->settled;

    uint32_t ta;
    uint32_t a = dispatch_nearest_ambulance(g, back, w, r, &ta);

    const uint32_t H = t->n_hosp;
    const uint32_t *row = t->d + (size_t)r->node * H;

    uint32_t best_h = INF32, best_total = INF32, best_tr = INF32, best_wt = 0;
    uint32_t considered = 0;

    /* pass 1 -- choose */
    for (uint32_t h = 0; h < H; h++) {
        uint32_t tr = row[h];
        if (tr == INF32) continue;
        considered++;
        if (hosp_reject_reason(&w->hosp[h], r) != REJ_NONE) continue;
        uint32_t wt = hosp_wait_ms(&w->hosp[h]);
        if (tr + wt < best_total) {
            best_total = tr + wt; best_h = h; best_tr = tr; best_wt = wt;
        }
    }

    d->amb = a; d->hosp = best_h;
    d->t_to_scene = ta; d->t_to_hosp = best_tr; d->wait_ms = best_wt;
    d->ok = (a != INF32 && best_h != INF32);
    d->t_total = d->ok ? ta + best_tr + best_wt : INF32;
    d->sla_met = d->ok && ta <= r->sla_ms;
    d->horizon_hit = (a == INF32) && r->max_reach_ms != 0;
    d->considered = considered;
    d->n_rejected = 0;

    /* pass 2 -- explain. Only hospitals CLOSER than the one chosen are worth
     * showing: those are the ones a human would otherwise ask about. */
    if (d->ok) {
        for (uint32_t h = 0; h < H; h++) {
            if (h == best_h) continue;
            uint32_t tr = row[h];
            if (tr == INF32 || tr >= best_tr) continue;
            uint8_t reason = hosp_reject_reason(&w->hosp[h], r);
            uint32_t wt = hosp_wait_ms(&w->hosp[h]);
            if (reason == REJ_NONE) reason = REJ_COSTLIER;   /* closer, but slower overall */
            note_reject(d, h, tr, wt, reason);
        }
    }
    d->settled = back->settled - s0;
}
