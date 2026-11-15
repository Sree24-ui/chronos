#pragma once
// The CHRONOS matching engine: pool-allocated intrusive FIFO levels, flat id table, and a
// price-level index that is either a tree, an array+bitmap, or switched adaptively at batch boundaries.
#include <chrono>
#include <stdexcept>
#include <string>

#include "adaptive.hpp"
#include "book.hpp"
#include "ds.hpp"

namespace chronos {

enum class Mode { Tree, Array, Adaptive };

struct Config {
  Band band{};
  Mode mode = Mode::Tree;
  AdaptCfg adapt{};
  size_t reserve = 1 << 16;  // preallocated order nodes / id-table sizing
};

struct Counters {
  uint64_t ops = 0, received = 0, matchedOrders = 0, rejects = 0, cancels = 0, trades = 0, tradedQty = 0;
};

class Engine {
  using Clock = std::chrono::steady_clock;
  struct Batch {
    uint64_t ops = 0, inserts = 0, cancels = 0, best = 0, levelOps = 0;
  };

  Config cfg_;
  Pool<Node> pool_;
  IdMap<Node> ids_;
  SideBook bids_, asks_;
  uint64_t seq_ = 0, tid_ = 0;
  Counters c_;
  Batch b_;
  BatchStats last_;
  std::vector<AdaptEvent> events_;
  std::string why_ = "no evaluation yet";
  Clock::time_point t0_ = Clock::now();
  Want pending_ = Want::Stay;
  int streak_ = 0, cooldown_ = 0;

  SideBook& book(Side s) { return s == Side::Buy ? bids_ : asks_; }

  void unlink(Node* n) {
    Level* l = n->lvl;
    (n->prev ? n->prev->next : l->head) = n->next;
    (n->next ? n->next->prev : l->tail) = n->prev;
    --l->n;
    --book(n->side).orders;
  }

  void cancelNode(Node* n) {
    Level* l = n->lvl;
    l->qty -= n->rem;
    unlink(n);
    ids_.erase(n->id);
    if (!l->n) {
      book(n->side).erase(l->price);
      ++b_.levelOps;
    }
    pool_.free(n);
    ++b_.cancels;
    ++c_.cancels;
  }

  Status add(Side side, Type type, Id id, Price price, Qty qty, uint64_t ts) {
    if (!id || qty <= 0 || (type == Type::Limit && (price < cfg_.band.lo || price >= cfg_.band.hi))) return Status::RejInvalid;
    if (ids_.find(id)) return Status::RejDup;
    const uint64_t seq = ++seq_;
    const bool buy = side == Side::Buy;
    SideBook& opp = buy ? asks_ : bids_;
    Qty rem = qty;
    while (rem > 0) {
      ++b_.best;
      Level* l = opp.best();
      if (!l || (type == Type::Limit && (buy ? l->price > price : l->price < price))) break;
      Node* n = l->head;
      const Qty f = rem < n->rem ? rem : n->rem;
      trades.push_back({++tid_, buy ? id : n->id, buy ? n->id : id, l->price, f, ts});
      ++c_.trades;
      c_.tradedQty += f;
      rem -= f;
      n->rem -= f;
      l->qty -= f;
      if (!n->rem) {
        unlink(n);
        ids_.erase(n->id);
        pool_.free(n);
        if (!l->n) {
          opp.erase(l->price);
          ++b_.levelOps;
        }
      }
    }
    if (rem < qty) ++c_.matchedOrders;
    if (!rem) return Status::Filled;
    if (type == Type::Market) return Status::Dropped;  // policy: cancel unfilled remainder
    Level* l = book(side).get(price);
    if (!l->n) ++b_.levelOps;
    Node* n = pool_.alloc();
    *n = Node{id, rem, seq, ts, l->tail, nullptr, l, side};
    (l->tail ? l->tail->next : l->head) = n;
    l->tail = n;
    l->qty += rem;
    ++l->n;
    ++book(side).orders;
    ids_.insert(id, n);
    ++b_.inserts;
    return Status::Resting;
  }

  Status modify(const Cmd& c) {
    Node* n = ids_.find(c.id);
    if (!n) return Status::RejNotFound;
    if (c.qty <= 0 || c.price < cfg_.band.lo || c.price >= cfg_.band.hi) return Status::RejInvalid;
    const Side s = n->side;
    cancelNode(n);  // replacement gets a fresh arrival seq: priority is lost by design
    return add(s, Type::Limit, c.id, c.price, c.qty, c.ts);
  }

