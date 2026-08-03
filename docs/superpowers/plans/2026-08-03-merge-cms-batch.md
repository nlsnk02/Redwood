# Merge feat/count-min-sketch-admission × feat/batch-merge-flush — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `feat/merge-cms-batch` combining 9 features from both feat branches onto master `b2e467e`.

**Architecture:** Start from master base, cherry-pick single-branch files, manually merge the 4 shared files (`cache_attachment.hpp/.cpp`, `tree.hpp/.cpp`) by taking Batch as base and adding CMS changes on top.

**Tech Stack:** C++20, CMake, GTests

## Global Constraints

- Base commit: `b2e467e` (master)
- New branch: `feat/merge-cms-batch`
- Build must pass: `cmake -B build -S . && cmake --build build -j$(nproc)`
- Tests must pass: `./build/cbtree_tests`
- Exclude: `.claude/settings.local.json`, `results_*.txt`, `results_*.dat` (Batch temp files)

---

## File Dependency Map

```
Layer 0 (constants/structs, no logic):
  types.hpp ──► independent
  chunk.hpp ──► independent
  key_lock_table.hpp ──► independent

Layer 1 (node struct, depends on Layer 0):
  node.hpp ──► uses chunk types

Layer 2 (new APIs):
  ssd_page_store.hpp ──► independent
  cache_attachment.hpp ──► uses types from types.hpp

Layer 3 (tree header, depends on Layer 1+2):
  tree.hpp ──► uses node, ssd_page_store, cache_attachment types

Layer 4 (implementations):
  ssd_page_store.cpp ──► depends on ssd_page_store.hpp
  cache_attachment.cpp ──► depends on cache_attachment.hpp, key_lock_table.hpp
  tree.cpp ──► depends on all above
  test_tree_evict.cpp ──► depends on tree.hpp
```

---

### Task 1: Setup — Create feature branch

**Files:**
- Create: (branch) `feat/merge-cms-branch`

- [ ] **Step 1: Create branch from master base**

```bash
cd /home/u332/mytree
git checkout b2e467e
git checkout -b feat/merge-cms-batch
```

- [ ] **Step 2: Verify starting point**

```bash
git log --oneline -1
# Expected: b2e467e feat: use CMS for placeholder cache selection (cache_A vs cache_B)
```

- [ ] **Step 3: Commit** (no-op, branch creation only)

---

### Task 2: `types.hpp` — p_placeholder default 0.75 (CMS)

**Files:**
- Modify: `include/cbtree/types.hpp:47`

**Interfaces:**
- Produces: `kDefaultPPlaceholder = 0.75` (was 1.0)

- [ ] **Step 1: Apply change**

```cpp
// Line ~47: change
inline constexpr double kDefaultPPlaceholder = 1.0;
// to
inline constexpr double kDefaultPPlaceholder = 0.75;
```

- [ ] **Step 2: Commit**

```bash
git add include/cbtree/types.hpp
git commit -m "feat: change default p_placeholder from 1.0 to 0.75

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: `chunk.hpp` — is_clean_only + is_absent (Batch)

**Files:**
- Modify: `include/cbtree/chunk.hpp:21,28`

**Interfaces:**
- Produces: `EvictChunk::is_clean_only` (bool), `EvictChunk::Entry::is_absent` (bool)

- [ ] **Step 1: Add is_clean_only flag to EvictChunk**

In `struct EvictChunk`, after `size_t num_entries;` add:

```cpp
  // Distinguishes dirty chunks (SSD safety net, must be written to SSD)
  // from clean chunks (read buffer, already on SSD, discard/compact on flush).
  bool is_clean_only = false;
```

- [ ] **Step 2: Add is_absent flag to Entry**

In `struct Entry`, after `Fingerprint fp;` add:

```cpp
    bool is_absent = false;  // true → negative-cache entry (Absent slot)
```

- [ ] **Step 3: Commit**

```bash
git add include/cbtree/chunk.hpp
git commit -m "feat: add is_clean_only and is_absent to EvictChunk

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: `key_lock_table.hpp` — atomic bitmap (Batch)

**Files:**
- Modify: `include/cbtree/key_lock_table.hpp` (full rewrite)

**Interfaces:**
- Produces: `KeyLockTable` with `lock(Key)` (CAS spin), `unlock(Key)` (fetch_and)
- Size: 8B (was 2560B)

- [ ] **Step 1: Replace entire file**

