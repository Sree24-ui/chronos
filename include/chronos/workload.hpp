#pragma once
// Deterministic synthetic workload generator (own PRNG: same seed => same stream on every platform).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "types.hpp"

namespace chronos {

// MarketLike was added after the switching thresholds were frozen: it is the held-out workload.
enum class Wl { Uniform, Concentrated, Sparse, HighCancel, Bursty, Mixed, MarketLike };
inline constexpr int kNumWl = 7;
inline const char* wlName(Wl w) {
  static const char* n[] = {"uniform", "concentrated", "sparse", "high_cancel", "bursty", "mixed", "market_like"};
  return n[int(w)];
}
inline bool wlParse(const std::string& s, Wl& out) {
  for (int i = 0; i < kNumWl; ++i)
    if (s == wlName(Wl(i))) return out = Wl(i), true;
  return false;
}

struct WlCfg {
  Wl kind = Wl::Mixed;
  size_t n = 500000;
  Band band{};
  Qty qmin = 1, qmax = 100;
  double buy = 0.5, market = 0.05, cancel = 0.2, modify = 0.0;
  uint64_t seed = 42;
};

// Kind-specific defaults for the ratios; CLI/API overrides are applied afterwards.
inline WlCfg preset(Wl k) {
  WlCfg c;
  c.kind = k;
  switch (k) {
    case Wl::Uniform: c.market = 0.02, c.cancel = 0.10; break;
    case Wl::Concentrated: c.market = 0.05, c.cancel = 0.20; break;
    case Wl::Sparse: c.market = 0.03, c.cancel = 0.10; break;
    case Wl::HighCancel: c.market = 0.03, c.cancel = 0.60; break;
    case Wl::Bursty: c.market = 0.04, c.cancel = 0.15; break;
    case Wl::Mixed: c.market = 0.08, c.cancel = 0.30, c.modify = 0.02; break;
    case Wl::MarketLike: c.market = 0.06, c.cancel = 0.45, c.modify = 0.03; break;
  }
  return c;
}

class Gen {
  WlCfg c_;
  uint64_t s_, i_ = 0, nextId_ = 1;
  double mid_;
  bool vol_ = false, lastBuy_ = true;  // MarketLike: volatility regime, last order side
  uint64_t regimeLeft_ = 0;
  Price anchors_[2][40];
  uint64_t rnd() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double u() { return double(rnd() >> 11) * (1.0 / 9007199254740992.0); }
  uint64_t below(uint64_t n) { return rnd() % n; }
  double gauss() { return (u() + u() + u() + u() - 2.0) * 1.7320508; }  // ~N(0,1), platform independent

  // signed tick offset from mid: positive = passive (away from the spread), negative = aggressive
  Price offset() {
    const Price W = (c_.band.hi - c_.band.lo) / 2;
    auto conc = [&](double sd) { return Price(std::fabs(gauss()) * sd) - 1; };
    auto wide = [&]() { return Price(u() * double(W)) - W / 16; };
    switch (c_.kind) {
      case Wl::Uniform: return wide();
      case Wl::Concentrated: return conc(12);
      case Wl::HighCancel: return conc(60);
      case Wl::Sparse: return u() < 0.15 ? -W : Price(-1);  // -W: sweep order priced through the book; -1 marks "use anchor"
      case Wl::Bursty: return ((i_ / 25000) & 1) ? wide() : conc(10);
      case Wl::Mixed: {
        double r = u();
        return r < 0.7 ? conc(15) : r < 0.9 ? conc(400) : wide();
      }
      case Wl::MarketLike: return 0;  // handled by nextMarket()
    }
    return 0;
  }

