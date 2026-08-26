#ifndef HTABLE_H
#define HTABLE_H

#include "dispatch.h"

/* Hospital distance table.
 *
 * The per-specialty proximity index this replaces answered "nearest hospital
 * with specialty X" by TRAVEL TIME alone. Once each hospital carries a queue
 * wait, that answer is wrong: a hospital three minutes further out with a
 * twenty minute shorter queue is the better destination. An index keyed on
 * travel cannot express that.
 *
 * So instead of 8 specialty layers we store the full travel time from every
 * node to every hospital: one backward Dijkstra per hospital, H * V entries.
 * The query is then an O(H) scan that can apply ANY cost function and ANY
 * eligibility rule -- doctor on shift, bed free, medicine in stock -- and is
 * exact by construction with no search at all.
 *
 *   build : H x O(E log V)          (~540 ms at H=60, V=50k)
 *   space : H * V * 4 bytes         (12 MB at H=60, V=50k)
 *   query : O(H), 4 cache lines
 *
 * Layout is NODE-MAJOR: all H travel times for one node are contiguous, so a
 * query touches 60 * 4 = 240 bytes -- four cache lines -- instead of H
 * strided misses. Build writes are strided instead, which is the right trade
 * because build happens once and queries happen constantly.
 *
 * It also makes the decision log possible: the cost to every hospital is
 * known, so the engine can say exactly which closer ones it passed over and
 * why.
 */
typedef struct {
    uint32_t  n_nodes, n_hosp;
    uint32_t *d;            /* n_nodes * n_hosp, node-major */
    uint32_t  generation;   /* bumped on rebuild */
} HospTable;

void   htable_init(HospTable *t, uint32_t n_nodes, uint32_t n_hosp);
void   htable_free(HospTable *t);
void   htable_build(HospTable *t, const Graph *g, const World *w, Search *scratch);
size_t htable_bytes(const HospTable *t);

/* Full dispatch: searched ambulance + table-scanned hospital. Fills in the
 * rejected-alternative breadcrumbs. */
void dispatch_table(const Graph *g, Search *back, const World *w,
                    const HospTable *t, const Request *r, Decision *d);

const char *reject_name(uint8_t reason);

#endif