```cpp
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
  KeyLockGuard(KeyLockTable& table, Key key) : table_(table), key_(key) {
    table_.lock(key_);
  }
  ~KeyLockGuard() { table_.unlock(key_); }
  KeyLockGuard(const KeyLockGuard&) = delete;
  KeyLockGuard& operator=(const KeyLockGuard&) = delete;

 private:
  KeyLockTable& table_;
  Key key_;
};

}  // namespace cbtree
```

- [ ] **Step 2: Commit**

```bash
git add include/cbtree/key_lock_table.hpp
git commit -m "perf: replace KeyLockTable 64×mutex with single atomic bitmap

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: `node.hpp` — remove leaf index, add per-leaf fields (Batch)

**Files:**
- Modify: `include/cbtree/node.hpp:27-34,44-48`

**Interfaces:**
- Removes: `leaf_keys`, `leaf_page_ids`, `leaf_index_mutex`
- Adds: `dirty_chunk_count_` (atomic), `flush_mutex_` (per-leaf, replaces global)

- [ ] **Step 1: Remove leaf index members**

Remove these lines from `struct Node`:
```cpp
  std::vector<Key> leaf_keys;         // ordered key list (populated only during flush)
  std::vector<PageId> leaf_page_ids;  // page id for each leaf_keys[i]
  PageId page_id{0};                  // SSD page for this leaf
  mutable std::mutex leaf_index_mutex; // protects leaf_keys / leaf_page_ids
```

Replace with:
```cpp
  PageId page_id{0};                  // SSD page for this leaf
```

(Keep `page_id` — it's still needed.)

- [ ] **Step 2: Move eviction_mutex, add flush_mutex**

After the `page_id` line, the existing `mutable std::mutex eviction_mutex;` stays. Add below it:
```cpp
  mutable std::mutex flush_mutex_;     // per-leaf flush serialization
```

- [ ] **Step 3: Add dirty_chunk_count_**

After `std::atomic<size_t> chunk_count_{0};` add:
```cpp
  std::atomic<size_t> dirty_chunk_count_{0};  // subset of chunk_count_ for dirty chunks
```

- [ ] **Step 4: Commit**

```bash
git add include/cbtree/node.hpp
git commit -m "perf: remove in-memory leaf key index, add per-leaf flush_mutex

Remove leaf_keys/leaf_page_ids/leaf_index_mutex — page fullness
is now checked by reading SSD pages directly.  Add per-leaf
flush_mutex_ (replaces global flush_mutex_) and dirty_chunk_count_
for two-phase eviction.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: `ssd_page_store.hpp` — IoStats + split_page overload (Batch)

**Files:**
- Modify: `include/cbtree/ssd_page_store.hpp:55-104`

**Interfaces:**
- Produces: `IoStats` struct, `io_stats()`, `reset_io_stats()`, `split_page(pid, mid, entries, &new_id)`

- [ ] **Step 1: Add split_page overload declaration**

After the existing `split_page` declaration, add:
```cpp
  // Split using pre-loaded entries — skips the internal read_page.
  // Caller must hold tree_mutex_ to serialise structural changes.
  Status split_page(PageId left_id, Key mid,
                    const std::vector<std::pair<Key, Value>>& entries,
                    PageId* new_right_id);
```

- [ ] **Step 2: Add IoStats struct and accessors in public section**

At the end of the class (before `};`), in the public section add:
```cpp
 public:
  // I/O operation counters (debug — atomic for concurrent access).
  struct IoStats {
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t dump_sorted = 0;
    uint64_t splits = 0;
    uint64_t write_entries = 0;
  };
  IoStats io_stats() const;
  void reset_io_stats();

 private:
  mutable std::atomic<uint64_t> io_reads_{0};
  mutable std::atomic<uint64_t> io_writes_{0};
  mutable std::atomic<uint64_t> io_dump_sorted_{0};
  mutable std::atomic<uint64_t> io_splits_{0};
  mutable std::atomic<uint64_t> io_write_entries_{0};
```

- [ ] **Step 3: Add `#include <atomic>`** at top if not present

- [ ] **Step 4: Commit**

