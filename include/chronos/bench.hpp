#pragma once
// Benchmark harness: identical pre-generated command stream replayed on every strategy.
// Per trial: (A) throughput pass - no per-op timing; counts heap allocations and live heap bytes.
//            (B) latency pass    - per-op steady_clock samples; trades hashed *outside* the timed region.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include "baseline.hpp"
#include "engine.hpp"
#include "workload.hpp"
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace chronos::bench {

// Optional global allocation counters; filled only by binaries that replace operator new (bench_main.cpp).
inline bool g_count = false;
inline uint64_t g_allocs = 0, g_frees = 0;

inline size_t heapInUse() {
#if defined(__APPLE__)
  malloc_statistics_t s;
  malloc_zone_statistics(nullptr, &s);
  return s.size_in_use;
#else
  return mallinfo2().uordblks;
#endif
}
inline double cpuSeconds() {
  rusage r;
  getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}
using Clk = std::chrono::steady_clock;
inline uint64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clk::now().time_since_epoch()).count(); }

struct Lat {
  double avg = 0, min = 0, max = 0, p50 = 0, p95 = 0, p99 = 0, p999 = 0;
};
inline Lat summarize(std::vector<uint32_t>& v) {
  Lat l;
  if (v.empty()) return l;
  std::sort(v.begin(), v.end());
  double s = 0;
  for (auto x : v) s += x;
  auto q = [&](double p) { return double(v[std::min(v.size() - 1, size_t(p * double(v.size())))]); };
  l = {s / double(v.size()), double(v.front()), double(v.back()), q(.5), q(.95), q(.99), q(.999)};
  return l;
}
inline double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct Trial {
  double secs = 0, cpuSecs = 0;
  uint64_t ops = 0, trades = 0, cancels = 0, allocs = 0, frees = 0, hash = 0, migrations = 0;
  size_t heapBytes = 0, active = 0, levels = 0;
  double migMs = 0;
  bool usedArray = false;
  Lat lat;
  uint64_t hist[34] = {};
};

template <class E, class Mk>
Trial runTrial(const std::vector<Cmd>& cmds, Mk mk) {
  Trial t;
  t.ops = cmds.size();
  {  // ---- pass A: throughput, allocations, memory ----
    const size_t h0 = heapInUse();
    auto e = mk();
    g_allocs = g_frees = 0, g_count = true;
    const double c0 = cpuSeconds();
    const uint64_t a = nowNs();
    for (const Cmd& c : cmds) {
      e->process(c);
      e->trades.clear();
    }
    const uint64_t b = nowNs();
    g_count = false;
    t.cpuSecs = cpuSeconds() - c0;
    t.secs = double(b - a) * 1e-9;
    t.allocs = g_allocs, t.frees = g_frees;
    t.heapBytes = heapInUse() - h0;
    t.active = e->active();
    if constexpr (std::is_same_v<E, Engine>) {
      t.cancels = e->counters().cancels, t.trades = e->counters().trades, t.levels = e->levels();
      t.migrations = e->events().size(), t.usedArray = e->usingArray();
      for (auto& ev : e->events()) t.migMs += ev.ms;
    }
  }
  {  // ---- pass B: per-operation latency ----
    auto e = mk();
    std::vector<uint32_t> lat(cmds.size());
    uint64_t h = 1469598103934665603ull, nt = 0, nc = 0;
    for (size_t i = 0; i < cmds.size(); ++i) {
      const uint64_t a = nowNs();
      Status s = e->process(cmds[i]);
      const uint64_t b = nowNs();
      lat[i] = uint32_t(std::min<uint64_t>(b - a, 0xFFFFFFFFu));
      ++t.hist[std::bit_width(lat[i])];
      if (s == Status::Cancelled) ++nc;
      for (const Trade& x : e->trades) h = (h ^ (x.buy * 31 + x.sell * 131 + uint64_t(x.price) * 7 + uint64_t(x.qty))) * 1099511628211ull;
      nt += e->trades.size();
      e->trades.clear();
    }
    t.hash = h, t.trades = nt, t.cancels = nc;
    t.lat = summarize(lat);
  }
  return t;
}

