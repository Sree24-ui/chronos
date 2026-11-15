// No framework: CHECK counts failures, main() returns non-zero if any. Run: ./build/chronos_tests
#include <cstdio>
#include <functional>
#include <set>
#include <unordered_map>

#include "chronos/baseline.hpp"
#include "chronos/engine.hpp"
#include "chronos/workload.hpp"

using namespace chronos;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(c)) ++g_fail, std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
  } while (0)

static Band kBand{0, 4096};

// ---- engine factories: every scenario runs on all of these ----
struct Wrap {
  virtual ~Wrap() = default;
  virtual Status run(const Cmd&) = 0;
  virtual std::vector<Trade>& trades() = 0;
  virtual std::vector<Row> dump() = 0;
  virtual size_t active() = 0;
  virtual std::string check() { return ""; }
  virtual void tick() {}  // hook for random migrations
};
template <class E>
struct W : Wrap {
  E e;
  template <class... A> W(A&&... a) : e(std::forward<A>(a)...) {}
  Status run(const Cmd& c) override { return e.process(c); }
  std::vector<Trade>& trades() override { return e.trades; }
  std::vector<Row> dump() override { return e.dump(); }
  size_t active() override { return e.active(); }
};
struct WC : W<Engine> {
  bool randomMigrate;
  uint64_t r = 12345;
  WC(Config c, bool rm = false) : W<Engine>(c), randomMigrate(rm) {}
  std::string check() override { return e.check(); }
  void tick() override {
    if (!randomMigrate) return;
    r = r * 6364136223846793005ull + 1442695040888963407ull;
    if ((r >> 33) % 7 == 0) e.forceMigrate(!e.usingArray());
  }
};
using Factory = std::function<std::unique_ptr<Wrap>()>;
static Config cfg(Mode m, size_t batch = 64) {
  Config c;
  c.band = kBand, c.mode = m, c.reserve = 256, c.adapt.batch = batch;
  return c;
}
static std::vector<std::pair<const char*, Factory>> engines() {
  auto flip = [] { Config c = cfg(Mode::Adaptive, 16); c.adapt.forceFlip = true, c.adapt.confirm = 1, c.adapt.cooldown = 0; return c; };
  return {
      {"ref", [] { return std::make_unique<W<RefEngine>>(kBand); }},
      {"std", [] { return std::make_unique<W<StdEngine>>(kBand); }},
      {"tree", [] { return std::make_unique<WC>(cfg(Mode::Tree)); }},
      {"array", [] { return std::make_unique<WC>(cfg(Mode::Array)); }},
      {"adaptive", [] { return std::make_unique<WC>(cfg(Mode::Adaptive)); }},
      {"adaptive-flip", [=] { return std::make_unique<WC>(flip()); }},
      {"tree+random-migrate", [] { return std::make_unique<WC>(cfg(Mode::Tree), true); }},
  };
}

// ---- tiny DSL ----
static uint64_t g_ts = 0;
struct T {
  std::unique_ptr<Wrap> e;
  Status lim(Side s, Id id, Price p, Qty q) { return e->run({Kind::New, s, Type::Limit, id, p, q, ++g_ts}); }
  Status mkt(Side s, Id id, Qty q) { return e->run({Kind::New, s, Type::Market, id, 0, q, ++g_ts}); }
  Status cancel(Id id) { return e->run({Kind::Cancel, Side::Buy, Type::Limit, id, 0, 0, ++g_ts}); }
  Status mod(Id id, Price p, Qty q) { return e->run({Kind::Modify, Side::Buy, Type::Limit, id, p, q, ++g_ts}); }
  std::vector<Trade>& tr() { return e->trades(); }
};
constexpr Side B = Side::Buy, S = Side::Sell;

