#pragma once
// Two comparison engines with the same API as Engine:
//   StdEngine - idiomatic STL (std::map + std::list + std::unordered_map, one malloc per order). Benchmark baseline A.
//   RefEngine - O(n) scan over a flat vector; obviously correct, used as the differential-testing oracle.
#include <algorithm>
#include <list>
#include <map>
#include <unordered_map>

#include "types.hpp"

namespace chronos {

class StdEngine {
  struct Ord {
    Id id;
    Qty rem;
    uint64_t seq, ts;
  };
  using L = std::list<Ord>;
  using M = std::map<Price, L>;
  struct Loc {
    Side side;
    Price price;
    L::iterator it;
  };
  Band band_;
  M bids_, asks_;
  std::unordered_map<Id, Loc> ids_;
  uint64_t seq_ = 0, tid_ = 0;

  Status add(Side side, Type type, Id id, Price price, Qty qty, uint64_t ts) {
    if (!id || qty <= 0 || (type == Type::Limit && (price < band_.lo || price >= band_.hi))) return Status::RejInvalid;
    if (ids_.count(id)) return Status::RejDup;
    const uint64_t seq = ++seq_;
    const bool buy = side == Side::Buy;
    M& om = buy ? asks_ : bids_;
    Qty rem = qty;
    while (rem > 0 && !om.empty()) {
      auto mit = buy ? om.begin() : std::prev(om.end());
      const Price bp = mit->first;
      if (type == Type::Limit && (buy ? bp > price : bp < price)) break;
      L& l = mit->second;
      Ord& o = l.front();
      const Qty f = std::min(rem, o.rem);
      trades.push_back({++tid_, buy ? id : o.id, buy ? o.id : id, bp, f, ts});
      rem -= f, o.rem -= f;
      if (!o.rem) {
        ids_.erase(o.id);
        l.pop_front();
        if (l.empty()) om.erase(mit);
      }
    }
    if (!rem) return Status::Filled;
    if (type == Type::Market) return Status::Dropped;
    L& l = (buy ? bids_ : asks_)[price];
    l.push_back({id, rem, seq, ts});
    ids_[id] = {side, price, std::prev(l.end())};
    return Status::Resting;
  }
  void remove(std::unordered_map<Id, Loc>::iterator it) {
    Loc& loc = it->second;
    M& m = loc.side == Side::Buy ? bids_ : asks_;
    auto mit = m.find(loc.price);
    mit->second.erase(loc.it);
    if (mit->second.empty()) m.erase(mit);
    ids_.erase(it);
  }

 public:
  std::vector<Trade> trades;
  explicit StdEngine(Band b = {}) : band_(b) {}
  Status process(const Cmd& c) {
    if (c.kind == Kind::New) return add(c.side, c.type, c.id, c.price, c.qty, c.ts);
    auto it = ids_.find(c.id);
    if (it == ids_.end()) return Status::RejNotFound;
    if (c.kind == Kind::Cancel) return remove(it), Status::Cancelled;
    if (c.qty <= 0 || c.price < band_.lo || c.price >= band_.hi) return Status::RejInvalid;
    const Side s = it->second.side;
    remove(it);
    return add(s, Type::Limit, c.id, c.price, c.qty, c.ts);
  }
  size_t active() const { return ids_.size(); }
  std::vector<Row> dump() const {
    std::vector<Row> r;
    for (auto m = bids_.rbegin(); m != bids_.rend(); ++m)
      for (auto& o : m->second) r.push_back({Side::Buy, m->first, o.id, o.rem, o.seq});
    for (auto& [p, l] : asks_)
      for (auto& o : l) r.push_back({Side::Sell, p, o.id, o.rem, o.seq});
    return r;
  }
};

class RefEngine {
  Band band_;
  std::vector<Row> v_;  // unordered; every query is a linear scan
  uint64_t seq_ = 0, tid_ = 0;
  bool has(Id id) const {
    for (auto& r : v_)
      if (r.id == id) return true;
    return false;
  }
  Status add(Side side, Type type, Id id, Price price, Qty qty, uint64_t ts) {
    if (id == 0 || qty <= 0) return Status::RejInvalid;
    if (type == Type::Limit && (price < band_.lo || price >= band_.hi)) return Status::RejInvalid;
    if (has(id)) return Status::RejDup;
    const uint64_t seq = ++seq_;
    const bool buy = side == Side::Buy;
    Qty rem = qty;
    while (rem > 0) {
      int best = -1;
      for (int i = 0; i < int(v_.size()); ++i) {
        const Row& r = v_[i];
        if (r.side == side) continue;
        if (type == Type::Limit && (buy ? r.price > price : r.price < price)) continue;
        if (best < 0) { best = i; continue; }
        const Row& b = v_[best];
        const bool better = buy ? r.price < b.price : r.price > b.price;
        if (better || (r.price == b.price && r.seq < b.seq)) best = i;
      }
      if (best < 0) break;
      Row& r = v_[best];
      const Qty f = std::min(rem, r.rem);
      trades.push_back({++tid_, buy ? id : r.id, buy ? r.id : id, r.price, f, ts});
      rem -= f, r.rem -= f;
      if (!r.rem) v_.erase(v_.begin() + best);
    }
    if (!rem) return Status::Filled;
    if (type == Type::Market) return Status::Dropped;
    v_.push_back({side, price, id, rem, seq});
    return Status::Resting;
  }

 public:
  std::vector<Trade> trades;
  explicit RefEngine(Band b = {}) : band_(b) {}
  Status process(const Cmd& c) {
    if (c.kind == Kind::New) return add(c.side, c.type, c.id, c.price, c.qty, c.ts);
    auto it = std::find_if(v_.begin(), v_.end(), [&](const Row& r) { return r.id == c.id; });
    if (it == v_.end()) return Status::RejNotFound;
    if (c.kind == Kind::Cancel) return v_.erase(it), Status::Cancelled;
    if (c.qty <= 0 || c.price < band_.lo || c.price >= band_.hi) return Status::RejInvalid;
    const Side s = it->side;
    v_.erase(it);
    return add(s, Type::Limit, c.id, c.price, c.qty, c.ts);
  }
  size_t active() const { return v_.size(); }
  std::vector<Row> dump() const {
    std::vector<Row> r = v_;
    std::sort(r.begin(), r.end(), [](const Row& a, const Row& b) {
      if (a.side != b.side) return a.side == Side::Buy;
      if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
      return a.seq < b.seq;
    });
    return r;
  }
};

}  // namespace chronos