struct Result {
  std::string workload, strategy;
  size_t n = 0;
  double opsPerSec = 0, tradesPerSec = 0, cancelsPerSec = 0, cpuPct = 0;
  Lat lat;
  double heapMB = 0, bytesPerActive = 0, allocsPerOp = 0, migMs = 0;
  uint64_t allocs = 0, frees = 0, trades = 0, cancels = 0, active = 0, migrations = 0, levels = 0;
  bool endedArray = false, consistent = true;
  std::vector<double> trialOps;
  uint64_t hist[34] = {};
};

struct Options {
  std::vector<Wl> workloads{Wl::Uniform, Wl::Concentrated, Wl::Sparse, Wl::HighCancel, Wl::Bursty, Wl::Mixed};
  std::vector<std::string> strategies{"std", "tree", "array", "adaptive"};
  WlCfg base;  // n, seed, ratio overrides (ratios < 0 mean "use workload preset")
  double buy = -1, market = -1, cancel = -1;
  int trials = 3, warmup = 1;
  AdaptCfg adapt;
  std::function<void(int, int, const std::string&)> progress;
};

inline std::unique_ptr<Engine> mkEngine(const std::string& s, Band b, const AdaptCfg& a, size_t n) {
  Config c;
  c.band = b, c.adapt = a, c.reserve = std::max<size_t>(1 << 16, n / 4);
  c.mode = s == "array" ? Mode::Array : s == "adaptive" ? Mode::Adaptive : Mode::Tree;
  return std::make_unique<Engine>(c);
}

inline Trial runStrategy(const std::string& s, const std::vector<Cmd>& cmds, Band b, const AdaptCfg& a) {
  if (s == "std") return runTrial<StdEngine>(cmds, [&] { return std::make_unique<StdEngine>(b); });
  return runTrial<Engine>(cmds, [&] { return mkEngine(s, b, a, cmds.size()); });
}

inline std::vector<Result> run(const Options& o) {
  std::vector<Result> out;
  const int rounds = o.warmup + o.trials;
  const int total = int(o.workloads.size()) * int(o.strategies.size()) * rounds;
  int done = 0;
  for (Wl w : o.workloads) {
    WlCfg c = preset(w);
    c.n = o.base.n, c.seed = o.base.seed, c.band = o.base.band, c.qmin = o.base.qmin, c.qmax = o.base.qmax;
    if (o.buy >= 0) c.buy = o.buy;
    if (o.market >= 0) c.market = o.market;
    if (o.cancel >= 0) c.cancel = o.cancel;
    const std::vector<Cmd> cmds = Gen(c).all();
    std::vector<std::vector<Trial>> tr(o.strategies.size());
    for (int r = 0; r < rounds; ++r)  // round-robin so drift/thermal effects hit every strategy equally
      for (size_t k = 0; k < o.strategies.size(); ++k) {
        Trial t = runStrategy(o.strategies[k], cmds, c.band, o.adapt);
        if (r >= o.warmup) tr[k].push_back(std::move(t));
        if (o.progress) o.progress(++done, total, std::string(wlName(w)) + "/" + o.strategies[k]);
      }
    for (size_t k = 0; k < o.strategies.size(); ++k) {
      Result R;
      R.workload = wlName(w), R.strategy = o.strategies[k], R.n = cmds.size();
      auto med = [&](auto f) {
        std::vector<double> v;
        for (auto& t : tr[k]) v.push_back(f(t));
        return median(v);
      };
      for (auto& t : tr[k]) R.trialOps.push_back(double(t.ops) / t.secs);
      R.opsPerSec = med([](const Trial& t) { return double(t.ops) / t.secs; });
      R.tradesPerSec = med([](const Trial& t) { return double(t.trades) / t.secs; });
      R.cancelsPerSec = med([](const Trial& t) { return double(t.cancels) / t.secs; });
      R.cpuPct = med([](const Trial& t) { return 100 * t.cpuSecs / t.secs; });
      R.lat = {med([](const Trial& t) { return t.lat.avg; }), med([](const Trial& t) { return t.lat.min; }), med([](const Trial& t) { return t.lat.max; }),
               med([](const Trial& t) { return t.lat.p50; }),  med([](const Trial& t) { return t.lat.p95; }), med([](const Trial& t) { return t.lat.p99; }),
               med([](const Trial& t) { return t.lat.p999; })};
      R.heapMB = med([](const Trial& t) { return double(t.heapBytes) / 1048576.0; });
      R.migMs = med([](const Trial& t) { return t.migMs; });
      const Trial& L = tr[k].back();
      R.allocs = L.allocs, R.frees = L.frees, R.trades = L.trades, R.cancels = L.cancels, R.active = L.active;
      R.migrations = L.migrations, R.levels = L.levels, R.endedArray = L.usedArray;
      R.allocsPerOp = double(L.allocs) / double(L.ops);
      R.bytesPerActive = L.active ? double(L.heapBytes) / double(L.active) : 0;
      std::copy(std::begin(L.hist), std::end(L.hist), R.hist);
      for (auto& t : tr[k]) R.consistent &= t.hash == tr[0][0].hash && t.trades == tr[0][0].trades && t.active == tr[0][0].active;
      out.push_back(std::move(R));
    }
  }
  return out;
}

