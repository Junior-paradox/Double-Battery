# HealthWay

Rural healthcare dispatch engine — doctors, ambulances and medicine — written
from scratch in C, with a live web simulation on top.

```
make              # portable build — bench, server and tests
make test         # 74 assertions, exits non-zero on failure, under a second
make test-all     # the above plus 31 protocol assertions against the daemon
./bench --quick   # benchmark in ~40 s   (plain ./bench for the full sweep)
./server 9090     # engine daemon
node web/bridge.js
```

**Full documentation:**

| Document | What's in it |
|---|---|
| **[docs/ALGORITHM.md](docs/ALGORITHM.md)** | The approach: problem model, objective function, the decomposition and why it is exact, pseudocode for each search, correctness proofs, complexity, the three hospital-side designs and why two were rejected, alternatives considered, known limitations |
| **[docs/TESTING.md](docs/TESTING.md)** | The test plan: 105 named test cases with IDs, six testing techniques and what each catches, mutation-testing evidence that the suite can actually fail, a requirement→test traceability matrix, and known gaps |

The build is **portable by default**. `-march=native` is opt-in via
`make NATIVE=1`, because a binary built with it targets the build machine's
exact CPU and will `SIGILL` on an older one or in a container on different
hardware. Header dependencies are tracked (`-MMD -MP`), so editing a header
actually triggers a rebuild instead of silently leaving a stale object behind.

Then open **http://127.0.0.1:8080**. No dependencies beyond libc, pthreads and
a bare Node install — there is nothing to `npm install`. The dataset is
generated from a fixed seed, so every run produces the identical network,
fleet, roster and request stream.

---

## The problem, and what actually makes it hard

The naive reading is "find the nearest hospital". That is not the problem. The
problem is that the nearest hospital is usually the *wrong* one:

- it may not have the department,
- it may have the department but no specialist **on shift right now**,
- it may have the specialist but no free bed,
- it may have all three but have run out of the **medicine batch** the patient needs,
- or it may have everything and a queue so long that a hospital twice as far
  away discharges the patient sooner.

So the engine minimises **total operational cost = travel time + wait time**
subject to four independent capacity constraints, and has to do it in
microseconds while roads close and the fleet saturates around it.

## Algorithmic design

> Summarised here. The full treatment — correctness proofs, pseudocode,
> complexity derivations, rejected alternatives and known limitations — is in
> **[docs/ALGORITHM.md](docs/ALGORITHM.md)**.

### The decomposition

The objective is

```
min over (ambulance a, hospital h) of   t(a → incident) + t(incident → h) + wait(h)
```

The two travel terms share only the incident node, so minimising each
independently is exactly optimal. That turns an `|A| × |H|` product search into
two independent problems rooted at the incident.

**Ambulance side — backward early-exit Dijkstra.** Search the reverse graph
from the incident; the first settled node holding a free, capable vehicle is
provably the best one, because there is no per-vehicle penalty to trade
against distance. It settles ~360 nodes out of 50,000.

**Hospital side — an O(H) table scan.** See below.

### Why the hospital side is a table, not a search

The first version searched forward from the incident and stopped at the first
eligible hospital. Adding a queue wait **breaks that**: with a wait term, the
first hospital found is no longer the answer, because a hospital three minutes
further out with a twenty-minute shorter queue wins. The correct search must
keep expanding until the frontier distance reaches the best *total* cost found
so far — and that is expensive. Measured: **7,631 nodes settled instead of
1,635, and 1,314 µs per dispatch instead of 286 µs.**

So the hospital side stopped being a search. Instead, one backward Dijkstra per
hospital precomputes the travel time from **every node to every hospital**:

|  | |
|---|---|
| build | `H × O(E log V)` — 667 ms at H=60 |
| space | `H × V × 4` bytes — 11.4 MB at H=60, V=50k |
| query | **O(H)**, four cache lines |

The layout is **node-major**, so all 60 travel times for one node are
contiguous: a query touches 240 bytes instead of taking 60 strided cache
misses. Build writes are strided instead, which is the right trade — build
happens once, queries happen constantly.

The payoff is not just speed. Because the table holds the cost to *every*
hospital, the query can apply **any** cost function and **any** eligibility
rule without re-deriving anything — and it can explain itself, which is what
the decision log needs.

