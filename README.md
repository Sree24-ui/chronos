# CHRONOS

A **limit-order-book matching engine** written from scratch in C++20, with a live web dashboard, a benchmark harness, and an experimental adaptive index (**CHRONOS-ADAPT**) that switches its internal data structure while running.

A matching engine is the part of a stock exchange that pairs buy orders with sell orders: best price first, earliest order first at equal price. CHRONOS runs it on **synthetic orders only**. There is no real exchange, no real money, and no network connection to any market.

> **Status: academic prototype.** All performance numbers are *simulated in-process replays on one thread*, not exchange throughput. Read [docs/RESULTS.md](docs/RESULTS.md) (generated from the raw result files) and the limitations in [docs/REPORT.md](docs/REPORT.md) before quoting anything.

## What is in the box

| Part | Where | What it is |
|---|---|---|
| Engine | `include/chronos/` | Header-only. Price-time priority, limit/market orders, partial fills, cancel, modify (cancel + replace), pool allocator, flat hash table, intrusive FIFO lists, tree index and array+bitmap index. |
| Baselines | `include/chronos/baseline.hpp` | `StdEngine` (idiomatic STL, the benchmark baseline) and `RefEngine` (O(n) scan, the correctness oracle). |
| CHRONOS-ADAPT | `include/chronos/adaptive.hpp`, `engine.hpp` | Batch-boundary monitor, switching policy, validated migration. |
| CI | `.github/workflows/ci.yml` | Build + tests on Linux, macOS, Windows; sanitizer job; server smoke test. |
| Tests | `tests/test.cpp` | Scenarios + structure tests + randomized differential tests vs. the reference, with invariant checks and random mid-stream migrations. |
| Benchmarks | `src/bench_main.cpp`, `include/chronos/bench.hpp` | Same command stream replayed on four strategies; warm-up, repeated trials, latency percentiles, allocation counts, memory, JSON/CSV export. |
| Gateway | `src/server.cpp` | C++ HTTP + WebSocket server (loopback only, no auth). REST for commands, batched WebSocket snapshots for live data. |
| Dashboards | `web2/` (current), `web/` (first version) | Vanilla JS, no build step, no CDN. `web2` is the Active-Theory-inspired UI with a live scroll story. |

## Requirements

- A C++20 compiler. Tested: Apple clang 21 (macOS arm64) and g++ 13 (Linux arm64, in Docker). Not tested: Windows, x86-64.
- CMake 3.16+ (or call the compiler directly; see below).
- Node and Python 3 are **optional** (only for the WebSocket smoke script and `scripts/make_report.py`).
- A modern browser for the dashboard.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Without CMake:

```bash
mkdir -p build
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude tests/test.cpp       -o build/chronos_tests
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude src/bench_main.cpp   -o build/chronos_bench
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude src/server.cpp       -o build/chronos_server -pthread
```

Sanitizer builds: `-DCHRONOS_SANITIZE=ON` (ASan+UBSan) or `-DCHRONOS_TSAN=ON` (ThreadSanitizer).

## Run the dashboard

```bash
./build/chronos_server --web web2        # then open http://localhost:8080
# options: --port N  --host ADDR (default 127.0.0.1)  --web DIR  --bench PATH|none  --results DIR  --idle-stop SECONDS
#          --band-hi N (price ticks, default 32768)  --batch N  --max-active N
```

Run it from the project root (it serves `web2/` and reads `results/` relative to the working directory). Open the page, scroll the story, then **Enter the trading floor**, and press **Start live market**. The server binds to `127.0.0.1` only.

## Deploy

Live demo: https://chronos-kxec.onrender.com (free tier: the first visit after idle takes a moment to wake; every push to `main` redeploys it automatically).

`Dockerfile` + `render.yaml` run the dashboard on a host such as Render: `docker build -t chronos . && docker run -p 8080:8080 chronos`. The image builds with g++ 13, runs the test suite during the build, serves `web2/` and the three stored result files, listens on `$PORT` / `0.0.0.0`, **disables live benchmark runs** (`--bench none`, they would let any visitor burn CPU), caps the book at 200k resting orders and pauses the feed 2 minutes after the last viewer leaves. There is **no authentication**: every visitor shares one engine and can start/stop the feed or reset the book. Use it as a demo, not as a service.

