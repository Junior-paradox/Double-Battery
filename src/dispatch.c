#include "dispatch.h"
#include "city_data.h"

/* ------------------------------------------------------------------ */
/* real city roster                                                    */

uint32_t city_count(void) { return (uint32_t)N_CITY; }
const char *city_data_version(void) { return CITY_DATA_VERSION; }
const char *city_data_source(void)  { return CITY_DATA_SOURCE; }

const CityInfo *city_info(uint32_t c) {
    return c < N_CITY ? &CITY_LIST[c] : NULL;
}
const CityHospital *city_hospital(uint32_t c, uint32_t k) {
    if (c >= N_CITY || k >= CITY_LIST[c].count) return NULL;
    return &CITY_HOSP[CITY_LIST[c].first + k];
}

uint32_t city_emergency_beds(uint32_t reported) {
    uint32_t pool = reported / 12;
    if (pool < 4)   pool = 4;      /* even a nursing home holds a few */
    if (pool > 200) pool = 200;
    return pool;
}

/* ------------------------------------------------------------------ */
/* world construction                                                  */

static int in_shift(const Doctor *d, uint32_t t) {
    return d->shift_start <= d->shift_end
         ? (t >= d->shift_start && t < d->shift_end)          /* normal */
         : (t >= d->shift_start || t < d->shift_end);         /* wraps midnight */
}

void world_set_clock(World *w, uint32_t t_ms) {
    w->clock_ms = t_ms % DAY_MS;
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        w->hosp[i].on_duty_mask = 0;
        w->hosp[i].docs_on_duty = 0;
    }
    for (uint32_t i = 0; i < w->n_doc; i++) {
        const Doctor *d = &w->doc[i];
        if (!in_shift(d, w->clock_ms)) continue;
        w->hosp[d->hosp].on_duty_mask |= 1u << d->spec;
        w->hosp[d->hosp].docs_on_duty++;
    }
}