static void scenarios(const char* name, const Factory& f) {
  std::printf("[scenario] %s\n", name);
  auto fresh = [&] { return T{f()}; };
  { // 1,2,12 no match, empty book
    T t = fresh();
    CHECK(t.lim(B, 1, 100, 10) == Status::Resting);
    CHECK(t.lim(S, 2, 110, 10) == Status::Resting);
    CHECK(t.tr().empty() && t.e->active() == 2);
    T u = fresh();
    CHECK(u.mkt(B, 1, 5) == Status::Dropped && u.tr().empty() && u.e->active() == 0);
    CHECK(u.cancel(1) == Status::RejNotFound);
  }
  { // 3 exact price match + 4 better-price execution (trade at resting price)
    T t = fresh();
    t.lim(S, 1, 100, 10);
    CHECK(t.lim(B, 2, 100, 10) == Status::Filled);
    CHECK(t.tr().size() == 1 && t.tr()[0].price == 100 && t.tr()[0].qty == 10 && t.e->active() == 0);
    t.tr().clear();
    t.lim(S, 3, 95, 10);
    CHECK(t.lim(B, 4, 105, 10) == Status::Filled && t.tr()[0].price == 95);
  }
  { // 5 partial fill (spec example) – remainder rests
    T t = fresh();
    t.lim(B, 1, 100, 100);
    CHECK(t.lim(S, 2, 100, 40) == Status::Filled);
    CHECK(t.tr().size() == 1 && t.tr()[0].qty == 40 && t.tr()[0].buy == 1 && t.tr()[0].sell == 2);
    auto d = t.e->dump();
    CHECK(d.size() == 1 && d[0].id == 1 && d[0].rem == 60);
    CHECK(t.lim(S, 3, 100, 100) == Status::Resting);  // 60 filled, 40 rests
    CHECK(t.tr().size() == 2 && t.tr()[1].qty == 60);
    d = t.e->dump();
    CHECK(d.size() == 1 && d[0].id == 3 && d[0].rem == 40);
  }
  { // 6 multiple fills across levels, price priority then FIFO (7)
    T t = fresh();
    t.lim(S, 1, 101, 10);
    t.lim(S, 2, 100, 10);
    t.lim(S, 3, 100, 10);
    t.lim(S, 4, 102, 10);
    CHECK(t.lim(B, 5, 101, 25) == Status::Filled);
    CHECK(t.tr().size() == 3);
    CHECK(t.tr()[0].sell == 2 && t.tr()[0].price == 100);  // earliest at best price
    CHECK(t.tr()[1].sell == 3 && t.tr()[1].price == 100);
    CHECK(t.tr()[2].sell == 1 && t.tr()[2].price == 101 && t.tr()[2].qty == 5);
    auto d = t.e->dump();
    CHECK(d.size() == 2 && d[0].id == 1 && d[0].rem == 5 && d[1].id == 4);
  }
  { // 7 FIFO within a level on the buy side
    T t = fresh();
    t.lim(B, 1, 100, 5);
    t.lim(B, 2, 100, 5);
    t.lim(B, 3, 101, 5);
    t.lim(S, 4, 100, 12);
    CHECK(t.tr().size() == 3 && t.tr()[0].buy == 3 && t.tr()[1].buy == 1 && t.tr()[2].buy == 2 && t.tr()[2].qty == 2);
  }
  { // 8,9 cancel active / nonexistent / already completed / twice
    T t = fresh();
    t.lim(B, 1, 100, 10);
    t.lim(B, 2, 100, 10);
    CHECK(t.cancel(1) == Status::Cancelled && t.e->active() == 1);
    CHECK(t.cancel(1) == Status::RejNotFound);
    CHECK(t.cancel(999) == Status::RejNotFound);
    t.lim(S, 3, 100, 10);  // fills order 2
    CHECK(t.cancel(2) == Status::RejNotFound && t.e->active() == 0);
  }
  { // 10,11 market orders: full, partial (remainder cancelled), none
    T t = fresh();
    t.lim(S, 1, 100, 10);
    t.lim(S, 2, 101, 10);
    CHECK(t.mkt(B, 3, 15) == Status::Filled && t.tr().size() == 2 && t.tr()[1].price == 101 && t.tr()[1].qty == 5);
    CHECK(t.mkt(B, 4, 100) == Status::Dropped);  // only 5 left; rest dropped, never rests
    CHECK(t.tr().size() == 3 && t.tr()[2].qty == 5 && t.e->active() == 0);
    CHECK(t.mkt(S, 5, 1) == Status::Dropped);
  }
  { // 13 duplicate ids, plus invalid input
    T t = fresh();
    CHECK(t.lim(B, 1, 100, 10) == Status::Resting);
    CHECK(t.lim(B, 1, 99, 10) == Status::RejDup && t.e->active() == 1);
    CHECK(t.lim(S, 1, 99, 10) == Status::RejDup && t.tr().empty());
    CHECK(t.lim(B, 0, 100, 10) == Status::RejInvalid);
    CHECK(t.lim(B, 7, 100, 0) == Status::RejInvalid);
    CHECK(t.lim(B, 8, 100, -5) == Status::RejInvalid);
    CHECK(t.lim(B, 9, kBand.hi, 5) == Status::RejInvalid);
    CHECK(t.lim(B, 9, -1, 5) == Status::RejInvalid);
    t.lim(S, 2, 100, 10);  // completes id 1
    CHECK(t.lim(B, 1, 100, 10) == Status::Resting);  // id reusable once the order is gone
  }
  { // 14 huge quantities
    T t = fresh();
    const Qty big = 1'000'000'000'000'000LL;
    t.lim(S, 1, 100, big);
    CHECK(t.lim(B, 2, 100, big / 2) == Status::Filled);
    CHECK(t.e->dump()[0].rem == big / 2);
    CHECK(t.lim(B, 3, 100, big) == Status::Resting && t.tr().back().qty == big / 2);
  }
  { // modify = cancel + replace, loses time priority, can cross
    T t = fresh();
    t.lim(B, 1, 100, 10);
    t.lim(B, 2, 100, 10);
    CHECK(t.mod(1, 100, 10) == Status::Resting);  // re-queued behind 2
    auto d = t.e->dump();
    CHECK(d.size() == 2 && d[0].id == 2 && d[1].id == 1 && d[1].seq > d[0].seq);
    CHECK(t.mod(77, 100, 1) == Status::RejNotFound);
    CHECK(t.mod(1, kBand.hi, 1) == Status::RejInvalid && t.e->active() == 2);  // invalid modify leaves order untouched
    t.lim(S, 3, 105, 10);
    CHECK(t.mod(1, 105, 10) == Status::Filled && t.tr().size() == 1 && t.tr()[0].buy == 1 && t.tr()[0].price == 105);
  }
  std::string e;
  { T t = fresh(); t.lim(B, 1, 5, 5); e = t.e->check(); CHECK(e.empty()); }
}

