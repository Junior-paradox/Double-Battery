#include "graph.h"

/* Road classes: {speed m/s, share}. Highway 25 m/s = 90 km/h. */
static const uint32_t ROAD_SPEED[3] = { 25, 14, 8 };

static uint32_t edge_time_ms(double metres, uint32_t speed_mps) {
    double ms = metres * 1000.0 / (double)speed_mps;
    if (ms < 1.0) ms = 1.0;
    return (uint32_t)ms;
}

void graph_build_grid(Graph *g, uint32_t gw, uint32_t gh, uint64_t seed) {
    Rng rng; rng_seed(&rng, seed);

    uint32_t V = gw * gh;
    /* undirected roads: horizontal + vertical grid links, plus bypasses */
    uint32_t n_bypass = V / 100;
    uint32_t U = (gw - 1) * gh + gw * (gh - 1) + n_bypass;
    uint32_t E = U * 2;                       /* stored as directed pairs */

    g->n_nodes = V;
    g->n_edges = E;
    g->max_speed_mms = ROAD_SPEED[0];

    g->x = xmalloc(sizeof(float) * V);
    g->y = xmalloc(sizeof(float) * V);

    /* 300 m nominal block size, +/- 40% jitter -> irregular street geometry */
    /* --- tile-order node numbering (cache locality, see graph.h) --- */
    const uint32_t T = 16;
    uint32_t *id_of = xmalloc(sizeof(uint32_t) * V);   /* (j*gw+i) -> node id */
    {
        uint32_t next = 0;
        for (uint32_t tj = 0; tj < gh; tj += T)
            for (uint32_t ti = 0; ti < gw; ti += T)
                for (uint32_t j = tj; j < tj + T && j < gh; j++)
                    for (uint32_t i = ti; i < ti + T && i < gw; i++)
                        id_of[j * gw + i] = next++;
    }

    for (uint32_t j = 0; j < gh; j++)
        for (uint32_t i = 0; i < gw; i++) {
            uint32_t id = id_of[j * gw + i];
            g->x[id] = (float)(i * 300.0 + (rng_f64(&rng) - 0.5) * 240.0);
            g->y[id] = (float)(j * 300.0 + (rng_f64(&rng) - 0.5) * 240.0);
        }

    /* --- collect undirected roads --- */
    uint32_t *ea = xmalloc(sizeof(uint32_t) * U);
    uint32_t *eb = xmalloc(sizeof(uint32_t) * U);
    uint32_t *ew = xmalloc(sizeof(uint32_t) * U);
    uint8_t  *ec = xmalloc(sizeof(uint8_t) * U);
    uint32_t m = 0;

    for (uint32_t j = 0; j < gh; j++)
        for (uint32_t i = 0; i < gw; i++) {
            uint32_t id = id_of[j * gw + i];
            if (i + 1 < gw) {
                uint32_t nb = id_of[j * gw + i + 1];
                /* every 12th column is an arterial, every 40th a highway */
                uint32_t cls = (j % 40 == 0) ? 0 : (j % 12 == 0) ? 1 : 2;
                ea[m] = id; eb[m] = nb; ec[m] = (uint8_t)cls;
                ew[m] = edge_time_ms(euclid(g, id, nb), ROAD_SPEED[cls]); m++;
            }
            if (j + 1 < gh) {
                uint32_t nb = id_of[(j + 1) * gw + i];
                uint32_t cls = (i % 40 == 0) ? 0 : (i % 12 == 0) ? 1 : 2;
                ea[m] = id; eb[m] = nb; ec[m] = (uint8_t)cls;
                ew[m] = edge_time_ms(euclid(g, id, nb), ROAD_SPEED[cls]); m++;
            }
        }
    /* long-range bypasses: break the pure-lattice metric so A-star and
       Dijkstra behave like a real network with ring roads. */
    for (uint32_t k = 0; k < n_bypass; k++) {
        uint32_t a = rng_u32(&rng, V), b = rng_u32(&rng, V);
        if (a == b) b = (b + 1) % V;
        ea[m] = a; eb[m] = b; ec[m] = 0;
        ew[m] = edge_time_ms(euclid(g, a, b), ROAD_SPEED[0]); m++;
    }

    /* --- CSR build: counting sort, two passes, no per-node allocation --- */
    g->out_head = xcalloc(V + 1, sizeof(uint32_t));
    g->in_head  = xcalloc(V + 1, sizeof(uint32_t));
    for (uint32_t k = 0; k < U; k++) {
        g->out_head[ea[k] + 1]++; g->in_head[eb[k] + 1]++;   /* a->b */
        g->out_head[eb[k] + 1]++; g->in_head[ea[k] + 1]++;   /* b->a */
    }
    for (uint32_t v = 0; v < V; v++) {
        g->out_head[v + 1] += g->out_head[v];
        g->in_head[v + 1]  += g->in_head[v];
    }

    g->out_e = xmalloc(sizeof(Edge) * E);
    g->in_e  = xmalloc(sizeof(Edge) * E);
    g->fwd_to_rev = xmalloc(sizeof(uint32_t) * E);
    g->base_w  = xmalloc(sizeof(uint32_t) * E);
    g->twin    = xmalloc(sizeof(uint32_t) * E);
    g->edge_class = xmalloc(sizeof(uint8_t) * E);

    uint32_t *ocur = xmalloc(sizeof(uint32_t) * V);
    uint32_t *icur = xmalloc(sizeof(uint32_t) * V);
    memcpy(ocur, g->out_head, sizeof(uint32_t) * V);
    memcpy(icur, g->in_head,  sizeof(uint32_t) * V);

    /* pass 1: place forward edges */
    uint32_t *fslot = xmalloc(sizeof(uint32_t) * E); /* road k -> fwd ids */
    for (uint32_t k = 0; k < U; k++) {
        uint32_t f1 = ocur[ea[k]]++;
        g->out_e[f1] = (Edge){ eb[k], ew[k] }; g->base_w[f1] = ew[k];
        uint32_t f2 = ocur[eb[k]]++;
        g->out_e[f2] = (Edge){ ea[k], ew[k] }; g->base_w[f2] = ew[k];
        fslot[2 * k] = f1; fslot[2 * k + 1] = f2;
        g->twin[f1] = f2; g->twin[f2] = f1;
        g->edge_class[f1] = g->edge_class[f2] = ec[k];
    }
    /* pass 2: reverse graph + fwd->rev index map (O(1) closure propagation) */
    for (uint32_t k = 0; k < U; k++) {
        uint32_t r1 = icur[eb[k]]++;
        g->in_e[r1] = (Edge){ ea[k], ew[k] };
        g->fwd_to_rev[fslot[2 * k]] = r1;
        uint32_t r2 = icur[ea[k]]++;
        g->in_e[r2] = (Edge){ eb[k], ew[k] };
        g->fwd_to_rev[fslot[2 * k + 1]] = r2;
    }

    free(ea); free(eb); free(ew); free(ocur); free(icur); free(fslot); free(id_of); free(ec);
}