void world_build(World *w, const Graph *g, uint32_t n_hosp, uint32_t n_amb,
                 uint32_t n_village, uint32_t docs_per_hosp, uint64_t seed) {
    Rng rng; rng_seed(&rng, seed);
    uint32_t V = g->n_nodes;

    w->n_hosp = n_hosp; w->n_amb = n_amb; w->n_village = n_village;
    w->hosp = xcalloc(n_hosp, sizeof(Hospital));
    w->amb  = xmalloc(sizeof(Ambulance) * n_amb);
    w->village = xmalloc(sizeof(uint32_t) * n_village);
    w->hosp_at  = xmalloc(sizeof(uint32_t) * V);
    w->amb_head = xmalloc(sizeof(uint32_t) * V);
    w->amb_next = xmalloc(sizeof(uint32_t) * n_amb);
    for (uint32_t v = 0; v < V; v++) { w->hosp_at[v] = INF32; w->amb_head[v] = INF32; }

    /* Tier-1 referral centres carry every department and a deep bed pool;
     * district hospitals carry a subset. */
    for (uint32_t i = 0; i < n_hosp; i++) {
        uint32_t nd;
        do { nd = rng_u32(&rng, V); } while (w->hosp_at[nd] != INF32);
        w->hosp_at[nd] = i;
        int tier1 = (i % 6 == 0);
        w->hosp[i].node = nd;
        w->hosp[i].spec_mask = tier1 ? 0xFFu : (CAP_TRAUMA | (rng_next(&rng) & 0xFEu));
        int32_t beds = tier1 ? 40 + (int32_t)rng_u32(&rng, 60)
                             : 5  + (int32_t)rng_u32(&rng, 20);
        w->hosp[i].beds_free = w->hosp[i].beds_total = beds;
        for (uint32_t m = 0; m < N_MED; m++) {
            int32_t cap = tier1 ? 60 + (int32_t)rng_u32(&rng, 120)
                                : 10 + (int32_t)rng_u32(&rng, 40);
            w->hosp[i].med_cap[m] = cap;
            w->hosp[i].med[m] = cap;
        }
    }

    /* Doctors on three 8-hour shifts. A department with only one doctor is
     * therefore uncovered two thirds of the day -- which is precisely the
     * scarcity the problem is about. */
    uint32_t total_docs = 0;
    for (uint32_t i = 0; i < n_hosp; i++)
        total_docs += (i % 6 == 0) ? docs_per_hosp * 2 : docs_per_hosp;
    w->doc = xmalloc(sizeof(Doctor) * total_docs);
    w->n_doc = 0;
    for (uint32_t i = 0; i < n_hosp; i++) {
        uint32_t have = w->hosp[i].spec_mask;
        uint32_t count = (i % 6 == 0) ? docs_per_hosp * 2 : docs_per_hosp;
        for (uint32_t k = 0; k < count; k++) {
            /* pick a specialty the hospital actually has a department for */
            uint32_t spec, guard = 0;
            do { spec = rng_u32(&rng, N_SPEC); }
            while (!((have >> spec) & 1u) && ++guard < 32);
            if (!((have >> spec) & 1u)) continue;
            uint32_t shift = rng_u32(&rng, 3);
            w->doc[w->n_doc++] = (Doctor){ i, (uint8_t)spec,
                shift * 8u * 3600000u, ((shift + 1u) % 3u) * 8u * 3600000u };
        }
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

    w->city = -1;
    world_set_clock(w, 10u * 3600000u);          /* 10:00, day shift */
}

/* Metro hospitals are not scattered uniformly: the big tertiary centres sit
 * in the old core and the smaller ones ring the outskirts. The dataset has no
 * coordinates, so rank stands in for one -- sample a handful of free
 * junctions and keep whichever sits nearest a radius derived from the
 * hospital's size rank. Deterministic given the seed. */
static uint32_t place_by_rank(const Graph *g, const World *w, Rng *rng,
                              double cx, double cy, double radius,
                              uint32_t rank, uint32_t of) {
    double want = radius * (0.10 + 0.80 * (of > 1 ? (double)rank / (of - 1) : 0.0));
    uint32_t best = INF32;
    double best_err = 0;
    for (uint32_t try = 0; try < 32; try++) {
        uint32_t nd = rng_u32(rng, g->n_nodes);
        if (w->hosp_at[nd] != INF32) continue;
        double dx = g->x[nd] - cx, dy = g->y[nd] - cy;
        double err = __builtin_fabs(__builtin_sqrt(dx * dx + dy * dy) - want);
        if (best == INF32 || err < best_err) { best = nd; best_err = err; }
    }
    /* Every candidate was already taken -- fall back to a linear probe so the
     * build cannot fail on an unlucky draw. */
    if (best == INF32)
        for (uint32_t nd = rng_u32(rng, g->n_nodes), k = 0; k < g->n_nodes; k++)
            if (w->hosp_at[(nd + k) % g->n_nodes] == INF32)
                { best = (nd + k) % g->n_nodes; break; }
    return best;
}

int world_build_city(World *w, const Graph *g, uint32_t city,
                     uint32_t n_village, uint64_t seed) {
    /* An empty roster would divide by zero when parking the fleet. The
     * generator cannot emit one today; this is here so that a regenerated
     * table with a different size threshold fails a build rather than a
     * dispatch. */
    const CityInfo *ci = city_info(city);
    if (!ci || !ci->count) return 0;

    Rng rng; rng_seed(&rng, seed ^ ((uint64_t)city << 32));
    uint32_t V = g->n_nodes, H = ci->count;

    /* A metro fields a bigger fleet than a rural district, and a 30-hospital
     * city a bigger one than an 8-hospital city. The floor matters more than
     * it looks: most of these vehicles wait at hospitals, hospitals cluster in
     * the core, and a fleet sized only for the roster leaves the edge of the
     * map with nothing inside the response horizon -- which reads as the
     * algorithm failing when it is really the fleet being too small. */
    uint32_t n_amb = H * 8;
    if (n_amb < 160) n_amb = 160;
    if (n_amb > 320) n_amb = 320;

    w->n_hosp = H; w->n_amb = n_amb; w->n_village = n_village;
    w->city = (int32_t)city;
    w->hosp = xcalloc(H, sizeof(Hospital));
    w->amb  = xmalloc(sizeof(Ambulance) * n_amb);
    w->village = xmalloc(sizeof(uint32_t) * n_village);
    w->hosp_at  = xmalloc(sizeof(uint32_t) * V);
    w->amb_head = xmalloc(sizeof(uint32_t) * V);
    w->amb_next = xmalloc(sizeof(uint32_t) * n_amb);
    for (uint32_t v = 0; v < V; v++) { w->hosp_at[v] = INF32; w->amb_head[v] = INF32; }

    double x0 = g->x[0], x1 = g->x[0], y0 = g->y[0], y1 = g->y[0];
    for (uint32_t v = 1; v < V; v++) {
        if (g->x[v] < x0) x0 = g->x[v];
        if (g->x[v] > x1) x1 = g->x[v];
        if (g->y[v] < y0) y0 = g->y[v];
        if (g->y[v] > y1) y1 = g->y[v];
    }
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double radius = 0.5 * ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0));

    /* Records arrive sorted by bed count, so index 0 is the city's referral
     * centre. Departments, beds and accreditation are the dataset's; the
     * medicine store is invented in proportion to size. */
    uint32_t total_docs = 0;
    for (uint32_t i = 0; i < H; i++) {
        const CityHospital *ch = city_hospital(city, i);
        uint32_t nd = place_by_rank(g, w, &rng, cx, cy, radius, i, H);
        w->hosp_at[nd] = i;
        w->hosp[i].node = nd;
        w->hosp[i].spec_mask = ch->caps;
        int32_t beds = (int32_t)city_emergency_beds(ch->beds);
        w->hosp[i].beds_free = w->hosp[i].beds_total = beds;
        for (uint32_t m = 0; m < N_MED; m++) {
            int32_t cap = 24 + (int32_t)(ch->beds / 8);
            if (cap > 400) cap = 400;
            w->hosp[i].med_cap[m] = cap;
            w->hosp[i].med[m] = cap;
        }
        uint32_t depts = (uint32_t)__builtin_popcount(ch->caps);
        uint32_t docs = 3 + ch->beds / 100;
        if (docs < depts) docs = depts;      /* a department with no doctor is never open */
        if (docs > 24) docs = 24;
        w->hosp[i].docs_on_duty = 0;
        total_docs += depts ? docs : 0;
    }

    /* Every department gets one doctor first, spread across the three shifts,
     * then the remaining headcount is dealt out at random. A small hospital
     * therefore covers each of its departments on exactly one shift in three
     * -- which is why a 2 a.m. cardiac call in this city drives past four
     * hospitals that do have a cardiology wing. */
    w->doc = xmalloc(sizeof(Doctor) * (total_docs ? total_docs : 1));
    w->n_doc = 0;
    for (uint32_t i = 0; i < H; i++) {
        uint32_t have = w->hosp[i].spec_mask;
        if (!have) continue;                 /* single-discipline centre, takes no emergencies */
        uint32_t depts = (uint32_t)__builtin_popcount(have);
        uint32_t docs = 3 + city_hospital(city, i)->beds / 100;
        if (docs < depts) docs = depts;
        if (docs > 24) docs = 24;

        uint32_t placed = 0, shift = rng_u32(&rng, 3);
        for (uint32_t s = 0; s < N_SPEC && placed < docs; s++) {
            if (!((have >> s) & 1u)) continue;
            w->doc[w->n_doc++] = (Doctor){ i, (uint8_t)s,
                shift * 8u * 3600000u, ((shift + 1u) % 3u) * 8u * 3600000u };
            shift = (shift + 1) % 3;
            placed++;
        }
        for (; placed < docs; placed++) {
            uint32_t spec, guard = 0;
            do { spec = rng_u32(&rng, N_SPEC); }
            while (!((have >> spec) & 1u) && ++guard < 32);
            if (!((have >> spec) & 1u)) continue;
            uint32_t sh = rng_u32(&rng, 3);
            w->doc[w->n_doc++] = (Doctor){ i, (uint8_t)spec,
                sh * 8u * 3600000u, ((sh + 1u) % 3u) * 8u * 3600000u };
        }
    }

    /* City ambulances are hospital-based, but not all of them: some wait at a
     * hospital and the rest hold standby points across the network, which is
     * what keeps the outer wards inside a response horizon at all.
     *
     * The parking rule must not share a period with the equipment rules. It
     * did -- both were `i % 3` -- and the result was that every single
     * ventilator-equipped vehicle sat at a hospital, hospitals cluster in the
     * core, and two thirds of the map could not be reached by the one kind of
     * ambulance a cardiac arrest needs. 5 is coprime with both 3 and 4, so
     * equipment and parking are now independent. */
    for (uint32_t i = 0; i < n_amb; i++) {
        uint32_t caps = CAP_ALS;
        if (i % 3 == 0) caps |= CAP_VENTILATOR;
        if (i % 7 == 0) caps |= CAP_NEONATAL;
        if (i % 4 == 1) caps &= ~(uint32_t)CAP_ALS;
        w->amb[i] = (Ambulance){ 0, caps, 0 };
        uint32_t node = (i % 5 == 0) ? w->hosp[i % H].node : rng_u32(&rng, V);
        world_place_ambulance(w, i, node);
    }
    for (uint32_t i = 0; i < n_village; i++) w->village[i] = rng_u32(&rng, V);

    world_set_clock(w, 10u * 3600000u);
    return 1;
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
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        w->hosp[i].beds_free = w->hosp[i].beds_total;
        w->hosp[i].queue_len = 0;
        for (uint32_t m = 0; m < N_MED; m++) w->hosp[i].med[m] = w->hosp[i].med_cap[m];
    }
    world_set_clock(w, w->clock_ms);
}