| hospital-side approach | µs/dispatch | nodes settled | correct with wait? |
|---|---|---|---|
| early-exit search | 286 | 1,635 | ✗ **wrong answer** |
| bounded search | 1,314 | 7,631 | ✓ |
| **distance table** | **51** | **0** | ✓ |

### Data structures

Everything is a flat contiguous array. No node objects, no linked lists in the
hot path, no allocation inside a query.

```
Graph      out_head[V+1] in_head[V+1]     CSR / forward-star
           out_e[E] in_e[E]               Edge{to,w} interleaved, 8B
           base_w[E] twin[E] edge_class[E] closure + styling bookkeeping
NodeState  {dist, stamp, parent, pad}     16B, four nodes per cache line
HospTable  d[V][H]                        node-major travel times
```

Three decisions that matter:

**Generation stamps instead of memset.** `dist[]` is never cleared between
queries; a node whose `stamp != gen` is implicitly infinite. Query reset is
O(1) rather than O(V) — at V=50k that alone saves ~50 µs per query.

**Bitmask capability screening.** Specialties, equipment and staffing are bits
in a `uint32`. Screening a candidate is `(have & need) == need`: one AND, one
compare, no strings, no per-candidate branching.

**Tile-order node numbering.** Nodes are numbered in 16×16 spatial blocks, so
a Dijkstra ball maps to a near-contiguous id range.

### Clinical routing

Dispatch is driven by presenting complaint, not distance:

| complaint | destination needs | vehicle needs | consumes |
|---|---|---|---|
| road accident | trauma | ALS + ventilator | 2× analgesic |
| cardiac arrest | cardiac | ALS + ventilator | 2× adrenaline |
| stroke | neuro | ALS | 1× anticoagulant |
| labour / delivery | obstetrics | **neonatal** | 1× oxytocin |
| severe burns | burns | ALS | 3× burn dressing |
| poisoning | toxicology | ALS | 2× antivenom |
| paediatric emergency | paediatrics | — | 1× paediatric AB |
| critical transfer | ICU **and** cardiac | ALS + ventilator | 1× sedative |

**Doctors are people on shifts, not hospital attributes.** 420 doctors sit on
three rotating 8-hour shifts. A department staffed by one doctor is therefore
uncovered two thirds of the day — which is exactly the scarcity the problem is
about. `world_set_clock()` recomputes every hospital's on-duty mask in
O(doctors) per tick, so the hot path stays a single AND. At 10:00, 140 doctors
cover 110 department-slots; at 03:00 the coverage map is different, and
routing changes with it.

This is the difference between *"Hospital C does cardiology"* and *"Hospital C
has a cardiologist on duty right now"* — and the second is the whole point.

**Medicine depletes and does not come back on its own.** Each hospital stocks
8 batch types. A dispatch consumes units permanently; only an explicit restock
replenishes them. A hospital with the right specialist and a free bed is still
rejected if the batch is out.

### Queues

Two priority structures, both O(log n):

- **Dijkstra frontier** — binary heap of packed `(dist<<32 | node)` keys with
  lazy deletion, so there is no decrease-key bookkeeping and no position array
  to keep cache-hot.
- **Request backlog** — a **min-max heap** (double-ended). `pop_min` is the
  most urgent, oldest-first within a tier, and is what runs when a vehicle
  comes back into service; `pop_max` is the least urgent, newest, and is what
  you shed under overload. Measured: push 27 ns, pop 307 ns over 1M entries.

A request that cannot be served is **queued by urgency, not dropped**, and
retried the moment capacity frees. A later critical case preempts a minor one
already waiting.

## Complexity

| Operation | Time | Space |
|---|---|---|
| Graph build | O(V + E) | O(V + E) |
| Query reset | **O(1)** (generation stamp) | — |
| Ambulance search | O(E′ log V), E′ = edges in the settled ball | O(V) per thread |
| Hospital selection | **O(H)** | H·V·4 bytes |
| Road closure / reopen | **O(1)** | O(1) |
| Capability / bed / medicine screen | **O(1)** per candidate | O(1) |
| Shift change (all hospitals) | O(doctors) | O(1) |
| Backlog push / pop-min / pop-max | O(log n) | O(n) |
| Distance table rebuild | H · O(E log V) | H·V·4 bytes |

## Results