inline double clockOverheadNs() {
  const int N = 2000000;
  uint64_t s = 0;
  const uint64_t a = nowNs();
  for (int i = 0; i < N; ++i) s += nowNs();
  const uint64_t b = nowNs();
  return double(b - a) / N + (s == 1);  // cost of one clock read, ns
}

inline std::string toJson(const std::vector<Result>& rs, const Options& o, const std::string& env) {
  std::string j = "{\n  \"env\": " + env + ",\n  \"config\": {";
  char b[640];
  std::snprintf(b, sizeof b, "\"n\":%zu,\"seed\":%llu,\"trials\":%d,\"warmup\":%d,\"policy\":%d,\"batch\":%zu,\"levelsHi\":%zu,\"levelsLo\":%zu,\"memBudgetMB\":%zu,\"denseHi\":%.3f,\"sparseLo\":%.3f,\"minLevels\":%zu,\"minChurn\":%.3f,\"band\":[%lld,%lld]},\n  \"results\": [\n",
                o.base.n, (unsigned long long)o.base.seed, o.trials, o.warmup, o.adapt.policy, o.adapt.batch, o.adapt.levelsHi, o.adapt.levelsLo, o.adapt.memBudget >> 20, o.adapt.denseHi, o.adapt.sparseLo, o.adapt.minLevels,
                o.adapt.minChurn, (long long)o.base.band.lo, (long long)o.base.band.hi);
  j += b;
  for (size_t i = 0; i < rs.size(); ++i) {
    const Result& r = rs[i];
    std::snprintf(b, sizeof b,
                  "    {\"workload\":\"%s\",\"strategy\":\"%s\",\"n\":%zu,\"opsPerSec\":%.0f,\"tradesPerSec\":%.0f,\"cancelsPerSec\":%.0f,\"cpuPct\":%.1f,"
                  "\"lat\":{\"avg\":%.1f,\"min\":%.0f,\"max\":%.0f,\"p50\":%.0f,\"p95\":%.0f,\"p99\":%.0f,\"p999\":%.0f},",
                  r.workload.c_str(), r.strategy.c_str(), r.n, r.opsPerSec, r.tradesPerSec, r.cancelsPerSec, r.cpuPct, r.lat.avg, r.lat.min, r.lat.max, r.lat.p50,
                  r.lat.p95, r.lat.p99, r.lat.p999);
    j += b;
    std::snprintf(b, sizeof b,
                  "\"heapMB\":%.2f,\"bytesPerActive\":%.1f,\"allocs\":%llu,\"frees\":%llu,\"allocsPerOp\":%.4f,\"trades\":%llu,\"cancels\":%llu,\"active\":%llu,"
                  "\"levels\":%llu,\"migrations\":%llu,\"migMs\":%.3f,\"endedArray\":%s,\"consistent\":%s,\"trialOps\":[",
                  r.heapMB, r.bytesPerActive, (unsigned long long)r.allocs, (unsigned long long)r.frees, r.allocsPerOp, (unsigned long long)r.trades,
                  (unsigned long long)r.cancels, (unsigned long long)r.active, (unsigned long long)r.levels, (unsigned long long)r.migrations, r.migMs,
                  r.endedArray ? "true" : "false", r.consistent ? "true" : "false");
    j += b;
    for (size_t k = 0; k < r.trialOps.size(); ++k) j += (k ? "," : "") + std::to_string(uint64_t(r.trialOps[k]));
    j += "],\"hist\":[";
    for (int k = 0; k < 34; ++k) j += (k ? "," : "") + std::to_string(r.hist[k]);
    j += i + 1 < rs.size() ? "]},\n" : "]}\n";
  }
  return j + "  ]\n}\n";
}

}  // namespace chronos::bench
