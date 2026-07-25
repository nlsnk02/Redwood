#include <algorithm>
#include "cbtree/cache_attachment.hpp"
#include "cbtree/fingerprint.hpp"

namespace cbtree {

// ---- Open-addressing helpers ----

// Probe chain: starting from fp % kCacheSlots, scan forward linearly.
// Empty   → terminates the chain (key not present)
// Tombstone → skipped but remembered as first candidate for insertion
// Occupied / Placeholder / Absent → check fp+key; if match, it's the target

Status CacheAttachment::upsert(Key k, Value v, bool known_new) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  int first_free = -1;  // first Tombstone or (end-of-chain) Empty

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    SlotState st = slots_[idx].state;

    if (st == SlotState::Empty) {
      if (known_new) {
        // Caller guarantees key not present — claim Empty immediately.
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        slots_[idx].fp = fp;
        slots_[idx].key = k;
        slots_[idx].value = v;
        slots_[idx].state = SlotState::Occupied;
        slots_[idx].dirty = true;
        slots_[idx].clock_bit.store(true, std::memory_order_release);
        slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        return Status::Ok;
      }
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;  // end of probe chain — key not present
    }

    if (st == SlotState::Tombstone) {
      if (known_new) {
        // Recycle tombstone immediately.
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        slots_[idx].fp = fp;
        slots_[idx].key = k;
        slots_[idx].value = v;
        slots_[idx].state = SlotState::Occupied;
        slots_[idx].dirty = true;
        slots_[idx].clock_bit.store(true, std::memory_order_release);
        slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        return Status::Ok;
      }
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;  // keep probing — key may still be further along
    }

    if (known_new) continue;  // skip existing-key check

    // st is Occupied, Placeholder, or Absent
    if (slots_[idx].fp != fp) continue;
    if (slots_[idx].key != k) continue;

    // Found existing entry for this key — update in-place
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    slots_[idx].value = v;
    slots_[idx].state = SlotState::Occupied;
    slots_[idx].dirty = true;
    slots_[idx].clock_bit.store(true, std::memory_order_release);
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    return Status::Ok;
  }

  if (known_new) return Status::Full;

  // Key not found — claim first available slot (tombstone or end-of-chain empty)
  if (first_free < 0) return Status::Full;

  {
    std::lock_guard<std::mutex> lock(slots_[first_free].slot_mutex);
    SlotState st = slots_[first_free].state;
    if (st == SlotState::Empty || st == SlotState::Tombstone) {
      if (st == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      slots_[first_free].fp = fp;
      slots_[first_free].key = k;
      slots_[first_free].value = v;
      slots_[first_free].state = SlotState::Occupied;
      slots_[first_free].dirty = true;
      slots_[first_free].clock_bit.store(true, std::memory_order_release);
      slots_[first_free].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
    } else {
      // Slot was taken by another thread's key — rare race, caller retries.
      return Status::Full;
    }
  }
  return Status::Ok;
}

LookupResult CacheAttachment::lookup(Key k) {
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    // Seqlock-consistent read: snapshot seq, read fields, verify seq unchanged.
    // Writers hold slot_mutex and increment seq around writes, so a stable seq
    // guarantees a consistent snapshot without the reader acquiring the mutex.
    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    bool ok = false;
    for (int r = 0; r < 4; ++r) {
      uint32_t s1 = slots_[idx].seq.load(std::memory_order_acquire);
      if (s1 & 1) continue;  // write in progress — retry
      st = slots_[idx].state;
      sfp = slots_[idx].fp;
      sk = slots_[idx].key;
      sv = slots_[idx].value;
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t s2 = slots_[idx].seq.load(std::memory_order_relaxed);
      if (s1 == s2) { ok = true; break; }
    }
    if (!ok) continue;  // concurrent write — skip this slot

    if (st == SlotState::Empty) {
      // End of probe chain — key not in this cache
      break;
    }

    if (st == SlotState::Tombstone) continue;

    // st is Occupied, Placeholder, or Absent
    if (sfp != fp) continue;
    if (sk != k) continue;

    // Found the key
    if (st == SlotState::Occupied) {
      slots_[idx].clock_bit.store(true, std::memory_order_release);
      return {Status::Ok, sv};
    }
    if (st == SlotState::Placeholder) {
      LookupResult r{Status::NotFound};
      r.placeholder_idx = static_cast<int>(idx);
      return r;
    }
    // Absent
    LookupResult r{Status::NotFound};
    r.absent = true;
    return r;
  }

  return {Status::NotFound};
}

