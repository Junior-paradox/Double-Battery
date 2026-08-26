/* Resident dispatch daemon.
 *
 * The engine holds the road network, fleet state and hospital index in memory
 * for its whole lifetime and answers over a socket. A query costs ~47 us;
 * spawning a process per request would cost 1-5 ms and rebuild ~100 ms of
 * state to do it, so the process boundary is crossed once at startup, not
 * once per emergency.
 *
 * Wire format: one plain-text command per line in, one JSON object per line
 * out. Text in because it parses with strtoul and no library; JSON out
 * because the browser consumes it directly.
 *
 *   DISPATCH <node> <need_hosp> <need_amb> <urgency> <sla_ms> <horizon> <geom>
 *   COMMIT <amb> <hosp>          reserve the vehicle and the bed
 *   RELEASE <amb>                vehicle back in service
 *   CLOSE <edge> | OPEN <edge>   road closure, O(1)
 *   REBUILD                      refresh the hospital index after closures
 *   NODE <index>                 resolve a village index to a node id
 *   HOSPITALS | FLEET | BOUNDS   static map data for the client
 *   ROADS <class> <from_node>    road geometry, paginated by node cursor
 *   STATS | QUIT
 */
#define _GNU_SOURCE
#include "hindex.h"
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define GRID_W 250
#define GRID_H 200
#define N_HOSP 60
#define N_AMB  200
#define N_VILLAGE 5000
#define PATH_CAP 16384
#define LINE_CAP 512
#define OUT_CAP  (1 << 20)

static Graph     G;
static World     W;
static HospIndex HI;
/* Readers (dispatch) run concurrently; writers (closures, state commits,
 * index rebuild) are exclusive. Queries are short enough that writer
 * starvation is not a concern at demo scale. */
static pthread_rwlock_t LOCK = PTHREAD_RWLOCK_INITIALIZER;
static uint64_t N_SERVED = 0, NS_TOTAL = 0;
static pthread_mutex_t STAT_LOCK = PTHREAD_MUTEX_INITIALIZER;

typedef struct { Search back, fwd, geo; uint32_t path[PATH_CAP]; char out[OUT_CAP]; } Ctx;

static uint32_t tok(char **p) {
    while (**p == ' ') (*p)++;
    return (uint32_t)strtoul(*p, p, 10);
}

/* Append "[x,y],..." for a node list, optionally reversed. */
static int emit_coords(char *b, int n, const uint32_t *v, uint32_t cnt, int rev) {
    int k = n;
    for (uint32_t i = 0; i < cnt; i++) {
        if (k > OUT_CAP - 64) break;          /* never write past the buffer */
        uint32_t id = rev ? v[cnt - 1 - i] : v[i];
        k += snprintf(b + k, (size_t)(OUT_CAP - k), "%s[%.1f,%.1f]",
                      i ? "," : "", (double)G.x[id], (double)G.y[id]);
    }
    return k;
}

