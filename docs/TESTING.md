# HealthWay — Test Plan and Test Cases

The evidence for the claims made in [ALGORITHM.md](ALGORITHM.md). Every
assertion listed here is executable, named, and fails loudly.

```
$ make test-all

74 checks, 0 failed        src/test.c            — engine, in-process
31 checks, 0 failed        scripts/protocol_test.sh — daemon, over TCP
────────────────────────
130 checks, 0 failed       in under 4 seconds
```

**Contents**

1. [How to run](#1-how-to-run)
2. [Test strategy](#2-test-strategy)
3. [Level 1 — engine suite (74 checks)](#3-level-1--engine-suite-74-checks)
4. [Level 2 — daemon protocol suite (31 checks)](#4-level-2--daemon-protocol-suite-31-checks)
5. [Level 3 — benchmark cross-checks](#5-level-3--benchmark-cross-checks)
6. [Mutation testing: proof the suite can fail](#6-mutation-testing-proof-the-suite-can-fail)
7. [Requirement traceability](#7-requirement-traceability)
8. [Test environment and reproducibility](#8-test-environment-and-reproducibility)
9. [Known gaps](#9-known-gaps)

---

## 1. How to run

```bash
make test            # engine suite      — 74 assertions, ~0.9 s
make test-protocol   # daemon suite      — 31 assertions, ~3 s
make test-all        # both
./bench --quick      # benchmark, includes its own cross-checks, ~40 s
```

Every target **exits non-zero if any check fails**, so all of them work as CI
gates.

Output is one line per check, named in plain English, with expected-vs-actual
printed on failure:

```
3. resource exhaustion — each constraint rejects on its own
  PASS  no hospital with the specialty -> request refused
  PASS  department staffed by nobody -> request refused
  FAIL  rejection is attributed to staffing, not to beds
        got reason 3 (no free bed)
```

---

## 2. Test strategy

The engine is a decision system, so "it compiles and returns a number" is not
evidence of anything. Six distinct techniques are used, each catching a class
of bug the others cannot.

| # | Technique | What it catches | Where |
|---|---|---|---|
| 1 | **Differential testing** — four implementations answer the same query | A wrong optimisation. The fast path is compared against a bounded search, an exhaustive Dijkstra, and a per-candidate A\* | U-1.\* |
| 2 | **Independent reference** — Bellman-Ford, sharing no code with the engine | A bug in the *shared* machinery (binary heap, generation stamps, settling rule) that all four implementations above would inherit | U-10.\* |
| 3 | **Structural invariants** — exhaustive checks over all 31,800 edges | A malformed data structure that produces plausible but wrong routes | U-9.\* |
| 4 | **Fault injection** — break one constraint at a time, verify the others still work | Constraints that are not actually independent, and misattributed rejection reasons | U-3.\*, U-4.\* |
| 5 | **Property and boundary testing** — constructed scenarios, exact off-by-one pairs | Logic that is right in the middle of the range and wrong at its edges | U-2.\*, U-16.\* |
| 6 | **Protocol / negative testing** — malformed input over a real socket | Crashes, desynchronised parsers, and lies about state | P-3.\* |

Two properties are deliberately tested from **both** sides:

- **Resource accounting** is checked in-process (U-7.\*) *and* over the wire
  against the daemon's shared locked state (P-4.\*).
- **Table staleness** is checked in-process (U-13.\*) *and* as a wire contract
  the daemon must honestly report (P-5.\*).

### Test ID scheme

`U-<group>.<index>` for the engine suite, `P-<group>.<index>` for the protocol
suite. IDs are scoped to their group, so adding a case to one group does not
renumber any other.

---

## 3. Level 1 — engine suite (86 checks)

**File:** `src/test.c` · **Run:** `make test` · **Runtime:** ~0.9 s

**Fixture.** A deterministic 100×80 network — 8,000 nodes, 31,800 directed
edges, 12 hospitals, 40 ambulances, 72 doctors, 500 villages — built from fixed
seeds (`0xC0FFEE` for the graph, `0xBEEF01` for the world). Smaller than the
benchmark's 50,000-node network on purpose, so the suite finishes in about a
second and a reviewer will actually wait for it. Nothing about the algorithm is
size-dependent; the benchmark covers full scale separately.

**Isolation.** `reset_state()` restores every mutable field — ambulance
positions and busy flags, beds, medicine, queue depths, the clock — to its
freshly-built value between groups, so a group cannot pass or fail because of
what the previous one did.

#### 1. shortest-path correctness — four independent implementations agree

Technique: **differential testing**. 60 requests across all 8 specialties, 4 urgencies and 8 medicine batches are answered by all four implementations in the codebase — the distance table, the bounded search, an exhaustive full Dijkstra, and a per-candidate A\*.

| ID | Assertion |
|---|---|
| `U-1.1` | distance table == bounded search |
| `U-1.2` | distance table == exhaustive full Dijkstra |
| `U-1.3` | distance table == per-candidate A* |
| `U-1.4` | most requests route on a healthy network |

> **Why it matters.** If three independently written algorithms agree on 60 varied inputs, the odds that all three share the same bug are small. This is the strongest single argument for correctness in the suite.

#### 2. queue wait genuinely changes the destination

Technique: **property testing on a constructed scenario**. Dispatch once on a clean network, note the chosen hospital, then swamp *only that hospital's* queue (`queue_len = 400`) and dispatch the identical request again. Nothing else changes.

| ID | Assertion |
|---|---|
| `U-2.1` | a long queue diverts the patient to a further hospital |
| `U-2.2` | the new destination is genuinely further away by road |
| `U-2.3` | wait-aware choice still matches exhaustive search |

> **Why it matters.** This is the property that a travel-time-only proximity index cannot express, and the entire reason the hospital side is a table rather than a nearest-neighbour lookup. The patient must be diverted to a hospital that is **genuinely further away by road**, and the new answer must still match exhaustive search.

#### 3. resource exhaustion — each constraint rejects on its own

Technique: **fault injection, one constraint at a time**. Each constraint is broken across every hospital in turn — department removed, roster emptied, beds zeroed, medicine batch drained, fleet marked busy — with the other three left healthy. State is restored between cases.

| ID | Assertion |
|---|---|
| `U-3.1` | no hospital with the specialty -> request refused |
| `U-3.2` | department staffed by nobody -> request refused |
| `U-3.3` | rejection is attributed to staffing, not to beds |
| `U-3.4` | every bed full -> request refused |
| `U-3.5` | medicine batch depleted everywhere -> request refused |
| `U-3.6` | depletion is attributed to medicine |
| `U-3.7` | restocking one hospital revives routing |
| `U-3.8` | all ambulances busy -> no vehicle assigned |

> **Why it matters.** Four independent capacity constraints have to refuse *independently*, and the engine has to attribute the refusal to the right one. Attributing a staffing shortage to a bed shortage would make the decision log actively misleading.

#### 4. road closures

Technique: **state round-trip**. Every road leaving the incident node is closed, the table is rebuilt, and the request is retried; then the roads are reopened and rebuilt again.

| ID | Assertion |
|---|---|
| `U-4.1` | node cut off from the network -> no route |
| `U-4.2` | an isolated node can reach zero hospitals |
| `U-4.3` | reopening the roads restores the identical decision |

> **Why it matters.** Closures are a core requirement, so an isolated node must fail cleanly rather than return a route through a closed road — and reopening must be lossless, not approximately lossless.

#### 5. doctor shifts change routing over the day

Technique: **differential testing across a state axis**. 40 requests are answered at 10:00 and again at 03:00, with nothing else changed.

| ID | Assertion |
|---|---|
| `U-5.1` | some cases route differently at 03:00 than at 10:00 |
| `U-5.2` | doctors are on duty in every shift |

> **Why it matters.** This is what separates *"the hospital has a cardiology department"* from *"the hospital has a cardiologist on duty right now"*. If routing were identical across the day, the shift model would be decorative.

#### 6. urgency preemption in the backlog

Technique: **ordering invariants**, plus a 4,000-element monotonicity sweep with randomised urgencies.

| ID | Assertion |
|---|---|
| `U-6.1` | a critical case preempts everything already waiting |
| `U-6.2` | equal urgency is served oldest-first |
| `U-6.3` | the least urgent is what gets shed under overload |
| `U-6.4` | 4,000 mixed-urgency requests drain in strict priority order |

> **Why it matters.** Under overload the backlog decides who is treated and who waits. Both ends of it have to be right: the wrong `pop_min` delays a critical case, the wrong `pop_max` sheds one.

#### 7. resource accounting is conserved

Technique: **conservation checking**. 32 dispatches are committed, then all are released, with beds, medicine, vehicle flags and queue depths totalled before, between and after.

| ID | Assertion |
|---|---|
| `U-7.1` | one commit takes exactly one bed |
| `U-7.2` | one commit occupies exactly one ambulance |
| `U-7.3` | one commit adds exactly one patient to a queue |
| `U-7.4` | medicine is consumed, not just counted |
| `U-7.5` | releasing returns every vehicle to service |
| `U-7.6` | releasing drains the hospital queues |
| `U-7.7` | medicine does NOT come back on release — only a restock refills it |

> **Why it matters.** Resources that leak or duplicate would make every subsequent decision wrong in a way no single-query test can see. Note the deliberately asymmetric case: medicine must **not** come back on release — only a restock refills it.

#### 8. the response horizon bounds the ambulance search only

Technique: **policy boundary testing**. The fleet is reduced to one distant vehicle, and the same request is run with a 1-second horizon and with none.

| ID | Assertion |
|---|---|
| `U-8.1` | a vehicle beyond the horizon is refused and flagged as such |
| `U-8.2` | the same request succeeds with no horizon |
| `U-8.3` | the transport leg is never horizon-capped: a distant specialist centre stays reachable |

> **Why it matters.** The horizon is a deliberate policy trade, and it must apply to the **response leg only**. A specialist centre 40 minutes away may be the only place that can treat the patient, so capping the transport leg would be a clinical error, not an optimisation.

#### 9. graph invariants — the CSR really is the network it claims to be

Technique: **structural invariants over the whole graph** — all 8,000 nodes and 31,800 directed edges, exhaustively, not sampled.

| ID | Assertion |
|---|---|
| `U-9.1` | CSR offsets are monotone and total exactly E |
| `U-9.2` | every road's two directions are twins of equal weight |
| `U-9.3` | fwd->rev map lands on the mirror edge (this is what makes closure O(1)) |
| `U-9.4` | no zero-cost or infinite road in the pristine network (Dijkstra needs w>0) |
| `U-9.5` | closing a road actually sets it impassable |
| `U-9.6` | close/open is a lossless round trip on both graph directions |

> **Why it matters.** Every search result rests on the CSR being a faithful encoding of the network. A broken twin or `fwd_to_rev` entry would make closures silently one-directional, which no routing test would reliably catch.

#### 10. engine search vs an independent Bellman-Ford reference

Technique: **differential testing against an independent reference**. `ref_spfa()` in `src/test.c` is a label-correcting Bellman-Ford with a FIFO queue — deliberately not Dijkstra, and deliberately not sharing the engine's binary heap, generation stamps or settling rule.

| ID | Assertion |
|---|---|
| `U-10.1` | every precomputed table cell equals the Bellman-Ford distance |
| `U-10.2` | A* returns the true optimum, not a good-enough path |
| `U-10.3` | distances obey the triangle inequality (the metric is a real metric) |

> **Why it matters.** Test group 1 compares the engine against itself: four implementations that share `search.h`. A bug in the shared heap or in the generation-stamp reset would be invisible to all four. Bellman-Ford shares nothing with them, so it can see what they cannot.

#### 11. the A* heuristic is admissible — it can never overestimate

Technique: **mathematical property verification**. For three targets, true distances are computed by Bellman-Ford over the reverse graph, then every 11th node is checked against the heuristic.

| ID | Assertion |
|---|---|
| `U-11.1` | straight-line/top-speed bound never exceeds the true drive time |
| `U-11.2` | the heuristic is exactly 0 at the target |
| `U-11.3` | the admissibility sample is not trivially small |

> **Why it matters.** A\* returns the optimum **only if** the heuristic never overestimates. Since `astar_h` divides straight-line metres by the fastest road speed in the network, admissibility depends on `max_speed_mms` genuinely being the maximum — a property that would break silently if a faster road class were ever added.

#### 12. reconstructed routes are real drivable roads, not just numbers

Technique: **end-to-end reconstruction**. Parent pointers are walked back into a node list, then every consecutive pair is looked up in the CSR to confirm a real road exists, and the weights are re-summed.

| ID | Assertion |
|---|---|
| `U-12.1` | the ambulance route starts at the vehicle, ends at the patient, and every step is a real road |
| `U-12.2` | summing the road segments reproduces the reported response time exactly |
| `U-12.3` | enough routes were checked to mean something |
| `U-12.4` | the transport route reconstructs from A* parents and costs what A* said |
| `U-12.5` | path reconstruction refuses rather than overflowing a short buffer |

> **Why it matters.** The decision log and the map both draw these routes. A path that reconstructs into a sequence the ambulance cannot actually drive would be a convincing-looking lie, and the reported drive time has to be the sum of the roads actually taken.

#### 13. distance table lifecycle — build, stale, rebuild

Technique: **lifecycle testing** — build, verify, rebuild-identical, invalidate, rebuild-restored — with full-table `memcmp` as the equality test.

| ID | Assertion |
|---|---|
| `U-13.1` | a hospital is zero minutes from itself |
| `U-13.2` | a rebuild bumps the generation counter |
| `U-13.3` | rebuilding an unchanged network reproduces the table bit for bit |
| `U-13.4` | cutting a hospital off makes it unreachable from the whole network |
| `U-13.5` | reopening the roads restores the original table exactly |

> **Why it matters.** The table is the engine's main correctness risk: it is precomputed, so it can be *stale* rather than merely wrong. These checks pin the whole invalidation cycle, including that an unchanged network rebuilds bit for bit.

#### 14. backlog queue invariants under stress

Technique: **stress and reference comparison**. 4,000 randomised pushes drain from the max end; 3,000 randomised interleaved push/pop operations are checked against a brute-force linear-scan reference; degenerate sizes of 1 and 2 are checked explicitly.

| ID | Assertion |
|---|---|
| `U-14.1` | the queue grew past its initial capacity without loss |
| `U-14.2` | shedding from the tail always sheds the least urgent first |
| `U-14.3` | interleaved arrivals and dispatches always yield the true most-urgent case |
| `U-14.4` | the queue and the reference agree on how many are waiting |
| `U-14.5` | a single waiting case is both the most and the least urgent |
| `U-14.6` | with two waiting, both ends resolve correctly |

> **Why it matters.** Min-max heaps are easy to get subtly wrong at the boundaries between min and max levels, and the bug shows up only on specific interleavings. A reference comparison over randomised interleaving finds what a scripted sequence does not.

#### 15. determinism — the same input always produces the same decision

Technique: **idempotence and reproducibility**. The same request is answered 25 times; 200 unrelated searches are then run between two identical queries; the world is rebuilt from the same seed.

| ID | Assertion |
|---|---|
| `U-15.1` | a query leaves no residue: 25 repeats give an identical decision |
| `U-15.2` | O(1) generation-stamp resets stay correct across 200 intervening searches |
| `U-15.3` | the same seed rebuilds the identical fleet layout |

> **Why it matters.** The `O(1)` generation-stamp reset (§11 of ALGORITHM.md) deliberately leaves stale data in `dist[]` between queries. If that reset were wrong, queries would contaminate each other — and the symptom would be intermittent, which is the worst kind of bug to have in a dispatch system.

#### 16. boundary and degenerate inputs

Technique: **boundary-value and degenerate-input testing**, including the exact off-by-one on the horizon comparison.

| ID | Assertion |
|---|---|
| `U-16.1` | an incident at a hospital's door has zero transport time |
| `U-16.2` | a vehicle already on scene has zero response time |
| `U-16.3` | a patient needing no medicine still routes with every batch empty |
| `U-16.4` | the same patient needing one unit is refused |
| `U-16.5` | a horizon exactly equal to the drive time still accepts the vehicle |
| `U-16.6` | one millisecond tighter and that vehicle is out of reach |
| `U-16.7` | equipment no vehicle carries is refused, not approximated |
| `U-16.8` | the decision log is capped at MAX_REJECT entries |
| `U-16.9` | logged alternatives are all closer than the chosen hospital, nearest first |

> **Why it matters.** Zero-distance cases, empty requirement masks and exact-boundary comparisons are where off-by-one errors live. The horizon pair is deliberately adjacent: `horizon = drive_time` must accept and `horizon = drive_time - 1` must refuse.

#### 17. real city rosters

Technique: **exhaustive data-driven validation** — every one of the 34 compiled-in rosters is built and checked, not a sample. A dataset defect that leaves one city unable to serve one case type produces a permanently failing dispatch, and the symptom looks like an algorithm bug.

| ID | Assertion |
|---|---|
| `U-17.1` | at least one city roster is compiled in |
| `U-17.2` | an out-of-range city index is refused, not clamped |
| `U-17.3` | every city builds a world with its full roster |
| `U-17.4` | hospital departments are exactly what the dataset lists |
| `U-17.5` | the emergency bed pool is derived from the reported total |
| `U-17.6` | inferred departments are always a subset of the departments |
| `U-17.7` | no two hospitals are placed on the same junction |
| `U-17.8` | every department has at least one doctor on some shift |
| `U-17.9` | every city can serve all eight case types somewhere in its roster |
| `U-17.10` | the same seed rebuilds the identical city |
| `U-17.11` | on a real roster the table still equals a fresh search |
| `U-17.12` | most requests route on a real roster |

> **Why it matters.** `U-17.9` is the one that earns its keep: 29 of the 34 cities list no burns unit anywhere, and without the referral fallback every burns call in those cities would fail forever. `U-17.11` re-runs the core exactness guarantee (§1) against a roster nobody designed for the algorithm — the hospital count, the department distribution and the bed spread are all whatever the dataset happened to say.

---

## 4. Level 2 — daemon protocol suite (44 checks)

**File:** `scripts/protocol_test.sh` · **Run:** `make test-protocol` ·
**Runtime:** ~3 s

`src/test.c` exercises the engine by calling it directly. This suite exercises
it the way the web bridge actually does: **over a TCP socket**, one plain-text
command per line in, one JSON object per line out.

The client is bash's own `/dev/tcp` — no `curl`, no `nc`, no Node, no Python.
The script starts the daemon on a scratch port (19099 by default), **polls
until it accepts connections** rather than sleeping a fixed interval so a slow
machine does not produce a false failure, and kills it on exit via a `trap`.

What this layer adds over Level 1: reply framing and ordering under
pipelining, error handling for malformed input, connection survival after bad
commands, and the fact that state-changing commands change the *daemon's*
shared, lock-protected state rather than a single-threaded copy of it.

#### 1. handshake and static map data

Sends five commands in a single write and reads five replies.

| ID | Assertion |
|---|---|
| `P-1.1` | five pipelined commands return five replies, in order |
| `P-1.2` | STATS reports engine state |
| `P-1.3` | BOUNDS returns the map extent |
| `P-1.4` | HOSPITALS returns the hospital list |
| `P-1.5` | FLEET returns the ambulance list |
| `P-1.6` | NODE resolves a village to a road node |
| `P-1.7` | the daemon serves a non-empty network |

> **Why it matters.** Establishes that the daemon answers, that framing is one-JSON-object-per-line, and that replies come back **in order** — which the bridge's connection pooling depends on.

#### 2. dispatch over the wire

A full dispatch over the socket, with and without route geometry.

| ID | Assertion |
|---|---|
| `P-2.1` | a well-formed dispatch succeeds |
| `P-2.2` | the reply carries its own decision latency |
| `P-2.3` | the reply carries the rejected alternatives |
| `P-2.4` | the reply names both an ambulance and a hospital |
| `P-2.5` | geom=1 returns the response leg as route geometry |
| `P-2.6` | geom=1 returns the transport leg as route geometry |
| `P-2.7` | both legs start at the incident, so the drawn route is continuous |

> **Why it matters.** The engine is only useful through this interface. The reply has to carry the decision, its own measured latency, the rejected alternatives, and — with `geom=1` — two route legs that join at the incident so the drawn line is continuous.

#### 3. malformed input is refused, not crashed on

**Negative testing**: an out-of-range node, an unknown verb, an out-of-range edge id, an out-of-range ambulance id, and an empty line — followed by a valid command on the same connection.

| ID | Assertion |
|---|---|
| `P-3.1` | an out-of-range node is rejected by name |
| `P-3.2` | an unknown verb is rejected by name |
| `P-3.3` | an out-of-range edge id is rejected by name |
| `P-3.4` | an out-of-range ambulance id is rejected by name |
| `P-3.5` | the connection survives every bad command |

> **Why it matters.** A public daemon must reject malformed input by name rather than crash, and — critically — the connection must survive it. The trailing valid command proves the parser resynchronised.

#### 4. state-changing commands really change state

State is read via `STATS`, mutated via `COMMIT` / `RELEASE` / `CLOCK` / `RESTOCK`, and read back.

| ID | Assertion |
|---|---|
| `P-4.1` | COMMIT is accepted |
| `P-4.2` | COMMIT takes exactly one bed |
| `P-4.3` | COMMIT occupies exactly one ambulance |
| `P-4.4` | RELEASE returns the bed and the vehicle |
| `P-4.5` | CLOCK moves the roster: 03:00 and 10:00 have different staffing |
| `P-4.6` | RESTOCK reports how many units it actually added |

> **Why it matters.** Mirrors unit group 7, but through the wire and against the daemon's own shared, lock-protected state rather than a single-threaded test harness.

#### 5. road closures and index rebuild

`CLOSE` → `REBUILD` → `OPEN` → `REBUILD`, checking the staleness flag and the generation counter.

| ID | Assertion |
|---|---|
| `P-5.1` | CLOSE flags the distance table as stale |
| `P-5.2` | CLOSE reports its own O(1) cost |
| `P-5.3` | REBUILD reports the new table generation |
| `P-5.4` | each rebuild advances the generation counter |

> **Why it matters.** The daemon must **admit** that a closure invalidated the table rather than quietly serving stale routes. This is the wire-level counterpart of unit group 13.

#### 6. throughput under pipelining

200 dispatches written to one socket in a single burst, then 200 replies read back.

| ID | Assertion |
|---|---|
| `P-6.1` | 200 pipelined dispatches return 200 replies |
| `P-6.2` | every pipelined reply is a successful decision |

> **Why it matters.** The bridge pipelines rather than issuing one blocking round trip per emergency. This confirms the daemon actually supports that, with no reply lost or reordered under depth.
---

#### 7. real city rosters over the wire

Loads a real hospital roster into a running daemon and checks that the swap is
total: roster size, named hospitals, dispatch afterwards, and a clean return to
the synthetic district. This is the level that catches a world swap which
leaves the distance table sized for the *previous* roster.

| ID | Assertion |
|---|---|
| `P-7.1` | CITIES lists the compiled-in rosters |
| `P-7.2` | the engine starts on the synthetic district |
| `P-7.3` | CITIES carries the dataset attribution |
| `P-7.4` | CITY loads a real roster |
| `P-7.5` | CITY warns that live mission ids are stale |
| `P-7.6` | STATS reports which roster is loaded |
| `P-7.7` | HOSPITALS carries real hospital names |
| `P-7.8` | HOSPITALS carries the reported bed count |
| `P-7.9` | HOSPITALS flags inferred departments |
| `P-7.10` | a dispatch resolves against the real roster |
| `P-7.11` | an out-of-range city index is refused |
| `P-7.12` | DISTRICT returns to the synthetic world |
| `P-7.13` | the roster count follows the world and restores on the way back |

> **Why it matters.** `P-7.5` is a contract, not a nicety: the fleet is rebuilt
> by the swap, so every ambulance and hospital id the client is holding becomes
> meaningless at that instant. The daemon says so in the reply rather than
> leaving the bridge to release a bed that no longer exists. `P-7.13` is the
> round trip — a swap that half-applies would leave the district reporting a
> city's hospital count.

---

## 5. Level 3 — benchmark cross-checks

`./bench` is a performance harness, but two of its sections are correctness
checks that run at **full scale** — 50,000 nodes, 60 hospitals, 200 ambulances
— rather than the 8,000-node fixture the unit suite uses:

| Check | What it does | Result |
|---|---|---|
| §2 correctness | 300 requests answered by the bounded search, an exhaustive full Dijkstra, and a per-candidate A\*; ETAs compared | **0 mismatches** |
| §4b table cross-check | The same requests answered by the distance table and by the bounded search | **0 mismatches** |
| §6 DEPQ order check | 1,000,000 mixed-urgency entries pushed, half drained, urgency order verified | **correct** |

These do not assert-and-exit like `make test` does; they print the mismatch
count, and a non-zero count is reported in red. They exist to confirm that the
properties proved on the small fixture still hold at production scale.

---

## 6. Mutation testing: proof the suite can fail

A green suite proves nothing on its own — a suite of 130 assertions that
happen to be vacuously true would look exactly the same. So each defence was
verified by **deliberately breaking the code it defends** and confirming the
right tests go red.

Six single-line mutations, each reverted immediately after:

| # | Mutation | Tests that caught it |
|---|---|---|
| **M1** | `astar_h` doubled — the A\* heuristic now overestimates, breaking admissibility | 3 failures: `U-1.3` (table ≠ A\*), `U-10.2` (A\* no longer optimal), `U-11.1` (heuristic exceeds true drive time) |
| **M2** | `graph_open_edge` no longer restores the reverse direction — reopening a road leaves it closed one way | 5 failures: `U-4.3`, `U-9.3`, `U-9.6`, `U-10.1`, `U-13.5` |
| **M3** | `note_reject` insertion sort removed — the decision log stops ordering alternatives | 1 failure: `U-16.9` (logged alternatives out of order) |
| **M4** | Medicine check deleted from `hosp_reject_reason` | 2 failures: `U-3.5`, `U-16.4` |
| **M5** | `depq_max_idx` always returns child 1 — the min-max heap picks the wrong maximum | 1 failure: `U-14.2` (shedding no longer sheds the least urgent) |
| **M6** | Bounded hospital search terminates at the first settled node — i.e. reverts to the *incorrect* Design A of ALGORITHM.md §6.1 | 2 failures: `U-1.1`, `U-2.3` |

Two of these are worth dwelling on.

**M5 is why the stress group exists.** The mutation slipped past `U-6.3` — the
original hand-written "least urgent gets shed" check — because on that small
scripted input, child 1 happened to *be* the maximum. It was caught only by
`U-14.2`, which drains 4,000 randomised entries. Scripted sequences test the
path you thought of; randomised sweeps test the one you didn't.

**M6 restores the design the engine deliberately rejected.** It reverts the
hospital search to "stop at the first eligible hospital", which is the natural
implementation and the wrong one. That the suite immediately reports two
failures is the direct executable evidence for the argument in ALGORITHM.md
§6.1 — that a queue term breaks the early-exit property.

To reproduce any of these: apply the one-line change, run `make test`, revert.

---

## 7. Requirement traceability

Every functional requirement mapped to the checks that cover it.

| Requirement | Covered by | Level |
|---|---|---|
| Routes are true shortest paths | `U-1.1`–`U-1.3`, `U-10.1`–`U-10.3`, `U-11.1`–`U-11.3`, bench §2 | 1, 3 |
| The chosen hospital minimises **time to treatment**, not distance | `U-2.1`–`U-2.3`, `U-1.1` | 1 |
| Specialty must exist at the destination | `U-3.1`, `U-3.3` | 1 |
| A specialist must be **on shift now**, not merely employed | `U-3.2`, `U-3.3`, `U-5.1`, `U-5.2`, `P-4.5` | 1, 2 |
| A bed must be free | `U-3.4`, `U-7.1` | 1 |
| The medicine batch must be in stock, and deplete when used | `U-3.5`, `U-3.6`, `U-3.7`, `U-7.4`, `U-7.7`, `U-16.3`, `U-16.4` | 1 |
| The vehicle must carry the required equipment | `U-16.7`, `U-15.1` | 1 |
| Roads close and reopen during operation | `U-4.1`–`U-4.3`, `U-9.5`, `U-9.6`, `U-13.4`, `U-13.5`, `P-5.1`–`P-5.4` | 1, 2 |
| Unreachable incidents fail cleanly | `U-4.1`, `U-4.2`, `U-13.4` | 1 |
| Fleet saturation is survivable | `U-3.8`, `U-8.1`–`U-8.3` | 1 |
| Simultaneous requests are prioritised by urgency | `U-6.1`–`U-6.4`, `U-14.1`–`U-14.6` | 1 |
| Resources are conserved across commit and release | `U-7.1`–`U-7.7`, `P-4.2`–`P-4.4` | 1, 2 |
| Every decision explains itself | `U-16.8`, `U-16.9`, `P-2.3` | 1, 2 |
| Routes are drawable and drivable | `U-12.1`–`U-12.5`, `P-2.5`–`P-2.7` | 1, 2 |
| The same input always gives the same answer | `U-15.1`–`U-15.3` | 1 |
| The engine is usable as a service | `P-1.1`–`P-1.7`, `P-2.1`–`P-2.4`, `P-6.1`, `P-6.2` | 2 |
| Bad input does not take the engine down | `P-3.1`–`P-3.5`, `P-7.11` | 2 |
| Real hospital rosters load and behave like the synthetic one | `U-17.1`–`U-17.12`, `P-7.1`–`P-7.13` | 1, 2 |
| Data provenance is carried to the client, not asserted in prose | `U-17.4`, `U-17.6`, `P-7.3`, `P-7.7`–`P-7.9` | 1, 2 |
| Latency, throughput, scaling, concurrency | `./bench` §3, §4, §7, §8 | 3 |

---

## 8. Test environment and reproducibility

Every test is **deterministic**. All randomness comes from an xorshift128+
generator seeded with fixed constants (`0xC0FFEE` for the graph, `0xBEEF01`
for the world, and `0x5EED` / `4242` / `777` / `99` for the stress sweeps), so
the network, the fleet, the roster, the request
stream and every randomised stress sequence are byte-identical on every run and
on every machine. There are no timing-dependent assertions, no sampled
tolerances, and no flaky checks.

Reference run for the results quoted in this document:

| | |
|---|---|
| Compiler | gcc 13.3.0 (Ubuntu 24.04) |
| Flags | `-O3 -flto -std=c11 -Wall -Wextra`, portable — **not** `-march=native` |
| Shell | GNU bash 5.2.21 (protocol suite) |
| Kernel | Linux 6.14 |
| Cores | 4 |
| Engine suite | 74 checks, 0 failed, 0.87 s |
| Protocol suite | 31 checks, 0 failed, ~3 s |

The build is portable by default for a reason: `-march=native` targets the
build machine's exact CPU and will `SIGILL` on an older one or in a container
on different hardware. `make NATIVE=1` opts in, for local benchmarking only.

Header dependencies are tracked (`-MMD -MP`), so editing a header actually
triggers a rebuild — without it, `make` leaves stale objects behind and you
test a binary that does not match the source.

---

## 9. Known gaps

Stated plainly, because a test plan that claims total coverage is not being
honest.

1. **No concurrency stress test.** `./bench` §7 measures throughput on 1, 2 and
   4 threads and the daemon takes a reader-writer lock, but nothing here
   deliberately races a `COMMIT` against a `DISPATCH` to prove the locking is
   correct. A ThreadSanitizer run under mixed read/write load is the obvious
   next addition.

2. **No memory-safety instrumentation in the suite.** The code allocates only
   at startup and the hot path is allocation-free, but the suite is not run
   under Valgrind or AddressSanitizer as a gate. Both are one flag away
   (`CFLAGS=-fsanitize=address`), just not wired into `make test`.

3. **Coverage is not measured.** Every function on the dispatch path is
   exercised, but there is no `gcov` number, so unreached branches — the
   generation-counter overflow path in `search_reset()`, for instance — are not
   distinguished from covered ones.

4. **The network is synthetic, even in real-city mode.** A jittered grid with
   mixed road classes and long-range bypasses, not OpenStreetMap data. The
   hospital *rosters* are real (§3.17), but the published dataset carries no
   coordinates, so real hospitals are placed on the generated grid. Nothing in
   the suite can validate a position that is not claimed to be a position. The
   algorithm consumes any weighted digraph, but measured settled-node counts
   would differ on real topology.

5. **The inferred referral designations are a judgement call, not a fact under
   test.** `U-17.6` proves an inferred department is always a real subset of
   the hospital's departments and `U-17.9` proves the fallback closes every
   coverage hole, but no test can establish that the *right* hospital was
   designated — the dataset is silent, which is why the fallback exists. The
   engine's obligation is to carry the flag to the client (`P-7.9`) so the UI
   can label it, and that is what is tested.

6. **`ROADS` pagination is untested.** The protocol suite covers every other
   command; the paginated geometry endpoint is exercised by the web client but
   has no assertion of its own.

7. **The web front end has no automated tests.** `web/index.html` and
   `web/bridge.js` are verified by use, not by assertion. The bridge's contract
   with the engine is covered by the protocol suite; the rendering above it is
   not.
