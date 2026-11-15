#pragma once
// Shared value types. Prices are integer ticks (no floating point in the engine).
#include <cstdint>
#include <vector>

namespace chronos {

using Price = int64_t;
using Qty = int64_t;
using Id = uint64_t;  // 0 is reserved (empty slot in IdMap) and rejected as an order id

enum class Side : uint8_t { Buy, Sell };
enum class Type : uint8_t { Limit, Market };
enum class Kind : uint8_t { New, Cancel, Modify };
enum class Status : uint8_t { Resting, Filled, Dropped, Cancelled, RejDup, RejInvalid, RejNotFound };

inline const char* name(Status s) {
  static const char* n[] = {"resting", "filled", "dropped", "cancelled", "rej_duplicate", "rej_invalid", "rej_not_found"};
  return n[static_cast<int>(s)];
}

struct Cmd {
  Kind kind = Kind::New;
  Side side = Side::Buy;
  Type type = Type::Limit;
  Id id = 0;
  Price price = 0;  // Modify: new price (limit). Ignored for Market / Cancel
  Qty qty = 0;      // Modify: new total quantity
  uint64_t ts = 0;  // caller-supplied monotonic timestamp (ns); engine never reads a clock per order
};

struct Trade {
  uint64_t tid;
  Id buy, sell;
  Price price;  // always the resting order's price
  Qty qty;
  uint64_t ts;
  bool same(const Trade& o) const { return tid == o.tid && buy == o.buy && sell == o.sell && price == o.price && qty == o.qty; }
};

// One active order in canonical priority order: bids best-first then asks best-first, FIFO inside a level.
struct Row {
  Side side;
  Price price;
  Id id;
  Qty rem;
  uint64_t seq;
  bool operator==(const Row&) const = default;
};

struct Depth {
  Price price;
  Qty qty;
  uint32_t n;
};

// Price collar: limit prices must be in [lo, hi). Applied identically by every engine, so the
// array index can be a fixed-size table and all strategies stay behaviourally identical.
struct Band {
  Price lo = 0, hi = 32768;
};

}  // namespace chronos
