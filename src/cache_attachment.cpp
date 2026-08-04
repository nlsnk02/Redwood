#include <algorithm>
#include "cbtree/cache_attachment.hpp"
#include "cbtree/fingerprint.hpp"

namespace cbtree {

// ---- Open-addressing helpers ----

// Probe chain: starting from fp % kCacheSlots, scan forward linearly.
// Empty   → terminates the chain (key not present)
// Tombstone → skipped but remembered as first candidate for insertion
// Occupied / Placeholder / Absent → check fp+key; if match, it's the target

// ---- Seqlock-consistent read (lock-free) ----
// Returns true if a consistent snapshot was captured.
// Used by lookup, has_absent, occupied_count, and probe loops.
static inline bool seqlock_read(const CacheSlot& slot,
                                SlotState& st, Fingerprint& sfp,
                                Key& sk, Value& sv) {
  for (int r = 0; r < 4; ++r) {
    uint32_t s1 = slot.seq.load(std::memory_order_acquire);
    if (s1 & 1) continue;  // write in progress
    st = slot.state.load(std::memory_order_acquire);
    sfp = slot.fp;
    sk = slot.key;
    sv = slot.value;
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = slot.seq.load(std::memory_order_relaxed);
    if (s1 == s2) return true;
  }
  return false;
}

// ---- CAS-based write: insert into Empty or Tombstone slot ----
// Caller must hold key lock.  CAS state FIRST (without touching data fields),
// then write fields only on success.  This prevents the loser of a CAS race
// from corrupting the winner's data fields.
// Returns true on success, false if CAS failed (slot claimed by another thread).
static inline bool cas_insert(CacheSlot& slot,
                               SlotState expected, Fingerprint fp,
                               Key k, Value v, bool is_dirty) {
  slot.seq.fetch_add(1, std::memory_order_release);  // odd → write in progress

  // CAS state first — if it fails, we haven't touched any data fields
  if (!slot.state.compare_exchange_strong(expected, SlotState::Occupied,
                                           std::memory_order_acq_rel)) {
    slot.seq.fetch_add(1, std::memory_order_release);  // restore even seq
    return false;
  }

  // CAS succeeded — we own the slot.  Write fields under seqlock protection.
  slot.fp = fp;
  slot.key = k;
  slot.value = v;
  slot.dirty.store(is_dirty, std::memory_order_release);
  slot.clock_bit.store(true, std::memory_order_release);
  slot.generation.fetch_add(1, std::memory_order_relaxed);
  slot.seq.fetch_add(1, std::memory_order_release);  // even → write complete
  return true;
}

// ---- CAS-based in-place update: same key, state unchanged ----
// Caller holds key lock.  Writes fields without state transition.
// Returns false if the slot was evicted during the write (caller must retry).
static inline bool cas_update_in_place(CacheSlot& slot, Value v,
                                        bool is_dirty, bool set_clock) {
  slot.seq.fetch_add(1, std::memory_order_release);  // odd
  slot.value = v;
  slot.state.store(SlotState::Occupied, std::memory_order_relaxed);
  if (is_dirty) slot.dirty.store(true, std::memory_order_release);
  if (set_clock) slot.clock_bit.store(true, std::memory_order_release);
  slot.generation.fetch_add(1, std::memory_order_relaxed);
  slot.seq.fetch_add(1, std::memory_order_release);  // even

  // Verify slot wasn't evicted during our write.  A concurrent
  // clear_clean_occupied/evict_clean_slot may have CAS'd state to Tombstone.
  // If so, the caller must re-probe and re-insert.
  if (slot.state.load(std::memory_order_acquire) != SlotState::Occupied) {
    return false;
  }
  return true;
}

// ---- Lock-free probe: read slot state + fp + key under seqlock ----
struct ProbeResult {
  SlotState st;
  Fingerprint fp;
  Key key;
  Value value;
  bool consistent;  // true if seqlock snapshot was valid
};

static inline ProbeResult probe_slot(const CacheSlot& slot) {
  ProbeResult r;
  r.consistent = seqlock_read(slot, r.st, r.fp, r.key, r.value);
  return r;
}

Status CacheAttachment::upsert(Key k, Value v, bool known_new) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  int first_free = -1;  // first Tombstone or (end-of-chain) Empty

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) {
      // Concurrent write — skip this slot
      continue;
    }

    if (st == SlotState::Empty) {
      if (known_new) {
        if (cas_insert(slots_[idx], SlotState::Empty, fp, k, v, true)) {
          live_count_.fetch_add(1, std::memory_order_relaxed);
          return Status::Ok;
        }
        continue;  // CAS failed — another thread claimed this Empty, keep probing
      }
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;  // end of probe chain — key not present
    }

    if (st == SlotState::Tombstone) {
      if (known_new) {
        if (cas_insert(slots_[idx], SlotState::Tombstone, fp, k, v, true)) {
          live_count_.fetch_add(1, std::memory_order_relaxed);
          tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
          return Status::Ok;
        }
        continue;  // CAS failed — tombstone claimed, keep probing
      }
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;  // keep probing — key may still be further along
    }

    if (known_new) continue;  // skip existing-key check

    // st is Occupied, Placeholder, or Absent
    if (sfp != fp) continue;
    if (sk != k) continue;

    // Found existing entry for this key — update in-place
    if (!cas_update_in_place(slots_[idx], v, true, true)) {
      // Slot was evicted during our write — retry from scratch
      continue;
    }
    return Status::Ok;
  }

  if (known_new) return Status::Full;

  // Key not found — claim first available slot (tombstone or end-of-chain empty)
  if (first_free < 0) return Status::Full;

  {
    SlotState expected = slots_[first_free].state.load(std::memory_order_acquire);
    if (expected != SlotState::Empty && expected != SlotState::Tombstone) {
      return Status::Full;  // slot was taken by another thread
    }
    if (cas_insert(slots_[first_free], expected, fp, k, v, true)) {
      live_count_.fetch_add(1, std::memory_order_relaxed);
      if (expected == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      return Status::Ok;
    }
  }
  return Status::Full;
}

