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
  // Seqlock: writers increment before/after modifying slot fields (while holding
  // slot_mutex).  Readers snapshot seq before reading; if odd or mismatched
  // after, they retry.  This eliminates mutex acquisition from the read path.
  mutable std::atomic<uint32_t> seq{0};

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
  // When known_new=true, skip the existing-key scan — caller guarantees
  // key is not present, so we probe for the first Empty/Tombstone directly.
  Status upsert(Key k, Value v, bool known_new = false);
  LookupResult lookup(Key k);
  bool has_absent(Key k) const;
  Status mark_absent(Key k);
  Status try_place_placeholder(Key k, int* out_idx);
  Status fill_placeholder(int idx, Value v);
  Status fill_placeholder_absent(int idx);
  int occupied_count() const;
  // Count Occupied + Placeholder in a single pass — both types consume
  // a slot that could otherwise be reused for a new entry.
  int live_count() const;

  bool sorted_flag() const {
    return sorted_flag_.load(std::memory_order_acquire);
  }
  void set_sorted_flag(bool v) {
    sorted_flag_.store(v, std::memory_order_release);
  }
  void clear_sorted_flag() {
    sorted_flag_.store(false, std::memory_order_release);
  }

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
  // Clear all Occupied slots that are NOT dirty (i.e., already flushed).
  // Safe to call after SSD write — data loss if called before.
  void clear_clean_occupied();
  // Clear a specific slot by key, but ONLY if Occupied and clean (!dirty).
  // Returns true if the slot was cleared.
  bool evict_clean_slot(Key k);

  // Task 12: range scan helpers
  void sort_and_set_flag();
  std::vector<Key> absent_keys() const;

  // Clear all slots (for debug, does not flush)
  void clear();

  KeyLockTable& key_locks() { return key_locks_; }

  // Compact all entries to remove tombstones — O(64), called rarely.
  void rehash();
  void maybe_rehash();

 private:
  CacheSlot slots_[kCacheSlots];
  std::atomic<size_t> hand_{0};
  std::atomic<bool> sorted_flag_{false};
  KeyLockTable key_locks_;
  std::atomic<int> tombstone_count_{0};
  std::atomic<int> live_count_{0};  // Occupied + Placeholder, maintained atomically
};

}  // namespace cbtree