  size_t levelsTotal() const { return bids_.levels() + asks_.levels(); }
  double density(size_t levels) const {
    if (!levels) return 0;
    Price lo = INT64_MAX, hi = INT64_MIN;
    for (const SideBook* s : {&bids_, &asks_})
      if (s->levels()) lo = std::min(lo, s->minP()), hi = std::max(hi, s->maxP());
    return double(levels) / double(hi - lo + 1);
  }

  // Validate a freshly built side before it replaces the live one (read-only on nodes).
  // checkLvl: verify node->lvl; stage: instead *assign* node->lvl (migration: validate + relink in one pass,
  // rolled back by the caller on failure); checkIds: verify the id table points at each node.
  std::string validateSide(SideBook& s, bool checkLvl, bool stage, bool checkIds, size_t& orders, Qty& qty) {
    std::string err;
    Price prev = 0;
    bool first = true;
    size_t lv = 0;
    s.each([&](Level& l) {
      ++lv;
      if (!first && (s.bid ? l.price >= prev : l.price <= prev)) err = "level order";
      first = false, prev = l.price;
      uint64_t lastSeq = 0;
      Qty q = 0;
      uint32_t c = 0;
      Node* p = nullptr;
      for (Node* n = l.head; n; n = n->next) {
        if (n->prev != p || n->rem <= 0 || n->seq <= lastSeq || n->side != (s.bid ? Side::Buy : Side::Sell)) err = "node link/seq/side";
        if (checkIds && ids_.find(n->id) != n) err = "id table mismatch";
        if (stage) n->lvl = &l;
        else if (checkLvl && n->lvl != &l) err = "node->lvl";
        lastSeq = n->seq, q += n->rem, ++c, p = n;
      }
      if (p != l.tail || q != l.qty || c != l.n || !c) err = "level aggregate";
      orders += c, qty += q;
      return err.empty();
    });
    if (err.empty() && lv != s.levels()) err = "level count";
    return err;
  }

  void migrate(bool toArr, const std::string& why) {
    if (toArr == bids_.arr) return;
    const auto t0 = Clock::now();
    const size_t before = bytes(), nOrders = ids_.size(), nLevels = levelsTotal();
    // 1. (caller is at a command boundary: nothing else mutates the book)
    // 2. build destination levels that *share* the existing order lists (no node is copied or touched)
    SideBook nb(true, cfg_.band, toArr), na(false, cfg_.band, toArr);
    auto build = [](SideBook& src, SideBook& dst) {
      src.each([&](Level& l) {
        Level* d = dst.get(l.price);
        d->qty = l.qty, d->n = l.n, d->head = l.head, d->tail = l.tail;
        return true;
      });
      dst.orders = src.orders;
    };
    build(bids_, nb);
    build(asks_, na);
    // 3/4. transfer references and validate in one pass over the nodes: counts, price order, FIFO seq order,
    //      aggregates. node->lvl is staged to the new levels; on any failure the old levels are restored.
    //      (The id table maps id->Node*, and nodes never move, so it is unaffected by migration.)
    auto rollback = [&] {
      for (SideBook* o : {&bids_, &asks_})
        o->each([](Level& l) {
          for (Node* n = l.head; n; n = n->next) n->lvl = &l;
          return true;
        });
    };
    size_t o = 0;
    Qty q = 0, srcQ = 0;
    bids_.each([&](Level& l) { return srcQ += l.qty, true; });
    asks_.each([&](Level& l) { return srcQ += l.qty, true; });
    std::string e = validateSide(nb, false, true, false, o, q);
    if (e.empty()) e = validateSide(na, false, true, false, o, q);
    if (e.empty() && (o != nOrders || q != srcQ)) e = "order/qty totals differ";
    if (!e.empty()) {
      rollback();
      throw std::runtime_error("migration validation failed: " + e);  // live book untouched
    }
    // 5. atomically replace the active representation
    bids_ = std::move(nb);
    asks_ = std::move(na);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (events_.size() < 2000) events_.push_back({c_.ops, toArr, ms, before, bytes(), nOrders, nLevels, why});
  }