Measured on a 2011 dual-core i3-2350M under frequency scaling. These are a
**floor**, not a target — the hardware-independent figures (nodes settled,
op counts, speedup ratios) are the ones that carry.

**Dataset:** 50,000 nodes · 200,100 edges · 5,000 villages · 60 hospitals ·
420 doctors · 200 ambulances. Graph 6.30 MB, distance table 11.44 MB,
workspace 1.14 MB per thread.

**Correctness.** Every path is cross-checked against a full Dijkstra *and* a
per-candidate A\*: **0 ETA mismatches over 300 requests**, and the distance
table matches the bounded search exactly.

**Dispatch latency** (20k requests, 1 core, µs)

| Variant | mean | p50 | p90 | p99 | settled |
|---|---|---|---|---|---|
| **distance table** | **50.8** | **28.4** | **116** | **361** | **359** |
| bounded search | 1,314 | 535 | 3,391 | 9,032 | 7,990 |
| full Dijkstra ×2 + scan | 17,809 | 17,726 | 18,803 | 20,014 | 100,000 |
| A\* per ambulance + hospital | 182,534 | 175,893 | 321,159 | 408,351 | — |

**19,671 dispatches/sec on one core**, 26× faster than the exhaustive search
and 350× faster than per-candidate A\*, with identical answers. 19,492 of
20,000 requests routed; 94.1% met their response SLA; 2.4 rejected
alternatives recorded per dispatch for the decision log.

**Latency is flat in network size**

| V | E | H | query | settled | table |
|---|---|---|---|---|---|
| 2,000 | 7,860 | 3 | 51 µs | 322 | 0.03 MB / 1.3 ms |
| 20,000 | 79,830 | 25 | 55 µs | 410 | 1.9 MB / 85 ms |
| 50,000 | 200,100 | 61 | 50 µs | 359 | 11.6 MB / 681 ms |
| 200,000 | 802,200 | 241 | 57 µs | 350 | 183.9 MB / 11.7 s |

**A 100× larger network costs 12% more per query.** The cost tracks the
density of nearby ambulances, not the size of the map. The table's `O(H·V)`
build and memory is the price, and it is visible in the last column.

**Dynamic closures.** 4,000 roads closed in 0.858 ms — 107 ns each, O(1), no
re-preprocessing. Post-closure dispatch latency is statistically unchanged
(52.0 µs vs 50.8 µs). This is why the engine does **not** use Contraction
Hierarchies: CH preprocessing would have to be redone on every closure. The
distance table does have to be rebuilt (672 ms), which is the honest cost of
the table approach.

**Fleet saturation.** When every vehicle is busy, the ambulance search has
nothing to early-exit on and degenerates into a full sweep. A per-request
search horizon caps it: **72× faster (29,910 ms → 412 ms for 4,000 requests,
p50 8,347 µs → 48 µs)**, at the cost of refusing vehicles more than 15 minutes
out. That is a policy trade, and it is deliberate — the horizon applies only to
the response leg. A specialist centre 40 minutes away may be the only place
that can treat the patient, so the transport leg is never horizon-capped.

**Concurrency.** 16,519 → 32,314 (1.96×) → 44,219 (2.68×) dispatches/sec on
1/2/4 threads. The graph is read-shared with zero locks; each thread owns a
private workspace.

## Running as a service

The engine is a **resident daemon**, not a per-request process and not a shared
library. A query costs ~50 µs; process spawn costs 1–5 ms and would rebuild
~700 ms of graph and table state to do it. A `.so` would keep state hot too,
but it welds the engine to the web server's lifetime — one segfault takes down
both — and gives up the engine's own thread scaling.

One plain-text command per line in, one JSON object per line out:

```
DISPATCH <node> <need_hosp> <need_amb> <need_med> <med_qty>
         <urgency> <sla_ms> <horizon> <geom>
COMMIT <amb> <hosp> <med> <qty>     reserve vehicle, bed and medicine
RELEASE <amb> <hosp>                vehicle back in service, queue drains
CLOCK <ms>                          set time of day; redrives doctor shifts
RESTOCK <hosp> <med> <qty>          replenish a medicine batch
CLOSE <edge> | OPEN <edge>          road closure, O(1)
REBUILD                             rebuild the distance table
ROADS <class> <from>                road geometry, paginated
HOSPITALS | FLEET | BOUNDS | NODE | STATS | QUIT
```

