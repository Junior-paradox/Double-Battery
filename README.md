# Emergency Dispatch Engine — speed demo

A from-scratch C engine for dynamic ambulance dispatch, plus a benchmark
harness that measures latency, throughput, memory and complexity growth.

```
make && ./bench
```

No dependencies beyond libc + pthreads. Dataset is generated from a fixed
seed, so every run produces the identical graph, fleet and request stream.

## What the demo covers

| Deliverable | Where |
|---|---|
| Road network, 50k nodes / 200k edges | `src/graph.c` |
| Nearest available + capable ambulance | `src/dispatch.c` |
| Nearest hospital with specialty **and** a free bed | `src/dispatch.c`, `src/hindex.c` |
| Dynamic road closures mid-operation | `graph_close_road()` |
| Resource depletion + urgency preemption | `src/depq.h`, bench §6 |
| No-route fallback | `Decision.ok == 0` |
| Concurrency | bench §7 |

Not yet built: HTTP/WebSocket layer, map UI, multi-stop route optimisation
(HGS/ALNS). This is the routing + assignment core only, which is where the
latency budget actually goes.

## Data model

Everything is a flat contiguous array. No node objects, no linked lists in the
hot path, no allocation inside a query.

```
Graph        out_head[V+1]  in_head[V+1]        CSR / forward-star
             out_e[E]       in_e[E]             Edge{to, w} interleaved, 8B
             base_w[E] twin[E] fwd_to_rev[E]    closure bookkeeping (cold)
             x[V] y[V]                          coordinates

NodeState    {dist, stamp, parent, pad}         16B, one cache line per 4 nodes
```

Three decisions that matter:

**Tile-order node numbering.** Nodes are numbered in 16×16 spatial blocks, not
row-major, so a Dijkstra ball around an incident maps to a near-contiguous id
range.

**Generation stamps instead of memset.** `dist[]` is never cleared between
queries; a node whose `stamp != gen` is implicitly infinite. Query reset is
O(1) rather than O(V) — at V=50k that alone is ~50 µs per query saved.

**Bitmask capability screening.** Specialties, equipment and ward types are
bits in a `uint32`. Screening a candidate is `(have & need) == need`: one AND,
one compare, no string work, no per-candidate branching.

## The core algorithm

The objective is

```
min over (ambulance a, hospital h) of   t(a → incident) + t(incident → h)
```

which **separates**: the two terms share only the incident node, so minimising
each independently is exactly optimal. That turns an `|A| × |H|` search into
two single-source searches rooted at the incident:

- **backward** Dijkstra on the reverse graph → first settled node holding a
  free, capable ambulance is the optimal ambulance;
- **forward** Dijkstra → first settled node holding a hospital with the
  specialty and a free bed is the optimal hospital.

Because Dijkstra settles nodes in nondecreasing distance, the *first* hit is
provably the best one — so the search stops there instead of exploring the
graph. Bench §2 verifies this against both a full Dijkstra and per-candidate
A*: **0 ETA mismatches over 300 requests**.

### Hospital proximity index

Profiling showed the hospital half was 1635 of 2013 settled nodes — 81% of the
work. Hospitals don't move, so it is precomputed: one multi-source backward
Dijkstra per specialty, seeded from every hospital offering it, yielding for
every node `(time to nearest such hospital, which one)`. The runtime hospital
lookup becomes a single array read.

Falls back to the live search — and is therefore never wrong — when the
indexed hospital has no free bed, or when a request names 2+ specialties.

### Search horizon

An ambulance 40 minutes away is clinically useless. Each request carries
`max_reach_ms`; the search abandons once the frontier passes it. This is the
difference between a bounded query and a full graph sweep when the fleet
saturates and "early exit" has nothing to exit on (§6: **18× faster, p50 9709
µs → 621 µs**).

## Complexity