// ---- randomized differential test against the O(n) reference, with invariant checks ----
static void differential(uint64_t seedBase, int seeds, size_t n) {
  std::printf("[differential] %d seeds x %zu cmds x all engines vs reference\n", seeds, n);
  static const Wl kinds[] = {Wl::Uniform, Wl::Concentrated, Wl::Sparse, Wl::HighCancel, Wl::Bursty, Wl::Mixed};
  for (int sd = 0; sd < seeds; ++sd) {
    WlCfg wc = preset(kinds[sd % 6]);
    wc.seed = seedBase + sd, wc.n = n, wc.band = kBand, wc.qmin = 1, wc.qmax = 30, wc.modify = 0.05;
    Gen gen(wc);
    std::vector<Cmd> cmds = gen.all();
    uint64_t r = wc.seed * 7919 + 1;
    for (auto& c : cmds) {  // inject hostile commands: duplicates, out-of-band prices, zero/neg qty, unknown ids
      r = r * 6364136223846793005ull + 1442695040888963407ull;
      unsigned k = (r >> 33) % 100;
      if (k == 0 && c.kind == Kind::New) c.id = 1 + (r >> 40) % 50;      // likely duplicate
      else if (k == 1) c.price = kBand.hi + 5;
      else if (k == 2) c.qty = 0;
      else if (k == 3) c.qty = -3;
      else if (k == 4 && c.kind != Kind::New) c.id = 1000000 + k;
    }
    auto list = engines();
    std::vector<std::unique_ptr<Wrap>> es;
    for (auto& [nm, f] : list) es.push_back(f());
    uint64_t ts = 0;
    size_t bad = 0;
    for (size_t i = 0; i < cmds.size() && bad < 3; ++i) {
      Cmd c = cmds[i];
      c.ts = ++ts;
      Status ref = es[0]->run(c);
      for (size_t k = 1; k < es.size(); ++k) {
        es[k]->tick();
        Status s = es[k]->run(c);
        bool ok = s == ref && es[k]->trades().size() == es[0]->trades().size();
        for (size_t t = 0; ok && t < es[0]->trades().size(); ++t) ok = es[k]->trades()[t].same(es[0]->trades()[t]);
        if (!ok) { ++bad; std::printf("  MISMATCH seed=%llu op=%zu engine=%s status %s vs ref %s\n", (unsigned long long)wc.seed, i, list[k].first, name(s), name(ref)); }
        es[k]->trades().clear();
      }
      es[0]->trades().clear();
      if (i % 61 == 0 || i + 1 == cmds.size()) {
        auto d0 = es[0]->dump();
        for (size_t k = 1; k < es.size(); ++k) {
          bool same = es[k]->dump() == d0;
          std::string e = es[k]->check();
          if (!same || !e.empty() || es[k]->active() != es[0]->active()) { ++bad; std::printf("  STATE seed=%llu op=%zu engine=%s same=%d check='%s'\n", (unsigned long long)wc.seed, i, list[k].first, same, e.c_str()); }
        }
      }
    }
    CHECK(bad == 0);
  }
}