void graph_free(Graph *g) {
    free(g->out_head); free(g->out_e);
    free(g->in_head); free(g->in_e);
    free(g->fwd_to_rev); free(g->base_w); free(g->twin); free(g->edge_class); free(g->x); free(g->y);
    memset(g, 0, sizeof(*g));
}

size_t graph_bytes(const Graph *g) {
    size_t V = g->n_nodes, E = g->n_edges;
    return (V + 1) * 4 * 2          /* out_head + in_head */
         + E * 8 * 2                /* out_e + in_e (interleaved to,w) */
         + E * 4 * 3                /* fwd_to_rev + base_w + twin */
         + E                        /* edge_class */
         + V * 8;                   /* x, y */
}

void graph_close_edge(Graph *g, uint32_t e) {
    g->out_e[e].w = INF32;
    g->in_e[g->fwd_to_rev[e]].w = INF32;
}
void graph_open_edge(Graph *g, uint32_t e) {
    g->out_e[e].w = g->base_w[e];
    g->in_e[g->fwd_to_rev[e]].w = g->base_w[e];
}
void graph_close_road(Graph *g, uint32_t e) {
    graph_close_edge(g, e); graph_close_edge(g, g->twin[e]);
}
void graph_open_road(Graph *g, uint32_t e) {
    graph_open_edge(g, e); graph_open_edge(g, g->twin[e]);
}
