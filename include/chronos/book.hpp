#pragma once
// Order node, price level, and the two interchangeable price-level index strategies.
#include <algorithm>
#include <bit>
#include <map>
#include <memory>
#include <vector>

#include "types.hpp"

namespace chronos {

struct Level;
struct Node {  // 64 bytes: one cache line; intrusive doubly linked list node
  Id id;
  Qty rem;
  uint64_t seq, ts;
  Node *prev, *next;
  Level* lvl;
  Side side;
};
struct Level {  // FIFO queue of orders at one price
  Price price = 0;
  Qty qty = 0;
  uint32_t n = 0;
  Node *head = nullptr, *tail = nullptr;
};

// ---- Strategy 1: red-black tree (std::map), best level cached -------------------------------
class TreeIndex {
  std::map<Price, Level> m_;
  Level* top_ = nullptr;  // best level (stable: map nodes never move)
  bool bid_;

 public:
  explicit TreeIndex(bool bid) : bid_(bid) {}
  Level* find(Price p) {
    auto it = m_.find(p);
    return it == m_.end() ? nullptr : &it->second;
  }
  Level* get(Price p) {
    auto [it, ins] = m_.try_emplace(p);
    if (ins) {
      it->second.price = p;
      if (!top_ || (bid_ ? p > top_->price : p < top_->price)) top_ = &it->second;
    }
    return &it->second;
  }
  void erase(Price p) {
    auto it = m_.find(p);
    if (&it->second == top_) {
      if (bid_) top_ = it == m_.begin() ? nullptr : &std::prev(it)->second;
      else {
        auto nx = std::next(it);
        top_ = nx == m_.end() ? nullptr : &nx->second;
      }
    }
    m_.erase(it);
  }
  Level* best() { return top_; }
  size_t size() const { return m_.size(); }
  Price minP() const { return m_.begin()->first; }
  Price maxP() const { return std::prev(m_.end())->first; }
  size_t bytes() const { return m_.size() * (sizeof(Level) + sizeof(Price) + 40); }  // 40 = rb-node header estimate
  template <class F>  // priority order (best first); F returns false to stop
  void each(F f) {
    if (bid_) {
      for (auto i = m_.rbegin(); i != m_.rend(); ++i)
        if (!f(i->second)) return;
    } else {
      for (auto i = m_.begin(); i != m_.end(); ++i)
        if (!f(i->second)) return;
    }
  }
};

// ---- Strategy 2: direct-indexed table + 3-level bitmap over a bounded price band ---------------
// level i of the bitmap summarises 64 words of the level below, so best-price discovery is
// at most three count-leading/trailing-zero instructions (plus a linear scan of the tiny top word array).
class ArrayIndex {
  bool bid_;
  Price lo_;
  size_t n_, cnt_ = 0;
  std::vector<Level> lv_;
  std::vector<uint64_t> b0_, b1_, b2_;
  static constexpr size_t npos = ~size_t(0);
  size_t lowest() const {
    for (size_t w = 0; w < b2_.size(); ++w)
      if (b2_[w]) {
        size_t j = (w << 6) + std::countr_zero(b2_[w]);
        size_t k = (j << 6) + std::countr_zero(b1_[j]);
        return (k << 6) + std::countr_zero(b0_[k]);
      }
    return npos;
  }
  size_t highest() const {
    for (size_t w = b2_.size(); w-- > 0;)
      if (b2_[w]) {
        size_t j = (w << 6) + 63 - std::countl_zero(b2_[w]);
        size_t k = (j << 6) + 63 - std::countl_zero(b1_[j]);
        return (k << 6) + 63 - std::countl_zero(b0_[k]);
      }
    return npos;
  }
  bool test(size_t i) const { return (b0_[i >> 6] >> (i & 63)) & 1; }
  void set(size_t i) {
    b0_[i >> 6] |= 1ull << (i & 63);
    b1_[i >> 12] |= 1ull << ((i >> 6) & 63);
    b2_[i >> 18] |= 1ull << ((i >> 12) & 63);
  }
  void clr(size_t i) {
    if (!(b0_[i >> 6] &= ~(1ull << (i & 63))))
      if (!(b1_[i >> 12] &= ~(1ull << ((i >> 6) & 63)))) b2_[i >> 18] &= ~(1ull << ((i >> 12) & 63));
  }

 public:
  ArrayIndex(bool bid, Price lo, size_t n)
      : bid_(bid), lo_(lo), n_(n), lv_(n), b0_((n + 63) / 64), b1_((n + 4095) / 4096), b2_((n + 262143) / 262144) {}
  Level* find(Price p) { return test(p - lo_) ? &lv_[p - lo_] : nullptr; }
  Level* get(Price p) {
    size_t i = p - lo_;
    if (!test(i)) {
      set(i);
      ++cnt_;
      lv_[i].price = p;
    }
    return &lv_[i];
  }
  void erase(Price p) {
    size_t i = p - lo_;
    clr(i);
    --cnt_;
    lv_[i] = Level{};
  }
  Level* best() {
    if (!cnt_) return nullptr;
    return &lv_[bid_ ? highest() : lowest()];
  }
  size_t size() const { return cnt_; }
  Price minP() const { return lo_ + lowest(); }
  Price maxP() const { return lo_ + highest(); }
  size_t bytes() const { return n_ * sizeof(Level) + (b0_.size() + b1_.size() + b2_.size()) * 8; }
  template <class F>
  void each(F f) {
    std::vector<uint32_t> idx;
    idx.reserve(cnt_);
    for (size_t j = 0; j < b1_.size(); ++j)
      for (uint64_t w = b1_[j]; w; w &= w - 1) {
        size_t k = (j << 6) + std::countr_zero(w);
        for (uint64_t x = b0_[k]; x; x &= x - 1) idx.push_back(uint32_t((k << 6) + std::countr_zero(x)));
      }
    if (bid_) std::reverse(idx.begin(), idx.end());
    for (auto i : idx)
      if (!f(lv_[i])) return;
  }
};

// One side of the book; `arr` selects the active strategy. The branch is perfectly predictable.
struct SideBook {
  bool bid, arr;
  Band band;
  size_t orders = 0;
  TreeIndex tree;
  std::unique_ptr<ArrayIndex> ar;
  SideBook(bool bid, Band b, bool arr) : bid(bid), arr(arr), band(b), tree(bid) {
    if (arr) ar = std::make_unique<ArrayIndex>(bid, b.lo, size_t(b.hi - b.lo));
  }
  Level* best() { return arr ? ar->best() : tree.best(); }
  Level* get(Price p) { return arr ? ar->get(p) : tree.get(p); }
  void erase(Price p) { arr ? ar->erase(p) : tree.erase(p); }
  size_t levels() const { return arr ? ar->size() : tree.size(); }
  Price minP() const { return arr ? ar->minP() : tree.minP(); }
  Price maxP() const { return arr ? ar->maxP() : tree.maxP(); }
  size_t bytes() const { return arr ? ar->bytes() : tree.bytes(); }
  template <class F>
  void each(F f) {
    arr ? ar->each(f) : tree.each(f);
  }
};

}  // namespace chronos