A dispatch reply carries the decision *and its justification*:

```json
{"ok":true,"amb":69,"hosp":2,"t_scene_ms":690000,"t_hosp_ms":1278000,
 "wait_ms":0,"t_total_ms":1968000,"considered":60,"latency_us":53,
 "rejected":[{"hosp":35,"travel_ms":642000,"why":"no specialist on duty"},
             {"hosp":42,"travel_ms":750000,"why":"no specialist on duty"},
             {"hosp":57,"travel_ms":1050000,"why":"no such department"}]}
```

**Measured over TCP, end to end, including JSON:** 42,616 dispatches/sec at 8
pipelined clients. A single un-pipelined client gets 3,645/sec and is bound by
round-trip latency, not the engine — so the bridge pipelines rather than
issuing one blocking call per emergency.

## Web simulation

`web/bridge.js` has **zero dependencies** — the WebSocket handshake and framing
are written directly against `node:crypto`. It holds a pool of 4 pipelined TCP
connections to the engine.

One page, two modes, one socket. A toggle switches between them with no
navigation and no reload.

### Story mode — one emergency at a time

The default. A case is narrated in plain English as it happens, paced for a
human rather than for a stress test: the call comes in, the engine searches,
an ambulance is assigned, a hospital is chosen, and then **each closer
hospital it rejected is named with the reason**. The map flies to the case and
labels the chosen hospital and the ruled-out ones in place, so the viewer never
has to look away from the map to follow the story.

```
🚨  Road accident reported in District 12
🔎  Checking every ambulance and hospital…  60 hospitals reachable
🚑  Ambulance 171 is on the way — 9.5 km away, reaches the patient in 13 min
🏥  Taking them to Oakwood Hospital — specialist on shift, bed free, medicine in stock
❌  Not Sunrise Regional Hospital — only 9.6 min away, but no specialist on duty
❌  Not Highland Regional Hospital — only 12.6 min away, but no specialist on duty
    total time to treatment 26 min · decided in 1,129 µs
```

Pacing is driven by the client: it asks the engine for exactly one case
(`oneshot`), plays the story, then asks for the next. Nothing is dropped and
nothing overlaps.

### Live city view — the whole network under load

The operational dashboard. Full road network, every ambulance and hospital,
all the meters, and the controls that break things: surge injection, 2,000
road closures, medicine restock, and a jump between day and night shift so the
staffing map visibly changes.

The full road network is drawn — **100,050 segments** (3,138 highway, 7,317
arterial, 89,595 local) as three multi-polylines on a canvas renderer, not
100k DOM nodes. Local streets only draw past a zoom threshold, since they are
sub-pixel when the whole district is in view. No tile server is contacted; the
demo runs with no internet.

Telemetry covers decision time, fleet utilisation, bed and medicine meters,
doctors on shift, the simulated clock, queue depth, backlog size, and requests
served after waiting. Hospital markers turn amber below 35% beds and grey when
full. A **why this hospital** panel carries the same rejection breadcrumbs as
story mode, and incidents get a floating label on the map as they appear.

## Tests

Two suites, 105 assertions, both exiting non-zero on failure. Full test plan
with per-case IDs, techniques and traceability: **[docs/TESTING.md](docs/TESTING.md)**.

```
make test          src/test.c               74 checks — the engine, in-process
make test-protocol scripts/protocol_test.sh  31 checks — the daemon, over TCP
make test-all      both                     105 checks, 0 failed, under 4 s
```

Every check is **named**, and a failure reports what was expected against what
happened. The engine suite runs on an 8,000-node network so it finishes in
under a second; the benchmark cross-checks the same properties at full scale.

**The engine suite** (`make test`), 16 groups:

```
 1. shortest-path correctness — four independent implementations agree
 2. queue wait genuinely changes the destination
 3. resource exhaustion — each constraint rejects on its own
 4. road closures
 5. doctor shifts change routing over the day
 6. urgency preemption in the backlog
 7. resource accounting is conserved
 8. the response horizon bounds the ambulance search only
 9. graph invariants — the CSR really is the network it claims to be
10. engine search vs an independent Bellman-Ford reference
11. the A* heuristic is admissible — it can never overestimate
12. reconstructed routes are real drivable roads, not just numbers
13. distance table lifecycle — build, stale, rebuild
14. backlog queue invariants under stress
15. determinism — the same input always produces the same decision
16. boundary and degenerate inputs

74 checks, 0 failed
```