## Windows

CI builds and tests on Windows (MSVC) on every push. Locally: install Visual Studio (C++ workload) and CMake, then

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\chronos_server.exe --web web2     # open http://localhost:8080
```

Not yet run by hand on a Windows machine: the dashboard. The Lab page's "run benchmark" starts `./build/chronos_bench`, a Unix-style path: on Windows run `chronos_bench.exe` from a terminal instead. Alternatively use WSL or Docker (`docker build -t chronos . && docker run -p 8080:8080 chronos`). The heap-bytes metric is only tracked inside `chronos_bench` on Windows; the dashboard shows 0 there.

## Real market data (optional)

`chronos_bench --lobster <message file> --out results/lobster.json` replays a [LOBSTER](https://lobsterdata.com)-format message file (free samples are offered by the provider; check its terms and do not commit the data). The replayer was written from documentation and tested only on a small hand-written sample; see docs/TESTING.md.

## Test

```bash
ctest --test-dir build --output-on-failure   # correctness + short soak + bench consistency
./build/chronos_tests 300 3000               # longer soak: 300 seeds x 3000 commands
```

Details and the results of the soak, sanitizer and fuzz runs: [docs/TESTING.md](docs/TESTING.md).

## Benchmark

```bash
./build/chronos_bench --n 500000 --trials 3 --out results/my_run.json --csv results/my_run.csv
./run_benchmarks.sh                       # the exact runs behind docs/RESULTS.md (~10 min)
python3 scripts/make_report.py            # regenerate docs/RESULTS.md from results/*.json
```

Flags: `--workloads a,b` `--strategies std,tree,array,adaptive` `--n` `--trials` `--warmup` `--seed` `--cancel` `--buy` `--market` `--band-hi` `--policy 1|2` `--levels-hi` `--levels-lo` `--mem-budget-mb` `--batch`. The same seed reproduces the same command stream. You can also launch benchmarks from the dashboard's **Lab** page (runs `chronos_bench` as a separate process; the live simulator pauses meanwhile).

## Documentation

- [docs/REPORT.md](docs/REPORT.md): abstract, problem, architecture, data structures, algorithms, complexity, CHRONOS-ADAPT, methodology, results summary, limits, future work.
- [docs/RESULTS.md](docs/RESULTS.md): generated benchmark tables and ratios.
- [docs/TESTING.md](docs/TESTING.md): what is tested and the evidence.
- [docs/API.md](docs/API.md): REST and WebSocket reference.
- [docs/INVENTION_DISCLOSURE.md](docs/INVENTION_DISCLOSURE.md): technical disclosure draft. **No prior-art search was done; nothing here claims novelty or patentability.**

## Architecture at a glance

```
 synthetic workload (seeded)         REST (commands)            WebSocket (batched snapshots, 4/s)
 or dashboard orders  ───────►  chronos_server ◄─────────────────────────────────────────────►  browser
                               one mutex around Engine
                                      │
                     ┌────────────────┴─────────────────┐
                     │ Engine::process(Cmd)             │
                     │  IdMap (id → Node*)              │
                     │  Pool<Node> (64 B nodes)         │
                     │  per side: price index           │
                     │    ├─ TreeIndex  (std::map)      │
                     │    └─ ArrayIndex (table+bitmap)  │
                     │  Level = intrusive FIFO list     │
                     │  CHRONOS-ADAPT at batch edges    │
                     └──────────────────────────────────┘
```

## Honest limits (short version)

- Single-threaded engine; the gateway serializes access with one mutex.
- Prices are integer ticks inside a fixed band (the same collar is applied to every engine).
- Benchmarks were run on a developer laptop that was not otherwise idle; trial-to-trial spread is wide and reported. Adaptive vs. fixed-array differences are mostly within that noise.
- The v2 switching thresholds were chosen after looking at these same workloads, so treat them as tuned, not validated.
