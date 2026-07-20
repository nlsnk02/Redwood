#include "cbtree/cache_attachment.hpp"
#include "cbtree/fingerprint.hpp"

namespace cbtree {

Status CacheAttachment::upsert(Key k, Value v) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);

  // Search for existing slot with matching key (Occupied, Absent, Placeholder)
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    // Found existing slot -- lock and update in-place
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    slots_[i].value = v;
    slots_[i].state = SlotState::Occupied;
    slots_[i].dirty = true;
    slots_[i].clock_bit.store(true, std::memory_order_release);
    return Status::Ok;
  }

  // No existing slot -- find an empty one
  int empty = -1;
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state == SlotState::Empty) {
      empty = i;
      break;
    }
  }

  if (empty < 0) {
    return Status::Full;
  }

  std::lock_guard<std::mutex> lock(slots_[empty].slot_mutex);
  slots_[empty].fp = fp;
  slots_[empty].key = k;
  slots_[empty].value = v;
  slots_[empty].state = SlotState::Occupied;
  slots_[empty].dirty = true;
  slots_[empty].clock_bit.store(true, std::memory_order_release);
  return Status::Ok;
}

LookupResult CacheAttachment::lookup(Key k) const {
  Fingerprint fp = fingerprint(k);
  for (int i = 0; i < kCacheSlots; ++i) {
    SlotState st = slots_[i].state;
    if (st == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    if (st == SlotState::Occupied) {
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
    if (slots_[i].state == SlotState::Empty) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key != k) continue;

    // Found -- mark as absent
    std::lock_guard<std::mutex> lock(slots_[i].slot_mutex);
    slots_[i].state = SlotState::Absent;
    slots_[i].dirty = true;
    return Status::Ok;
  }

  // Key not present -- insert as absent
  int empty = -1;
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state == SlotState::Empty) {
      empty = i;
      break;
    }
  }

  if (empty < 0) {
    return Status::Full;
  }

  std::lock_guard<std::mutex> lock(slots_[empty].slot_mutex);
  slots_[empty].fp = fp;
  slots_[empty].key = k;
  slots_[empty].state = SlotState::Absent;
  slots_[empty].dirty = true;
  return Status::Ok;
}

Status CacheAttachment::try_place_placeholder(Key k, int* out_idx) {
  KeyLockGuard key_guard(key_locks_, k);
  Fingerprint fp = fingerprint(k);

  // Check if placeholder already exists for this key
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state != SlotState::Placeholder) continue;
    if (slots_[i].fp != fp) continue;
    if (slots_[i].key == k) {
      if (out_idx) *out_idx = i;
      return Status::Ok;
    }
  }

  // Find empty slot
  int empty = -1;
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state == SlotState::Empty) {
      empty = i;
      break;
    }
  }

  if (empty < 0) {
    return Status::Full;
  }

  std::lock_guard<std::mutex> lock(slots_[empty].slot_mutex);
  slots_[empty].fp = fp;
  slots_[empty].key = k;
  slots_[empty].state = SlotState::Placeholder;
  if (out_idx) *out_idx = empty;
  return Status::Ok;
}

Status CacheAttachment::fill_placeholder(int idx, Value v) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Read key -- set during try_place_placeholder, stable while placeholder
  Key k = slots_[idx].key;

  KeyLockGuard key_guard(key_locks_, k);
  std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);

  if (slots_[idx].state != SlotState::Placeholder) {
    return Status::Error;
  }
  slots_[idx].value = v;
  slots_[idx].state = SlotState::Occupied;
  slots_[idx].dirty = true;
  return Status::Ok;
}

Status CacheAttachment::fill_placeholder_absent(int idx) {
  if (idx < 0 || idx >= kCacheSlots) return Status::Error;

  // Read key -- set during try_place_placeholder, stable while placeholder
  Key k = slots_[idx].key;

  KeyLockGuard key_guard(key_locks_, k);
  std::lock_guard<std::mutex> lock(slots_[idx].slot_mutex);

  if (slots_[idx].state != SlotState::Placeholder) {
    return Status::Error;
  }
  slots_[idx].state = SlotState::Absent;
  slots_[idx].dirty = true;
  return Status::Ok;
}

int CacheAttachment::occupied_count() const {
  int count = 0;
  for (int i = 0; i < kCacheSlots; ++i) {
    if (slots_[i].state == SlotState::Occupied) ++count;
  }
  return count;
}

// Stub implementations for future tasks (not required for Task 4)
Status CacheAttachment::pick_clock_victim(Key*, Value*, bool*) {
  return Status::NotImplemented;
}

Status CacheAttachment::split_into(Key, CacheAttachment*) {
  return Status::NotImplemented;
}

std::vector<std::pair<Key, Value>> CacheAttachment::occupied_sorted() {
  return {};
}

}  // namespace cbtree