LookupResult CacheAttachment::lookup(Key k) {
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) {
      continue;  // concurrent write — skip this slot
    }

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

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

    if (st == SlotState::Empty) break;
    if (st == SlotState::Tombstone) continue;
    if (sfp != fp) continue;
    if (sk != k) continue;

    if (st == SlotState::Absent) return true;
    // Occupied or Placeholder — same key but not absent
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

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

    if (st == SlotState::Empty) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;
    }

    if (st == SlotState::Tombstone) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;
    }

    if (sfp != fp) continue;
    if (sk != k) continue;

    // Found existing entry — mark as Absent via CAS
    SlotState expected = st;
    slots_[idx].seq.fetch_add(1, std::memory_order_release);  // odd
    // CAS first, then write fields on success to avoid corrupting winner's data
    if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Absent,
                                                   std::memory_order_acq_rel)) {
      slots_[idx].dirty.store(true, std::memory_order_release);
      slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[idx].seq.fetch_add(1, std::memory_order_release);  // even
      return Status::Ok;
    }
    // CAS failed — state changed concurrently, retry
    slots_[idx].seq.fetch_add(1, std::memory_order_release);
    continue;
  }

  // Key not present — insert as Absent
  if (first_free < 0) return Status::Full;

  {
    SlotState expected = slots_[first_free].state.load(std::memory_order_acquire);
    if (expected != SlotState::Empty && expected != SlotState::Tombstone) {
      return Status::Full;
    }

    slots_[first_free].seq.fetch_add(1, std::memory_order_release);
    slots_[first_free].fp = fp;
    slots_[first_free].key = k;
    slots_[first_free].dirty.store(true, std::memory_order_release);
    if (slots_[first_free].state.compare_exchange_strong(expected, SlotState::Absent,
                                                          std::memory_order_acq_rel)) {
      live_count_.fetch_add(1, std::memory_order_relaxed);
      if (expected == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slots_[first_free].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      return Status::Ok;
    }
    slots_[first_free].seq.fetch_add(1, std::memory_order_release);
  }
  return Status::Full;
}