bool CacheAttachment::has_absent(Key k) const {
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    // Seqlock-consistent read
    SlotState st;
    Fingerprint sfp;
    Key sk;
    bool ok = false;
    for (int r = 0; r < 4; ++r) {
      uint32_t s1 = slots_[idx].seq.load(std::memory_order_acquire);
      if (s1 & 1) continue;
      st = slots_[idx].state;
      sfp = slots_[idx].fp;
      sk = slots_[idx].key;
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t s2 = slots_[idx].seq.load(std::memory_order_relaxed);
      if (s1 == s2) { ok = true; break; }
    }
    if (!ok) continue;

    if (st == SlotState::Empty) break;
    if (st == SlotState::Tombstone) continue;
    if (st != SlotState::Absent) {
      if (sfp != fp) continue;
      if (sk != k) continue;
    }
    // Only return true for Absent with matching fp+key
    if (sfp != fp) continue;
    if (sk != k) continue;
    if (st == SlotState::Absent) return true;
    // Occupied or Placeholder — still the same key, but not absent
    break;
  }
  return false;
}

Status CacheAttachment::mark_absent(Key k) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  int first_free = -1;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    SlotState st = slots_[idx].state;

    if (st == SlotState::Empty) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;
    }

    if (st == SlotState::Tombstone) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;
    }

    if (slots_[idx].fp != fp) continue;
    if (slots_[idx].key != k) continue;

    // Found existing entry — mark as Absent
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    slots_[idx].state = SlotState::Absent;
    slots_[idx].dirty = true;
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    return Status::Ok;
  }

  // Key not present — insert as Absent
  if (first_free < 0) return Status::Full;

  {
    std::lock_guard<std::mutex> lock(slots_[first_free].slot_mutex);
    SlotState st = slots_[first_free].state;
    if (st == SlotState::Empty || st == SlotState::Tombstone) {
      if (st == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      slots_[first_free].fp = fp;
      slots_[first_free].key = k;
      slots_[first_free].state = SlotState::Absent;
      slots_[first_free].dirty = true;
      slots_[first_free].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      return Status::Ok;
    }
  }
  return Status::Full;
}

