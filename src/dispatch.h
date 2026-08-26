#ifndef DISPATCH_H
#define DISPATCH_H

#include "search.h"

/* Capability bits. A hospital/ambulance is suitable iff (have & need) == need,
 * i.e. one AND + one CMP per candidate -- single-cycle screening, no strings,
 * no per-candidate branching over a specialty list. */
enum {
    CAP_TRAUMA    = 1u << 0,
    CAP_CARDIAC   = 1u << 1,
    CAP_NEURO     = 1u << 2,
    CAP_BURNS     = 1u << 3,
    CAP_OBSTETRIC = 1u << 4,
    CAP_PAEDS     = 1u << 5,
    CAP_TOXICOL   = 1u << 6,
    CAP_ICU       = 1u << 7,
    /* ambulance-side */
    CAP_ALS       = 1u << 8,   /* advanced life support */
    CAP_VENTILATOR= 1u << 9,
    CAP_NEONATAL  = 1u << 10
};

typedef struct { uint32_t node, spec_mask; int32_t beds_free, beds_total; } Hospital;
typedef struct { uint32_t node, caps_mask; uint8_t busy; }                 Ambulance;

typedef struct {
    uint32_t node;         /* incident location */
    uint32_t need_hosp;    /* required hospital specialties */
    uint32_t need_amb;     /* required ambulance equipment */
    uint8_t  urgency;      /* 0 = lowest .. 3 = life-threatening */
    uint32_t sla_ms;       /* response-time target */
    uint32_t max_reach_ms; /* hard search horizon: an ambulance further away
                              than this is clinically useless, so the search
                              refuses to look for it. Bounds worst-case
                              latency instead of degenerating to a full sweep
                              when the fleet is depleted. 0 = unbounded. */
} Request;

typedef struct {
    uint32_t amb, hosp;            /* INF32 if none found */
    uint32_t t_to_scene, t_to_hosp, t_total;
    uint8_t  ok;                   /* 1 = dispatched, 0 = fallback required */
    uint8_t  horizon_hit;          /* search stopped at max_reach_ms */
    uint8_t  sla_met;
    uint64_t settled;              /* nodes settled across both searches */
} Decision;

typedef struct {
    Hospital  *hosp;   uint32_t n_hosp;
    Ambulance *amb;    uint32_t n_amb;
    uint32_t  *hosp_at;   /* node -> hospital idx, INF32 if none */
    uint32_t  *amb_head;  /* node -> first ambulance idx, INF32 if none */
    uint32_t  *amb_next;  /* ambulance -> next at same node */
    uint32_t  *village;   uint32_t n_village;
} World;

void world_build(World *w, const Graph *g, uint32_t n_hosp, uint32_t n_amb,
                 uint32_t n_village, uint64_t seed);
void world_free(World *w);
size_t world_bytes(const World *w);
void world_reset_state(World *w, const Graph *g, uint64_t seed);
void world_place_ambulance(World *w, uint32_t amb, uint32_t node);

/* Production path: two early-exit Dijkstras (backward for the ambulance,
 * forward for the hospital). Exactly optimal because the objective
 *   min over (a,h) of  t(a->v) + t(v->h)
 * separates into two independent minimisations sharing the incident node v. */
void dispatch_fast(const Graph *g, Search *back, Search *fwd,
                   const World *w, const Request *r, Decision *d);

/* Baseline A: point-to-point A-star from every free ambulance and to every
 * valid hospital. This is what a naive implementation does. */
void dispatch_naive_astar(const Graph *g, Search *s,
                          const World *w, const Request *r, Decision *d);

/* Baseline B: full Dijkstra with no early exit (measures what early
 * termination actually buys). */
void dispatch_full_dijkstra(const Graph *g, Search *back, Search *fwd,
                            const World *w, const Request *r, Decision *d);

void decision_commit(World *w, const Decision *d);

/* Exposed for the index-accelerated path, which replaces only the hospital
 * half of the query. Both return INF32 if nothing suitable is reachable. */
uint32_t dispatch_nearest_ambulance(const Graph *g, Search *s, const World *w,
                                    const Request *r, uint32_t *out_t);
uint32_t dispatch_nearest_hospital(const Graph *g, Search *s, const World *w,
                                   const Request *r, uint32_t *out_t);

#endif