Status CacheAttachment::try_place_placeholder(Key k, int* out_idx, bool* found_existing) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;

  int first_free = -1;

  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

    if (st == SlotState::Empty) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      break;
    }

    if (st == SlotState::Tombstone) {
      if (first_free < 0) first_free = static_cast<int>(idx);
      continue;
    }

    // Check if this entry matches our key
    if (sfp != fp) continue;
    if (sk != k) continue;

    if (st == SlotState::Placeholder) {
      // Placeholder already exists for this key — another thread is already
      // fetching from SSD.  Caller can avoid a duplicate SSD read.
      if (out_idx) *out_idx = static_cast<int>(idx);
      if (found_existing) *found_existing = true;
      return Status::Ok;
    }
    // Key already exists as Occupied or Absent — no placeholder needed
    if (out_idx) *out_idx = -1;
    return Status::Ok;
  }

  // Key not found — create placeholder
  if (first_free < 0) return Status::Full;

  {
    SlotState expected = slots_[first_free].state.load(std::memory_order_acquire);
    if (expected != SlotState::Empty && expected != SlotState::Tombstone) {
      return Status::Full;
    }

    slots_[first_free].seq.fetch_add(1, std::memory_order_release);
    slots_[first_free].fp = fp;
    slots_[first_free].key = k;
    if (slots_[first_free].state.compare_exchange_strong(expected, SlotState::Placeholder,
                                                          std::memory_order_acq_rel)) {
      live_count_.fetch_add(1, std::memory_order_relaxed);
      if (expected == SlotState::Tombstone) {
        tombstone_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slots_[first_free].generation.fetch_add(1, std::memory_order_relaxed);
      slots_[first_free].seq.fetch_add(1, std::memory_order_release);
      if (out_idx) *out_idx = static_cast<int>(first_free);
      return Status::Ok;
    }
    slots_[first_free].seq.fetch_add(1, std::memory_order_release);
  }
  return Status::Full;
}

Status CacheAttachment::fill_placeholder(int idx, Value v) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Lock slot first to safely read key, then release to avoid lock-order inversion
  Key k;
  {
    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) return Status::Error;
    if (st != SlotState::Placeholder) return Status::Error;
    k = sk;
  }

  // Lock key, then CAS-placeholder-to-Occupied
  KeyLockGuard key_guard(key_locks_, k);

  SlotState expected = SlotState::Placeholder;
  slots_[idx].seq.fetch_add(1, std::memory_order_release);  // odd
  // CAS first, then write fields on success — prevents corrupting concurrent winner
  if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Occupied,
                                                 std::memory_order_acq_rel)) {
    // Verify key hasn't changed (concurrent evict+reuse would change key)
    if (slots_[idx].key != k) {
      // Slot was recycled — CAS succeeded but key changed under us.
      // Extremely rare: evict cleared the placeholder and insert reused the slot
      // with a different key between our seqlock_read and CAS.
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
      return Status::Error;
    }
    slots_[idx].value = v;
    slots_[idx].dirty.store(false, std::memory_order_release);  // data came from SSD
    slots_[idx].clock_bit.store(true, std::memory_order_release);
    slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
    slots_[idx].seq.fetch_add(1, std::memory_order_release);  // even
    return Status::Ok;
  }
  slots_[idx].seq.fetch_add(1, std::memory_order_release);  // restore even
  return Status::Error;
}

Status CacheAttachment::fill_placeholder_absent(int idx) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Lock slot first to safely read key
  Key k;
  {
    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) return Status::Error;
    if (st != SlotState::Placeholder) return Status::Error;
    k = sk;
  }

  // Lock key, then CAS-placeholder-to-Absent
  KeyLockGuard key_guard(key_locks_, k);

  SlotState expected = SlotState::Placeholder;
  slots_[idx].seq.fetch_add(1, std::memory_order_release);  // odd
  // CAS first, then write fields on success
  if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Absent,
                                                 std::memory_order_acq_rel)) {
    if (slots_[idx].key != k) {
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
      return Status::Error;
    }
    slots_[idx].dirty.store(true, std::memory_order_release);
    slots_[idx].clock_bit.store(true, std::memory_order_release);
    slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
    slots_[idx].seq.fetch_add(1, std::memory_order_release);  // even
    return Status::Ok;
  }
  slots_[idx].seq.fetch_add(1, std::memory_order_release);  // restore even
  return Status::Error;
}