Status CacheAttachment::try_place_placeholder(Key k, int* out_idx) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  int first_free = -1;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    SlotState st = slots_[idx].state;

    if (st == SlotState::Empty) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;
    }

    if (st == SlotState::Tombstone) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;
    }

    // Check if this entry matches our key
    if (slots_[idx].fp != fp) continue;
    if (slots_[idx].key != k) continue;

    if (st == SlotState::Placeholder) {
      // Placeholder already exists for this key
      if (out_idx) *out_idx = static_cast<int>(idx);
      return Status::Ok;
    }
    // Key already exists as Occupied or Absent — no placeholder needed
    if (out_idx) *out_idx = -1;
    return Status::Ok;
  }

  // Key not found — create placeholder
  if (first_free < 0) return Status::Full;

  {
    std::lock_guard<std::mutex> lock(slots_[first_free].slot_mutex);
    SlotState st = slots_[first_free].state;
    if (st == SlotState::Empty || st == SlotState::Tombstone) {
      if (st == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      slots_[first_free].fp = fp;
      slots_[first_free].key = k;
      slots_[first_free].state = SlotState::Placeholder;
      slots_[first_free].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      if (out_idx) *out_idx = static_cast<int>(first_free);
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
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
  slots_[idx].value = v;
  slots_[idx].state = SlotState::Occupied;
  slots_[idx].dirty = true;
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
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
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
  slots_[idx].state = SlotState::Absent;
  slots_[idx].dirty = true;
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
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

    {
      std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
      SlotState st = slots_[idx].state;
      if (st == SlotState::Empty || st == SlotState::Placeholder ||
          st == SlotState::Tombstone)
        continue;

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
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
      slots_[idx].state = SlotState::Tombstone;
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      slots_[idx].clock_bit.store(false, std::memory_order_release);
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
    }
    return Status::Ok;
  }

  // Fallback: all non-empty/non-tombstone slots are PLACEHOLDERs — evict the first one
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state != SlotState::Placeholder) continue;

    *out_key = slots_[i].key;
    *out_val = 0;
    *out_dirty = false;
    slots_[i].seq.fetch_add(1, std::memory_order_release);
    slots_[i].state = SlotState::Tombstone;
    tombstone_count_.fetch_add(1, std::memory_order_relaxed);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
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
      if (st == SlotState::Empty || st == SlotState::Placeholder ||
          st == SlotState::Tombstone)
        continue;

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

  // Fallback: all non-empty/non-tombstone slots are PLACEHOLDERs
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
  // Allow clearing Occupied, Absent, and Placeholder states.
  if (slots_[idx].state != SlotState::Occupied &&
      slots_[idx].state != SlotState::Absent &&
      slots_[idx].state != SlotState::Placeholder) return false;
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
  slots_[idx].state = SlotState::Tombstone;
  tombstone_count_.fetch_add(1, std::memory_order_relaxed);
  slots_[idx].clock_bit.store(false, std::memory_order_release);
  slots_[idx].seq.fetch_add(1, std::memory_order_release);
  return true;
}

Status CacheAttachment::split_into(Key mid, CacheAttachment* right) {
  if (!right) return Status::Error;

  // Collect all valid entries from this cache.
  struct Entry {
    Key key;
    Value value;
    SlotState state;
    bool dirty;
    Fingerprint fp;
  };
  std::vector<Entry> entries;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    SlotState st = slots_[i].state;
    if (st == SlotState::Occupied || st == SlotState::Placeholder ||
        st == SlotState::Absent) {
      entries.push_back(
          {slots_[i].key, slots_[i].value, st, slots_[i].dirty, slots_[i].fp});
    }
  }

  // Clear both caches (resets to Empty, zero tombstones).
  clear();
  right->clear();

  // Re-insert into appropriate cache using open addressing.
  for (const auto& e : entries) {
    CacheAttachment* target = (e.key < mid) ? this : right;
    Fingerprint fp = e.fp;
    size_t start = fp % kCacheSlots;
    bool placed = false;
    for (size_t i = 0; i < kCacheSlots; ++i) {
      size_t idx = (start + i) % kCacheSlots;
      std::lock_guard<std::mutex> lock(target->slots_[idx].slot_mutex);
      if (target->slots_[idx].state == SlotState::Empty) {
        target->slots_[idx].seq.fetch_add(1, std::memory_order_release);
        target->slots_[idx].fp = fp;
        target->slots_[idx].key = e.key;
        target->slots_[idx].value = e.value;
        target->slots_[idx].state = e.state;
        target->slots_[idx].dirty = e.dirty;
        target->slots_[idx].clock_bit.store(false, std::memory_order_release);
        target->slots_[idx].seq.fetch_add(1, std::memory_order_release);
        placed = true;
        break;
      }
    }
    if (!placed) {
      // This shouldn't happen — both caches are empty after clear().
      return Status::Full;
    }
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
      slots_[i].seq.fetch_add(1, std::memory_order_release);
      slots_[i].dirty = false;
      slots_[i].seq.fetch_add(1, std::memory_order_release);
    }
  }
}