What it actually pins down:

- the distance table agrees with the bounded search, an exhaustive full
  Dijkstra, and a per-candidate A\* — and separately with a **Bellman-Ford
  reference that shares no code with any of them**, so a bug in the common
  heap or in the generation-stamp reset cannot hide;
- **swamping the chosen hospital's queue diverts the patient to one that is
  genuinely further away by road**, and the new choice still matches
  exhaustive search — this is the property a travel-time index cannot express;
- each constraint refuses on its own: no department, no doctor on shift, no
  bed, no medicine, no free vehicle — and restocking a single hospital revives
  routing to exactly that hospital;
- the A\* heuristic never overestimates, over thousands of sampled nodes —
  which is the precondition for A\* returning the optimum at all;
- reconstructed routes are **checked edge by edge against the road network**,
  and re-summing the segments reproduces the reported drive time exactly;
- a node cut off by closures reaches zero hospitals, and **reopening the roads
  restores the byte-identical decision and the byte-identical distance table**;
- some cases route differently at 03:00 than at 10:00;
- a critical case preempts everything already queued, equal urgency is served
  oldest-first, and randomised interleavings of arrivals and dispatches always
  yield the true most-urgent case;
- one commit takes exactly one bed, one vehicle and one queue slot, and
  medicine does **not** come back on release — only a restock refills it;
- a horizon exactly equal to the drive time accepts the vehicle and one
  millisecond tighter refuses it.

**The daemon suite** (`make test-protocol`) drives the engine the way the web
bridge does — over a TCP socket, using bash's own `/dev/tcp`, no extra
dependencies. It covers reply framing and ordering under pipelining, route
geometry, state changes through `COMMIT`/`RELEASE`/`CLOCK`/`RESTOCK`, closure
staleness flags, 200 pipelined dispatches, and **negative cases**: an
out-of-range node, an unknown verb, a bad edge id, a bad ambulance id and an
empty line are each rejected by name, and the connection survives all of them.

**The suite is verified to fail.** Six single-line mutations were injected into
the engine and each was caught by the right assertions — including reverting
the hospital search to the naive "stop at the first eligible hospital", which
immediately breaks two checks. The full mutation table is in
[docs/TESTING.md](docs/TESTING.md#6-mutation-testing-proof-the-suite-can-fail).

## Deploying

A C daemon plus a raw-socket Node bridge cannot run on a static or serverless
host. It needs one container — which Render, Google Cloud Run, Fly.io and
Railway all accept as a plain Dockerfile.

**Render free plan**, using the included `render.yaml`:

1. push this repo to GitHub
2. Render dashboard → **New → Blueprint** → pick the repo
3. it reads `render.yaml`, builds the Dockerfile, and gives you a URL

The free plan is 512 MB RAM and 0.1 CPU, and **spins down after 15 minutes
without traffic**, taking about a minute to wake. The monthly allowance is 750
instance-hours — and a calendar month is 744 hours, so one service kept awake
fits inside the free tier exactly. Point any free uptime pinger at the URL
every 10 minutes and it never sleeps.

The image is two stages: the first compiles the engine with portable flags and
**runs the test suite as a build step**, so a failing test fails the deploy. The
second carries just the binary, `web/`, and Node. `docker-start.sh` starts the
engine, polls until it accepts connections, then `exec`s the bridge so the
container's lifetime tracks the web process. The bridge honours `$PORT`.

Resident footprint is ~18 MB for the engine plus Node, so the smallest instance
anywhere is enough. On 0.1 CPU the one-time distance-table build takes a few
seconds instead of 680 ms; queries are unaffected.

## Edge cases

| Case | Handling |
|---|---|
| No road route | `ok:false`, reason names which half failed |
| Specialist unavailable at nearest centre | Core path — rejected with reason, search continues outward |
| Specialist exists but off shift | Distinguished from "no department" in the log |
| All ambulances occupied | Queued by urgency in a min-max heap, retried on release |
| Hospital bed full | Rejected, next-best chosen |
| Medicine batch depleted | Rejected, next-best chosen; `RESTOCK` clears it |
| Simultaneous high-priority requests | Urgency preempts in the backlog |
| Roads close mid-operation | O(1) weight update; table rebuild flagged |