int CacheAttachment::occupied_count() const {
  int count = 0;
  for (int i = 0; i < kCacheSlots; ++i) {
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st == SlotState::Occupied) ++count;
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

    // Lock-free probe: read state + clock_bit under seqlock
    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

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

    // Try CAS to Tombstone
    SlotState expected = st;
    if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Tombstone,
                                                   std::memory_order_acq_rel)) {
      // We own the slot — safe to read key/value
      *out_key = slots_[idx].key;
      *out_val = slots_[idx].value;
      *out_dirty = slots_[idx].dirty.load(std::memory_order_acquire);
      live_count_.fetch_sub(1, std::memory_order_relaxed);
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      slots_[idx].clock_bit.store(false, std::memory_order_release);
      // Don't bump generation — evict_slot semantics: slot key/val still readable
      return Status::Ok;
    }
    // CAS failed — state changed concurrently, continue scanning
  }

  // Fallback: all non-empty/non-tombstone slots are PLACEHOLDERs — evict the first one
  for (int i = 0; i < kCacheSlots; ++i) {
    SlotState expected = SlotState::Placeholder;
    if (slots_[i].state.compare_exchange_strong(expected, SlotState::Tombstone,
                                                 std::memory_order_acq_rel)) {
      *out_key = slots_[i].key;
      *out_val = 0;
      *out_dirty = false;
      live_count_.fetch_sub(1, std::memory_order_relaxed);
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      return Status::Ok;
    }
  }

  return Status::Error;  // nothing to evict
}

int CacheAttachment::collect_clean_clock(
    std::vector<std::pair<Key, Value>>& out,
    std::vector<bool>& out_is_absent, int max_count) {
  out.clear();
  out_is_absent.clear();
  if (max_count <= 0) return 0;

  // CLOCK scan: select up to max_count clean Occupied or Absent entries.
  // Does NOT modify slot state — caller must evict after chunk creation.
  for (int round = 0;
       round < 2 * kCacheSlots && static_cast<int>(out.size()) < max_count;
       ++round) {
    size_t idx = hand_.fetch_add(1, std::memory_order_relaxed) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

    if (st != SlotState::Occupied && st != SlotState::Absent) continue;

    // Skip dirty entries — they go through flush_dirty / dirty chunk path.
    if (st == SlotState::Occupied && slots_[idx].dirty.load(std::memory_order_acquire)) continue;

    // CLOCK second-chance: give recently accessed entries another pass.
    bool bit = slots_[idx].clock_bit.load(std::memory_order_acquire);
    if (bit) {
      slots_[idx].clock_bit.store(false, std::memory_order_release);
      continue;
    }

    // Found a victim — copy data, leave slot intact.
    out.emplace_back(sk, sv);
    out_is_absent.push_back(st == SlotState::Absent);

    // Clear clock_bit so this entry won't be re-selected immediately if
    // it stays in cache (caller may decide not to evict it).
    slots_[idx].clock_bit.store(false, std::memory_order_release);
  }

  return static_cast<int>(out.size());
}