 public:
  explicit Gen(const WlCfg& c) : c_(c), s_(c.seed * 0x2545F4914F6CDD1Dull + 1), mid_(double(c.band.lo + c.band.hi) / 2) {
    for (auto& side : anchors_)
      for (auto& a : side) a = 1 + Price(u() * double((c_.band.hi - c_.band.lo) / 2 - 2));  // fixed sparse price set
  }
  Id nextId() const { return nextId_; }
  // Market-like stream built from commonly reported stylized facts (not calibrated to any dataset): volatility
  // clustering (quiet/volatile regimes), exponential + power-law-tail limit distances from the mid, a few crossing
  // orders, persistent order-flow sign, log-normal sizes with round-lot bias, and most cancels hitting recent orders.
  Cmd nextMarket(uint64_t ts) {
    ++i_;
    if (regimeLeft_ == 0) vol_ = u() < 0.3, regimeLeft_ = 5000 + below(30000);
    --regimeLeft_;
    if (u() < (vol_ ? 0.15 : 0.02)) mid_ += (u() < 0.5 ? -1.0 : 1.0) * (vol_ ? 1.0 + double(below(3)) : 1.0);
    const double lim = double(c_.band.hi - c_.band.lo) / 8, centre = double(c_.band.lo + c_.band.hi) / 2;
    mid_ = std::fmax(centre - lim, std::fmin(centre + lim, mid_));
    Cmd c;
    c.ts = ts;
    const double r = u();
    if (nextId_ > 1 && r < c_.cancel + c_.modify) {
      const uint64_t age = std::min<uint64_t>(nextId_ - 2, uint64_t(-std::log(1.0 - u()) * 400.0));
      c.id = nextId_ - 1 - age;
      if (r < c_.cancel) return c.kind = Kind::Cancel, c;
      c.kind = Kind::Modify;
    } else c.kind = Kind::New, c.id = nextId_++;
    if (u() >= 0.6) lastBuy_ = u() < 0.5;  // order-flow signs persist
    c.side = lastBuy_ ? Side::Buy : Side::Sell;
    static const Qty lots[4] = {100, 200, 500, 1000};
    c.qty = u() < 0.3 ? lots[below(4)] : std::max<Qty>(1, std::min<Qty>(5000, Qty(std::exp(3.0 + 0.9 * gauss()))));
    if (c.kind == Kind::New && u() < c_.market) return c.type = Type::Market, c;
    const double d = u();
    const Price W = (c_.band.hi - c_.band.lo) / 2;
    Price off = d < 0.08 ? -Price(1 + below(3))  // crossing limit order
              : d < 0.68 ? 1 + Price(-std::log(1.0 - u()) * 3.0)
              : d < 0.93 ? 1 + Price(-std::log(1.0 - u()) * 25.0)
                         : std::min<Price>(W, Price(20.0 / std::pow(1.0 - u(), 1.0 / 1.2)));  // heavy tail
    Price p = Price(mid_) + (c.side == Side::Buy ? -off : off);
    c.price = std::max(c_.band.lo, std::min(c_.band.hi - 1, p));
    return c;
  }
  Cmd next(uint64_t ts = 0) {
    if (c_.kind == Wl::MarketLike) return nextMarket(ts);
    ++i_;
    if (below(100) == 0) mid_ += (u() < 0.5 ? -1 : 1);  // slow random walk of the mid price
    const double lim = double(c_.band.hi - c_.band.lo) / 8;
    const double centre = double(c_.band.lo + c_.band.hi) / 2;
    mid_ = std::fmax(centre - lim, std::fmin(centre + lim, mid_));

    Cmd c;
    c.ts = ts;
    double r = u();
    double cancelP = c_.cancel;
    if (c_.kind == Wl::Bursty) cancelP = ((i_ / 25000) & 1) ? 0.05 : 0.25;
    if (nextId_ > 1 && r < cancelP + c_.modify) {
      const uint64_t win = std::min<uint64_t>(nextId_ - 1, 4000);
      c.id = nextId_ - 1 - below(win);
      if (r < cancelP) {
        c.kind = Kind::Cancel;
        return c;
      }
      c.kind = Kind::Modify, c.type = Type::Limit;
    } else {
      c.kind = Kind::New;
      c.id = nextId_++;
    }
    c.side = u() < c_.buy ? Side::Buy : Side::Sell;
    c.qty = c_.qmin + Qty(below(uint64_t(c_.qmax - c_.qmin + 1)));
    if (c.kind == Kind::New && u() < c_.market) {
      c.type = Type::Market;
      return c;
    }
    Price off = offset();
    if (c_.kind == Wl::Sparse && off == -1) off = anchors_[c.side == Side::Buy ? 0 : 1][below(40)];
    Price p = Price(mid_) + (c.side == Side::Buy ? -off : off);
    c.price = std::max(c_.band.lo, std::min(c_.band.hi - 1, p));
    return c;
  }
  std::vector<Cmd> all() {
    std::vector<Cmd> v;
    v.reserve(c_.n);
    for (size_t k = 0; k < c_.n; ++k) v.push_back(next());
    return v;
  }
};

}  // namespace chronos