bool CacheAttachment::evict_clean_slot(Key k) {
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;
  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;
    std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
    SlotState st = slots_[idx].state;

    if (st == SlotState::Empty) return false;  // end of chain, key not found
    if (st == SlotState::Tombstone) continue;
    if (st != SlotState::Occupied) continue;
    if (slots_[idx].fp != fp) continue;
    if (slots_[idx].key != k) continue;
    // Only clear if clean — another thread may have re-dirtied it.
    if (slots_[idx].dirty) return false;
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    slots_[idx].state = SlotState::Tombstone;
    tombstone_count_.fetch_add(1, std::memory_order_relaxed);
    slots_[idx].clock_bit.store(false, std::memory_order_release);
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    return true;
  }
  return false;
}

void CacheAttachment::clear_clean_occupied() {
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    if (slots_[i].state == SlotState::Occupied && !slots_[i].dirty) {
      slots_[i].seq.fetch_add(1, std::memory_order_release);
      slots_[i].state = SlotState::Tombstone;
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      slots_[i].clock_bit.store(false, std::memory_order_release);
      slots_[i].seq.fetch_add(1, std::memory_order_release);
    }
  }
}

void CacheAttachment::clear() {
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
    slots_[i].state = SlotState::Empty;
    slots_[i].clock_bit.store(false, std::memory_order_release);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
  }
  tombstone_count_.store(0, std::memory_order_relaxed);
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
  // Open addressing: entries are at probe-determined positions, not key order.
  // occupied_sorted() always does a full scan + sort, so this is a no-op
  // that simply marks the flag to avoid future calls.
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

void CacheAttachment::rehash() {
  // Collect all live entries (Occupied, Placeholder, Absent).
  struct Entry {
    Key key;
    Value value;
    SlotState state;
    bool dirty;
    Fingerprint fp;
  };
  std::vector<Entry> entries;
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    SlotState st = slots_[i].state;
    if (st == SlotState::Occupied || st == SlotState::Placeholder ||
        st == SlotState::Absent) {
      entries.push_back(
          {slots_[i].key, slots_[i].value, st, slots_[i].dirty, slots_[i].fp});
    }
  }

  // Clear all slots to Empty, reset tombstones.
  for (int i = 0; i < kCacheSlots; ++i) {
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
    slots_[i].state = SlotState::Empty;
    slots_[i].clock_bit.store(false, std::memory_order_release);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
  }
  tombstone_count_.store(0, std::memory_order_relaxed);

  // Re-insert using open addressing.
  for (const auto& e : entries) {
    size_t start = e.fp % kCacheSlots;
    for (size_t i = 0; i < kCacheSlots; ++i) {
      size_t idx = (start + i) % kCacheSlots;
      std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);
      if (slots_[idx].state == SlotState::Empty) {
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        slots_[idx].fp = e.fp;
        slots_[idx].key = e.key;
        slots_[idx].value = e.value;
        slots_[idx].state = e.state;
        slots_[idx].dirty = e.dirty;
        slots_[idx].clock_bit.store(false, std::memory_order_release);
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        break;
      }
    }
    // If we can't re-insert, the cache is logically full — shouldn't happen
    // since we're inserting the same number of entries we collected.
  }
}

void CacheAttachment::maybe_rehash() {
  // Automatic rehash is intentionally disabled.
  // Tombstones are naturally recycled by insertions (upsert/upsert_new
  // reclaim the first Tombstone in the probe chain).  Rehash remains
  // available as a public method for explicit maintenance calls.
  //
  // Reasoning: rehash clears all slots and rebuilds, which creates a
  // window where concurrent lookups see all-Empty slots and miss live
  // entries.  Natural tombstone recycling avoids this race entirely.
}

}  // namespace cbtree