```bash
git add include/cbtree/ssd_page_store.hpp
git commit -m "feat: add IoStats counters and split_page(entries) overload

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: `ssd_page_store.cpp` — IoStats impl + split_page overload (Batch)

**Files:**
- Modify: `src/ssd_page_store.cpp`

**Interfaces:**
- Implements: `io_stats()`, `reset_io_stats()`, `split_page(pid, mid, entries, &new_id)`
- Increments IoStats counters in: `read_page_locked`, `write_page_locked`, `dump_sorted`, `write_page_entries`, both `split_page` overloads

**Strategy:** Take the file from `feat/batch-merge-flush` as-is (it contains all master logic plus the new additions).

- [ ] **Step 1: Copy from batch branch**

```bash
git checkout feat/batch-merge-flush -- src/ssd_page_store.cpp
```

- [ ] **Step 2: Stage and commit**

```bash
git add src/ssd_page_store.cpp
git commit -m "feat: add IoStats tracking and split_page(entries) to SsDPageStore

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: `cache_attachment.hpp` — lock-free redesign + live_count (Batch + CMS merge)

**Files:**
- Modify: `include/cbtree/cache_attachment.hpp`

**Interfaces:**
- Consumes: types from `types.hpp`
- Produces: Lock-free `CacheSlot` (32B), `CacheAttachment` with `live_count()`, `collect_clean_clock()`, `flush_dirty()`
- CMS additions: `live_count_` atomic, `live_count()` method

**Strategy:** Start with Batch version, add `live_count_` and `live_count()` from CMS.

- [ ] **Step 1: Copy Batch version as base**

```bash
git checkout feat/batch-merge-flush -- include/cbtree/cache_attachment.hpp
```

- [ ] **Step 2: Add live_count_ member**

In the `CacheAttachment` private section (before `};`), find the end of member declarations. After `tombstone_count_` add:

```cpp
  std::atomic<int> live_count_{0};  // Occupied + Placeholder, maintained atomically
```

- [ ] **Step 3: Add live_count() accessor in public section**

After `occupied_count() const;` add:

```cpp
  // Count Occupied + Placeholder in a single pass — both types consume
  // a slot that could otherwise be reused for a new entry.
  int live_count() const;
```

- [ ] **Step 4: Commit**

```bash
git add include/cbtree/cache_attachment.hpp
git commit -m "refactor: lock-free CacheSlot + live_count accessor

Merge Batch's lock-free CacheSlot redesign (seqlock+CAS, 32B slots)
with CMS's atomic live_count_ (Occupied+Placeholder).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: `cache_attachment.cpp` — full lock-free rewrite + live_count (Batch + CMS merge)

**Files:**
- Modify: `src/cache_attachment.cpp` (major rewrite)

**Interfaces:**
- Consumes: `cache_attachment.hpp`, `key_lock_table.hpp` (atomic bitmap)
- Produces: All lock-free slot operations, `live_count()`, `collect_clean_clock()`, `flush_dirty()`
- CMS integration: `live_count_` maintenance at every slot state change

**Strategy:** Start with Batch's full rewrite, then add CMS `live_count_` calls at 15 precise locations.

- [ ] **Step 1: Copy Batch version as base**

```bash
git checkout feat/batch-merge-flush -- src/cache_attachment.cpp
```

- [ ] **Step 2: Add live_count_ increment in upsert — Empty slot path**

In `Status CacheAttachment::upsert(...)`, locate the `cas_insert` call for `SlotState::Empty`. After the successful CAS block (where `dirty` and `clock_bit` are set), add:

```cpp
        live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 3: Add live_count_ increment in upsert — Tombstone slot path**

In the same function, locate the `cas_insert` call for `SlotState::Tombstone`. After the same fields are set, add:

```cpp
        live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 4: Add live_count_ in mark_absent — new entry path**

In `Status CacheAttachment::mark_absent(...)`, after `cas_insert` of Absent state from Empty/Tombstone, add:

```cpp
      live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 5: Add live_count_ in try_place_placeholder**

In `Status CacheAttachment::try_place_placeholder(...)`, after CAS from Empty/Tombstone to Placeholder, add:

```cpp
      live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 6: Add live_count_ in fill_placeholder and fill_placeholder_absent**

In both `fill_placeholder` and `fill_placeholder_absent`, the slot transitions Placeholder→Occupied and Placeholder→Absent respectively. Both keep the slot live, so NO live_count_ change needed (already counted). **Skip.**

- [ ] **Step 7: Add live_count_ decrement in pick_clock_victim — Occupied/Absent eviction**

In `Status CacheAttachment::pick_clock_victim(...)`, after CAS to Tombstone succeeds for an Occupied or Absent slot, add:

```cpp
      live_count_.fetch_sub(1, std::memory_order_relaxed);
```

- [ ] **Step 8: Add live_count_ decrement in pick_clock_victim — Placeholder eviction (fallback)**

In the fallback path of `pick_clock_victim`, after evicting a non-Empty/non-Tombstone slot (which catches Placeholder slots), add:

```cpp
      live_count_.fetch_sub(1, std::memory_order_relaxed);