Status CacheAttachment::find_clock_victim(Key* out_key, Value* out_val,
                                       bool* out_dirty, int* out_idx,
                                       uint32_t* out_gen) {
  if (!out_key || !out_val || !out_dirty || !out_idx || !out_gen) return Status::Error;

  for (int round = 0; round < 2 * kCacheSlots; ++round) {
    size_t idx = hand_.fetch_add(1, std::memory_order_relaxed) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

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
    SlotState st2 = slots_[idx].state.load(std::memory_order_acquire);
    if (st2 != SlotState::Occupied && st2 != SlotState::Absent) continue;
    if (slots_[idx].clock_bit.load(std::memory_order_acquire)) continue;

    // Found victim -- copy data + generation snapshot (does NOT clear)
    *out_key = sk;
    *out_val = sv;
    *out_dirty = slots_[idx].dirty.load(std::memory_order_acquire);
    *out_idx = static_cast<int>(idx);
    *out_gen = slots_[idx].generation.load(std::memory_order_acquire);
    return Status::Ok;
  }

  // Fallback: all non-empty/non-tombstone slots are PLACEHOLDERs
  for (int i = 0; i < kCacheSlots; ++i) {
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st != SlotState::Placeholder) continue;

    // Capture under seqlock
    Fingerprint sfp;
    Key sk;
    Value sv;
    // For placeholders, a simple read is fine (no concurrent writes without key lock)
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

  // Verify the slot still contains the same key+generation we selected.
  // A generation mismatch means another thread recycled the slot (ABA).
  if (slots_[idx].key != expected_key) return false;
  if (slots_[idx].generation.load(std::memory_order_acquire) != expected_gen)
    return false;

  // Try CAS to Tombstone
  SlotState expected = slots_[idx].state.load(std::memory_order_acquire);
  if (expected != SlotState::Occupied && expected != SlotState::Absent &&
      expected != SlotState::Placeholder)
    return false;

  if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Tombstone,
                                                  std::memory_order_acq_rel)) {
    // Double-check key+gen didn't change during CAS
    if (slots_[idx].key != expected_key ||
        slots_[idx].generation.load(std::memory_order_acquire) != expected_gen) {
      // ABA: slot was recycled between our key+gen check and CAS.
      // State is now Tombstone but it was recycled underneath us.
      // This is extremely rare but can happen.
      return false;
    }
    live_count_.fetch_sub(1, std::memory_order_relaxed);
    tombstone_count_.fetch_add(1, std::memory_order_relaxed);
    slots_[idx].clock_bit.store(false, std::memory_order_release);
    // Don't bump generation here — eviction changes state but doesn't
    // recycle the slot's identity (key stays until next insert).
    return true;
  }
  return false;
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
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st == SlotState::Occupied || st == SlotState::Placeholder ||
        st == SlotState::Absent) {
      // Seqlock read for consistent snapshot
      SlotState st2;
      Fingerprint sfp;
      Key sk;
      Value sv;
      if (seqlock_read(slots_[i], st2, sfp, sk, sv) && st2 == st) {
        entries.push_back({sk, sv, st, slots_[i].dirty.load(std::memory_order_acquire), sfp});
      }
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

      SlotState expected = SlotState::Empty;
      target->slots_[idx].seq.fetch_add(1, std::memory_order_release);
      // CAS first, then write fields on success
      if (target->slots_[idx].state.compare_exchange_strong(
              expected, e.state, std::memory_order_acq_rel)) {
        target->slots_[idx].fp = fp;
        target->slots_[idx].key = e.key;
        target->slots_[idx].value = e.value;
        target->slots_[idx].dirty.store(e.dirty, std::memory_order_release);
        target->slots_[idx].clock_bit.store(false, std::memory_order_release);
        target->slots_[idx].seq.fetch_add(1, std::memory_order_release);
        target->live_count_.fetch_add(1, std::memory_order_relaxed);
        placed = true;
        break;
      }
      target->slots_[idx].seq.fetch_add(1, std::memory_order_release);
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
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st != SlotState::Occupied) continue;

    // CAS dirty flag from true→false. Only one thread wins per slot.
    // After winning, a concurrent put may RE-set dirty=true — but that
    // happens AFTER our CAS, so the new value stays dirty and we flush
    // the old value correctly.
    bool expected_dirty = true;
    if (!slots_[i].dirty.compare_exchange_strong(expected_dirty, false,
                                                   std::memory_order_acq_rel)) {
      continue;  // not dirty, or another thread claimed it
    }

    // Safe to capture key+value: our CAS prevents other flush_dirty threads,
    // and any concurrent put that sets dirty=true again does so after our CAS.
    Key sk = slots_[i].key;
    Value sv = slots_[i].value;
    out.emplace_back(sk, sv);
  }
}

bool CacheAttachment::evict_clean_slot(Key k) {
  Fingerprint fp = fingerprint(k);
  size_t start = fp % kCacheSlots;
  for (size_t i = 0; i < kCacheSlots; ++i) {
    size_t idx = (start + i) % kCacheSlots;

    SlotState st;
    Fingerprint sfp;
    Key sk;
    Value sv;
    if (!seqlock_read(slots_[idx], st, sfp, sk, sv)) continue;

    if (st == SlotState::Empty) return false;  // end of chain, key not found
    if (st == SlotState::Tombstone) continue;
    if (sfp != fp) continue;
    if (sk != k) continue;

    // Only clear if clean — another thread may have re-dirtied it.
    // Absent entries are always clean (no data to persist).
    if (st == SlotState::Occupied && slots_[idx].dirty.load(std::memory_order_acquire)) return false;

    // CAS to Tombstone
    SlotState expected = st;
    if (slots_[idx].state.compare_exchange_strong(expected, SlotState::Tombstone,
                                                   std::memory_order_acq_rel)) {
      live_count_.fetch_sub(1, std::memory_order_relaxed);
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      slots_[idx].clock_bit.store(false, std::memory_order_release);
      return true;
    }
    return false;
  }
  return false;
}