void world_free(World *w) {
    free(w->hosp); free(w->amb); free(w->doc); free(w->village);
    free(w->hosp_at); free(w->amb_head); free(w->amb_next);
}
size_t world_bytes(const World *w) {
    return sizeof(Hospital) * w->n_hosp + sizeof(Ambulance) * w->n_amb
         + sizeof(Doctor) * w->n_doc
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
            if (nd < sdist(s, v)) { srelax(s, v, nd, u); heap_push(s, nd, v); }
        }
    }
    *out_t = INF32;
    return INF32;
}

/* forward bounded Dijkstra minimising travel + queue wait.
 *
 * NOTE the termination rule. With travel alone, the first settled hospital is
 * the answer. With a queue wait added, it is NOT: a hospital three minutes
 * further out with a twenty minute shorter queue wins. So the search runs on
 * until the frontier distance reaches the best TOTAL cost found -- past that
 * point no unexplored hospital can win, because travel only grows and wait is
 * never negative. Still exact, still bounded. */
uint32_t dispatch_search_hospital(const Graph *g, Search *s, const World *w,
                                  const Request *r, uint32_t *out_travel,
                                  uint32_t *out_wait) {
    search_reset(s);
    srelax(s, r->node, 0, r->node);
    heap_push(s, 0, r->node);

    uint32_t best_total = INF32, best_h = INF32, best_tr = INF32, best_wt = 0;

    while (s->hsize) {
        uint64_t top = heap_pop(s);
        uint32_t d = (uint32_t)(top >> 32), u = (uint32_t)top;
        if (d > sdist(s, u)) continue;
        if (d >= best_total) break;                  /* provably done */
        s->settled++;

        uint32_t h = w->hosp_at[u];
        if (h != INF32 && hosp_reject_reason(&w->hosp[h], r) == REJ_NONE) {
            uint32_t wt = hosp_wait_ms(&w->hosp[h]);
            if (d + wt < best_total) {
                best_total = d + wt; best_h = h; best_tr = d; best_wt = wt;
            }
        }
        for (uint32_t e = g->out_head[u]; e < g->out_head[u + 1]; e++) {
            Edge ed = g->out_e[e];
            if (ed.w == INF32) continue;
            uint32_t v = ed.to, nd = d + ed.w;
            if (nd < sdist(s, v)) { srelax(s, v, nd, u); heap_push(s, nd, v); }
        }
    }
    *out_travel = best_tr; *out_wait = best_wt;
    return best_h;
}