```

- [ ] **Step 9: Add live_count_ decrement in evict_slot**

In `bool CacheAttachment::evict_slot(...)`, after CAS to Tombstone succeeds:

```cpp
  live_count_.fetch_sub(1, std::memory_order_relaxed);
```

- [ ] **Step 10: Add live_count_ increment in split_into**

In `Status CacheAttachment::split_into(...)`, after successfully inserting an entry into the right cache (via `cas_insert`):

```cpp
        target->live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 11: Add live_count_ decrement in evict_clean_slot**

In `bool CacheAttachment::evict_clean_slot(...)`, after CAS to Tombstone succeeds:

```cpp
    live_count_.fetch_sub(1, std::memory_order_relaxed);
```

- [ ] **Step 12: Add live_count_ decrement in clear_clean_occupied**

In `void CacheAttachment::clear_clean_occupied()`, after each CAS to Tombstone:

```cpp
      live_count_.fetch_sub(1, std::memory_order_relaxed);
```

- [ ] **Step 13: Add live_count_ reset in clear()**

In `void CacheAttachment::clear()`, after resetting `tombstone_count_`:

```cpp
  live_count_.store(0, std::memory_order_relaxed);
```

- [ ] **Step 14: Add live_count_ reset in rehash()**

In `void CacheAttachment::rehash()`, after resetting `tombstone_count_` (before the re-insert loop):

```cpp
  live_count_.store(0, std::memory_order_relaxed);
```

And inside the re-insert loop, after each successful `cas_insert`:

```cpp
        live_count_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 15: Implement live_count() accessor**

At the end of the file, add:

```cpp
int CacheAttachment::live_count() const {
  return live_count_.load(std::memory_order_relaxed);
}
```

- [ ] **Step 16: Commit**

```bash
git add src/cache_attachment.cpp
git commit -m "perf: lock-free cache operations with CMS live_count tracking

Full rewrite of CacheAttachment with seqlock+CAS protocol plus
CMS's atomic live_count_ (Occupied+Placeholder) for eviction
threshold decisions.  Includes collect_clean_clock() and
flush_dirty() for two-phase clean chunk eviction.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: `tree.hpp` — merge headers (Batch remove + CMS add)

**Files:**
- Modify: `include/cbtree/tree.hpp`

**Interfaces:**
- Removes: `register_in_leaf_index` declaration
- Adds: `EvictDebugCounters` struct + `evict_debug_counters()` (CMS)
- Adds: `debug_leaf_count()` (Batch)
- Adds: `io_stats()` delegation (Batch)
- Adds: 4 atomic eviction counter members (CMS)
- Removes: global `flush_mutex_` (replaced by per-leaf)
- Removes: `flush_batch_threshold_` member + setter/getter (replaced by per-leaf thresholds)

**Strategy:** Start with Batch version (has removals + Batch additions), add CMS additions on top.

- [ ] **Step 1: Copy Batch version as base**

```bash
git checkout feat/batch-merge-flush -- include/cbtree/tree.hpp
```

- [ ] **Step 2: Replace EvictDebugCounters compat stub with real CMS implementation**

Find the Batch compat stub:
```cpp
  // Debug: eviction counters (compat stub — no-op on this branch)
  struct EvictDebugCounters {
    uint64_t evict_a_calls = 0;
    uint64_t evict_a_actual = 0;
    uint64_t evict_b_calls = 0;
    uint64_t evict_b_actual = 0;
  };
  EvictDebugCounters evict_debug_counters() const { return {}; }
```

Replace with CMS real implementation:
```cpp
  // Debug: eviction counters on the get() placeholder path.
  struct EvictDebugCounters {
    uint64_t evict_a_calls;
    uint64_t evict_a_actual;
    uint64_t evict_b_calls;
    uint64_t evict_b_actual;
  };
  EvictDebugCounters evict_debug_counters() const {
    return {
      get_evict_a_calls_.load(std::memory_order_relaxed),
      get_evict_a_actual_.load(std::memory_order_relaxed),
      get_evict_b_calls_.load(std::memory_order_relaxed),
      get_evict_b_actual_.load(std::memory_order_relaxed),
    };
  }
```

- [ ] **Step 3: Add CMS atomic counter members in private section**

In the private section of `Tree`, find where `enable_hit_tracking_` is declared. After it, add:

```cpp
  // Debug: eviction counters for get()-path placeholder eviction analysis.
  mutable std::atomic<uint64_t> get_evict_a_calls_{0};
  mutable std::atomic<uint64_t> get_evict_a_actual_{0};
  mutable std::atomic<uint64_t> get_evict_b_calls_{0};
  mutable std::atomic<uint64_t> get_evict_b_actual_{0};
```

- [ ] **Step 4: Verify no global flush_mutex_ remains**

Grep for `flush_mutex_` in the private section. It should NOT be present (Batch removed it, replaced by per-leaf in node.hpp).

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/tree.hpp
git commit -m "refactor: merge tree.hpp — remove leaf index, add EvictDebugCounters

Remove register_in_leaf_index, global flush_mutex_, flush_batch_threshold.
Add CMS EvictDebugCounters with real atomic counter implementation.
Add debug_leaf_count() and io_stats() from batch branch.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: `tree.cpp` — full functional merge (Batch base + CMS additions)

**Files:**
- Modify: `src/tree.cpp` (major merge)

**Interfaces:**
- Consumes: All headers from Tasks 2-10
- Produces: Merged tree logic combining all 9 features

**Strategy:** Start with Batch's tree.cpp (has all structural changes), then:
1. Replace `get()` placeholder routing with CMS version
2. Ensure `evict_cache_A_if_needed` uses `live_count()` (CMS)
3. Verify `flush_batch_merged` does NOT call `register_in_leaf_index`

- [ ] **Step 1: Copy Batch version as base**

```bash
git checkout feat/batch-merge-flush -- src/tree.cpp
```

- [ ] **Step 2: Update evict_cache_A_if_needed to use live_count()**

Find `evict_cache_A_if_needed`. The Batch version uses `occupied_count()`. Change to `live_count()`:

```cpp
Status Tree::evict_cache_A_if_needed(Node* leaf) {
  // Count Occupied + Placeholder: both consume a slot that could hold new data.
  if (leaf->cache_A->live_count() <=
      static_cast<int>(kCacheSlots * kParentFillThreshold))
    return Status::Ok;
  // ... rest stays the same
```

- [ ] **Step 3: Replace get() placeholder routing with CMS version**

Find the placeholder placement section in `get()` (search for `try_place_placeholder`). Replace the entire placeholder placement block with CMS's simplified logic:

```cpp
    // Placeholder placement: hot keys always go to cache_A; cold keys
    // use cache_B with probability p_placeholder_.  No cross-cache fallback —
    // if the target cache is full, evict a victim and retry once.  This keeps
    // cache_A free of cold-key pollution and cache_B free of cold-key churn
    // (when p_placeholder_ < 1.0).
    bool has_placed = false;
    int placeholder_idx = -1;
    CacheAttachment* ph_cache = nullptr;

    auto try_place = [&](CacheAttachment* cache) -> bool {
      Status s = cache->try_place_placeholder(k, &placeholder_idx);
      if (s == Status::Ok) {
        has_placed = true;
        ph_cache = cache;
        return true;
      }
      return false;
    };

    uint64_t freq = cms_.estimate(k);
    bool hot = (freq >= static_cast<uint64_t>(cms_admission_threshold_));

    if (hot) {
      // Hot key: always place in cache_A.  Evict if full, no fallback to B.
      if (!try_place(leaf->cache_A.get())) {
        get_evict_a_calls_.fetch_add(1, std::memory_order_relaxed);
        int before = leaf->cache_A->live_count();
        evict_cache_A_if_needed(leaf);
        int after = leaf->cache_A->live_count();
        if (after < before)
          get_evict_a_actual_.fetch_add(1, std::memory_order_relaxed);
        try_place(leaf->cache_A.get());
      }
    } else {
      // Cold key: p_placeholder_ controls whether to place in cache_B.
      // Evict if full, no fallback to A.
      if (p_placeholder_ >= 1.0 ||
          (p_placeholder_ > 0.0 &&
           std::bernoulli_distribution{p_placeholder_}(rng))) {
        if (!try_place(leaf->cache_B.get())) {
          get_evict_b_calls_.fetch_add(1, std::memory_order_relaxed);
          int before = leaf->cache_B->occupied_count();
          evict_leaf_if_needed(leaf);
          int after = leaf->cache_B->occupied_count();
          if (after < before)
            get_evict_b_actual_.fetch_add(1, std::memory_order_relaxed);
          try_place(leaf->cache_B.get());
        }
      }
    }
```

**Note:** The `rng` variable (thread_local std::mt19937_64) must be present. Batch's tree.cpp already declares it; verify it's there. If not, add before `get()`:

