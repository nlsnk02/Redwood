#include <algorithm>
#include "cbtree/cache_attachment.hpp"
#include "cbtree/fingerprint.hpp"

namespace cbtree {

Status CacheAttachment::upsert(Key k, Value v) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);

  // Search for existing slot with matching key (Occupied, Absent, Placeholder)
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    // Found existing slot -- update in-place
    slots_[i].value = v;
    slots_[i].state = SlotState::Occupied;
    slots_[i].dirty = true;
    slots_[i].clock_bit.store(true, std::memory_order_release);
    return Status::Ok;
  }

  // No existing slot -- find an empty one and claim it
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) {
      slots_[i].fp = fp;
      slots_[i].key = k;
      slots_[i].value = v;
      slots_[i].state = SlotState::Occupied;
      slots_[i].dirty = true;
      slots_[i].clock_bit.store(true, std::memory_order_release);
      slots_[i].generation.fetch_add(1, std::memory_order_relaxed);
      return Status::Ok;
    }
  }

  return Status::Full;
}

LookupResult CacheAttachment::lookup(Key k) {
  Fingerprint fp = fingerprint(k);
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    SlotState st = slots_[i].state;
    if (st == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    if (st == SlotState::Occupied) {
      slots_[i].clock_bit.store(true, std::memory_order_release);
      return {Status::Ok, slots_[i].value};
    }
    // Placeholder or Absent -- not a hit
    return {Status::NotFound};
  }
  return {Status::NotFound};
}

bool CacheAttachment::has_absent(Key k) const {
  Fingerprint fp = fingerprint(k);
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    SlotState st = slots_[i].state;
    if (st != SlotState::Absent) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key == k) return true;
  }
  return false;
}

Status CacheAttachment::mark_absent(Key k) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);

  // Check if key already exists in any slot
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    // Found -- mark as absent
    slots_[i].state = SlotState::Absent;
    slots_[i].dirty = true;
    return Status::Ok;
  }

  // Key not present -- insert as absent
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) {
      slots_[i].fp = fp;
      slots_[i].key = k;
      slots_[i].state = SlotState::Absent;
      slots_[i].dirty = true;
      slots_[i].generation.fetch_add(1, std::memory_order_relaxed);
      return Status::Ok;
    }
  }

  return Status::Full;
}

Status CacheAttachment::try_place_placeholder(Key k, int* out_idx) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);

  // Check if placeholder already exists for this key
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state != SlotState::Placeholder) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key == k) {
      if (out_idx) *out_idx = i;
      return Status::Ok;
    }
  }

  // Find empty slot and claim it
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) {
      slots_[i].fp = fp;
      slots_[i].key = k;
      slots_[i].state = SlotState::Placeholder;
      slots_[i].generation.fetch_add(1, std::memory_order_relaxed);
      if (out_idx) *out_idx = i;
      return Status::Ok;
    }
  }

  return Status::Full;
}

Status CacheAttachment::fill_placeholder(int idx, Value v) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Lock slot first to safely read key, then release to avoid lock-order inversion
  Key k;
  {
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    if (slots_[idx].state != SlotState::Placeholder) {
      return Status::Error;
    }
    k = slots_[idx].key;
  }

  // Lock key, then re-lock slot (consistent with key->slot ordering)
  KeyLockGuard key_guard(key_locks_, k);
  std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
  if (slots_[idx].state != SlotState::Placeholder || slots_[idx].key != k) {
    return Status::Error;
  }
  slots_[idx].value = v;
  slots_[idx].state = SlotState::Occupied;
  slots_[idx].dirty = true;
  return Status::Ok;
}

Status CacheAttachment::fill_placeholder_absent(int idx) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Lock slot first to safely read key, then release to avoid lock-order inversion
  Key k;
  {
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    if (slots_[idx].state != SlotState::Placeholder) {
      return Status::Error;
    }
    k = slots_[idx].key;
  }

  // Lock key, then re-lock slot (consistent with key->slot ordering)
  KeyLockGuard key_guard(key_locks_, k);
  std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
  if (slots_[idx].state != SlotState::Placeholder || slots_[idx].key != k) {
    return Status::Error;
  }
  slots_[idx].state = SlotState::Absent;
  slots_[idx].dirty = true;
  return Status::Ok;
}

int CacheAttachment::occupied_count() const {
  int count = 0;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Occupied) ++count;
  }
  return count;
}