// ---- data structure unit tests ----
static void structures() {
  std::printf("[structures]\n");
  { // IdMap vs unordered_map under random insert/erase (exercises backward-shift deletion and growth)
    IdMap<Node> m(4);
    std::unordered_map<Id, Node*> ref;
    Node dummy[8];
    uint64_t r = 1;
    bool ok = true;
    for (int i = 0; i < 200000 && ok; ++i) {
      r = r * 6364136223846793005ull + 1442695040888963407ull;
      Id k = 1 + (r >> 33) % 3000;
      if ((r >> 20) & 1) ok &= m.insert(k, &dummy[k & 7]) == ref.emplace(k, &dummy[k & 7]).second;
      else m.erase(k), ref.erase(k);
      ok &= m.size() == ref.size() && m.find(k) == (ref.count(k) ? ref[k] : nullptr);
    }
    for (auto& [k, v] : ref) ok &= m.find(k) == v;
    CHECK(ok);
  }
  { // Pool: reuse, stats
    Pool<Node> p(4);
    std::vector<Node*> v;
    for (int i = 0; i < 50000; ++i) v.push_back(p.alloc());
    auto s = p.stats();
    CHECK(s.used == 50000 && s.allocs == 50000 && s.capacity >= 50000);
    std::set<Node*> uniq(v.begin(), v.end());
    CHECK(uniq.size() == v.size());
    for (auto* n : v) p.free(n);
    size_t cap = p.stats().capacity;
    for (int i = 0; i < 50000; ++i) p.alloc();
    CHECK(p.stats().capacity == cap && p.stats().frees == 50000 && p.stats().peak == 50000);  // fully reused, no growth
  }
  { // ArrayIndex bitmap vs std::set (best / min / max / iteration order)
    for (bool bid : {true, false}) {
      ArrayIndex a(bid, 1000, 300000);  // > 2^18 so the top bitmap level has 2 words
      std::set<Price> ref;
      uint64_t r = 9;
      bool ok = true;
      for (int i = 0; i < 100000 && ok; ++i) {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        Price p = 1000 + (r >> 33) % 300000;
        if ((r >> 20) % 3) a.get(p), ref.insert(p);
        else if (ref.count(p)) a.erase(p), ref.erase(p);
        Level* b = a.best();
        ok &= ref.empty() ? !b : (b && b->price == (bid ? *ref.rbegin() : *ref.begin()));
        ok &= a.size() == ref.size();
      }
      std::vector<Price> got;
      a.each([&](Level& l) { return got.push_back(l.price), true; });
      std::vector<Price> want(ref.begin(), ref.end());
      if (bid) std::reverse(want.begin(), want.end());
      CHECK(ok && got == want);
    }
  }
  { // Ring
    Ring<int> r(3);
    for (int i = 1; i <= 5; ++i) r.push(i);
    CHECK(r.size() == 3 && r.at(0) == 3 && r.at(2) == 5);
  }
}

