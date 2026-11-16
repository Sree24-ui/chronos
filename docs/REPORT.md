# CHRONOS project report

Numbers below are copied from `results/*.json` (see [RESULTS.md](RESULTS.md) for full tables, trial ranges and the generator script) or from the test and soak logs described in [TESTING.md](TESTING.md). Nothing here is a claim about real exchanges.

## 1. Abstract

CHRONOS is a limit-order-book matching engine in C++20 built to study how data-structure choices affect order-processing latency, throughput, and memory. It implements strict price-time priority with limit orders, market orders, partial fills, cancellation, and modification, on a custom memory pool, a flat open-addressing id table, and intrusive doubly linked FIFO lists. The price-level index has two interchangeable implementations (a red-black tree and a direct-indexed array with a hierarchical bitmap), and an experimental module, **CHRONOS-ADAPT**, switches between them at safe batch boundaries with validation and rollback. On identical synthetic workloads the custom engine ran 1.45×–2.58× faster than an idiomatic STL implementation (tree) and 2.38×–3.33× (array) in the default configuration, with ~10⁵–10⁶ fewer heap allocations. The array index was faster than the tree in every workload we measured; the adaptive index matched the array's speed within run-to-run noise on most workloads and used up to 13× less memory than the array in a wide-price-band setting, but it did not improve tail latency. Correctness is established by randomized differential testing against an O(n) reference, including forced migrations.

## 2. Problem statement

An exchange must match orders in price-then-time order, correctly, under bursts of insertions and cancellations. The engine's inner data structures determine per-order cost (cache misses, allocations, tree rebalancing) and memory footprint. Which price-level index is best depends on the workload (number of active price levels, churn, price range), so a fixed choice is a compromise. The question explored here: **can a workload-aware switch between a tree and an array index give the speed of one and the memory of the other, without ever violating price-time priority?**

## 3. Limitations of existing (baseline) systems

The baseline implemented here is the textbook STL design: `std::map<price, std::list<order>>` plus `std::unordered_map<id, iterator>`. It allocates one heap node per resting order (and per level and hash entry), chases pointers through scattered memory, and pays logarithmic cost for level lookup. In our runs it performed 0.74M–1.78M heap allocations per million commands (see RESULTS.md, "allocs"). We make no claim about proprietary exchange engines, which we have not studied.

## 4. Proposed system

1. A pool-allocated, cache-line-sized order node (64 B) in an intrusive list per price level.
2. A flat id table (open addressing, backward-shift deletion) for O(1) cancel/status.
3. Two price-index strategies behind one small interface, plus a monitor and policy to choose between them at runtime (CHRONOS-ADAPT).
4. A differential-testing harness, a benchmark harness, a C++ gateway and a live dashboard.

## 5. Objectives

Correct price-time priority; fast cancel and lookup; measurable memory use; benchmark the STL baseline against the custom engine against the adaptive engine on identical workloads; report negative results; stay reproducible (seeded workloads, scripts, raw JSON).

## 6. System architecture

