# Testing

Everything below was run during development; counts are from the actual logs. "Not tested" is listed explicitly at the end.

## 1. Automated tests (`tests/test.cpp`)

```bash
ctest --test-dir build --output-on-failure   # correctness, short soak, benchmark consistency
./build/chronos_tests [seeds] [commands]     # defaults 36 x 1500; try 300 3000
```

| Layer | What it checks |
|---|---|
| **Scenarios** (run on 7 engine variants: reference, STL, tree, array, adaptive, adaptive that flips representation every 16 commands, tree with random mid-stream migrations) | Required cases: no-match buy/sell; empty book; exact and better-price execution (trade at the resting price); partial fill (the spec's 100 vs 40 example); multi-fill across levels; FIFO on both sides; cancel active/nonexistent/already-filled/twice; market order full, partial (remainder dropped, never rests) and no liquidity; duplicate ids and invalid input (id 0, qty ≤ 0, price outside band) and id reuse after completion; quantities of 10¹⁵; modify = cancel+replace (priority lost, invalid modify leaves the order untouched, modify can cross). |
| **Structures** | `IdMap` vs `std::unordered_map` over 200,000 random inserts/erases (exercises growth and backward-shift deletion); `Pool` uniqueness, reuse without growth, stats; `ArrayIndex` bitmap vs `std::set` for best/min/max/iteration on both sides with a band over 2¹⁸ (two top-level words); `Ring` overwrite order. |
| **Migration** | Tree ↔ array six times in a row: the full book dump (side, price, id, remaining, **sequence**) is identical each time, active count unchanged, `check()` clean, and trading continues afterwards; policy decisions (v1 and v2, hysteresis band, memory budget); an adaptive engine under a dense workload switches by itself. |
| **Differential** (default 36 seeds × 1,500 commands; soak 300 × 3,000) | Six workload kinds, modify ratio 5%, plus injected hostile commands (duplicate ids, price outside the band, zero and negative quantity, unknown ids). Every command: status and every trade must equal the reference's. Every 61 commands and at the end: the entire ordered book must equal the reference's, `Engine::check()` must pass, active counts must match. |
| **Engine invariants** (`Engine::check()`) | Strictly ordered levels; every node's `prev` link, side, positive remaining quantity, strictly increasing sequence inside a level; level aggregates (count, quantity); `node->lvl` back-pointers; the id table points at every node; active-count reconciliation; book never crossed. |

## 2. Evidence

| Run | Result |
|---|---|
| Default suite under ASan + UBSan (`-fno-sanitize-recover`) | 389 checks, 0 failures |
| Soak `./chronos_tests 300 3000` under ASan + UBSan | 653 checks, 0 failures (300 seeds × 3,000 commands × 7 variants vs reference) |
| CTest via CMake (Release) | 3/3 passed (correctness, soak 100×2000, benchmark consistency) |
| Benchmark consistency | In all three recorded benchmark runs every strategy produced the identical trade-stream hash and final active-order count for every workload |
| Gateway under **ThreadSanitizer** | 0 warnings under 12 s of load: 6 WebSocket clients (re-sending depth requests every 200 ms), ~7,700 REST calls (orders, cancels, modifies, state, trades, meta), repeated engine mode changes and simulator restarts, short-lived WebSocket connects/disconnects, simulator at 100–150k orders/s |
| Gateway fuzzing (manual, `curl`/`node`) | Rejected cleanly: quantity 10¹³; price −1 and non-numeric; id beyond range; rate 99,999,999; shell metacharacters in the strategy list (`tree;rm -rf /`); 99 MB `Content-Length`; `../../etc/passwd`; `/.env`; `file=../x.json`. 400 parallel requests: server stayed up. A garbage WebSocket frame (length 2⁶⁴−1): server stayed up. |
| Simulator safety valve | With `--max-active 50000` a 500k orders/s uniform workload stopped itself at 50,285 resting orders, reported `simNote`, and the server stayed up |
| Frame cost of the dashboard (1024×768, DPR 2, 300 frames each, JS time only) | Portal scene frame: median 0.1 ms, p95 0.5 ms, max 2.2 ms. Floor snapshot update: median 0.5 ms, p95 0.9 ms, max 4.5 ms (runs 4×/s). Budget at 60 fps: 16.7 ms |

## 3. Phone and tablet layout

Emulated 375×812 (mobile preset) in the in-app browser against `web2`:

- **Found:** the five-link nav wrapped to three rows and spilled above the viewport; the Floor was squeezed into a fixed-height grid with the portal drawn over the text.
- **Fixed:** below 760 px the nav is a bottom tab bar; below 1000 px the pages scroll vertically in a single column and the portal becomes a dim backdrop.
- **Verified after the fix:** no horizontal overflow on any of the five pages (checked programmatically: document width and elements wider than the viewport outside scrollable tables); Floor shows 9 rows per side; tables scroll horizontally inside their panel.

## 4a. Continuous integration (GitHub Actions, `.github/workflows/ci.yml`)

Every push builds with CMake and runs CTest (correctness, soak 100x2000, benchmark consistency) on **ubuntu-latest, macos-latest and windows-latest (MSVC)**, then starts the server and does an HTTP round trip (order, fill, simulator, static page). A separate Linux job runs the soak under ASan+UBSan. All jobs passed on the commit that introduced Windows support (MSVC build and the three tests included). Not covered by CI: ThreadSanitizer, WebSocket, any browser, the benchmark's absolute numbers (shared runners are too noisy to measure performance).

## 4. Linux build (added at deployment)

On `gcc:13` (Linux arm64, Docker): `server.cpp`, `test.cpp` and `bench_main.cpp` compile with `-Wall -Wextra` and no warnings; the test suite passes (365 checks, 0 failures, run during the image build); the benchmark runs and every strategy reports a consistent trade hash (`bench.hpp`'s glibc `mallinfo2` branch compiled and ran). Running container smoke test: `/api/meta`, static files, order/trade, simulator, WebSocket with depth negotiation, and `/api/bench/run` correctly refused with 403. Porting found two real defects: `SO_NOSIGPIPE` does not exist on Linux (compile error) and the server crashed rather than dropped a connection if a handler threw or a thread could not be started (now caught).

## 5. Not tested (be honest about this)

- No frame-rate (fps) measurement: the in-app browser pane was hidden/throttled, so `requestAnimationFrame` timing was not meaningful. Only per-frame CPU cost was measured.
- Browsers other than the in-app one; real phones; touch interaction; widths above 1600 px (the wide layout was checked at 1600×900 and the CSS has steps for larger screens).
- No automated UI tests; dashboard flows were checked by hand: submitting orders and Run benchmark were checked at the API level, not by clicking through every state.
- No multi-hour memory soak (the simulator's active-order cap is the safeguard).
- Local runs were on arm64 (macOS Apple clang; Linux g++ 13 in Docker). Windows (MSVC) and x86-64 Linux are covered only by the CI build and tests (section 4a), nobody has run the dashboard on a Windows machine by hand. Sanitizers run on Linux in CI (ASan/UBSan); TSan was run on macOS only.
- LOBSTER replayer: implemented from my recollection of the format (headerless CSV: time, type, id, size, price x 10000, direction; type 1 new, 2 partial cancel, 3 delete, 4 visible execution, 5 hidden, 6 cross, 7 halt) and tested only on a small hand-written sample with the same layout. A real LOBSTER file has not been replayed; confirm the layout against the official documentation before trusting results. Execution events are replayed as market orders from the opposite side and partial cancels as modifies (losing queue priority), so the replayed book is an approximation of the real one.
- Held-out evidence is small: one new workload in two bands plus one fresh seed.
- Phone/browser checks listed below have not been done by anyone yet.
- The long-run behaviour of benchmark child processes started from the dashboard (cancel/kill) is not handled: a run can only finish.
- Latency measurements on a loaded laptop: trial-to-trial variation is large (see ranges in RESULTS.md).

## 6. Checklist for teammates (manual; please tick and commit)

Open the live site (see README) on your own device and note what breaks. Takes ~10 minutes each.

- [ ] Phone (name, model, browser): story page scrolls, "Enter the trading floor" works, Start live market works, the five tabs are reachable, nothing overflows sideways.
- [ ] Windows laptop: build with `cmake` + Visual Studio (or use Docker), run `chronos_tests`, run `chronos_server --web web2`, open http://localhost:8080, start the market, place a limit order and a market order from the Orders page.
- [ ] Windows laptop: run `chronos_bench --n 1000000 --trials 5 --out results/machines/<yourname>.json` and commit the JSON (see `results/machines/README.md`).
- [ ] Firefox and Safari/Chrome: Floor page shows the book and trade tape updating.
- [ ] Download a LOBSTER free sample (check the site's terms; do not commit the data) and run `chronos_bench --lobster <message file> --out results/lobster.json`; report whether the parser's summary line looks right.