| Operation | Time | Space |
|---|---|---|
| Graph build | O(V + E) | O(V + E) |
| Query reset | **O(1)** (generation stamp) | — |
| Dispatch (early-exit ×2) | O(E' log V), E' = edges within the settled ball | O(V) per thread |
| Dispatch (indexed hospital) | ambulance search + **O(1)** | + 8·V·8 B |
| Road closure / reopen | **O(1)** | O(1) |
| Capability screen | **O(1)** per candidate | O(1) |
| Backlog push / pop-min / pop-max | O(log n) | O(n) |
| Index rebuild | 8 · O(E log V) | 8·V·8 B |

Measured memory at V=50k, E=200k: graph **6.11 MB**, hospital index **3.05 MB**,
per-thread workspace **1.14 MB**, fleet+villages **24 KB**.

## Results

Numbers below are from a 2011 dual-core i3-2350M under frequency scaling —
they are a **floor**, not a target spec. The hardware-independent figures
(settled nodes per query, op counts, speedup ratios) are the ones that carry.

**Dispatch latency, 20k requests, 1 core** (µs)

| Variant | mean | p50 | p90 | p99 | settled/query |
|---|---|---|---|---|---|
| early-exit ×2 | 286.0 | 205.1 | 619.8 | 1394 | 2013 |
| **+ hospital index** | **130.7** | **47.5** | **344.2** | **1209** | **883** |
| full Dijkstra ×2 + scan | 17938 | 17827 | 18796 | 21990 | 100000 |
| A* per ambulance + hospital | 184208 | 173295 | 304896 | 418026 | — |

**52× faster than a full-Dijkstra implementation, 538× faster than
per-candidate A*, with identical answers.** With the index, 7648
dispatches/sec on one core; the index pays for its 85 ms build after ~550
queries.

**Dynamic closures.** 4000 roads closed in 0.605 ms (75.7 ns each, O(1), no
re-preprocessing). Post-closure dispatch latency is statistically unchanged
(mean 288 µs vs 286 µs) — nothing to invalidate, nothing to rebuild. This is
the reason the engine does *not* use Contraction Hierarchies: CH's preprocessing
would have to be redone on every closure.

**Scaling — latency is flat in V**

| V | E | mean query | settled | % of V |
|---|---|---|---|---|
| 2,000 | 7,860 | 97 µs | 814 | 20.4% |
| 20,000 | 79,830 | 236 µs | 1798 | 4.5% |
| 50,000 | 200,100 | 269 µs | 1889 | 1.9% |
| 200,000 | 802,200 | 297 µs | 1873 | 0.47% |

**A 100× larger network costs 3× more per query.** Early exit means cost tracks
the *density of valid targets*, not the size of the map — the settled count is
essentially constant while V grows two orders of magnitude.

**Concurrency.** 1.96× on 2 threads (2 physical cores), 2.64× on 4 (SMT). The
graph is read-shared with zero locks; each thread owns a private workspace.

**Backlog queue.** Min-max heap: push 26 ns/op, pop-min 321 ns/op over 1M
entries, urgency ordering verified. Pop-min = most urgent oldest-first
(dispatch); pop-max = least urgent newest (shed / defer under overload).

## Honest gaps

- Section 6's horizon dispatches 1429 vs 1463 requests without it — it refuses
  ambulances >15 min out. That is a *policy* trade (18× latency for 2% fewer
  dispatches), and the horizon should be tuned per urgency tier.
- 22% of requests still fall back to a live hospital search because the
  synthetic generator emits random 2-specialty requirements. Real EMS protocols
  use ~12 enumerated destination categories; indexing on that enum instead of
  arbitrary bitmask combinations would take fallbacks to near zero.
- The index is stale under road closures. It is exact for bed/multi-specialty
  misses (they re-verify by search), but a closure that changes which hospital
  is nearest is not detected until rebuild. Needs either periodic background
  rebuild (85 ms) or dirty-region invalidation.
- Ambulances are stationary between dispatches; en-route re-planning (D* Lite
  territory) is not implemented.
- The concurrency benchmark is read-only. Contention on fleet/bed state under
  real concurrent mutation is unmeasured.