Status CacheAttachment::pick_clock_victim(Key* out_key, Value* out_val,
                                         bool* out_dirty) {
  if (!out_key || !out_val || !out_dirty) return Status::Error;

  // Scan up to 2 * kCacheSlots positions so that even when every
  // occupied slot starts with clock_bit == true the first pass
  // clears all bits and the second pass finds a victim.
  for (int round = 0; round < 2 * kCacheSlots; ++round) {
    size_t idx = hand_.fetch_add(1, std::memory_order_relaxed) % kCacheSlots;

    // Lock slot to safely read state
    {
      std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
      SlotState st = slots_[idx].state;
      if (st == SlotState::Empty || st == SlotState::Placeholder) continue;

      // OCCUPIED or ABSENT -- check clock bit
      bool bit = slots_[idx].clock_bit.load(std::memory_order_acquire);
      if (bit) {
        slots_[idx].clock_bit.store(false, std::memory_order_release);
        continue;  // give it a second chance
      }

      // Re-check after clock_bit update (someone might have touched it)
      if (slots_[idx].clock_bit.load(std::memory_order_acquire)) {
        continue;
      }

      // Verify state is still Occupied or Absent
      if (slots_[idx].state != SlotState::Occupied &&
          slots_[idx].state != SlotState::Absent) {
        continue;
      }

      // It's ours
      *out_key = slots_[idx].key;
      *out_val = slots_[idx].value;
      *out_dirty = slots_[idx].dirty;
      slots_[idx].state = SlotState::Empty;
      slots_[idx].clock_bit.store(false, std::memory_order_release);
    }
    return Status::Ok;
  }

  // Fallback: all non-empty slots are PLACEHOLDERs -- evict the first one
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state != SlotState::Placeholder) continue;

    *out_key = slots_[i].key;
    *out_val = 0;
    *out_dirty = false;
    slots_[i].state = SlotState::Empty;
    return Status::Ok;
  }

  return Status::Error;  // nothing to evict
}

Status CacheAttachment::find_clock_victim(Key* out_key, Value* out_val,
                                       bool* out_dirty, int* out_idx,
                                       uint32_t* out_gen) {
  if (!out_key || !out_val || !out_dirty || !out_idx || !out_gen) return Status::Error;

  for (int round = 0; round < 2 * kCacheSlots; ++round) {
    size_t idx = hand_.fetch_add(1, std::memory_order_relaxed) % kCacheSlots;

    {
      std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
      SlotState st = slots_[idx].state;
      if (st == SlotState::Empty || st == SlotState::Placeholder) continue;

      // OCCUPIED or ABSENT -- check clock bit
      bool bit = slots_[idx].clock_bit.load(std::memory_order_acquire);
      if (bit) {
        slots_[idx].clock_bit.store(false, std::memory_order_release);
        continue;
      }

      // Re-verify state and clock_bit
      if (slots_[idx].state != SlotState::Occupied &&
          slots_[idx].state != SlotState::Absent) {
        continue;
      }
      if (slots_[idx].clock_bit.load(std::memory_order_acquire)) {
        continue;
      }

      // Found victim -- copy data + generation snapshot (does NOT clear)
      *out_key = slots_[idx].key;
      *out_val = slots_[idx].value;
      *out_dirty = slots_[idx].dirty;
      *out_idx = static_cast<int>(idx);
      *out_gen = slots_[idx].generation.load(std::memory_order_acquire);
    }
    return Status::Ok;
  }

  // Fallback: all non-empty slots are PLACEHOLDERs
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state != SlotState::Placeholder) continue;

    *out_key = slots_[i].key;
    *out_val = 0;
    *out_dirty = false;
    *out_idx = i;
    *out_gen = slots_[i].generation.load(std::memory_order_acquire);
    return Status::Ok;
  }

  return Status::Error;
}

bool CacheAttachment::evict_slot(int idx, Key expected_key, uint32_t expected_gen) {
  if (idx < 0 || idx >= kCacheSlots) return false;
  std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
  // Verify the slot still contains the same key+generation we selected.
  // A generation mismatch means another thread recycled the slot (ABA).
  if (slots_[idx].key != expected_key) return false;
  if (slots_[idx].generation.load(std::memory_order_acquire) != expected_gen)
    return false;
  // Allow clearing Occupied, Absent, and Placeholder states (C1 fix).
  if (slots_[idx].state != SlotState::Occupied &&
      slots_[idx].state != SlotState::Absent &&
      slots_[idx].state != SlotState::Placeholder) return false;
  slots_[idx].state = SlotState::Empty;
  slots_[idx].clock_bit.store(false, std::memory_order_release);
  return true;
}

