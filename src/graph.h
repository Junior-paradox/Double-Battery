#ifndef GRAPH_H
#define GRAPH_H

#include "common.h"

/* Forward-star / CSR road network.
 * All topology lives in flat contiguous uint32 arrays: no pointers, no node
 * objects. Neighbours of u are out_head[u] .. out_head[u+1]-1, so a scan of a
 * node's edges is a linear sweep over one cache line-friendly region.
 *
 * Space: (V+1)*4  +  E*4 (target) + E*4 (weight)  for the forward graph,
 *        mirrored for the reverse graph, + E*4 for the fwd->rev edge map,
 *        + V*8 for coordinates.   => O(V + E), ~4.4 MB at V=50k, E=200k.
 */
typedef struct { uint32_t to, w; } Edge;   /* 8 bytes, 8 per cache line */

typedef struct {
    uint32_t n_nodes;
    uint32_t n_edges;         /* directed edge count */

    /* forward graph. target and weight are INTERLEAVED: a relaxation needs
     * both, so one 8-byte struct = one cache line fetch, not two. */
    uint32_t *out_head;       /* n_nodes + 1 */
    Edge     *out_e;          /* n_edges */

    /* reverse graph (for backward search: "who can reach the incident?") */
    uint32_t *in_head;        /* n_nodes + 1 */
    Edge     *in_e;           /* n_edges */

    uint32_t *fwd_to_rev;     /* n_edges: forward edge id -> reverse edge id */
    uint32_t *base_w;         /* n_edges: pristine weights (undo closures) */
    uint32_t *twin;           /* n_edges: opposite direction of the same road */
    uint8_t  *edge_class;     /* n_edges: 0 highway, 1 arterial, 2 local */

    float *x, *y;             /* node coords, metres */
    uint32_t max_speed_mms;   /* fastest road, mm per ms -> used for A* bound */
} Graph;

/* Deterministic synthetic road network: a `gw` x `gh` grid with positional
 * jitter, mixed road classes (highway / arterial / local) and a scattering of
 * long-range "bypass" edges so the network is not a pure lattice.
 *
 * Nodes are numbered in TILE order (16x16 blocks), not row-major. A Dijkstra
 * ball around an incident is a spatially local region, so tile numbering makes
 * that region a near-contiguous id range -- the adjacency and node-state
 * arrays are then streamed rather than randomly probed. */
void graph_build_grid(Graph *g, uint32_t gw, uint32_t gh, uint64_t seed);
void graph_free(Graph *g);
size_t graph_bytes(const Graph *g);

/* Dynamic road closure: O(1). Sets both directions of an undirected road to
 * INF32 (impassable) or restores the pristine weight. `edge` is a forward id. */
void graph_close_edge(Graph *g, uint32_t edge);
void graph_open_edge(Graph *g, uint32_t edge);
/* Close/open both directions of the underlying road. Still O(1). */
void graph_close_road(Graph *g, uint32_t edge);
void graph_open_road(Graph *g, uint32_t edge);

static inline double euclid(const Graph *g, uint32_t a, uint32_t b) {
    double dx = (double)g->x[a] - g->x[b];
    double dy = (double)g->y[a] - g->y[b];
    return __builtin_sqrt(dx * dx + dy * dy);
}
/* Admissible A* heuristic: straight-line metres / fastest possible speed. */
static inline uint32_t astar_h(const Graph *g, uint32_t a, uint32_t b) {
    double m = euclid(g, a, b);
    return (uint32_t)(m * 1000.0 / (double)g->max_speed_mms);
}

#endif