```cpp
  thread_local std::mt19937_64 rng(std::random_device{}());
```

- [ ] **Step 4: Verify flush_batch_merged does NOT call register_in_leaf_index**

Search for `register_in_leaf_index` in the file. It must NOT appear (Batch removed the function entirely). If found, remove those calls.

- [ ] **Step 5: Verify flush_leaf uses per-leaf flush_mutex_**

Search for `std::unique_lock<std::mutex> flush_lock`. It should use `leaf->flush_mutex_`, NOT `flush_mutex_`:

```cpp
  std::unique_lock<std::mutex> flush_lock(leaf->flush_mutex_, std::try_to_lock);
```

- [ ] **Step 6: Replace CMS block placements in clean chunk compaction**

Find the clean chunk compaction section in `flush_leaf` (search for `compact_clean`). The CMS frequency scoring must reference `cms_.estimate(k)`. The Batch version already has this — verify it uses `cms_.estimate`.

- [ ] **Step 7: Commit**

```bash
git add src/tree.cpp
git commit -m "feat: merge tree.cpp — batch flush + CMS get() routing + clean chunks

Merge all structural changes from batch-merge-flush (leaf index removal,
per-leaf flush, two-phase eviction) with CMS's simplified get() placeholder
routing (hot→A only, cold→B+p_placeholder) and EvictDebugCounters.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: `test_tree_evict.cpp` — adopt Batch test change

**Files:**
- Modify: `tests/test_tree_evict.cpp:49-60`

**Interfaces:**
- Produces: Updated test `FlushAndLeafIndexRemoved` reflecting leaf index removal

- [ ] **Step 1: Copy test from Batch branch**

```bash
git checkout feat/batch-merge-flush -- tests/test_tree_evict.cpp
```

- [ ] **Step 2: Commit**

```bash
git add tests/test_tree_evict.cpp
git commit -m "test: update FlushAndLeafIndexRemoved for leaf index removal

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: `docs/get-path-design.md` — adopt CMS design doc

**Files:**
- Create: `docs/get-path-design.md`

**Interfaces:**
- Produces: Design documentation for get()-path placeholder routing

- [ ] **Step 1: Copy from CMS branch**

```bash
git checkout feat/count-min-sketch-admission -- docs/get-path-design.md
```

- [ ] **Step 2: Commit**

```bash
git add docs/get-path-design.md
git commit -m "docs: add get()-path placeholder routing design doc

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: Build verification

**Files:**
- Verify: all source files compile and link

- [ ] **Step 1: Clean build**

```bash
cd /home/u332/mytree
rm -rf build_merge
cmake -B build_merge -S .
cmake --build build_merge -j$(nproc)
```

Expected: Build completes with zero errors. Warnings may exist (e.g., unused-result from system() calls in cbtree_adapter).

- [ ] **Step 2: Run tests**

```bash
./build_merge/cbtree_tests
```

Expected: All tests pass, including `FlushAndLeafIndexRemoved`.

- [ ] **Step 3: If build fails**

Inspect the error. Common failure points:
- Missing `#include` in reordered headers
- `flush_mutex_` reference in tree.cpp not updated to `leaf->flush_mutex_`
- `register_in_leaf_index` call not removed
- `live_count_` access with wrong type or scope

Fix the error and rebuild. Do NOT commit yet — fix within this task.

- [ ] **Step 4: Commit** (only if fixes were needed)

```bash
git add -u
git commit -m "fix: build fixes for merge branch

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 15: Benchmark verification

**Files:**
- Verify: benchmark runs correctly

- [ ] **Step 1: Build YCSB adapter**

```bash
cd /home/u332/ycsb/adapters/cbtree
make clean
make CXXFLAGS="-std=c++20 -Wall -Wextra -O2 -g -I../../src -I/home/u332/mytree/include" \
     LDFLAGS="-L/home/u332/mytree/build_merge -lcbtree -L/home/u332/ycsb/build -lycsb -lpthread"
```

- [ ] **Step 2: Quick smoke test — workload A, single run**

```bash
./ycsb_cbtree_bench --dio -t 3 -r 1 --records 10000 -p /tmp/ycsb_merge_smoke A
```

Expected: Completes without segfault. Throughput is nonzero.

- [ ] **Step 3: Full benchmark**

Run the 3-way sequential benchmark pattern: DIO, 3 threads, 3 rounds, workloads A/B/C, 10k records. Compare against earlier CMS and Batch results.

No commit needed — verification only.
