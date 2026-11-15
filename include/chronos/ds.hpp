#pragma once
// Hand-written data structures: slab pool, open-addressing id table, circular buffer.
#include <bit>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace chronos {

struct PoolStats {
  size_t used, peak, capacity, allocs, frees, bytes;
};

// Slab allocator: chunked storage (stable addresses), intrusive free list, O(1) alloc/free.
template <class T>
class Pool {
  union Slot {
    Slot* next;
    alignas(T) unsigned char raw[sizeof(T)];
  };
  static constexpr size_t kChunk = 1 << 14;
  std::vector<std::unique_ptr<Slot[]>> chunks_;
  Slot *free_ = nullptr, *cur_ = nullptr;
  size_t left_ = 0, cap_ = 0, used_ = 0, peak_ = 0, allocs_ = 0, frees_ = 0;
  void grow(size_t n) {
    chunks_.push_back(std::make_unique<Slot[]>(n));
    cur_ = chunks_.back().get();
    left_ = n;
    cap_ += n;
  }

 public:
  explicit Pool(size_t reserve = 0) {
    if (reserve) grow(reserve);
  }
  T* alloc() {
    ++allocs_;
    if (++used_ > peak_) peak_ = used_;
    Slot* s;
    if (free_) {
      s = free_;
      free_ = s->next;
    } else {
      if (!left_) grow(kChunk);
      s = cur_++;
      --left_;
    }
    return new (s) T();
  }
  void free(T* p) {
    ++frees_;
    --used_;
    p->~T();
    Slot* s = reinterpret_cast<Slot*>(p);
    s->next = free_;
    free_ = s;
  }
  PoolStats stats() const { return {used_, peak_, cap_, allocs_, frees_, cap_ * sizeof(Slot)}; }
};

// Open addressing, linear probing, power-of-two capacity, load <= 0.5, backward-shift deletion
// (no tombstones). Maps Id -> Node* (any pointer type).
template <class V>
class IdMap {
  struct E {
    uint64_t k = 0;
    V* v = nullptr;
  };
  std::vector<E> t_;
  size_t mask_, n_ = 0;
  int shift_;
  size_t home(uint64_t k) const { return (k * 0x9E3779B97F4A7C15ull) >> shift_; }
  void init(size_t cap) {
    t_.assign(cap, E{});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
  }
  void grow() {
    std::vector<E> old = std::move(t_);
    init(old.size() * 2);
    n_ = 0;
    for (auto& e : old)
      if (e.k) insert(e.k, e.v);
  }

 public:
  explicit IdMap(size_t expect = 1024) {
    size_t cap = 16;
    while (cap < expect * 2) cap <<= 1;
    init(cap);
  }
  size_t size() const { return n_; }
  size_t bytes() const { return t_.size() * sizeof(E); }
  V* find(uint64_t k) const {
    for (size_t i = home(k);; i = (i + 1) & mask_) {
      if (t_[i].k == k) return t_[i].v;
      if (!t_[i].k) return nullptr;
    }
  }
  bool insert(uint64_t k, V* v) {
    if ((n_ + 1) * 2 > t_.size()) grow();
    for (size_t i = home(k);; i = (i + 1) & mask_) {
      if (t_[i].k == k) return false;
      if (!t_[i].k) {
        t_[i] = {k, v};
        ++n_;
        return true;
      }
    }
  }
  void erase(uint64_t k) {
    size_t i = home(k);
    while (t_[i].k != k) {
      if (!t_[i].k) return;
      i = (i + 1) & mask_;
    }
    for (size_t j = i;;) {  // backward shift: pull later probe-chain members into the hole
      j = (j + 1) & mask_;
      if (!t_[j].k) break;
      if (((j - home(t_[j].k)) & mask_) >= ((j - i) & mask_)) {
        t_[i] = t_[j];
        i = j;
      }
    }
    t_[i] = E{};
    --n_;
  }
};

// Fixed-capacity circular buffer, overwrites the oldest element when full. Index 0 = oldest.
template <class T>
class Ring {
  std::vector<T> b_;
  size_t head_ = 0, n_ = 0;

 public:
  explicit Ring(size_t cap) : b_(cap) {}
  void push(const T& v) {
    if (n_ == b_.size()) {
      b_[head_] = v;
      head_ = (head_ + 1) % b_.size();
    } else {
      b_[(head_ + n_++) % b_.size()] = v;
    }
  }
  size_t size() const { return n_; }
  const T& at(size_t i) const { return b_[(head_ + i) % b_.size()]; }
  void clear() { head_ = n_ = 0; }
};

}  // namespace chronos
