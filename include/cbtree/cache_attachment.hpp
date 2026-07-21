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
  mutable std::mutex slot_mutex;
  std::atomic<uint32_t> generation{0};  // incremented on reuse, for ABA detection
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
  // Two-phase eviction: find victim without clearing, then evict after SSD write.
  // Captures slot generation for ABA detection.
  Status find_clock_victim(Key* out_key, Value* out_val, bool* out_dirty,
                           int* out_idx, uint32_t* out_gen);
  // Verify key + generation match before clearing. Aborts if slot was recycled.
  // Returns true if slot was cleared, false if generation/stale mismatch.
  bool evict_slot(int idx, Key expected_key, uint32_t expected_gen);
  Status split_into(Key mid, CacheAttachment* right);
  std::vector<std::pair<Key, Value>> occupied_sorted();
  void flush_dirty(std::vector<std::pair<Key, Value>>& out);

  // Task 12: range scan helpers
  void sort_and_set_flag();
  std::vector<Key> absent_keys() const;

  // Clear all slots (for debug, does not flush)
  void clear();

  KeyLockTable& key_locks() { return key_locks_; }

 private:
  CacheSlot slots_[kCacheSlots];
  std::atomic<size_t> hand_{0};
  std::atomic<bool> sorted_flag_{false};
  KeyLockTable key_locks_;
};

}  // namespace cbtree