static void finish(Decision *d, const Request *r, uint32_t a, uint32_t ta,
                   uint32_t h, uint32_t th, uint32_t wt) {
    d->amb = a; d->hosp = h;
    d->t_to_scene = ta; d->t_to_hosp = th; d->wait_ms = wt;
    d->ok = (a != INF32 && h != INF32);
    d->t_total = d->ok ? ta + th + wt : INF32;
    d->sla_met = d->ok && ta <= r->sla_ms;
    d->horizon_hit = !d->ok && r->max_reach_ms != 0;
    d->n_rejected = 0;
    d->considered = 0;
}

void dispatch_fast(const Graph *g, Search *back, Search *fwd,
                   const World *w, const Request *r, Decision *d) {
    uint64_t s0 = back->settled + fwd->settled;
    uint32_t ta, th, wt;
    uint32_t a = dispatch_nearest_ambulance(g, back, w, r, &ta);
    uint32_t h = dispatch_search_hospital(g, fwd, w, r, &th, &wt);
    finish(d, r, a, ta, h, th, wt);
    d->settled = (back->settled + fwd->settled) - s0;
}

/* ---- Baseline: A-star per candidate ---- */
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
    uint32_t best_h = INF32, best_total = INF32, best_th = INF32, best_wt = 0;
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        if (hosp_reject_reason(&w->hosp[i], r) != REJ_NONE) continue;
        uint32_t t = dispatch_astar(g, s, r->node, w->hosp[i].node);
        if (t == INF32) continue;
        uint32_t wt = hosp_wait_ms(&w->hosp[i]);
        if (t + wt < best_total) { best_total = t + wt; best_h = i; best_th = t; best_wt = wt; }
    }
    finish(d, r, best_a, best_ta, best_h, best_th, best_wt);
    d->settled = s->settled - s0;
}

