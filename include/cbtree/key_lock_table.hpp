#pragma once
#include <atomic>
#include <cstddef>
#include "cbtree/types.hpp"

namespace cbtree {

// Key-level write serialization via 64-bit atomic bitmap.
// Each of the 64 stripes is one bit; lock acquires via CAS on that bit.
// Replaces the old 64 × std::mutex (2560 B) with a single 8 B atomic.
// With 64 stripes and short critical sections, contention is negligible —
// threads spin briefly on CAS failure, then retry.
class KeyLockTable {
 public:
  static constexpr size_t kStripes = 64;

  void lock(Key key) {
    size_t stripe = key % kStripes;
    uint64_t mask = 1ULL << stripe;
    while (true) {
      uint64_t cur = locks_.load(std::memory_order_acquire);
      if (!(cur & mask)) {
        if (locks_.compare_exchange_weak(cur, cur | mask,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
          return;
        }
      }
      // Contention is rare — busy-wait is cheaper than a kernel transition
      // for the microsecond-scale critical sections in this codebase.
    }
  }

  void unlock(Key key) {
    size_t stripe = key % kStripes;
    locks_.fetch_and(~(1ULL << stripe), std::memory_order_release);
  }

 private:
  std::atomic<uint64_t> locks_{0};
};

class KeyLockGuard {
 public:
  KeyLockGuard(KeyLockTable& table, Key key)
      : table_(&table), key_(key) {
    table_->lock(key_);
  }
  ~KeyLockGuard() { table_->unlock(key_); }
  KeyLockGuard(const KeyLockGuard&) = delete;
  KeyLockGuard& operator=(const KeyLockGuard&) = delete;

 private:
  KeyLockTable* table_;
  Key key_;
};

}  // namespace cbtree