See the diagram in the [README](../README.md#architecture-at-a-glance). The engine is a header-only library with no I/O. `chronos_server` wraps one `Engine` behind one mutex: a simulator thread and REST handlers lock it per small chunk of commands, and a broadcaster thread locks it for microseconds to copy a snapshot every 250 ms. Rendering happens in the browser; the engine never waits on a client (a slow WebSocket client is dropped after a 200 ms send timeout). Benchmarks run in a separate process.

## 7. Data-structure implementation

| Structure | Where | Used for |
|---|---|---|
| Red-black tree (`std::map`) | `TreeIndex` in `book.hpp` | Sorted price levels (strategy 1); best level cached in a pointer. |
| Direct-indexed table + 3-level bitmap | `ArrayIndex` in `book.hpp` | Price → level in O(1); best price by `ctz`/`clz` (strategy 2). |
| Hash table, open addressing | `IdMap` in `ds.hpp` | id → order; load ≤ 0.5, Fibonacci hashing, backward-shift deletion (no tombstones). |
| Intrusive doubly linked list | `Node`/`Level` in `book.hpp` | FIFO queue per price; O(1) tail insert, O(1) removal given the node. |
| Slab pool allocator | `Pool<T>` in `ds.hpp` | Chunked storage with stable addresses, free list, usage/peak/alloc/free counters. |
| Circular buffer | `Ring<T>` in `ds.hpp` | Bounded trade history and latency samples in the gateway. |

Not used on purpose: a heap/priority queue (nothing in the matching path needs one) and a circular queue as engine input (the benchmark pre-generates the command vector, which preserves deterministic order).

## 8. Algorithms

- **Matching.** Reject invalid or duplicate ids; take the best opposite level; while quantity remains and the price crosses (always, for market orders): fill against the head of the level's FIFO at the *resting* price, unlink and recycle fully filled nodes, erase emptied levels. A limit remainder rests at the tail of its level; a market remainder is dropped.
- **Cancel.** Id table → node → unlink from its level → erase level if empty → recycle node.
- **Modify.** Validate first (an invalid modify leaves the order untouched), then cancel + add with a **new sequence number**, so priority is lost by design and may cross immediately.
- **Array best price.** Three bitmap levels (64-ary): level-2 words → level-1 word → level-0 word, each narrowed by `ctz` (asks) or `clz` (bids).
- **Migration** (see §10).

## 9. Complexity

*n* = active orders, *L* = active price levels, *k* = fills, *N* = band width in ticks.

| Operation | Tree index | Array index |
|---|---|---|
| Best price | O(1) (cached pointer) | O(1) (≤ 3 bit scans + scan of ⌈N/2¹⁸⌉ top words) |
| Insert resting order | O(log L) if the level is new, else O(1) | O(1) |
| Match | O(k) + O(log L) per emptied level | O(k) |
| Cancel | O(1) + O(log L) if the level empties | O(1) |
| Lookup by id | expected O(1) (flat hash) | same |
| Memory | ≈ L·(40 B level + tree node) + 64 B·n | 2·N·40 B + bitmaps + 64 B·n |
| Migration | O(n + L) (+ O(N) to allocate/zero an array) | same |

## 10. CHRONOS-ADAPT

**Monitor.** Every `batch` commands (default 8,192; the gateway uses 4,096) the engine records: active levels, density (levels ÷ price span), insert rate, cancel rate, best-lookups per op, level churn (level creations + erasures per op), recent ns/op, and memory.

**Policies** (`adaptive.hpp`, selectable with `--policy`):

- **v1 (density rule)**: tree → array when levels ≥ 64, density ≥ 0.20 and churn ≥ 0.05; back when density < 0.05. This was the first design. It performed poorly (§12) and is kept as a reproducible negative result.
- **v2 (level-count rule, default)**: tree → array when levels ≥ 1,024, churn ≥ 0.02 and the array's fixed footprint fits `memBudget` (512 MB); back to tree below 256 levels. Two agreeing batches are required (`confirm`) and a cool-down follows each switch. The reason string for every decision is recorded and shown on the dashboard.

**Safe migration** (`Engine::migrate`), always between commands:
1. Build the destination index; create levels that *share* the existing order lists (no node is copied).
2. In one pass over the nodes: check strict price order, strictly increasing sequence numbers inside every level (FIFO), per-level quantity/count aggregates, side, and totals against the source; stage `node->lvl` to the new levels.
3. On any failure: restore the old `node->lvl` pointers and throw; the live book was never replaced.
4. Otherwise swap the active representation. The id table is untouched (nodes never move).

Orders keep their id, remaining quantity, timestamp and sequence number because the node objects are never copied or modified, so price-time priority cannot change.

**Fixed-strategy mode** (`Mode::Tree`, `Mode::Array`) exists for baselines; `forceFlip` and `forceMigrate` exist only for tests.

## 11. Experimental methodology

Identical pre-generated command streams (own PRNG, same seed ⇒ same stream on any platform) are replayed on `std` (STL baseline), `tree`, `array`, and `adaptive`. Six workloads: uniform (wide price spread), concentrated (tight around the mid), sparse (a fixed set of 40 price levels per side plus sweep orders), high_cancel (60% cancels), bursty (alternating calm/burst phases), mixed (blend with market orders and modifies). Per trial: (A) an untimed pass for throughput, heap allocations (replaced `operator new`) and live heap bytes; (B) a pass timing each command with `steady_clock` for latency percentiles, with trade hashing *outside* the timed region. One warm-up round, then repeated trials in round-robin order across strategies. Every strategy's trade-stream hash and final active-order count must match, or the run reports failure. Raw JSON includes per-trial throughput.

Experiments: default band (32,768 ticks) with policy v2 and v1, and a wide band (4,194,304 ticks, where the array index costs ~336 MB). 1,000,000 commands per trial; 5 trials (3 for the wide band).

**Held-out evaluation.** After the v2 thresholds were frozen we added a seventh workload, `market_like`, and ran (a) it alone, default and wide band, seed 777, and (b) the original six workloads on a fresh seed (2024). `market_like` is generated from commonly reported stylized facts (volatility clustering via quiet/volatile regimes; exponential plus power-law-tail limit distances from the mid; a few crossing limit orders; persistent order-flow sign; log-normal sizes with round-lot bias; most cancels hitting recent orders). It is **not calibrated to any dataset**. A replayer for LOBSTER-format recorded data (`--lobster FILE`) is also provided; **no recorded data has been run through it yet** and the file format was implemented from documentation recalled by the author and checked only against a hand-written sample (see TESTING.md).

## 12. Benchmark results (summary; details in RESULTS.md)

- **Custom vs STL (default band).** Tree ÷ STL throughput 1.45×–2.58×, array ÷ STL 2.38×–3.33×. Heap allocations during the run: e.g. uniform, STL 1,783,067 vs array 39.
- **Tree vs array.** Array ÷ tree throughput 1.09×–1.87× (default band) and 1.01×–4.76× (wide band, where the tree degrades with ~500k levels in `uniform`). *Q1/Q2:* on the original workloads the array was never slower than the tree on throughput (but see the held-out wide `market_like` result below, where the tree was faster); the tree's advantage is memory when the book has few levels in a wide band (wide `concentrated`: tree 23 MB vs array 344 MB heap, equal throughput).
- **Adaptive (policy v2).** Adaptive ÷ array throughput 0.80×–1.08× (default) and 0.85×–1.01× (wide); adaptive ÷ tree 1.02×–1.82× (default) and 1.00×–4.82× (wide). Adaptive and array per-trial ranges overlap on most workloads, so many of these differences are indistinguishable from noise (RESULTS.md lists which). Adaptive stayed on the tree for `concentrated` and `high_cancel` (fewer than 1,024 levels) and switched once in `uniform`, `sparse`, `bursty` and `mixed`.
- **Overhead (Q3).** Each migration took 0.18–0.67 ms in the default band and 5.6–6.7 ms in the wide band (dominated by allocating and zeroing the large arrays). That single event shows up in the maximum latency (milliseconds), not in p99.
- **Tail latency (Q4).** Adaptive p99 equalled array p99 or differed by one clock tick (~42 ns) in all 12 comparisons. We found **no p99 improvement** from adaptation.
- **Memory (Q5).** Default band: adaptive heap 0.91×–1.00× of array and 1.00×–1.04× of tree (band is small, so the array is cheap). Wide band: adaptive heap 0.07×–1.00× of array, i.e. up to 13× less where it stayed on the tree, at a throughput cost on `high_cancel` (array 36.4 vs adaptive 31.1 Mops/s).
- **Policy v1 (negative result, `results/bench_policy1.json`).** Adaptive ÷ array throughput 0.73×–0.98×. In `sparse` it never switched, so it ran at tree speed (0.97× of tree) while fixed array was 1.32× faster than tree there; in the default band that gives up speed without any memory benefit. Density was a poor predictor: the array beat the tree in every workload, including ones v1 judged too sparse to switch. v2 uses level count, churn and a memory budget instead.
- **Held-out results (thresholds frozen; `bench_heldout*.json`, `bench_newseed.json`).** `market_like`, default band: adaptive switched once to the array and ran at 0.90× of fixed array, with identical heap (array ÷ tree 1.13×, tree ÷ STL 1.70×). `market_like`, wide band: adaptive again switched (the 344 MB array fits the 512 MB budget), ran at 0.88× of array and used the **same 344 MB**; here the tree was faster than the array (array ÷ tree 0.91×). Original six workloads, seed 2024: adaptive ÷ array 0.89×–1.02× (median about 0.92×), array ÷ tree 1.03×–1.93×, tree ÷ STL 1.27×–2.44×; adaptive stayed on the tree for `concentrated` and `high_cancel` as before. **Reading:** on unseen data adaptive does not beat fixed array on speed (it is typically a few percent to ~10% slower), and the memory benefit seen in the first experiments did not appear on the held-out wide-band workload because the policy's 512 MB budget let it choose the 344 MB array. The budget is a tunable we set by hand; a lower budget would have kept the tree there, which we have not measured. Run-to-run spread on a laptop is large (ranges in RESULTS.md), so single ratios of 0.9× are suggestive, not proven.
- **Priority across transitions (Q6).** Verified by tests (§13), not by benchmarks.

**Caveat on novelty/overfitting.** v2's thresholds were chosen after inspecting these workloads. Array-indexed price ladders with bitmaps are, to our knowledge, an established idea in the trading-systems community; we did not search the literature or patents (see INVENTION_DISCLOSURE.md).

## 13. Testing

Summarized here; evidence in [TESTING.md](TESTING.md). Scenario tests cover required cases 1–14 plus modify on seven engine variants (migration, order preservation and stress cases 15–17 are covered by the migration and differential tests); randomized differential tests compare statuses, trades, and the full book (every 61 commands and at the end) against an O(n) reference, with hostile input injected and migrations forced at random; structure tests check the hash table, pool and bitmap against `std::unordered_map`/`std::set`. Soak: 300 seeds × 3,000 commands under ASan+UBSan: 653 checks, 0 failures. The gateway ran under ThreadSanitizer with concurrent REST, WebSocket and simulator load: 0 reports.

## 14. Limitations

- Single-threaded engine; one global lock in the gateway (`ponytail:` ceiling: add a command queue if the dashboard must never contend).
- Fixed integer price band; out-of-band limit orders are rejected by all engines.
- Benchmarks: main results from one machine (Apple silicon laptop, macOS), not isolated (teammates' machines can be added under `results/machines/`), wide trial ranges; timer quantization ~42 ns makes per-command percentiles coarse; the first trials in some rows are visibly slower (see ranges).
- Latency includes two clock reads per command; throughput does not.
- Workloads are synthetic and our own. `market_like` follows stylized facts but is not calibrated to data. The LOBSTER replayer exists but has not been run on a real file.
- No persistence, no network protocol (FIX/ITCH), no self-trade prevention, no order types beyond limit/market.
- The array index's memory is fixed by the band; huge bands are impractical.
- v2 thresholds were tuned on the six original workloads. One held-out workload (`market_like`, 2 bands) and one fresh seed were evaluated after freezing them; that is a small held-out set, and it did not confirm an advantage over fixed array.
- Dashboard is tested manually in one browser; no automated UI tests.

## 15. Future scope

Real-trace workloads (run the LOBSTER replayer on downloaded samples; check the data licence before redistributing anything); a lower memory budget evaluated on held-out data; tuning thresholds by measurement (an online A/B regret check was considered and deferred); a sparse/paged array index to cap memory; multi-instrument sharding; a lock-free SPSC command queue between gateway and engine; more order types (IOC/FOK/stop/iceberg); persistence and replay; CPU-cycle counters instead of `steady_clock`; a proper literature and prior-art search.

## 16. Conclusion

A from-scratch engine with a pool allocator, flat hash table and intrusive lists ran 1.45×–3.33× faster than the STL design on identical synthetic workloads and stayed correct under extensive differential testing. Within it, a direct-indexed bitmap price index beat the tree on speed in every case measured, and CHRONOS-ADAPT's value, if any, is **memory**: it keeps tree-sized memory on small books and array speed on large ones, at the price of a one-off millisecond-scale migration and no tail-latency gain. Held-out runs (one new workload, fresh seed) did not show a speed advantage over fixed array and, in the wide band, the policy chose the large array, so the memory benefit is conditional on the budget setting. Whether the mechanism is worth it needs more held-out workloads, real traces, and a literature check before any novelty claim.