static int cmd_dispatch(Ctx *c, char *arg) {
    Request r;
    r.node         = tok(&arg);
    r.need_hosp    = tok(&arg);
    r.need_amb     = tok(&arg);
    r.urgency      = (uint8_t)tok(&arg);
    r.sla_ms       = tok(&arg);
    r.max_reach_ms = tok(&arg);
    uint32_t geom  = tok(&arg);

    if (r.node >= G.n_nodes)
        return snprintf(c->out, OUT_CAP, "{\"ok\":false,\"error\":\"bad node\"}\n");

    Decision d; uint32_t fb = 0;
    uint64_t t0 = now_ns();
    pthread_rwlock_rdlock(&LOCK);
    dispatch_indexed(&G, &c->back, &c->fwd, &W, &HI, &r, &d, &fb);

    uint32_t n_a = 0, n_h = 0;
    static _Thread_local uint32_t hpath[PATH_CAP];
    if (geom && d.ok) {
        /* ambulance -> incident comes free from the backward search's parents */
        n_a = search_path(&c->back, W.amb[d.amb].node, c->path, PATH_CAP);
        /* incident -> hospital needs its own targeted A*, since the index
         * answered the hospital question without ever walking the road */
        if (dispatch_astar(&G, &c->geo, r.node, W.hosp[d.hosp].node) != INF32)
            n_h = search_path(&c->geo, W.hosp[d.hosp].node, hpath, PATH_CAP);
    }
    pthread_rwlock_unlock(&LOCK);
    double us = (now_ns() - t0) / 1000.0;

    pthread_mutex_lock(&STAT_LOCK);
    N_SERVED++; NS_TOTAL += (uint64_t)(us * 1000.0);
    pthread_mutex_unlock(&STAT_LOCK);

    if (!d.ok)
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":false,\"reason\":\"%s\",\"latency_us\":%.1f}\n",
            d.horizon_hit ? "no unit within horizon" : "no route", us);

    int n = snprintf(c->out, OUT_CAP,
        "{\"ok\":true,\"amb\":%u,\"amb_node\":%u,\"hosp\":%u,\"hosp_node\":%u,"
        "\"beds_free\":%d,\"t_scene_ms\":%u,\"t_hosp_ms\":%u,\"t_total_ms\":%u,"
        "\"sla_met\":%s,\"settled\":%llu,\"indexed\":%s,\"latency_us\":%.1f",
        d.amb, W.amb[d.amb].node, d.hosp, W.hosp[d.hosp].node,
        W.hosp[d.hosp].beds_free, d.t_to_scene, d.t_to_hosp, d.t_total,
        d.sla_met ? "true" : "false", (unsigned long long)d.settled,
        fb ? "false" : "true", us);

    if (n_a) { n += snprintf(c->out + n, OUT_CAP - n, ",\"leg1\":[");
               n = emit_coords(c->out, n, c->path, n_a, 1);
               n += snprintf(c->out + n, OUT_CAP - n, "]"); }
    if (n_h) { n += snprintf(c->out + n, OUT_CAP - n, ",\"leg2\":[");
               n = emit_coords(c->out, n, hpath, n_h, 1);
               n += snprintf(c->out + n, OUT_CAP - n, "]"); }
    n += snprintf(c->out + n, OUT_CAP - n, "}\n");
    return n;
}