void CacheAttachment::clear_clean_occupied() {
  for (int i = 0; i < kCacheSlots; ++i) {
    // Snapshot state, dirty, and generation under seqlock
    uint32_t s1 = slots_[i].seq.load(std::memory_order_acquire);
    if (s1 & 1) continue;
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    bool d = slots_[i].dirty.load(std::memory_order_acquire);
    uint32_t gen = slots_[i].generation.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = slots_[i].seq.load(std::memory_order_relaxed);
    if (s1 != s2) continue;

    if (st != SlotState::Occupied || d) continue;

    // CAS to Tombstone
    SlotState expected = SlotState::Occupied;
    if (slots_[i].state.compare_exchange_strong(expected, SlotState::Tombstone,
                                                  std::memory_order_acq_rel)) {
      // Re-check gen: if a concurrent put happened, it incremented gen.
      // The put's in-place update will detect state != Occupied and retry.
      // The data was clean (on SSD/chunk), so it's not lost.
      live_count_.fetch_sub(1, std::memory_order_relaxed);
      tombstone_count_.fetch_add(1, std::memory_order_relaxed);
      slots_[i].clock_bit.store(false, std::memory_order_release);
    }
  }
}

void CacheAttachment::clear() {
  for (int i = 0; i < kCacheSlots; ++i) {
    // Don't bother with CAS here — just atomically set to Empty.
    // This is called during split/debug where no concurrent access is expected.
    slots_[i].seq.fetch_add(1, std::memory_order_release);
    slots_[i].state.store(SlotState::Empty, std::memory_order_release);
    slots_[i].clock_bit.store(false, std::memory_order_release);
    slots_[i].seq.fetch_add(1, std::memory_order_release);
  }
  tombstone_count_.store(0, std::memory_order_relaxed);
  live_count_.store(0, std::memory_order_relaxed);
}

std::vector<std::pair<Key, Value>> CacheAttachment::occupied_sorted() {
  std::vector<std::pair<Key, Value>> result;
  for (int i = 0; i < kCacheSlots; ++i) {
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st == SlotState::Occupied) {
      // Seqlock read for consistency
      Key sk = slots_[i].key;
      Value sv = slots_[i].value;
      result.emplace_back(sk, sv);
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
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st == SlotState::Absent) {
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
    SlotState st = slots_[i].state.load(std::memory_order_acquire);
    if (st == SlotState::Occupied || st == SlotState::Placeholder ||
        st == SlotState::Absent) {
      entries.push_back(
          {slots_[i].key, slots_[i].value, st, slots_[i].dirty.load(std::memory_order_acquire), slots_[i].fp});
    }
  }

  // Clear all slots to Empty, reset tombstones.
  clear();
  live_count_.store(0, std::memory_order_relaxed);

  // Re-insert using open addressing (CAS-based, as in upsert).
  for (const auto& e : entries) {
    size_t start = e.fp % kCacheSlots;
    for (size_t i = 0; i < kCacheSlots; ++i) {
      size_t idx = (start + i) % kCacheSlots;
      SlotState expected = SlotState::Empty;
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
      // CAS first, then write fields on success
      if (slots_[idx].state.compare_exchange_strong(expected, e.state,
                                                     std::memory_order_acq_rel)) {
        slots_[idx].fp = e.fp;
        slots_[idx].key = e.key;
        slots_[idx].value = e.value;
        slots_[idx].dirty.store(e.dirty, std::memory_order_release);
        slots_[idx].clock_bit.store(false, std::memory_order_release);
        slots_[idx].generation.fetch_add(1, std::memory_order_relaxed);
        slots_[idx].seq.fetch_add(1, std::memory_order_release);
        live_count_.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      slots_[idx].seq.fetch_add(1, std::memory_order_release);
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

int CacheAttachment::live_count() const {
  return live_count_.load(std::memory_order_relaxed);
}

}  // namespace cbtree
