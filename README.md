# HealthWay

Emergency dispatch engine — speed demo. A from-scratch C engine for dynamic ambulance dispatch, plus a benchmark
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
| Resident daemon + wire protocol | `src/server.c` |
| Route geometry for map display | `search_path()` |
| Interactive map + live telemetry | `web/index.html` |
| WebSocket bridge | `web/bridge.js` |

Not yet built: multi-stop route optimisation (HGS/ALNS), en-route replanning.

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

## Running as a service

```
make serve          # or: ./server 9090
```

The engine is a **resident daemon**, not a per-request process and not a
shared library. A query costs ~47 us; process spawn costs 1-5 ms and would
rebuild ~100 ms of graph and index state to do it, so the process boundary is
crossed once at startup. A `.so` would also keep state hot, but it welds the
engine to the web server's lifetime -- one segfault takes down both -- and
gives up the engine's own thread scaling. A daemon keeps state resident,
isolates crashes, and is callable from any backend language.

One plain-text command per line in, one JSON object per line out:

```
DISPATCH <node> <need_hosp> <need_amb> <urgency> <sla_ms> <horizon> <geom>
COMMIT <amb> <hosp>       RELEASE <amb>
CLOSE <edge>              OPEN <edge>         REBUILD
NODE <village>            STATS               QUIT
```

`geom=1` additionally returns `leg1` (ambulance to incident, free from the
backward search's parent pointers) and `leg2` (incident to hospital, a
targeted A* since the index answers without ever walking the road) as
coordinate arrays ready to draw.

Startup: **102.8 ms**, **9.18 MB resident** for the whole 50k-node service.

**Measured over TCP, end to end, including JSON**

| clients | pipeline | req/s | µs/req |
|---|---|---|---|
| 1 | 1 | 3,645 | 274.4 |
| 1 | 32 | 12,195 | 82.0 |
| 2 | 32 | 27,439 | 36.4 |
| 4 | 32 | 39,444 | 25.4 |
| 8 | 32 | 42,616 | 23.5 |

**42,616 dispatches/sec end to end.** The first row is the important one for
whoever writes the backend: a single un-pipelined client gets 3,645 req/s and
is bound by round-trip latency, not by the engine. Pipelining alone is 3.3x
before any extra cores are involved, so the Node bridge should batch rather
than issue one blocking call per emergency.

Writers (`CLOSE`, `COMMIT`, `REBUILD`) take an exclusive lock; dispatches take
a shared one and run concurrently.

## Web demo

Three processes, no build step, no npm install:

```
make            # builds bench + server
./server 9090   # engine daemon, ~103 ms startup
node web/bridge.js
```

Then open **http://127.0.0.1:8080**.

`web/bridge.js` has **zero dependencies** — the WebSocket handshake and
framing are written directly against `node:crypto`, so it runs on a bare Node
install. It holds a pool of 4 pipelined TCP connections to the engine rather
than one blocking call per emergency, for the reason the throughput table
above shows.

The map is Leaflet on `L.CRS.Simple`, mapping engine metres straight to map
units. The network is synthetic, so there is no real geography to put beneath
it and no tile server is contacted — the demo works with no internet.

Controls: run/pause, emergencies per second, inject a 60-case surge, close
2000 roads, rebuild the index, reopen. The live panel shows p50/p99 engine
latency, nodes settled per query, index hit rate, fleet utilisation, beds
remaining, SLA attainment, and a scrolling decision log.

Verified end to end: 60 hospitals and 200 ambulances loaded, dispatches
returning route geometry at 96 µs with 87 nodes settled, 2000 roads closed in
116 ms over the wire, index rebuilt in 135 ms, telemetry at 2 Hz.

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
- `CLOSE` marks the hospital index stale but does not rebuild it; the caller
  must issue `REBUILD` (~95 ms). Dirty-region invalidation would be cheaper.
- The daemon has no auth, no rate limiting and binds to loopback only. It is a
  demo service, not an exposed one. The bridge inherits that.
- Leaflet is loaded from unpkg. Vendor it locally before demoing anywhere the
  network might not cooperate.
- Closing 2000 roads takes 116 ms through the bridge versus 0.15 ms in the
  engine — that is 2000 individual commands and JSON parses, not engine cost.
  A bulk `CLOSE a,b,c` command would remove it.
- Ambulances return to their home node on release rather than continuing from
  where they finished.
