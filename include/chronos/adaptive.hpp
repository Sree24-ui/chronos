#pragma once
// CHRONOS-ADAPT policy: batch statistics -> "stay / switch to array / switch to tree".
// Pure function of measured statistics, so it is unit-testable and the reason is recorded.
//   policy 1 (v1, density rule)    : tree->array when density >= denseHi & levels >= minLevels & churn >= minChurn.
//                                    Kept for reproducibility: it was the first design and benchmarks poorly (see docs).
//   policy 2 (v2, level-count rule): tree->array when levels >= levelsHi & churn >= minChurn & array fits memBudget.
#include <cstdint>
#include <cstdio>
#include <string>

namespace chronos {

struct AdaptCfg {
  int policy = 2;
  size_t batch = 8192;          // evaluation boundary (commands); migration only ever happens here
  int confirm = 2;              // consecutive batches that must agree
  int cooldown = 4;             // batches to wait after a switch
  double minChurn = 0.02;       // level create/erase per op must be >= this (else the tree is not hurting)
  // policy 2
  size_t levelsHi = 1024, levelsLo = 256;  // hysteresis band on active price levels
  size_t memBudget = 512ull << 20;         // refuse array if its fixed footprint exceeds this
  // policy 1
  double denseHi = 0.20, sparseLo = 0.05;
  size_t minLevels = 64;
  bool forceFlip = false;       // TEST HOOK: flip representation every batch
};

struct BatchStats {
  uint64_t ops = 0;
  size_t levels = 0;
  double density = 0, insertRate = 0, cancelRate = 0, bestRate = 0, churn = 0, nsPerOp = 0;
  size_t bytes = 0, arrayBytes = 0;
};

struct AdaptEvent {
  uint64_t atOp;
  bool toArray;
  double ms;
  size_t bytesBefore, bytesAfter, orders, levels;
  std::string reason;
};

enum class Want { Stay, ToArray, ToTree };

inline Want decide(const AdaptCfg& c, bool arr, const BatchStats& b, std::string& why) {
  char buf[300];
  bool go;
  if (c.policy == 1) {
    if (!arr) {
      go = b.levels >= c.minLevels && b.density >= c.denseHi && b.churn >= c.minChurn;
      std::snprintf(buf, sizeof buf, "%s: levels=%zu (need>=%zu) density=%.4f (need>=%.3f) churn=%.3f (need>=%.3f)", go ? "dense+churning -> array" : "stay tree", b.levels,
                    c.minLevels, b.density, c.denseHi, b.churn, c.minChurn);
    } else {
      go = b.density < c.sparseLo || b.levels < c.minLevels / 2;
      std::snprintf(buf, sizeof buf, "%s: levels=%zu (floor %zu) density=%.4f (floor %.3f)", go ? "sparse -> tree" : "stay array", b.levels, c.minLevels / 2, b.density, c.sparseLo);
    }
  } else if (!arr) {
    const bool fits = b.arrayBytes <= c.memBudget;
    go = b.levels >= c.levelsHi && b.churn >= c.minChurn && fits;
    std::snprintf(buf, sizeof buf, "%s: levels=%zu (need>=%zu) churn=%.3f (need>=%.3f) array=%zuMB (budget %zuMB) density=%.4f", go ? "many levels -> array" : "stay tree", b.levels,
                  c.levelsHi, b.churn, c.minChurn, b.arrayBytes >> 20, c.memBudget >> 20, b.density);
  } else {
    go = b.levels < c.levelsLo;
    std::snprintf(buf, sizeof buf, "%s: levels=%zu (floor %zu) churn=%.3f density=%.4f", go ? "few levels -> tree" : "stay array", b.levels, c.levelsLo, b.churn, b.density);
  }
  why = buf;
  return !go ? Want::Stay : arr ? Want::ToTree : Want::ToArray;
}

}  // namespace chronos
