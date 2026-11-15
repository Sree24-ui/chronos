# API reference

The gateway (`chronos_server`) listens on `127.0.0.1:<port>` only. There is **no authentication**; do not expose it. JSON bodies are flat objects. Errors: HTTP 400/404/409 with `{"error": "..."}`.

Limits and defaults are not hardcoded in the dashboard: it reads them from `GET /api/meta`.

## REST

| Method & path | Body / query | Response |
|---|---|---|
| `GET /api/meta` | – | `band`, `workloads`, `modes`, `strategies`, `limits` (max qty, rate, cancel ratio, benchmark size/trials, depth range, max active orders), `defaults`. |
| `GET /api/state` | – | One snapshot (same shape as a WebSocket message, without the trade delta). |
| `GET /api/trades?limit=N` | `N ≤ 5000` | `[[tradeId, buyId, sellId, price, qty, tsMillis], ...]` oldest first. |
| `POST /api/order` | `side` buy\|sell, `type` limit\|market, `qty`, `price` (limit only), optional `id` | `{id, status, trades[]}`. `status` ∈ resting, filled, dropped, cancelled, rej_duplicate, rej_invalid, rej_not_found. Ids are auto-assigned from 2^40 when omitted. |
| `POST /api/cancel` | `id` | `{status, trades[]}` (`cancelled` or `rej_not_found`). |
| `POST /api/modify` | `id`, `price`, `qty` | Cancel + replace. The replacement gets a new sequence number (loses queue priority) and may trade immediately. |
| `POST /api/engine` | `mode` adaptive\|tree\|array | Replaces the engine (book reset). |
| `POST /api/sim` | `action` start\|stop; start also takes `workload`, `rate`, `seed`, optional `cancel` | Starting resets the book (generator ids restart at 1). The simulator stops itself at `--max-active` resting orders and reports `simNote`. |
| `POST /api/bench/run` | `workload` (name or `all`), `n`, `trials`, `strategies` (comma list), optional `cancel` 0..0.95 | Starts `chronos_bench` as a child process and pauses the live simulator. 409 if one is running. |
| `GET /api/bench/status` | – | `{running, done, total, label, file, error}`. |
| `GET /api/bench/list` | – | Result file names in the results directory. |
| `GET /api/bench/results?file=NAME` | `NAME` matches `[A-Za-z0-9_.-]+`, no leading dot | The JSON result file. |

Static files are served from `--web` for `GET` only; names must match the same safe pattern (no paths).

## WebSocket `/ws`

One batched snapshot every 250 ms to every client (never one message per engine event). Server-to-client message:

```
{ t, mode, sim, simKind, simNote, depth, rate, band:[lo,hi],
  stats:{ received, matched, trades, tradedQty, cancels, rejects, ops, activeBuy, activeSell, levels,
          opsPerSec, avgNs, p50Ns, p99Ns, p999Ns, engineBytes, heapBytes, poolUsed, poolCap, poolAllocs, poolFrees },
  book:{ bids:[[price, qty, orders, cumulativeQty],...], asks:[...] },       // best first, `depth` levels per side
  trades:[[id,buy,sell,price,qty,tsMillis],...],                              // new since the previous snapshot (max 60)
  adapt:{ usingArray, levels, density, insertRate, cancelRate, bestRate, churn, nsPerOp, bytes, reason,
          cfg:{policy,batch,levelsHi,levelsLo,memBudgetMB,denseHi,sparseLo,minLevels,minChurn,confirm},
          nEvents, events:[{op,toArray,ms,before,after,orders,levels,why}, ...] } }   // last 30 events
```

Client-to-server: only `{"depth": N}` (text frame) is understood; it sets how many book levels per side the next snapshots contain (clamped to `limits.minDepth..maxDepth`). The dashboard sends the number of rows that fit its panels. Frames over 4 KB close the connection.

`latency` figures are wall time around each `Engine::process` call in the gateway (includes ~20–40 ns of clock reads), over the last 32,768 commands.
