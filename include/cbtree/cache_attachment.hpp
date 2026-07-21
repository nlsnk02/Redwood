#pragma once
#include <atomic>
#include <mutex>
#include <cstddef>
#include <vector>
#include <utility>
#include "cbtree/types.hpp"
#include "cbtree/key_lock_table.hpp"

namespace cbtree {

struct CacheSlot {
  SlotState state{SlotState::Empty};
  Key key{};
  Value value{};
  Fingerprint fp{};
  bool dirty{false};
  std::atomic<bool> clock_bit{false};
  std::mutex slot_mutex;
};

class CacheAttachment {
 public:
  Status upsert(Key k, Value v);
  LookupResult lookup(Key k);
  bool has_absent(Key k) const;
  Status mark_absent(Key k);
  Status try_place_placeholder(Key k, int* out_idx);
  Status fill_placeholder(int idx, Value v);
  Status fill_placeholder_absent(int idx);
  int occupied_count() const;

  bool sorted_flag() const {
    return sorted_flag_.load(std::memory_order_acquire);
  }
  void set_sorted_flag(bool v) {
    sorted_flag_.store(v, std::memory_order_release);
  }
  void clear_sorted_flag() {
    sorted_flag_.store(false, std::memory_order_release);
  }

  // Future tasks: declared only, not implemented in Task 4
  Status pick_clock_victim(Key* out_key, Value* out_val, bool* out_dirty);
  Status split_into(Key mid, CacheAttachment* right);
  std::vector<std::pair<Key, Value>> occupied_sorted();
  void flush_dirty(std::vector<std::pair<Key, Value>>& out);

  KeyLockTable& key_locks() { return key_locks_; }

 private:
  CacheSlot slots_[kCacheSlots];
  std::atomic<size_t> hand_{0};
  std::atomic<bool> sorted_flag_{false};
  KeyLockTable key_locks_;
};

}  // namespace cbtree