// ---- migration specifics ----
static void migration() {
  std::printf("[migration]\n");
  Config c = cfg(Mode::Tree);
  Engine e(c);
  Gen g([&] { WlCfg w = preset(Wl::Concentrated); w.band = kBand, w.n = 5000, w.cancel = 0.1; return w; }());
  uint64_t ts = 0;
  for (int i = 0; i < 3000; ++i) { Cmd x = g.next(++ts); e.process(x); e.trades.clear(); }
  auto before = e.dump();
  size_t nBefore = e.active();
  CHECK(!before.empty());
  for (int k = 0; k < 6; ++k) {  // tree->array->tree... order, seq and FIFO must be bit-identical every time
    e.forceMigrate(!e.usingArray());
    CHECK(e.dump() == before && e.active() == nBefore && e.check().empty());
  }
  CHECK(e.events().size() == 6 && e.events()[0].toArray && e.events()[0].orders == nBefore);
  // keep trading on the migrated structure
  for (int i = 0; i < 3000; ++i) { Cmd x = g.next(++ts); e.process(x); e.trades.clear(); }
  CHECK(e.check().empty());

  // policy: dense+churny flips tree->array; collapsing density flips it back
  AdaptCfg a;
  BatchStats b;
  std::string why;
  a.policy = 1;  // v1 density rule
  b.levels = 500, b.density = 0.5, b.churn = 0.3;
  CHECK(decide(a, false, b, why) == Want::ToArray);
  b.density = 0.01;
  CHECK(decide(a, false, b, why) == Want::Stay && decide(a, true, b, why) == Want::ToTree);
  b.density = 0.5, b.churn = 0.0;
  CHECK(decide(a, false, b, why) == Want::Stay);
  a.policy = 2;  // v2 level-count rule with hysteresis + memory budget
  b.levels = 5000, b.churn = 0.1, b.arrayBytes = 10 << 20;
  CHECK(decide(a, false, b, why) == Want::ToArray);
  b.arrayBytes = a.memBudget + 1;
  CHECK(decide(a, false, b, why) == Want::Stay);  // array would blow the memory budget
  b.arrayBytes = 10 << 20, b.levels = 500;
  CHECK(decide(a, false, b, why) == Want::Stay && decide(a, true, b, why) == Want::Stay);  // inside hysteresis band
  b.levels = 100;
  CHECK(decide(a, true, b, why) == Want::ToTree);

  // end-to-end: adaptive engine under a dense workload really switches by itself
  Config ac = cfg(Mode::Adaptive, 2000);
  ac.band = {0, 32768};
  Engine ae(ac);
  WlCfg w = preset(Wl::Uniform);
  w.n = 60000, w.band = ac.band;
  Gen ug(w);
  for (size_t i = 0; i < w.n; ++i) { ae.process(ug.next(i + 1)); ae.trades.clear(); }
  CHECK(!ae.events().empty() && ae.events()[0].toArray && ae.check().empty());
  std::printf("  adaptive engine switched %zu times; first at op %llu (%s)\n", ae.events().size(),
              (unsigned long long)(ae.events().empty() ? 0 : ae.events()[0].atOp), ae.events().empty() ? "" : ae.events()[0].reason.c_str());
}

int main(int argc, char** argv) {
  const int seeds = argc > 1 ? std::atoi(argv[1]) : 36;      // ./chronos_tests [seeds] [cmds-per-seed] for a longer soak
  const size_t cmds = argc > 2 ? std::strtoull(argv[2], 0, 10) : 1500;
  for (auto& [nm, f] : engines()) scenarios(nm, f);
  structures();
  migration();
  differential(1000, seeds, cmds);
  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
