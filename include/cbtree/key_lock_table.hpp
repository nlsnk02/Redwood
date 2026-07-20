#pragma once
#include <mutex>
#include <cstddef>
#include "cbtree/types.hpp"

namespace cbtree {

class KeyLockTable {
 public:
  static constexpr size_t kStripes = 64;

  void lock(Key key) { stripes_[key % kStripes].lock(); }
  void unlock(Key key) { stripes_[key % kStripes].unlock(); }

 private:
  std::mutex stripes_[kStripes];
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
