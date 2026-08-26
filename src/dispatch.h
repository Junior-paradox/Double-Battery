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
#define N_SPEC 8
#define N_MED  8                    /* medicine batch types */
#define DAY_MS 86400000u
#define SERVICE_MS 720000u          /* 12 min of doctor time per patient */

/* A doctor is attached to a hospital, holds one specialty, and is only on
 * duty inside a shift window. This is what makes "Hospital C has an on-duty
 * specialist" different from "Hospital C has a cardiology department". */
typedef struct {
    uint32_t hosp;
    uint8_t  spec;                  /* specialty index 0..N_SPEC-1 */
    uint32_t shift_start, shift_end;  /* ms into a 24h cycle; may wrap */
} Doctor;

typedef struct {
    uint32_t node;
    uint32_t spec_mask;      /* departments this hospital has at all */
    uint32_t on_duty_mask;   /* specialties with a doctor on shift right now */
    int32_t  beds_free, beds_total;
    int32_t  med[N_MED];     /* medicine stock on hand, by batch type */
    int32_t  med_cap[N_MED];
    uint32_t queue_len;      /* patients already waiting to be seen */
    uint32_t docs_on_duty;   /* headcount on shift, sets the service rate */
} Hospital;

typedef struct { uint32_t node, caps_mask; uint8_t busy; } Ambulance;

typedef struct {
    uint32_t node;         /* incident location */
    uint32_t need_hosp;    /* required hospital specialties */
    uint32_t need_amb;     /* required ambulance equipment */
    uint8_t  need_med;     /* medicine batch type required */
    uint32_t med_qty;      /* units required */
    uint8_t  urgency;      /* 0 = lowest .. 3 = life-threatening */
    uint32_t sla_ms;       /* response-time target */
    uint32_t max_reach_ms; /* AMBULANCE search horizon, 0 = unbounded. Applies
                              to the response leg only: a vehicle 40 min away
                              is useless, but a specialist centre 40 min away
                              may be the only place that can treat the patient,
                              so the transport leg is never horizon-capped. */
} Request;

/* Why a hospital was not chosen. Ordered roughly by how interesting it is to
 * a human reading the decision log. */
enum {
    REJ_NONE = 0,
    REJ_NO_DEPT,       /* hospital does not do this specialty at all */
    REJ_NO_DOCTOR,     /* department exists, nobody on duty */
    REJ_NO_BED,
    REJ_NO_MEDICINE,
    REJ_UNREACHABLE,   /* no road route (closures) */
    REJ_COSTLIER       /* eligible, simply not the cheapest */
};
#define MAX_REJECT 4

typedef struct {
    uint32_t hosp, travel_ms, wait_ms;
    uint8_t  reason;
} Reject;

typedef struct {
    uint32_t amb, hosp;            /* INF32 if none found */
    uint32_t t_to_scene;           /* ambulance -> incident */
    uint32_t t_to_hosp;            /* incident -> hospital */
    uint32_t wait_ms;              /* queue wait once there */
    uint32_t t_total;              /* the objective: travel + wait */
    uint8_t  ok, sla_met, horizon_hit;
    uint32_t considered;           /* hospitals evaluated */
    Reject   rejected[MAX_REJECT]; /* closer-but-unsuitable, for the log */
    uint32_t n_rejected;
    uint64_t settled;
} Decision;

typedef struct {
    Hospital  *hosp;   uint32_t n_hosp;
    Ambulance *amb;    uint32_t n_amb;
    Doctor    *doc;    uint32_t n_doc;
    uint32_t  *hosp_at;   /* node -> hospital idx, INF32 if none */
    uint32_t  *amb_head;  /* node -> first ambulance idx, INF32 if none */
    uint32_t  *amb_next;  /* ambulance -> next at same node */
    uint32_t  *village;   uint32_t n_village;
    uint32_t   clock_ms;  /* time of day, drives shifts */
} World;

void world_build(World *w, const Graph *g, uint32_t n_hosp, uint32_t n_amb,
                 uint32_t n_village, uint32_t docs_per_hosp, uint64_t seed);
void world_free(World *w);
size_t world_bytes(const World *w);
void world_reset_state(World *w, const Graph *g, uint64_t seed);
void world_place_ambulance(World *w, uint32_t amb, uint32_t node);

/* Advance the wall clock. Recomputes every hospital's on-duty mask and
 * headcount: O(n_doctors), done per tick rather than per query so the hot
 * path stays a single AND. */
void world_set_clock(World *w, uint32_t t_ms);

/* Queue wait at a hospital: backlog drained at one patient per SERVICE_MS
 * per on-duty doctor. */
static inline uint32_t hosp_wait_ms(const Hospital *h) {
    uint32_t docs = h->docs_on_duty ? h->docs_on_duty : 1;
    uint64_t wait = ((uint64_t)h->queue_len * SERVICE_MS) / docs;
    return wait > 21600000ull ? 21600000u : (uint32_t)wait;   /* clamp at 6 h */
}

/* Can this hospital take this patient? Returns REJ_NONE if yes. */
static inline uint8_t hosp_reject_reason(const Hospital *h, const Request *r) {
    if ((h->spec_mask & r->need_hosp) != r->need_hosp)    return REJ_NO_DEPT;
    if ((h->on_duty_mask & r->need_hosp) != r->need_hosp) return REJ_NO_DOCTOR;
    if (h->beds_free <= 0)                                return REJ_NO_BED;
    if (r->med_qty && h->med[r->need_med] < (int32_t)r->med_qty) return REJ_NO_MEDICINE;
    return REJ_NONE;
}

/* Nearest free, capable ambulance. Backward early-exit Dijkstra: the first
 * settled node holding one is provably the best, because there is no
 * per-vehicle penalty to trade against distance. */
uint32_t dispatch_nearest_ambulance(const Graph *g, Search *s, const World *w,
                                    const Request *r, uint32_t *out_t);

/* Exhaustive reference for the hospital half: a bounded forward Dijkstra
 * minimising travel + wait over all eligible hospitals. Used to verify the
 * distance table, and as the fallback when the table is stale. */
uint32_t dispatch_search_hospital(const Graph *g, Search *s, const World *w,
                                  const Request *r, uint32_t *out_travel,
                                  uint32_t *out_wait);

/* Full dispatch via search only (no precomputed table). */
void dispatch_fast(const Graph *g, Search *back, Search *fwd,
                   const World *w, const Request *r, Decision *d);

/* Baseline: point-to-point A-star from every free ambulance and to every
 * valid hospital -- what a naive implementation does. */
void dispatch_naive_astar(const Graph *g, Search *s,
                          const World *w, const Request *r, Decision *d);

/* Baseline: full Dijkstra with no early exit. */
void dispatch_full_dijkstra(const Graph *g, Search *back, Search *fwd,
                            const World *w, const Request *r, Decision *d);

void decision_commit(World *w, const Decision *d, const Request *r);
void decision_release(World *w, uint32_t amb, uint32_t hosp);

uint32_t dispatch_astar(const Graph *g, Search *s, uint32_t src, uint32_t dst);
uint32_t search_path(const Search *s, uint32_t from, uint32_t *out, uint32_t cap);

#endif
