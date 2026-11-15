# Invention disclosure (draft, for discussion with a patent professional)

> **Read first.** No prior-art search was performed. Nothing in this repository claims that CHRONOS-ADAPT is novel, non-obvious, or patentable, and none of it is legal advice. Array-indexed price ladders, bitmaps for best-price discovery, memory pools and adaptive data structures are all established general techniques. **Do not publish implementation details (repository, paper, talk) until you have discussed filing strategy with a qualified patent attorney.** The code in this folder is a working prototype, not a public disclosure.

## 1. Technical problem

A limit-order matching engine's price-level index trades speed against memory depending on the live book: a tree has memory proportional to active levels but pointer-chasing cost; a direct-indexed table is fast but has a footprint fixed by the price range. A fixed choice is a compromise whenever the book's shape changes over time.

## 2. Existing approaches (to be established by a real search)

Unverified starting points only: general descriptions of order-book implementations (sorted trees, hash maps, price-indexed arrays), adaptive/self-tuning data-structure literature, and patents on matching engines and low-latency trading systems. **None have been reviewed.** A professional search should cover at least: matching-engine order-book data structures; dynamic or hybrid index selection in latency-critical systems; online index/representation migration with validation.

## 3. Proposed mechanism as implemented

1. Two interchangeable price-level indexes over the same intrusive FIFO order lists.
2. A monitor sampling, per batch of commands: active levels, density, insert and cancel rates, best-lookup rate, level churn, ns/op, memory.
3. A policy with hysteresis, confirmation across consecutive batches, a cool-down, and a memory-budget gate, producing a recorded reason string.
4. **Migration at a command boundary that does not copy or modify order nodes:** the destination index is built around the existing lists; every order is validated for price order, FIFO sequence order, aggregates and totals in the same pass that stages the new back-pointers; failure restores the old back-pointers and aborts; success swaps the representation. Order id, remaining quantity, timestamp and sequence number are untouched by construction.

## 4. Technical advantages (measured, with caveats)

From `docs/RESULTS.md`, on synthetic workloads on one laptop: in a wide price band, adaptive kept a tree-sized heap (e.g. 23 MB vs 344 MB for the array in the `concentrated` workload, equal throughput) and array-level throughput where the book had many levels (e.g. 11.0 vs 2.3 Mops/s for the tree in `uniform`). Costs: a one-off migration of 0.18–0.67 ms (default band) or 5.6–6.7 ms (wide band); **no p99 improvement**; throughput below fixed array in some workloads (e.g. 31.1 vs 36.4 Mops/s in wide `high_cancel`); most default-band adaptive-vs-array differences are inside run-to-run noise.

## 5. Experimental evidence

`results/bench_default.json`, `bench_wide.json`, `bench_policy1.json` (raw), reproducible with `run_benchmarks.sh`; correctness evidence in `docs/TESTING.md` (differential tests with forced migrations, ASan/UBSan, TSan on the gateway).

## 6. Potentially distinguishing features (hypotheses to test against prior art, not claims)

- Migration that shares order nodes and validates-and-relinks in a single pass with rollback.
- The combined gate of level count, churn and a memory budget with hysteresis.
- Recording a human-readable reason for each representation switch and exposing it live.

## 7. What would be needed next

A professional prior-art search; held-out workloads and real market traces; evaluation against a broader set of baselines; a decision on what, if anything, is worth filing before any publication.
