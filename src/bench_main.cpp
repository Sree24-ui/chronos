// chronos_bench: replays identical synthetic workloads on std / tree / array / adaptive engines.
//   ./chronos_bench --n 500000 --trials 3 --out results/bench.json
#include <sys/utsname.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>

#include "chronos/bench.hpp"

using namespace chronos;
using namespace chronos::bench;

// Count heap traffic (only while g_count is set, i.e. during the throughput pass).
void* operator new(size_t n) {
  if (g_count) ++g_allocs;
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  if (g_count && p) ++g_frees;
  std::free(p);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }

static std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> v;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');) v.push_back(t);
  return v;
}

int main(int argc, char** argv) {
  Options o;
  o.base.n = 500000;
  std::string out = "results/bench.json", csv;
  bool progress = false;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); }
      return argv[++i];
    };
    if (k == "--n") o.base.n = std::strtoull(val(), 0, 10);
    else if (k == "--seed") o.base.seed = std::strtoull(val(), 0, 10);
    else if (k == "--trials") o.trials = std::atoi(val());
    else if (k == "--warmup") o.warmup = std::atoi(val());
    else if (k == "--cancel") o.cancel = std::atof(val());
    else if (k == "--buy") o.buy = std::atof(val());
    else if (k == "--market") o.market = std::atof(val());
    else if (k == "--qmin") o.base.qmin = std::atoll(val());
    else if (k == "--qmax") o.base.qmax = std::atoll(val());
    else if (k == "--batch") o.adapt.batch = std::strtoull(val(), 0, 10);
    else if (k == "--policy") o.adapt.policy = std::atoi(val());
    else if (k == "--levels-hi") o.adapt.levelsHi = std::strtoull(val(), 0, 10);
    else if (k == "--levels-lo") o.adapt.levelsLo = std::strtoull(val(), 0, 10);
    else if (k == "--mem-budget-mb") o.adapt.memBudget = std::strtoull(val(), 0, 10) << 20;
    else if (k == "--dense-hi") o.adapt.denseHi = std::atof(val());
    else if (k == "--sparse-lo") o.adapt.sparseLo = std::atof(val());
    else if (k == "--min-levels") o.adapt.minLevels = std::strtoull(val(), 0, 10);
    else if (k == "--min-churn") o.adapt.minChurn = std::atof(val());
    else if (k == "--band-hi") o.base.band.hi = std::atoll(val());
    else if (k == "--out") out = val();
    else if (k == "--csv") csv = val();
    else if (k == "--progress") progress = true;
    else if (k == "--strategies") o.strategies = split(val());
    else if (k == "--workloads") {
      o.workloads.clear();
      for (auto& s : split(val())) {
        Wl w;
        if (!wlParse(s, w)) { std::fprintf(stderr, "unknown workload '%s'\n", s.c_str()); return 2; }
        o.workloads.push_back(w);
      }
    } else { std::fprintf(stderr, "unknown flag %s\n", k.c_str()); return 2; }
  }
  for (auto& s : o.strategies)
    if (s != "std" && s != "tree" && s != "array" && s != "adaptive") { std::fprintf(stderr, "unknown strategy '%s'\n", s.c_str()); return 2; }
  if (o.trials < 1 || o.base.n < 1000) { std::fprintf(stderr, "need --trials >= 1 and --n >= 1000\n"); return 2; }
  if (progress) o.progress = [](int d, int t, const std::string& w) { std::printf("PROGRESS %d %d %s\n", d, t, w.c_str()); std::fflush(stdout); };

  const double clk = clockOverheadNs();
  auto rs = run(o);

  utsname u;
  uname(&u);
  char env[512];
  std::snprintf(env, sizeof env, "{\"os\":\"%s %s\",\"arch\":\"%s\",\"compiler\":\"%s\",\"optimized\":%s,\"clockReadNs\":%.1f,\"note\":\"simulated in-process replay; not exchange throughput\"}",
                u.sysname, u.release, u.machine, __VERSION__,
#ifdef __OPTIMIZE__
                "true",
#else
                "false",
#endif
                clk);
  std::ofstream f(out);
  if (!f) { std::fprintf(stderr, "cannot write %s (does the directory exist?)\n", out.c_str()); return 1; }
  f << toJson(rs, o, env);
  if (!csv.empty()) {
    std::ofstream c(csv);
    c << "workload,strategy,ops_per_sec,trades_per_sec,avg_ns,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,heap_mb,bytes_per_active,allocs,migrations,consistent\n";
    for (auto& r : rs)
      c << r.workload << ',' << r.strategy << ',' << uint64_t(r.opsPerSec) << ',' << uint64_t(r.tradesPerSec) << ',' << r.lat.avg << ',' << r.lat.p50 << ',' << r.lat.p95 << ',' << r.lat.p99
        << ',' << r.lat.p999 << ',' << r.lat.max << ',' << r.heapMB << ',' << r.bytesPerActive << ',' << r.allocs << ',' << r.migrations << ',' << r.consistent << '\n';
  }

  std::printf("\nCHRONOS benchmark  n=%zu  trials=%d (+%d warmup)  clock-read=%.1f ns  [simulated replay, single thread]\n", o.base.n, o.trials, o.warmup, clk);
  std::printf("%-13s %-9s %9s %8s %7s %7s %8s %9s %8s %9s %5s %s\n", "workload", "strategy", "Mops/s", "avg ns", "p50", "p99", "p99.9", "max us", "heapMB", "allocs", "migr", "ok");
  for (auto& r : rs)
    std::printf("%-13s %-9s %9.3f %8.0f %7.0f %7.0f %8.0f %9.1f %8.1f %9llu %5llu %s\n", r.workload.c_str(), r.strategy.c_str(), r.opsPerSec / 1e6, r.lat.avg, r.lat.p50, r.lat.p99,
                r.lat.p999, r.lat.max / 1e3, r.heapMB, (unsigned long long)r.allocs, (unsigned long long)r.migrations, r.consistent ? "ok" : "MISMATCH");
  std::printf("\nwrote %s\n", out.c_str());
  for (auto& r : rs)
    if (!r.consistent) { std::printf("ERROR: strategies disagree on trades for %s/%s\n", r.workload.c_str(), r.strategy.c_str()); return 1; }
  return 0;
}