static int handle(Ctx *c, char *line) {
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    size_t klen = (size_t)(arg - line);

    if (!strncmp(line, "DISPATCH", klen > 8 ? klen : 8)) return cmd_dispatch(c, arg);

    if (!strncmp(line, "COMMIT", 6)) {
        uint32_t a = tok(&arg), h = tok(&arg);
        if (a >= W.n_amb || h >= W.n_hosp)
            return snprintf(c->out, OUT_CAP, "{\"ok\":false,\"error\":\"bad id\"}\n");
        pthread_rwlock_wrlock(&LOCK);
        W.amb[a].busy = 1; W.hosp[h].beds_free--;
        int bf = W.hosp[h].beds_free;
        pthread_rwlock_unlock(&LOCK);
        return snprintf(c->out, OUT_CAP, "{\"ok\":true,\"beds_free\":%d}\n", bf);
    }
    if (!strncmp(line, "RELEASE", 7)) {
        uint32_t a = tok(&arg);
        if (a >= W.n_amb)
            return snprintf(c->out, OUT_CAP, "{\"ok\":false,\"error\":\"bad id\"}\n");
        pthread_rwlock_wrlock(&LOCK);
        W.amb[a].busy = 0;
        pthread_rwlock_unlock(&LOCK);
        return snprintf(c->out, OUT_CAP, "{\"ok\":true}\n");
    }
    if (!strncmp(line, "CLOSE", 5) || !strncmp(line, "OPEN", 4)) {
        int close = line[0] == 'C';
        uint32_t e = tok(&arg);
        if (e >= G.n_edges)
            return snprintf(c->out, OUT_CAP, "{\"ok\":false,\"error\":\"bad edge\"}\n");
        uint64_t t0 = now_ns();
        pthread_rwlock_wrlock(&LOCK);
        if (close) graph_close_road(&G, e); else graph_open_road(&G, e);
        pthread_rwlock_unlock(&LOCK);
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":true,\"edge\":%u,\"closed\":%s,\"took_ns\":%llu,"
            "\"index_stale\":true}\n",
            e, close ? "true" : "false", (unsigned long long)(now_ns() - t0));
    }
    if (!strncmp(line, "REBUILD", 7)) {
        uint64_t t0 = now_ns();
        pthread_rwlock_wrlock(&LOCK);
        hindex_build(&HI, &G, &W, &c->geo);
        pthread_rwlock_unlock(&LOCK);
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":true,\"took_ms\":%.2f,\"generation\":%u}\n",
            (now_ns() - t0) / 1e6, HI.generation);
    }
    if (!strncmp(line, "NODE", 4)) {
        uint32_t i = tok(&arg) % W.n_village;
        uint32_t nd = W.village[i];
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":true,\"node\":%u,\"x\":%.1f,\"y\":%.1f}\n", nd, G.x[nd], G.y[nd]);
    }
    if (!strncmp(line, "ROADS", 5)) {
        uint32_t cls = tok(&arg), start = tok(&arg);
        /* Each undirected road is emitted once, by the endpoint with the
         * lower id. Paginated by node cursor because the full network is far
         * larger than one response buffer. */
        int n = snprintf(c->out, OUT_CAP, "{\"ok\":true,\"class\":%u,\"seg\":[", cls);
        uint32_t u = start, first = 1;
        for (; u < G.n_nodes; u++) {
            if (n > OUT_CAP - 4096) break;
            for (uint32_t e = G.out_head[u]; e < G.out_head[u + 1]; e++) {
                uint32_t v = G.out_e[e].to;
                if (v <= u || G.edge_class[e] != cls) continue;
                n += snprintf(c->out + n, (size_t)(OUT_CAP - n),
                              "%s%.0f,%.0f,%.0f,%.0f", first ? "" : ",",
                              G.x[u], G.y[u], G.x[v], G.y[v]);
                first = 0;
            }
        }
        return n + snprintf(c->out + n, (size_t)(OUT_CAP - n),
                            "],\"next\":%d}\n", u >= G.n_nodes ? -1 : (int)u);
    }
    if (!strncmp(line, "HOSPITALS", 9)) {
        pthread_rwlock_rdlock(&LOCK);
        int n = snprintf(c->out, OUT_CAP, "{\"ok\":true,\"hospitals\":[");
        for (uint32_t i = 0; i < W.n_hosp; i++) {
            uint32_t nd = W.hosp[i].node;
            n += snprintf(c->out + n, (size_t)(OUT_CAP - n),
                "%s{\"id\":%u,\"node\":%u,\"x\":%.1f,\"y\":%.1f,\"spec\":%u,"
                "\"beds_free\":%d,\"beds_total\":%d}",
                i ? "," : "", i, nd, G.x[nd], G.y[nd], W.hosp[i].spec_mask,
                W.hosp[i].beds_free, W.hosp[i].beds_total);
        }
        pthread_rwlock_unlock(&LOCK);
        return n + snprintf(c->out + n, (size_t)(OUT_CAP - n), "]}\n");
    }
    if (!strncmp(line, "FLEET", 5)) {
        pthread_rwlock_rdlock(&LOCK);
        int n = snprintf(c->out, OUT_CAP, "{\"ok\":true,\"fleet\":[");
        for (uint32_t i = 0; i < W.n_amb; i++) {
            uint32_t nd = W.amb[i].node;
            n += snprintf(c->out + n, (size_t)(OUT_CAP - n),
                "%s{\"id\":%u,\"node\":%u,\"x\":%.1f,\"y\":%.1f,\"caps\":%u,\"busy\":%u}",
                i ? "," : "", i, nd, G.x[nd], G.y[nd], W.amb[i].caps_mask, W.amb[i].busy);
        }
        pthread_rwlock_unlock(&LOCK);
        return n + snprintf(c->out + n, (size_t)(OUT_CAP - n), "]}\n");
    }
    if (!strncmp(line, "BOUNDS", 6)) {
        float x0 = G.x[0], x1 = G.x[0], y0 = G.y[0], y1 = G.y[0];
        for (uint32_t v = 1; v < G.n_nodes; v++) {
            if (G.x[v] < x0) x0 = G.x[v];
            if (G.x[v] > x1) x1 = G.x[v];
            if (G.y[v] < y0) y0 = G.y[v];
            if (G.y[v] > y1) y1 = G.y[v];
        }
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":true,\"minx\":%.1f,\"miny\":%.1f,\"maxx\":%.1f,\"maxy\":%.1f,"
            "\"villages\":%u,\"edges\":%u}\n", x0, y0, x1, y1, W.n_village, G.n_edges);
    }
    if (!strncmp(line, "STATS", 5)) {
        pthread_rwlock_rdlock(&LOCK);
        uint32_t busy = 0, beds = 0;
        for (uint32_t i = 0; i < W.n_amb; i++) busy += W.amb[i].busy;
        for (uint32_t i = 0; i < W.n_hosp; i++) beds += (uint32_t)W.hosp[i].beds_free;
        pthread_rwlock_unlock(&LOCK);
        pthread_mutex_lock(&STAT_LOCK);
        uint64_t n = N_SERVED, tot = NS_TOTAL;
        pthread_mutex_unlock(&STAT_LOCK);
        return snprintf(c->out, OUT_CAP,
            "{\"ok\":true,\"nodes\":%u,\"edges\":%u,\"ambulances\":%u,\"busy\":%u,"
            "\"hospitals\":%u,\"beds_free\":%u,\"served\":%llu,\"mean_us\":%.1f}\n",
            G.n_nodes, G.n_edges, W.n_amb, busy, W.n_hosp, beds,
            (unsigned long long)n, n ? tot / 1000.0 / n : 0.0);
    }
    return snprintf(c->out, OUT_CAP, "{\"ok\":false,\"error\":\"unknown command\"}\n");
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    Ctx *c = xmalloc(sizeof(Ctx));
    search_init(&c->back, G.n_nodes);
    search_init(&c->fwd,  G.n_nodes);
    search_init(&c->geo,  G.n_nodes);

    char buf[LINE_CAP * 4]; size_t have = 0;
    for (;;) {
        ssize_t r = read(fd, buf + have, sizeof(buf) - have - 1);
        if (r <= 0) break;
        have += (size_t)r;
        buf[have] = 0;

        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', have - (size_t)(start - buf)))) {
            *nl = 0;
            if (!strncmp(start, "QUIT", 4)) goto done;
            int n = handle(c, start);
            if (n > 0 && write(fd, c->out, (size_t)n) < 0) goto done;
            start = nl + 1;
        }
        have -= (size_t)(start - buf);
        if (have >= sizeof(buf) - 1) have = 0;      /* overlong line, drop */
        else memmove(buf, start, have);
    }
done:
    close(fd);
    search_free(&c->back); search_free(&c->fwd); search_free(&c->geo);
    free(c);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9090;

    uint64_t t0 = now_ns();
    graph_build_grid(&G, GRID_W, GRID_H, 0xC0FFEEull);
    world_build(&W, &G, N_HOSP, N_AMB, N_VILLAGE, 0xBEEF01ull);
    hindex_init(&HI, G.n_nodes);
    Search boot; search_init(&boot, G.n_nodes);
    hindex_build(&HI, &G, &W, &boot);
    search_free(&boot);
    fprintf(stderr, "engine ready in %.1f ms | %u nodes %u edges | %.2f MB resident\n",
            (now_ns() - t0) / 1e6, G.n_nodes, G.n_edges,
            (graph_bytes(&G) + hindex_bytes(&HI) + world_bytes(&W)) / 1048576.0);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "listening on 127.0.0.1:%d\n", port);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(th);
        else close(fd);
    }
}