/* ---- Baseline: full Dijkstra, no early exit ---- */
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
            if (nd < sdist(s, v)) { srelax(s, v, nd, u); heap_push(s, nd, v); }
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
    uint32_t best_h = INF32, best_total = INF32, best_th = INF32, best_wt = 0;
    for (uint32_t i = 0; i < w->n_hosp; i++) {
        if (hosp_reject_reason(&w->hosp[i], r) != REJ_NONE) continue;
        uint32_t t = sdist(fwd, w->hosp[i].node);
        if (t == INF32) continue;
        uint32_t wt = hosp_wait_ms(&w->hosp[i]);
        if (t + wt < best_total) { best_total = t + wt; best_h = i; best_th = t; best_wt = wt; }
    }
    finish(d, r, best_a, best_ta, best_h, best_th, best_wt);
    d->settled = (back->settled + fwd->settled) - s0;
}

void decision_commit(World *w, const Decision *d, const Request *r) {
    if (!d->ok) return;
    w->amb[d->amb].busy = 1;
    Hospital *h = &w->hosp[d->hosp];
    h->beds_free--;
    h->queue_len++;
    if (r->med_qty) h->med[r->need_med] -= (int32_t)r->med_qty;
}
void decision_release(World *w, uint32_t amb, uint32_t hosp) {
    if (amb < w->n_amb) w->amb[amb].busy = 0;
    if (hosp < w->n_hosp) {
        Hospital *h = &w->hosp[hosp];
        if (h->queue_len) h->queue_len--;
        if (h->beds_free < h->beds_total) h->beds_free++;
    }
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