Status CacheAttachment::split_into(Key mid, CacheAttachment* right) {
  if (!right) return Status::Error;

  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Empty) continue;
    if (slots_[i].key < mid) continue;

    // Find an empty slot in the right cache
    int right_empty = -1;
    for (int j = 0; j < kCacheSlots; ++j) {
      std::lock_guard<std::mutex> rlock(right->slots_[j].slot_mutex);
      if (right->slots_[j].state == SlotState::Empty) {
        right_empty = j;
        break;
      }
    }
    if (right_empty < 0) return Status::Full;

    // Copy slot contents to right (mutex is not copyable)
    {
      std::lock_guard<std::mutex> rlock(right->slots_[right_empty].slot_mutex);
      right->slots_[right_empty].state = slots_[i].state;
      right->slots_[right_empty].key = slots_[i].key;
      right->slots_[right_empty].value = slots_[i].value;
      right->slots_[right_empty].fp = slots_[i].fp;
      right->slots_[right_empty].dirty = slots_[i].dirty;
      right->slots_[right_empty].clock_bit.store(false,
                                                  std::memory_order_release);
    }

    // Clear the left slot
    slots_[i].state = SlotState::Empty;
    slots_[i].clock_bit.store(false, std::memory_order_release);
  }

  clear_sorted_flag();
  right->clear_sorted_flag();
  return Status::Ok;
}

void CacheAttachment::flush_dirty(std::vector<std::pair<Key, Value>>& out) {
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Occupied && slots_[i].dirty) {
      out.emplace_back(slots_[i].key, slots_[i].value);
      slots_[i].dirty = false;
    }
  }
}

bool CacheAttachment::evict_clean_slot(Key k) {
  Fingerprint fp = fingerprint(k);
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state != SlotState::Occupied) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;
    // Only clear if clean — another thread may have re-dirtied it.
    if (slots_[i].dirty) return false;
    slots_[i].state = SlotState::Empty;
    slots_[i].clock_bit.store(false, std::memory_order_release);
    return true;
  }
  return false;
}

void CacheAttachment::clear_clean_occupied() {
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Occupied && !slots_[i].dirty) {
      slots_[i].state = SlotState::Empty;
      slots_[i].clock_bit.store(false, std::memory_order_release);
    }
  }
}

void CacheAttachment::clear() {
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    slots_[i].state = SlotState::Empty;
    slots_[i].clock_bit.store(false, std::memory_order_release);
  }
}

std::vector<std::pair<Key, Value>> CacheAttachment::occupied_sorted() {
  std::vector<std::pair<Key, Value>> result;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Occupied) {
      result.emplace_back(slots_[i].key, slots_[i].value);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void CacheAttachment::sort_and_set_flag() {
  // Collect all Occupied and Absent entries with their metadata
  struct SlotData {
    Key key;
    Value value;
    SlotState state;
    bool dirty;
    Fingerprint fp;
  };
  std::vector<SlotData> entries;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    SlotState st = slots_[i].state;
    if (st == SlotState::Occupied || st == SlotState::Absent) {
      entries.push_back({slots_[i].key, slots_[i].value, st, slots_[i].dirty,
                         slots_[i].fp});
    }
  }

  // Sort by key
  std::sort(entries.begin(), entries.end(),
            [](const SlotData& a, const SlotData& b) { return a.key < b.key; });

  // Rearrange: sorted entries first, then empty slots
  size_t pos = 0;
  for (const auto& e : entries) {
    std::lock_guard<std::mutex> lock(slots_[pos].slot_mutex);
    slots_[pos].key = e.key;
    slots_[pos].value = e.value;
    slots_[pos].state = e.state;
    slots_[pos].dirty = e.dirty;
    slots_[pos].fp = e.fp;
    slots_[pos].clock_bit.store(false, std::memory_order_release);
    ++pos;
  }
  for (; pos < kCacheSlots; ++pos) {
    std::lock_guard<std::mutex> lock(slots_[pos].slot_mutex);
    slots_[pos].state = SlotState::Empty;
    slots_[pos].clock_bit.store(false, std::memory_order_release);
  }

  set_sorted_flag(true);
}

std::vector<Key> CacheAttachment::absent_keys() const {
  std::vector<Key> result;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Absent) {
      result.push_back(slots_[i].key);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace cbtree