  void endBatch() {
    BatchStats s;
    s.ops = b_.ops;
    s.nsPerOp = std::chrono::duration<double, std::nano>(Clock::now() - t0_).count() / double(b_.ops);
    s.levels = levelsTotal();
    s.density = density(s.levels);
    s.insertRate = double(b_.inserts) / b_.ops, s.cancelRate = double(b_.cancels) / b_.ops;
    s.bestRate = double(b_.best) / b_.ops, s.churn = double(b_.levelOps) / b_.ops;
    s.bytes = bytes();
    s.arrayBytes = 2 * (size_t(cfg_.band.hi - cfg_.band.lo) * sizeof(Level) + size_t(cfg_.band.hi - cfg_.band.lo) / 8);
    last_ = s;
    b_ = {};
    Want w = cfg_.adapt.forceFlip ? (bids_.arr ? Want::ToTree : Want::ToArray) : decide(cfg_.adapt, bids_.arr, s, why_);
    if (cooldown_ > 0) --cooldown_, w = Want::Stay;
    if (w != Want::Stay && w == pending_) ++streak_;
    else pending_ = w, streak_ = w == Want::Stay ? 0 : 1;
    if (w != Want::Stay && streak_ >= cfg_.adapt.confirm) {
      migrate(w == Want::ToArray, why_);
      cooldown_ = cfg_.adapt.cooldown, streak_ = 0, pending_ = Want::Stay;
    }
    t0_ = Clock::now();  // migration time is excluded from the next batch's ns/op
  }

 public:
  std::vector<Trade> trades;  // appended by process(); caller drains/clears

  explicit Engine(Config c = {})
      : cfg_(c), pool_(c.reserve), ids_(c.reserve), bids_(true, c.band, c.mode == Mode::Array), asks_(false, c.band, c.mode == Mode::Array) {}

  Status process(const Cmd& c) {
    Status s;
    switch (c.kind) {
      case Kind::New:
        ++c_.received;
        s = add(c.side, c.type, c.id, c.price, c.qty, c.ts);
        break;
      case Kind::Cancel: {
        Node* n = ids_.find(c.id);
        if (!n) s = Status::RejNotFound;
        else cancelNode(n), s = Status::Cancelled;
        break;
      }
      default:
        ++c_.received;
        s = modify(c);
    }
    if (s >= Status::RejDup) ++c_.rejects;
    ++c_.ops;
    if (cfg_.mode == Mode::Adaptive && ++b_.ops >= cfg_.adapt.batch) endBatch();
    return s;
  }

  // ---- introspection (used by tests, gateway, benchmarks) ----
  const Counters& counters() const { return c_; }
  size_t active() const { return ids_.size(); }
  size_t activeBuy() const { return bids_.orders; }
  size_t activeSell() const { return asks_.orders; }
  size_t levels() const { return levelsTotal(); }
  bool usingArray() const { return bids_.arr; }
  const BatchStats& lastBatch() const { return last_; }
  const std::string& reason() const { return why_; }
  const std::vector<AdaptEvent>& events() const { return events_; }
  PoolStats poolStats() const { return pool_.stats(); }
  const Config& config() const { return cfg_; }
  size_t bytes() const { return pool_.stats().bytes + ids_.bytes() + bids_.bytes() + asks_.bytes(); }
  void forceMigrate(bool toArr) { migrate(toArr, "manual"); }

  std::vector<Depth> depth(Side s, size_t maxLevels) {
    std::vector<Depth> d;
    book(s).each([&](Level& l) {
      d.push_back({l.price, l.qty, l.n});
      return d.size() < maxLevels;
    });
    return d;
  }
  std::vector<Row> dump() {
    std::vector<Row> r;
    for (SideBook* s : {&bids_, &asks_})
      s->each([&](Level& l) {
        for (Node* n = l.head; n; n = n->next) r.push_back({n->side, l.price, n->id, n->rem, n->seq});
        return true;
      });
    return r;
  }
  // Full structural invariant check. Returns "" when healthy.
  std::string check() {
    size_t o = 0;
    Qty q = 0;
    std::string e = validateSide(bids_, true, false, true, o, q);
    if (e.empty()) e = validateSide(asks_, true, false, true, o, q);
    if (!e.empty()) return e;
    if (o != ids_.size() || bids_.orders + asks_.orders != o) return "active count mismatch";
    Level *b = bids_.best(), *a = asks_.best();
    if (b && a && b->price >= a->price) return "crossed book";
    return "";
  }
};

}  // namespace chronos
